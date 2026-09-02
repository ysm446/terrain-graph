#include "core/Window.h"

#include "core/Log.h"
#include "core/ResourceIds.h"

#include <shellapi.h>

#include <algorithm>

namespace tg {
namespace {

constexpr const wchar_t* kWindowClassName = L"TerrainGraphWindowClass";

}  // namespace

Window::~Window() {
    Destroy();
}

bool Window::Create(const wchar_t* title, uint32_t width, uint32_t height) {
    const HINSTANCE instance = ::GetModuleHandleW(nullptr);
    const UINT systemDpi = ::GetDpiForSystem();
    const auto loadIcon = [instance, systemDpi](int widthMetric, int heightMetric) {
        const int iconWidth = ::GetSystemMetricsForDpi(widthMetric, systemDpi);
        const int iconHeight = ::GetSystemMetricsForDpi(heightMetric, systemDpi);
        return static_cast<HICON>(::LoadImageW(instance, MAKEINTRESOURCEW(TG_APP_ICON),
                                               IMAGE_ICON, iconWidth, iconHeight,
                                               LR_DEFAULTCOLOR | LR_SHARED));
    };
    const HICON largeIcon = loadIcon(SM_CXICON, SM_CYICON);
    const HICON smallIcon = loadIcon(SM_CXSMICON, SM_CYSMICON);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &Window::WndProcThunk;
    wc.hInstance = instance;
    wc.hIcon = largeIcon;
    wc.hIconSm = smallIcon;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kWindowClassName;
    if (::RegisterClassExW(&wc) == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        TG_LOG_ERROR("RegisterClassExW に失敗しました (0x%08lX)", ::GetLastError());
        return false;
    }

    // width / height はクライアント領域（描画される中身）のサイズとして扱う。
    // AdjustWindowRect は 96 DPI の枠しか見ないため、実際の DPI 版を使う。
    // 高 DPI で枠が太くなるぶんクライアント領域が縮むのを防ぐ。
    RECT rect = {0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    ::AdjustWindowRectExForDpi(&rect, WS_OVERLAPPEDWINDOW, FALSE, 0, ::GetDpiForSystem());

    m_hwnd = ::CreateWindowExW(0, kWindowClassName, title, WS_OVERLAPPEDWINDOW,
                               CW_USEDEFAULT, CW_USEDEFAULT,
                               rect.right - rect.left, rect.bottom - rect.top,
                               nullptr, nullptr, instance, this);
    if (m_hwnd == nullptr) {
        TG_LOG_ERROR("CreateWindowExW に失敗しました (0x%08lX)", ::GetLastError());
        return false;
    }

    // クラスアイコンだけではタイトルバーへ反映されない環境があるため、
    // 作成済みウィンドウにも大小アイコンを明示する。
    ::SendMessageW(m_hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(largeIcon));
    ::SendMessageW(m_hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(smallIcon));

    // 要求どおりのクライアント領域で開く。**入りきらなくても縮めない。**
    // スクリーンショットや録画の解像度を固定するのが目的なので、
    // モニタの作業領域に合わせて縮めると目的を果たせない。位置だけ調整する。
    ResizeClient(width, height);

    RECT client = {};
    ::GetClientRect(m_hwnd, &client);
    m_width = static_cast<uint32_t>(client.right - client.left);
    m_height = static_cast<uint32_t>(client.bottom - client.top);

    // エクスプローラからのドロップを受け付ける（WM_DROPFILES）。
    ::DragAcceptFiles(m_hwnd, TRUE);

    ::ShowWindow(m_hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(m_hwnd);
    return true;
}

// クライアント領域（描画される中身）を指定サイズへ合わせる。
//
// 最大化やスナップは解除する。解除しないと SetWindowPos の大きさが無視される。
// 入りきらない場合でも**縮めない**。位置だけ作業領域の中へ寄せる。
void Window::ResizeClient(uint32_t width, uint32_t height) {
    if (m_hwnd == nullptr || width == 0 || height == 0) {
        return;
    }

    if (::IsZoomed(m_hwnd)) {
        ::ShowWindow(m_hwnd, SW_RESTORE);
    }

    // AdjustWindowRect は 96 DPI の枠しか見ないため、実際の DPI 版を使う。
    RECT rect = {0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    const UINT dpi = ::GetDpiForWindow(m_hwnd);
    ::AdjustWindowRectExForDpi(&rect, WS_OVERLAPPEDWINDOW, FALSE, 0,
                               (dpi != 0) ? dpi : ::GetDpiForSystem());
    const LONG outerWidth = rect.right - rect.left;
    const LONG outerHeight = rect.bottom - rect.top;

    // 位置は、いま載っているモニタの作業領域の左上へ寄せる。
    // タイトルバーが画面外へ出ると窓を掴めなくなる。
    LONG x = 0;
    LONG y = 0;
    MONITORINFO monitorInfo = {};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (::GetMonitorInfoW(::MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST), &monitorInfo)) {
        RECT windowRect = {};
        ::GetWindowRect(m_hwnd, &windowRect);
        x = windowRect.left;
        y = windowRect.top;

        const LONG maxX = monitorInfo.rcWork.right - outerWidth;
        const LONG maxY = monitorInfo.rcWork.bottom - outerHeight;
        x = (maxX < monitorInfo.rcWork.left) ? monitorInfo.rcWork.left : std::min(x, maxX);
        y = (maxY < monitorInfo.rcWork.top) ? monitorInfo.rcWork.top : std::min(y, maxY);
        x = std::max(x, monitorInfo.rcWork.left);
        y = std::max(y, monitorInfo.rcWork.top);

        if (outerWidth > (monitorInfo.rcWork.right - monitorInfo.rcWork.left) ||
            outerHeight > (monitorInfo.rcWork.bottom - monitorInfo.rcWork.top)) {
            TG_LOG_WARN("ウィンドウ (%ux%u) がモニタの作業領域に収まりません。"
                        "はみ出したまま開きます", width, height);
        }
    }

    ::SetWindowPos(m_hwnd, nullptr, x, y, outerWidth, outerHeight,
                   SWP_NOZORDER | SWP_NOACTIVATE);
}

void Window::SetTitle(const wchar_t* title) {
    if (m_hwnd != nullptr && title != nullptr) {
        ::SetWindowTextW(m_hwnd, title);
    }
}

void Window::Destroy() {
    if (m_hwnd != nullptr) {
        ::DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
}

bool Window::PumpMessages() {
    MSG msg = {};
    while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
        if (msg.message == WM_QUIT) {
            m_shouldClose = true;
        }
    }
    return !m_shouldClose;
}

LRESULT CALLBACK Window::WndProcThunk(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    Window* self = nullptr;
    if (msg == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = static_cast<Window*>(create->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->m_hwnd = hwnd;
    } else {
        self = reinterpret_cast<Window*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self != nullptr) {
        return self->WndProc(hwnd, msg, wparam, lparam);
    }
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

LRESULT Window::WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (m_messageHook && m_messageHook(hwnd, msg, wparam, lparam)) {
        return 1;
    }

    switch (msg) {
        case WM_SIZE: {
            m_minimized = (wparam == SIZE_MINIMIZED);
            const auto width = static_cast<uint32_t>(LOWORD(lparam));
            const auto height = static_cast<uint32_t>(HIWORD(lparam));
            if (!m_minimized && width > 0 && height > 0 &&
                (width != m_width || height != m_height)) {
                m_width = width;
                m_height = height;
                if (m_resizeCallback) {
                    m_resizeCallback(m_width, m_height);
                }
            }
            return 0;
        }
        case WM_DROPFILES: {
            // 落とされたパスを集めて呼び出し側へ渡す。ここでは読み込まない
            // （読み込みは GPU 待機を伴うので、フレームの外の保留処理で行う）。
            const auto drop = reinterpret_cast<HDROP>(wparam);
            const UINT count = ::DragQueryFileW(drop, 0xFFFFFFFFu, nullptr, 0);
            std::vector<std::filesystem::path> paths;
            paths.reserve(count);
            for (UINT i = 0; i < count; ++i) {
                // 返る長さは終端を含まない。バッファには終端のぶんを足して渡す。
                const UINT length = ::DragQueryFileW(drop, i, nullptr, 0);
                if (length == 0) {
                    continue;
                }
                std::wstring buffer(length, L' ');
                if (::DragQueryFileW(drop, i, buffer.data(), length + 1) != 0) {
                    paths.emplace_back(buffer);
                }
            }
            ::DragFinish(drop);
            if (!paths.empty() && m_dropCallback) {
                m_dropCallback(paths);
            }
            return 0;
        }
        case WM_SYSCOMMAND:
            // Alt キー単独でのシステムメニュー表示を抑止する。
            if ((wparam & 0xFFF0) == SC_KEYMENU) {
                return 0;
            }
            break;
        case WM_CLOSE:
            m_shouldClose = true;
            return 0;
        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

}  // namespace tg
