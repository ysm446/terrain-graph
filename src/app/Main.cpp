#include "app/Application.h"

#include "core/Log.h"

#include <Windows.h>
#include <shellapi.h>

#include <cstdlib>
#include <string>

// --- DirectX 12 Agility SDK ------------------------------------------------
// 実行ファイルからエクスポートすることで、D3D12/ 配下の新しいランタイムが使われる。
extern "C" {
__declspec(dllexport) extern const UINT D3D12SDKVersion = 619;
__declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\";
}

namespace {

// 使い方:
//   terrain_graph.exe [--project <path>] [--save-project <path>]
//                       [--hdri <path>] [--texture <path>]...
//                       [--export <dir>]
//                       [--screenshot <path>] [--screenshot-ui <path>]
//                       [--screenshot-frame <n>]
tg::StartupOptions ParseCommandLine() {
    tg::StartupOptions options;

    int argc = 0;
    LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    if (argv == nullptr) {
        return options;
    }

    for (int i = 1; i < argc; ++i) {
        const std::wstring argument = argv[i];
        if (argument == L"--project" && (i + 1) < argc) {
            options.projectPath = argv[i + 1];
            ++i;
        } else if (argument == L"--save-project" && (i + 1) < argc) {
            options.saveProjectPath = argv[i + 1];
            ++i;
        } else if (argument == L"--hdri" && (i + 1) < argc) {
            options.hdriPath = argv[i + 1];
            ++i;
        } else if (argument == L"--texture" && (i + 1) < argc) {
            options.texturePaths.emplace_back(argv[i + 1]);
            ++i;
        } else if (argument == L"--screenshot" && (i + 1) < argc) {
            options.screenshotPath = argv[i + 1];
            ++i;
        } else if (argument == L"--export" && (i + 1) < argc) {
            options.exportDirectory = argv[i + 1];
            ++i;
        } else if (argument == L"--screenshot-ui" && (i + 1) < argc) {
            options.uiScreenshotPath = argv[i + 1];
            ++i;
        } else if (argument == L"--screenshot-frame" && (i + 1) < argc) {
            options.screenshotFrame = static_cast<uint32_t>(::_wtoi(argv[i + 1]));
            ++i;
        } else {
            TG_LOG_WARN("不明な引数です: %ls", argument.c_str());
        }
    }

    ::LocalFree(argv);
    return options;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    const tg::StartupOptions options = ParseCommandLine();

    tg::Application app;
    if (!app.Initialize(options)) {
        app.Shutdown();
        return 1;
    }

    const int result = app.Run();
    app.Shutdown();
    return result;
}
