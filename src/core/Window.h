#pragma once

#include <Windows.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <vector>

namespace tg {

// Win32 ウィンドウ。メッセージフックを差し込めるようにして、UI 層への依存を持たない。
class Window {
public:
    // 追加のメッセージ処理。true を返すとそのメッセージはウィンドウ側で処理しない。
    using MessageHook = std::function<bool(HWND, UINT, WPARAM, LPARAM)>;
    using ResizeCallback = std::function<void(uint32_t, uint32_t)>;
    // エクスプローラからファイルを落とされたときに呼ばれる。
    using DropCallback = std::function<void(const std::vector<std::filesystem::path>&)>;

    Window() = default;
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    // width / height はクライアント領域（描画される中身）のサイズ。
    // ウィンドウ枠のぶんは内部で足す。
    bool Create(const wchar_t* title, uint32_t width, uint32_t height);
    void Destroy();

    // メニューなどからアプリを閉じる。
    void RequestClose() { m_shouldClose = true; }

    // タイトルバーの文字列を差し替える。開いているプロジェクト名を出すのに使う。
    void SetTitle(const wchar_t* title);

    // クライアント領域（描画される中身）を指定サイズへ合わせる。
    // 最大化やスナップは解除する。**作業領域に入りきらなくても縮めない**
    // （スクリーンショットや録画の解像度を固定するのが目的なので）。
    void ResizeClient(uint32_t width, uint32_t height);

    // 溜まっているメッセージを処理する。終了要求が来ていたら false を返す。
    bool PumpMessages();

    void SetMessageHook(MessageHook hook) { m_messageHook = std::move(hook); }
    void SetResizeCallback(ResizeCallback callback) { m_resizeCallback = std::move(callback); }
    // ファイルのドロップを受け取る。呼ばれるのはメッセージ処理の中（フレームの外）。
    void SetDropCallback(DropCallback callback) { m_dropCallback = std::move(callback); }

    HWND Handle() const { return m_hwnd; }
    uint32_t Width() const { return m_width; }
    uint32_t Height() const { return m_height; }
    bool IsMinimized() const { return m_minimized; }
    // 前面（アクティブ）かどうか。背面のときはフレームレートを落とす。
    bool IsForeground() const { return ::GetForegroundWindow() == m_hwnd; }

private:
    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
    LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

    HWND m_hwnd = nullptr;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    bool m_shouldClose = false;
    bool m_minimized = false;
    MessageHook m_messageHook;
    ResizeCallback m_resizeCallback;
    DropCallback m_dropCallback;
};

}  // namespace tg
