#pragma once

#include "rhi/Common.h"
#include "ui/UiStyle.h"

namespace tg {

// ImGui のレイアウト保存先。作業ディレクトリからの相対パス。
inline constexpr const char* kImGuiIniFileName = "terrain_graph_imgui.ini";

class Window;

namespace rhi {
class Device;
}

// Dear ImGui (docking) の初期化とフレーム制御をまとめる。
class ImGuiLayer {
public:
    ImGuiLayer() = default;
    ~ImGuiLayer();

    ImGuiLayer(const ImGuiLayer&) = delete;
    ImGuiLayer& operator=(const ImGuiLayer&) = delete;

    // ウィンドウを作る前に呼ぶこと。呼ばないと高 DPI 環境で
    // Windows にウィンドウごと拡大され、描画解像度が落ちてぼやける。
    static void EnableDpiAwareness();

    bool Initialize(Window& window, rhi::Device& device);
    void Shutdown();

    void BeginFrame();
    void EndFrame(ID3D12GraphicsCommandList* commandList);

    // Window のメッセージフックから呼ぶ。true ならウィンドウ側では処理しない。
    bool HandleMessage(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

    // UI の拡大率を変える。フォントは ImGui 1.92 の FontScaleDpi で動的に拡縮するので、
    // アトラスの作り直しも GPU 待機も要らない。フレームの外で呼ぶこと。
    void SetUiScale(float scale);
    float UiScale() const { return m_uiScale; }

    // 文字の基準サイズ（px、拡大率を掛ける前）を変える。
    // 拡大率と同じく 1.92 の動的ラスタライズに任せるので、アトラスは作り直さない。
    // **部品の高さは文字の高さから決まる**ので、行の高さも一緒に変わる。
    void SetFontSize(float sizeInPixels);
    float FontSize() const { return m_fontSize; }
    // Windows の表示スケール（150% なら 1.5）。設定の表示と「追従」に使う。
    float MonitorScale() const { return m_monitorScale; }

private:
    void LoadFonts();
    // 拡大率と文字サイズをスタイルへ書く。ApplyTheme はスタイルを既定から
    // 作り直すので、**呼んだあとは必ずここを通す**こと。
    void ApplyScaleToStyle();

    rhi::Device* m_device = nullptr;
    float m_uiScale = 1.0f;
    float m_fontSize = ui::kDefaultFontSize;
    float m_monitorScale = 1.0f;
    bool m_initialized = false;
};

}  // namespace tg
