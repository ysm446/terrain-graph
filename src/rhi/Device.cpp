#include "rhi/Device.h"

#include "core/ImageIo.h"

#include "core/Log.h"

#include <cstddef>
#include <vector>

#include <pix3.h>

#include <utility>

namespace tg::rhi {
namespace {

// **シェーダから触るものは全部ここへ並ぶ**（読み込んだ素材、合成の中間テクスチャ、
// マスク、サムネイル、環境マップ、ImGui のフォント）。素材 1 枚でも
// 本体 + sRGB + チャンネル別の 4 本を使うので、1024 では素材を並べただけで届く。
// ディスクリプタは 1 枠 32 バイト程度（8192 枠で 256KB ほど）、上限は
// Tier 1 でも 100 万枠なので、ここは余裕を持たせるほうが安い。
constexpr uint32_t kSrvHeapCapacity = 8192;
constexpr uint32_t kRtvHeapCapacity = 64;
constexpr uint32_t kDsvHeapCapacity = 32;

// 1 フレームあたりのアップロード容量。定数バッファと小さめの転送を想定した初期値。
constexpr uint64_t kUploadBytesPerFrame = 16ull * 1024 * 1024;

}  // namespace

Device::~Device() {
    Shutdown();
}

bool Device::Initialize(HWND hwnd, uint32_t width, uint32_t height, bool enableDebugLayer) {
    m_width = width;
    m_height = height;

    if (!CreateFactoryAndDevice(enableDebugLayer)) {
        return false;
    }
    if (!CreateCommandObjects()) {
        return false;
    }
    if (!CreateSwapChain(hwnd, width, height)) {
        return false;
    }
    if (!m_rtvHeap.Create(m_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kRtvHeapCapacity,
                          false)) {
        return false;
    }
    if (!m_dsvHeap.Create(m_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, kDsvHeapCapacity,
                          false)) {
        return false;
    }
    if (!m_srvHeap.Create(m_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                          kSrvHeapCapacity, true)) {
        return false;
    }
    if (!CreateBackBufferViews()) {
        return false;
    }
    if (!m_allocator.Create(m_device.Get(), m_adapter.Get(), &m_srvHeap, &m_rtvHeap, &m_dsvHeap)) {
        return false;
    }
    if (!m_uploadRing.Create(m_allocator, kUploadBytesPerFrame)) {
        return false;
    }

    m_initialized = true;
    return true;
}

void Device::Defer(ComPtr<IUnknown> object) {
    // 現在記録中のフレームは m_nextFenceValue で Signal される。
    m_deletionQueue.Push(std::move(object), m_nextFenceValue);
}

void Device::SetAuxiliaryFence(ID3D12Fence* fence, uint64_t value) {
    m_auxiliaryFence = fence;
    m_auxiliaryFenceValue = value;
    m_deletionQueue.SetAuxiliaryFence(m_auxiliaryFence, value);
}

void Device::RequestBackBufferCapture(const std::filesystem::path& path,
                                      CaptureCallback onComplete) {
    m_capturePath = path;
    m_captureCallback = std::move(onComplete);
}

// 撮影の後始末。成否を呼び出し側へ伝えてから、保留していた状態を捨てる。
void Device::FinishCapture(bool success) {
    const CaptureCallback callback = std::move(m_captureCallback);
    const std::filesystem::path path = m_capturePath;
    m_captureCallback = {};
    m_capturePath.clear();
    m_pendingCapture = GpuBuffer{};
    if (callback) {
        callback(success, path, m_width, m_height);
    }
}

void Device::DeferFree(DescriptorHeap& heap, const DescriptorHandle& handle) {
    m_deletionQueue.Push(&heap, handle, m_nextFenceValue);
}

// ミップごとの UAV / SRV だけを返す。**ミップ連鎖を作り終えたら呼ぶ。**
// これらはミップを作る間しか使わないのに、テクスチャ 1 枚で
// ミップ数 × 2 枠（2K で 24 枠）を占め続ける。SRV ヒープの枠は有限なので、
// 用が済んだら返さないと素材を並べただけで枯渇する。
void Device::DeferFreeMipViews(GpuTexture& texture) {
    // uav は mipUavs[0] と同じハンドルなので、返したあとは一緒に落とす
    // （残すと解放済みのディスクリプタを指したままになる）。
    for (const DescriptorHandle& handle : texture.mipUavs) {
        DeferFree(m_srvHeap, handle);
    }
    for (const DescriptorHandle& handle : texture.mipSrvs) {
        DeferFree(m_srvHeap, handle);
    }
    texture.mipUavs.clear();
    texture.mipSrvs.clear();
    texture.uav = DescriptorHandle{};
}

void Device::DeferRelease(GpuTexture& texture) {
    // uav は mipUavs[0] と同じハンドルなので、二重解放しないよう mipUavs 側だけ返す。
    DeferFree(m_srvHeap, texture.srv);
    for (const DescriptorHandle& handle : texture.mipUavs) {
        DeferFree(m_srvHeap, handle);
    }
    for (const DescriptorHandle& handle : texture.mipSrvs) {
        DeferFree(m_srvHeap, handle);
    }
    DeferFree(m_rtvHeap, texture.rtv);
    DeferFree(m_dsvHeap, texture.dsv);
    Defer(texture.resource);
    Defer(texture.allocation);
    texture = GpuTexture{};
}

void Device::DeferRelease(GpuBuffer& buffer) {
    DeferFree(m_srvHeap, buffer.srv);
    DeferFree(m_srvHeap, buffer.uav);
    Defer(buffer.resource);
    Defer(buffer.allocation);
    buffer = GpuBuffer{};
}

Device::VideoMemory Device::QueryVideoMemory() const {
    VideoMemory result;

    // D3D12MA の Budget は、内部で QueryVideoMemoryInfo を呼んだプロセス全体の値と、
    // アロケータ自身の統計をまとめて返す。両方をここで一度に受け取る。
    if (D3D12MA::Allocator* allocator = m_allocator.Raw(); allocator != nullptr) {
        D3D12MA::Budget local = {};
        allocator->GetBudget(&local, nullptr);
        result.usage = local.UsageBytes;
        result.budget = local.BudgetBytes;
        result.allocated = local.Stats.AllocationBytes;
        result.reserved = local.Stats.BlockBytes;
        return result;
    }

    // アロケータがまだ無い場合でも、プロセス全体の値だけは取れる。
    if (m_adapter) {
        DXGI_QUERY_VIDEO_MEMORY_INFO info = {};
        if (SUCCEEDED(m_adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
            result.usage = info.CurrentUsage;
            result.budget = info.Budget;
        }
    }
    return result;
}

uint64_t Device::CompletedFenceValue() const {
    return m_fence ? m_fence->GetCompletedValue() : 0;
}

bool Device::CreateFactoryAndDevice(bool enableDebugLayer) {
    UINT factoryFlags = 0;

    if (enableDebugLayer) {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
            debug->EnableDebugLayer();
            TG_LOG_INFO("D3D12 デバッグレイヤーを有効化しました");

            ComPtr<ID3D12Debug1> debug1;
            if (SUCCEEDED(debug.As(&debug1))) {
                debug1->SetEnableGPUBasedValidation(TRUE);
                TG_LOG_INFO("GPU ベースバリデーションを有効化しました");
            }
            factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
        } else {
            TG_LOG_WARN("D3D12 デバッグレイヤーを取得できませんでした（Graphics Tools 未導入の可能性）");
        }
    }

    if (!TG_CHECK_HR(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&m_factory)))) {
        return false;
    }

    BOOL allowTearing = FALSE;
    if (SUCCEEDED(m_factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                                                 &allowTearing, sizeof(allowTearing)))) {
        m_allowTearing = (allowTearing == TRUE);
    }

    // 高性能アダプタから順に、D3D12 デバイスを作れるものを選ぶ。
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter4> adapter;
        if (m_factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                  IID_PPV_ARGS(&adapter)) == DXGI_ERROR_NOT_FOUND) {
            break;
        }

        DXGI_ADAPTER_DESC3 desc = {};
        adapter->GetDesc3(&desc);
        if ((desc.Flags & DXGI_ADAPTER_FLAG3_SOFTWARE) != 0) {
            continue;
        }

        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0,
                                        IID_PPV_ARGS(&m_device)))) {
            m_adapter = adapter;
            TG_LOG_INFO("アダプタ: %ls (VRAM %llu MB)", desc.Description,
                        static_cast<unsigned long long>(desc.DedicatedVideoMemory / (1024 * 1024)));
            break;
        }
    }

    if (!m_device) {
        TG_LOG_ERROR("D3D12 デバイスを作成できるアダプタが見つかりませんでした");
        return false;
    }

    // シェーダモデル 6.6 を要求する（bindless / 合成シェーダの前提）。
    D3D12_FEATURE_DATA_SHADER_MODEL shaderModel = {D3D_SHADER_MODEL_6_6};
    if (FAILED(m_device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shaderModel,
                                             sizeof(shaderModel))) ||
        shaderModel.HighestShaderModel < D3D_SHADER_MODEL_6_6) {
        TG_LOG_ERROR("シェーダモデル 6.6 に対応していません");
        return false;
    }

    // bindless（ResourceDescriptorHeap）は Resource Binding Tier 3 を前提とする。
    D3D12_FEATURE_DATA_D3D12_OPTIONS options = {};
    if (FAILED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options,
                                             sizeof(options))) ||
        options.ResourceBindingTier < D3D12_RESOURCE_BINDING_TIER_3) {
        TG_LOG_ERROR("Resource Binding Tier 3 に対応していません（bindless に必要）");
        return false;
    }

    // 合成パスは R11G11B10F / RG16F / RGBA8 / R8 の UAV を読み書きする。
    // 無条件で保証される typed UAV load は R32 系のみなので、追加フォーマット対応を必須とする。
    if (!options.TypedUAVLoadAdditionalFormats) {
        TG_LOG_ERROR("Typed UAV Load の追加フォーマットに対応していません（合成パスに必要）");
        return false;
    }

    // デバッガ未接続で SetBreakOnSeverity を有効にすると、警告のたびにプロセスが落ちる。
    // デバッガ接続時のみ break させ、それ以外はメッセージの出力に留める。
    if (enableDebugLayer && SUCCEEDED(m_device.As(&m_infoQueue))) {
        // デバッガ未接続で break させるとプロセスごと落ちる。繋がっているときだけ。
        if (::IsDebuggerPresent()) {
            m_infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
            m_infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
            m_infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, TRUE);
        }
    }
    return true;
}

// デバッグレイヤーのメッセージをログへ流す。
//
// 溜めたままにすると上限で古いものが捨てられるので、汲んだら消す。
void Device::DrainDebugMessages() {
    if (m_infoQueue == nullptr) {
        return;
    }

    const UINT64 count = m_infoQueue->GetNumStoredMessages();
    // D3D12_MESSAGE として読むため、最大アライメントの要素で確保する。
    std::vector<std::max_align_t> buffer;
    for (UINT64 i = 0; i < count; ++i) {
        SIZE_T length = 0;
        if (FAILED(m_infoQueue->GetMessage(i, nullptr, &length)) || length == 0) {
            continue;
        }
        buffer.resize((length + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t));
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(buffer.data());
        if (FAILED(m_infoQueue->GetMessage(i, message, &length))) {
            continue;
        }

        switch (message->Severity) {
            case D3D12_MESSAGE_SEVERITY_CORRUPTION:
            case D3D12_MESSAGE_SEVERITY_ERROR:
                TG_LOG_ERROR("D3D12: %s", message->pDescription);
                break;
            case D3D12_MESSAGE_SEVERITY_WARNING:
                TG_LOG_WARN("D3D12: %s", message->pDescription);
                break;
            default:
                // INFO と MESSAGE は数が多く、内容も定型なので出さない。
                break;
        }
    }
    m_infoQueue->ClearStoredMessages();
}

bool Device::CreateCommandObjects() {
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    if (!TG_CHECK_HR(m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_commandQueue)))) {
        return false;
    }
    m_commandQueue->SetName(L"MainDirectQueue");

    for (uint32_t i = 0; i < kFrameCount; ++i) {
        if (!TG_CHECK_HR(m_device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_commandAllocators[i])))) {
            return false;
        }
    }

    if (!TG_CHECK_HR(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                 m_commandAllocators[0].Get(), nullptr,
                                                 IID_PPV_ARGS(&m_commandList)))) {
        return false;
    }
    // 作成直後は開いた状態なので閉じておく。
    if (!TG_CHECK_HR(m_commandList->Close())) {
        return false;
    }

    if (!TG_CHECK_HR(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                      IID_PPV_ARGS(&m_immediateAllocator)))) {
        return false;
    }
    if (!TG_CHECK_HR(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                 m_immediateAllocator.Get(), nullptr,
                                                 IID_PPV_ARGS(&m_immediateCommandList)))) {
        return false;
    }
    if (!TG_CHECK_HR(m_immediateCommandList->Close())) {
        return false;
    }

    if (!TG_CHECK_HR(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)))) {
        return false;
    }

    m_fenceEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (m_fenceEvent == nullptr) {
        TG_LOG_ERROR("フェンス用イベントの作成に失敗しました");
        return false;
    }
    return true;
}

bool Device::CreateSwapChain(HWND hwnd, uint32_t width, uint32_t height) {
    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.Format = kBackBufferFormat;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = kFrameCount;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    desc.Flags = m_allowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u;

    ComPtr<IDXGISwapChain1> swapChain1;
    if (!TG_CHECK_HR(m_factory->CreateSwapChainForHwnd(m_commandQueue.Get(), hwnd, &desc, nullptr,
                                                       nullptr, &swapChain1))) {
        return false;
    }
    // Alt+Enter による自動フルスクリーン切り替えは使わない。
    if (!TG_CHECK_HR(m_factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER))) {
        return false;
    }
    if (!TG_CHECK_HR(swapChain1.As(&m_swapChain))) {
        return false;
    }
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
    return true;
}

bool Device::CreateBackBufferViews() {
    for (uint32_t i = 0; i < kFrameCount; ++i) {
        if (!TG_CHECK_HR(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_backBuffers[i])))) {
            return false;
        }
        wchar_t name[32] = {};
        ::swprintf_s(name, L"BackBuffer%u", i);
        m_backBuffers[i]->SetName(name);

        if (!m_backBufferRtvs[i].IsValid()) {
            m_backBufferRtvs[i] = m_rtvHeap.Allocate();
            if (!m_backBufferRtvs[i].IsValid()) {
                return false;
            }
        }
        m_device->CreateRenderTargetView(m_backBuffers[i].Get(), nullptr, m_backBufferRtvs[i].cpu);
    }
    return true;
}

void Device::ReleaseBackBuffers() {
    for (auto& buffer : m_backBuffers) {
        buffer.Reset();
    }
}

void Device::Resize(uint32_t width, uint32_t height) {
    if (!m_initialized || width == 0 || height == 0) {
        return;
    }
    if (width == m_width && height == m_height) {
        return;
    }

    WaitForGpu();
    ReleaseBackBuffers();

    const UINT flags = m_allowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u;
    if (!TG_CHECK_HR(m_swapChain->ResizeBuffers(kFrameCount, width, height, kBackBufferFormat,
                                                flags))) {
        // バックバッファは既に手放しているため、続行すると null 参照になる。
        // デバイスロスト相当として描画を止める。
        TG_LOG_ERROR("スワップチェーンのリサイズに失敗しました。描画を停止します");
        m_initialized = false;
        return;
    }

    m_width = width;
    m_height = height;
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
    if (!CreateBackBufferViews()) {
        TG_LOG_ERROR("バックバッファビューの再作成に失敗しました。描画を停止します");
        m_initialized = false;
    }
}

ID3D12GraphicsCommandList* Device::BeginFrame(const float clearColor[4]) {
    if (!m_initialized || m_frameOpen) {
        return nullptr;
    }

    // このフレームスロットが前回投入した処理の完了を待つ。
    // m_fenceValues[i] == 0 は未使用スロットなので待たない。
    const uint64_t pending = m_fenceValues[m_frameIndex];
    if (pending != 0 && m_fence->GetCompletedValue() < pending) {
        if (!TG_CHECK_HR(m_fence->SetEventOnCompletion(pending, m_fenceEvent))) {
            return nullptr;
        }
        ::WaitForSingleObjectEx(m_fenceEvent, INFINITE, FALSE);
    }

    // このスロットの処理は完了しているので、解放待ちを回収してリングを巻き戻す。
    m_deletionQueue.Collect(m_fence->GetCompletedValue());
    m_uploadRing.BeginFrame(m_frameIndex);

    if (!TG_CHECK_HR(m_commandAllocators[m_frameIndex]->Reset())) {
        return nullptr;
    }
    if (!TG_CHECK_HR(m_commandList->Reset(m_commandAllocators[m_frameIndex].Get(), nullptr))) {
        return nullptr;
    }

    PIXBeginEvent(m_commandList.Get(), PIX_COLOR(0, 128, 255), "Frame");

    const auto toRenderTarget = CD3DX12_RESOURCE_BARRIER::Transition(
        m_backBuffers[m_frameIndex].Get(), D3D12_RESOURCE_STATE_PRESENT,
        D3D12_RESOURCE_STATE_RENDER_TARGET);
    m_commandList->ResourceBarrier(1, &toRenderTarget);

    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_backBufferRtvs[m_frameIndex].cpu;
    m_commandList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    m_commandList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);

    const auto viewport = CD3DX12_VIEWPORT(0.0f, 0.0f, static_cast<float>(m_width),
                                           static_cast<float>(m_height));
    const auto scissor = CD3DX12_RECT(0, 0, static_cast<LONG>(m_width),
                                      static_cast<LONG>(m_height));
    m_commandList->RSSetViewports(1, &viewport);
    m_commandList->RSSetScissorRects(1, &scissor);

    ID3D12DescriptorHeap* heaps[] = {m_srvHeap.Heap()};
    m_commandList->SetDescriptorHeaps(1, heaps);

    m_frameOpen = true;
    return m_commandList.Get();
}

void Device::CaptureBackBuffer() {
    ID3D12Resource* backBuffer = m_backBuffers[m_frameIndex].Get();

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT rowCount = 0;
    UINT64 rowSizeInBytes = 0;
    UINT64 totalBytes = 0;
    const D3D12_RESOURCE_DESC desc = backBuffer->GetDesc();
    m_device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rowCount, &rowSizeInBytes,
                                    &totalBytes);

    GpuBuffer readback;
    if (!m_allocator.CreateReadbackBuffer(totalBytes, L"BackBufferCapture", readback)) {
        FinishCapture(false);
        return;
    }

    // 記録中のコマンドリストへコピーを積む。Present 後はフリップモデルだと
    // バックバッファの内容が破棄されうるため、必ずフレームの中で写す。
    const auto toCopySource = CD3DX12_RESOURCE_BARRIER::Transition(
        backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    m_commandList->ResourceBarrier(1, &toCopySource);

    const CD3DX12_TEXTURE_COPY_LOCATION destination(readback.resource.Get(), footprint);
    const CD3DX12_TEXTURE_COPY_LOCATION source(backBuffer, 0);
    m_commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);

    const auto toRenderTarget = CD3DX12_RESOURCE_BARRIER::Transition(
        backBuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    m_commandList->ResourceBarrier(1, &toRenderTarget);

    // 保存はこのフレームを流し終えてから。EndFrame の末尾で回収する。
    m_pendingCapture = std::move(readback);
    m_pendingCaptureFootprint = footprint;
}

void Device::EndFrame(bool vsync) {
    if (!m_frameOpen) {
        return;
    }

    if (!m_capturePath.empty()) {
        CaptureBackBuffer();
    }

    const auto toPresent = CD3DX12_RESOURCE_BARRIER::Transition(
        m_backBuffers[m_frameIndex].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_PRESENT);
    m_commandList->ResourceBarrier(1, &toPresent);

    PIXEndEvent(m_commandList.Get());

    if (!TG_CHECK_HR(m_commandList->Close())) {
        m_frameOpen = false;
        // このフレームは投入されない。キャプチャを抱えたままにすると、
        // 次フレームで未実行のリードバックを上書きしてゴミを保存してしまう。
        FinishCapture(false);
        return;
    }

    ID3D12CommandList* lists[] = {m_commandList.Get()};
    m_commandQueue->ExecuteCommandLists(1, lists);

    const UINT syncInterval = vsync ? 1u : 0u;
    const UINT presentFlags = (!vsync && m_allowTearing) ? DXGI_PRESENT_ALLOW_TEARING : 0u;
    const HRESULT presentResult = m_swapChain->Present(syncInterval, presentFlags);
    if (presentResult == DXGI_ERROR_DEVICE_REMOVED || presentResult == DXGI_ERROR_DEVICE_RESET) {
        const HRESULT reason = m_device->GetDeviceRemovedReason();
        TG_LOG_ERROR(
            "デバイスロストを検出しました (Present=0x%08X, Reason=0x%08X)。描画を停止します",
            static_cast<unsigned int>(presentResult), static_cast<unsigned int>(reason));
        m_initialized = false;
    } else {
        TG_CHECK_HR(presentResult);
    }

    m_frameOpen = false;
    MoveToNextFrame();

    if (m_pendingCapture.IsValid()) {
        // 開発用の書き出しなので、その場で待って保存する。
        WaitForGpu();

        bool saved = false;
        void* mapped = nullptr;
        const D3D12_RANGE readRange = {0, static_cast<SIZE_T>(m_pendingCapture.sizeInBytes)};
        if (TG_CHECK_HR(m_pendingCapture.resource->Map(0, &readRange, &mapped))) {
            saved = SaveRgba8Png(
                m_capturePath, m_width, m_height, m_pendingCaptureFootprint.Footprint.RowPitch,
                static_cast<const uint8_t*>(mapped) + m_pendingCaptureFootprint.Offset);
            const D3D12_RANGE writtenRange = {0, 0};
            m_pendingCapture.resource->Unmap(0, &writtenRange);
            if (!saved) {
                TG_LOG_ERROR("バックバッファの書き出しに失敗しました");
            }
        }

        Defer(m_pendingCapture.resource);
        Defer(m_pendingCapture.allocation);
        FinishCapture(saved);
    }
}

void Device::BindBackBuffer(ID3D12GraphicsCommandList* commandList) {
    if (commandList == nullptr || !m_initialized) {
        return;
    }
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_backBufferRtvs[m_frameIndex].cpu;
    commandList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

    const auto viewport = CD3DX12_VIEWPORT(0.0f, 0.0f, static_cast<float>(m_width),
                                           static_cast<float>(m_height));
    const auto scissor = CD3DX12_RECT(0, 0, static_cast<LONG>(m_width),
                                      static_cast<LONG>(m_height));
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);
}

bool Device::ExecuteImmediate(const std::function<void(ID3D12GraphicsCommandList*)>& record) {
    if (!m_immediateCommandList || !record) {
        return false;
    }
    if (m_frameOpen) {
        // 内部で WaitForGpu するため、フレーム記録中には使えない（WaitForGpu と同じ理由）。
        TG_LOG_ERROR("BeginFrame と EndFrame の間で ExecuteImmediate が呼ばれました（無視します）");
        return false;
    }
    if (!TG_CHECK_HR(m_immediateAllocator->Reset())) {
        return false;
    }
    if (!TG_CHECK_HR(m_immediateCommandList->Reset(m_immediateAllocator.Get(), nullptr))) {
        return false;
    }

    ID3D12DescriptorHeap* heaps[] = {m_srvHeap.Heap()};
    m_immediateCommandList->SetDescriptorHeaps(1, heaps);

    record(m_immediateCommandList.Get());

    if (!TG_CHECK_HR(m_immediateCommandList->Close())) {
        return false;
    }
    ID3D12CommandList* lists[] = {m_immediateCommandList.Get()};
    m_commandQueue->ExecuteCommandLists(1, lists);
    WaitForGpu();
    return true;
}

void Device::MoveToNextFrame() {
    const uint64_t value = m_nextFenceValue++;
    if (!TG_CHECK_HR(m_commandQueue->Signal(m_fence.Get(), value))) {
        // Signal に失敗するのは実質デバイスロスト時のみ。古いフェンス値のまま続けると、
        // GPU が実行中のアロケータを次の BeginFrame が Reset してしまうため止める。
        TG_LOG_ERROR("フェンスの Signal に失敗しました。描画を停止します");
        m_initialized = false;
        return;
    }
    m_fenceValues[m_frameIndex] = value;

    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
}

void Device::WaitForGpu() {
    if (!m_commandQueue || !m_fence || m_fenceEvent == nullptr) {
        return;
    }
    if (m_frameOpen) {
        // フレーム記録中に待つと、記録中のコマンドが参照するオブジェクトの
        // フェンス値まで完了扱いになり、削除キューが早回収してしまう。
        TG_LOG_ERROR("BeginFrame と EndFrame の間で WaitForGpu が呼ばれました（無視します）");
        return;
    }

    const uint64_t value = m_nextFenceValue++;
    if (!TG_CHECK_HR(m_commandQueue->Signal(m_fence.Get(), value))) {
        return;
    }
    if (m_fence->GetCompletedValue() < value) {
        if (!TG_CHECK_HR(m_fence->SetEventOnCompletion(value, m_fenceEvent))) {
            return;
        }
        ::WaitForSingleObjectEx(m_fenceEvent, INFINITE, FALSE);
    }

    // 補助フェンスの仕事（別キューの合成の評価）も終わるまで待つ。
    // 「GPU が止まった」と信じて消す側は、そちらが参照中かどうかを知らない。
    if (m_auxiliaryFence && m_auxiliaryFence->GetCompletedValue() < m_auxiliaryFenceValue) {
        if (TG_CHECK_HR(m_auxiliaryFence->SetEventOnCompletion(m_auxiliaryFenceValue,
                                                                m_fenceEvent))) {
            ::WaitForSingleObjectEx(m_fenceEvent, INFINITE, FALSE);
        }
    }

    // ここまでで全スロットの処理は完了している。
    for (auto& fenceValue : m_fenceValues) {
        fenceValue = 0;
    }

    // GPU アイドルが確定したので、解放待ちをここで回収する。
    // ExecuteImmediate が連続する読み込み経路でステージングが滞留しないようにする。
    m_deletionQueue.Collect(m_fence->GetCompletedValue());
}

void Device::Shutdown() {
    if (m_initialized) {
        WaitForGpu();
    }
    m_initialized = false;
    m_frameOpen = false;

    // Close 失敗などで残ったキャプチャを、アロケータ破棄より先に手放す。
    m_pendingCapture = GpuBuffer{};
    m_capturePath.clear();
    m_captureCallback = {};

    m_deletionQueue.Flush();
    m_deletionQueue.SetAuxiliaryFence(nullptr, 0);
    m_auxiliaryFence.Reset();
    m_auxiliaryFenceValue = 0;
    m_uploadRing.Destroy();
    ReleaseBackBuffers();
    m_rtvHeap.Destroy();
    m_dsvHeap.Destroy();
    m_srvHeap.Destroy();
    // アロケータは、そこから確保した全リソースを解放したあとで破棄する。
    m_allocator.Destroy();
    m_commandList.Reset();
    m_immediateCommandList.Reset();
    m_immediateAllocator.Reset();
    for (auto& allocator : m_commandAllocators) {
        allocator.Reset();
    }
    m_swapChain.Reset();
    m_fence.Reset();
    if (m_fenceEvent != nullptr) {
        ::CloseHandle(m_fenceEvent);
        m_fenceEvent = nullptr;
    }
    m_commandQueue.Reset();
    // InfoQueue はデバイスへの参照を持つため、先に手放さないとデバイスが解放されない。
    m_infoQueue.Reset();
    m_device.Reset();
    m_adapter.Reset();
    m_factory.Reset();
}

}  // namespace tg::rhi
