#include "core/Shell.h"

#include "core/Log.h"
#include "core/PathUtf8.h"

#include <Windows.h>

#include <shellapi.h>

#include <string>
#include <system_error>

namespace tg {

void RevealFileInExplorer(const std::filesystem::path& path) {
    if (path.empty()) {
        return;
    }

    std::error_code error;
    const std::filesystem::path absolute = std::filesystem::absolute(path, error);
    const std::filesystem::path& target = error ? path : absolute;

    // /select はファイルを選択した状態で親フォルダを開く。
    // パスに空白が入るため、必ず引用符で囲む。
    const std::wstring args = L"/select,\"" + target.wstring() + L"\"";
    const HINSTANCE result =
        ::ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    // ShellExecuteW は成功すると 32 より大きい値を返す。
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        TG_LOG_WARN("エクスプローラを開けませんでした: %s", ToUtf8Display(target).c_str());
    }
}

}  // namespace tg
