#include "ui/ImGuiLayer.h"

#include "core/Log.h"
#include "core/Window.h"
#include "rhi/Device.h"
#include "ui/UiStyle.h"

#include <imgui.h>
#include <imgui_impl_dx12.h>
#include <imgui_impl_win32.h>

#include <algorithm>
#include <cmath>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wparam,
                                                             LPARAM lparam);

namespace tg {
namespace {

// フォントの基準サイズ。実際の大きさは ImGui 側で UI 拡大率が掛かる。
// アトラスへ読むときのサイズ。1.92 は必要なサイズで動的にラスタライズするので、
// これは出発点でしかない（実際の大きさは style.FontSizeBase で決める）。
constexpr float kFontAtlasSize = ui::kDefaultFontSize;

// ImGui バックエンドからのディスクリプタ確保要求を、こちらのアロケータへ橋渡しする。
void SrvDescriptorAlloc(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* outCpu,
                        D3D12_GPU_DESCRIPTOR_HANDLE* outGpu) {
    auto* device = static_cast<rhi::Device*>(info->UserData);
    const rhi::DescriptorHandle handle = device->SrvHeap().Allocate();
    *outCpu = handle.cpu;
    *outGpu = handle.gpu;
}

void SrvDescriptorFree(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE cpu,
                       D3D12_GPU_DESCRIPTOR_HANDLE gpu) {
    (void)gpu;
    auto* device = static_cast<rhi::Device*>(info->UserData);
    rhi::DescriptorHeap& heap = device->SrvHeap();
    const D3D12_CPU_DESCRIPTOR_HANDLE start = heap.At(0).cpu;
    const uint32_t index =
        static_cast<uint32_t>((cpu.ptr - start.ptr) / heap.DescriptorSize());
    heap.Free(heap.At(index));
}

}  // namespace

void ImGuiLayer::EnableDpiAwareness() {
    ImGui_ImplWin32_EnableDpiAwareness();
}

ImGuiLayer::~ImGuiLayer() {
    Shutdown();
}

bool ImGuiLayer::Initialize(Window& window, rhi::Device& device) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = kImGuiIniFileName;
    // ウィンドウの移動はタイトルバーからだけにする。
    // これをしないと、ビューポートの余白をドラッグしただけでパネルが動いてしまう。
    io.ConfigWindowsMoveFromTitleBarOnly = true;

    // クライアント領域を実ピクセルで固定しているので、UI の拡大率は既定では
    // モニタの DPI に追従させない（固定したはずの作業面積が変わってしまうため）。
    // DPI 認識自体は有効なままなので、OS によるビットマップ拡大は起きない。
    // 追従させるかは設定で選べる（Application が SetUiScale を呼ぶ）。
    m_monitorScale = ImGui_ImplWin32_GetDpiScaleForHwnd(window.Handle());
    TG_LOG_INFO("モニタの表示スケール: %.0f%%（UI の拡大率は %.0f%%）", m_monitorScale * 100.0f,
                m_uiScale * 100.0f);

    // 配色と余白はここで一括して決める。個々のパネルで色を積まない。
    ui::ApplyTheme(m_uiScale);

    if (!ImGui_ImplWin32_Init(window.Handle())) {
        TG_LOG_ERROR("ImGui_ImplWin32_Init に失敗しました");
        return false;
    }

    ImGui_ImplDX12_InitInfo info = {};
    info.Device = device.GetDevice();
    info.CommandQueue = device.GetCommandQueue();
    info.NumFramesInFlight = static_cast<int>(rhi::kFrameCount);
    info.RTVFormat = rhi::kBackBufferFormat;
    info.DSVFormat = DXGI_FORMAT_UNKNOWN;
    info.UserData = &device;
    info.SrvDescriptorHeap = device.SrvHeap().Heap();
    info.SrvDescriptorAllocFn = &SrvDescriptorAlloc;
    info.SrvDescriptorFreeFn = &SrvDescriptorFree;

    if (!ImGui_ImplDX12_Init(&info)) {
        TG_LOG_ERROR("ImGui_ImplDX12_Init に失敗しました");
        return false;
    }

    LoadFonts();
    // 読み込んだ大きさではなく、こちらが持っている基準サイズで描く。
    ApplyScaleToStyle();

    m_device = &device;
    m_initialized = true;
    return true;
}

void ImGuiLayer::SetUiScale(float scale) {
    const float clamped = std::clamp(scale, 0.5f, 4.0f);
    if (std::abs(clamped - m_uiScale) < 0.001f) {
        return;
    }
    m_uiScale = clamped;

    // 余白・角丸・部品幅はテーマ側でまとめて掛け直す。
    ui::ApplyTheme(m_uiScale);
    ApplyScaleToStyle();

    TG_LOG_INFO("UI の拡大率を %.0f%% にしました", m_uiScale * 100.0f);
}

void ImGuiLayer::SetFontSize(float sizeInPixels) {
    const float clamped = std::clamp(sizeInPixels, ui::kMinFontSize, ui::kMaxFontSize);
    if (std::abs(clamped - m_fontSize) < 0.001f) {
        return;
    }
    m_fontSize = clamped;

    // テーマは触らない。**余白や部品幅は拡大率だけで決める**ので、
    // 文字サイズを変えても行の詰まり方は変わらない（高さだけが文字に追従する）。
    ApplyScaleToStyle();

    TG_LOG_INFO("フォントサイズを %.0f px にしました", m_fontSize);
}

// フォントは 1.92 の動的ラスタライズに任せる。アトラスは作り直さない。
// 実際に描かれる大きさは FontSizeBase × FontScaleDpi。
void ImGuiLayer::ApplyScaleToStyle() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.FontSizeBase = m_fontSize;
    style.FontScaleDpi = m_uiScale;
}

void ImGuiLayer::LoadFonts() {
    // 日本語を表示できるよう、システムフォントを優先して読み込む。
    static const char* const kCandidates[] = {
        "C:/Windows/Fonts/YuGothM.ttc",
        "C:/Windows/Fonts/meiryo.ttc",
        "C:/Windows/Fonts/msgothic.ttc",
    };

    ImGuiIO& io = ImGui::GetIO();
    for (const char* path : kCandidates) {
        // 基準サイズで読む。拡大率は FontScaleDpi が掛ける。
        if (io.Fonts->AddFontFromFileTTF(path, kFontAtlasSize) != nullptr) {
            TG_LOG_INFO("フォントを読み込みました: %s", path);
            return;
        }
    }
    TG_LOG_WARN("日本語フォントが見つかりませんでした。既定フォントを使用します");
    io.Fonts->AddFontDefault();
}

void ImGuiLayer::Shutdown() {
    if (!m_initialized) {
        return;
    }
    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    m_initialized = false;
    m_device = nullptr;
    m_uiScale = 1.0f;
    m_fontSize = ui::kDefaultFontSize;
}

void ImGuiLayer::BeginFrame() {
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
}

void ImGuiLayer::EndFrame(ID3D12GraphicsCommandList* commandList) {
    ImGui::Render();
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), commandList);
}

bool ImGuiLayer::HandleMessage(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (!m_initialized) {
        return false;
    }
    return ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam) != 0;
}

}  // namespace tg
