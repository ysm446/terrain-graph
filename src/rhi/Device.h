#pragma once

#include "rhi/Common.h"
#include "rhi/DeletionQueue.h"
#include "rhi/DescriptorHeap.h"
#include "rhi/GpuResource.h"
#include "rhi/UploadRing.h"

#include <filesystem>
#include <functional>

namespace tg::rhi {

// DX12 のデバイス、キュー、スワップチェーン、フレーム同期をまとめて持つ。
// 1 フレームは BeginFrame() / EndFrame() の対で表す。
class Device {
public:
    Device() = default;
    ~Device();

    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    bool Initialize(HWND hwnd, uint32_t width, uint32_t height, bool enableDebugLayer);
    void Shutdown();

    void Resize(uint32_t width, uint32_t height);

    // バックバッファをレンダーターゲット状態にしてクリアしたコマンドリストを返す。
    // 失敗した場合は nullptr。
    ID3D12GraphicsCommandList* BeginFrame(const float clearColor[4]);
    void EndFrame(bool vsync);

    // バックバッファをレンダーターゲットとして再バインドする。
    // 途中で別のターゲットへ描いたあと、ImGui を描く前に呼ぶ。
    void BindBackBuffer(ID3D12GraphicsCommandList* commandList);

    // 初期化時のアップロードや事前計算のように、その場で実行して完了を待ちたい処理に使う。
    // フレームループの中では使わない。
    bool ExecuteImmediate(const std::function<void(ID3D12GraphicsCommandList*)>& record);

    // GPU の完了を待つ。リソース破棄やリサイズの前に必ず呼ぶ。
    // 補助フェンス（`SetAuxiliaryFence`）が登録されていれば、その仕事の完了も待つ。
    void WaitForGpu();

    // フレームの外で走る仕事（合成の評価を流すコンピュートキュー）の完了条件を登録する。
    // 以降の DeferRelease はこの完了も待ち、WaitForGpu もこれを含めて待つ。
    // 仕事を投入するたびに、その完了値で呼び直すこと。
    void SetAuxiliaryFence(ID3D12Fence* fence, uint64_t value);

    // フレームのフェンスと、いま記録中のフレームが EndFrame で立てる値。
    // 別のキューが「このフレームまで流し終えたら」と待つのに使う。
    ID3D12Fence* FrameFence() const { return m_fence.Get(); }
    uint64_t NextFenceValue() const { return m_nextFenceValue; }
    // デバッグレイヤーが溜めたメッセージをアプリのログへ流す。
    //
    // **これが無いと、検証エラーはデバッガを繋がない限り誰の目にも触れない。**
    // デバッグレイヤーを有効にしている意味が無くなるので、毎フレーム汲み出す。
    void DrainDebugMessages();

    // 撮影が終わったときに呼ばれる。成功したかと、書き出した場所と大きさを受け取る。
    using CaptureCallback = std::function<void(bool success, const std::filesystem::path& path,
                                               uint32_t width, uint32_t height)>;

    // 次の EndFrame でバックバッファを PNG に書き出す。
    // **画面からの切り出しではなくバックバッファの読み戻し**なので、
    // 手前に別のウィンドウが重なっていても、画面外へはみ出していても欠けない。
    // タイトルバーやウィンドウ枠も入らない。
    void RequestBackBufferCapture(const std::filesystem::path& path,
                                  CaptureCallback onComplete = {});

    ID3D12Device* GetDevice() const { return m_device.Get(); }
    ID3D12CommandQueue* GetCommandQueue() const { return m_commandQueue.Get(); }
    DescriptorHeap& SrvHeap() { return m_srvHeap; }
    DescriptorHeap& RtvHeap() { return m_rtvHeap; }
    DescriptorHeap& DsvHeap() { return m_dsvHeap; }
    ResourceAllocator& Allocator() { return m_allocator; }
    UploadRing& Upload() { return m_uploadRing; }

    // GPU がまだ参照している可能性のあるオブジェクトを、
    // 現在記録中のフレームが完了してから解放する。直接 Reset しないこと。
    void Defer(ComPtr<IUnknown> object);

    // ディスクリプタを、現在記録中のフレームが完了してからフリーリストへ返す。
    // ヒープの Free を直接呼ばないこと（実行中のフレームがまだ参照している）。
    void DeferFree(DescriptorHeap& heap, const DescriptorHandle& handle);

    // ミップごとの UAV / SRV だけを返す。ミップ連鎖を作り終えたら呼ぶこと
    // （作る間しか使わないのに、1 枚でミップ数 × 2 枠を占め続けるため）。
    // ミップ 0 の uav も同じハンドルなので一緒に落ちる。
    void DeferFreeMipViews(GpuTexture& texture);

    // テクスチャ / バッファの本体とディスクリプタを、まとめて遅延解放する。
    // GPU 待機は不要。呼び出し後、引数は空になる。
    void DeferRelease(GpuTexture& texture);
    void DeferRelease(GpuBuffer& buffer);

    // いま VRAM をどれだけ使っているか。
    //
    // usage / budget は**プロセス全体**の値（OS がこのプロセスへ割り当てている枠）で、
    // allocated / reserved は D3D12MA を通して確保した分だけを表す。
    // スワップチェーンやディスクリプタヒープ、ドライバの内部確保は allocated に入らないので、
    // 常に usage のほうが大きくなる。
    struct VideoMemory {
        uint64_t usage = 0;      // プロセスが使っている VRAM
        uint64_t budget = 0;     // OS がこのプロセスへ許している上限の目安
        uint64_t allocated = 0;  // D3D12MA のアロケーションが実際に占めている分
        uint64_t reserved = 0;   // D3D12MA がドライバから借りているブロックの合計
    };
    VideoMemory QueryVideoMemory() const;

    uint64_t CompletedFenceValue() const;
    size_t PendingDeletionCount() const { return m_deletionQueue.PendingCount(); }
    uint32_t FrameIndex() const { return m_frameIndex; }
    uint32_t Width() const { return m_width; }
    uint32_t Height() const { return m_height; }

private:
    bool CreateFactoryAndDevice(bool enableDebugLayer);
    bool CreateCommandObjects();
    bool CreateSwapChain(HWND hwnd, uint32_t width, uint32_t height);
    bool CreateBackBufferViews();
    // EndFrame から呼ぶ。コピーを記録し、そのフレームの完了を待ってから保存する。
    void CaptureBackBuffer();
    // 撮影の後始末。成否を通知し、保留していた状態を捨てる。
    void FinishCapture(bool success);
    void ReleaseBackBuffers();
    void MoveToNextFrame();

    ComPtr<IDXGIFactory6> m_factory;
    ComPtr<IDXGIAdapter4> m_adapter;
    ComPtr<ID3D12Device> m_device;
    // デバッグレイヤーのメッセージ置き場。Release ビルドでは null。
    ComPtr<ID3D12InfoQueue> m_infoQueue;
    ComPtr<ID3D12CommandQueue> m_commandQueue;
    ComPtr<IDXGISwapChain3> m_swapChain;

    ComPtr<ID3D12CommandAllocator> m_commandAllocators[kFrameCount];
    ComPtr<ID3D12GraphicsCommandList> m_commandList;
    // ExecuteImmediate 専用。フレームのコマンドリストとは分ける。
    ComPtr<ID3D12CommandAllocator> m_immediateAllocator;
    ComPtr<ID3D12GraphicsCommandList> m_immediateCommandList;
    ComPtr<ID3D12Resource> m_backBuffers[kFrameCount];
    DescriptorHandle m_backBufferRtvs[kFrameCount];

    DescriptorHeap m_rtvHeap;
    DescriptorHeap m_dsvHeap;
    DescriptorHeap m_srvHeap;
    ResourceAllocator m_allocator;
    UploadRing m_uploadRing;
    DeletionQueue m_deletionQueue;

    ComPtr<ID3D12Fence> m_fence;
    HANDLE m_fenceEvent = nullptr;
    // 補助フェンス。フレームの外で走る仕事の完了条件（WaitForGpu が併せて待つ）。
    ComPtr<ID3D12Fence> m_auxiliaryFence;
    uint64_t m_auxiliaryFenceValue = 0;
    // 次に Signal する値。0 は「まだ一度も投入していない」を表すため 1 から始める。
    uint64_t m_nextFenceValue = 1;
    // 各フレームスロットが最後に投入した Signal 値。0 なら未使用。
    uint64_t m_fenceValues[kFrameCount] = {};

    std::filesystem::path m_capturePath;
    CaptureCallback m_captureCallback;
    GpuBuffer m_pendingCapture;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT m_pendingCaptureFootprint = {};

    uint32_t m_frameIndex = 0;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    bool m_allowTearing = false;
    bool m_frameOpen = false;
    bool m_initialized = false;
};

}  // namespace tg::rhi
