#include "core/FileDialog.h"

#include "core/Log.h"

#include <Windows.h>
#include <shobjidl.h>
#include <wrl/client.h>

namespace tg {
namespace {

using Microsoft::WRL::ComPtr;

// COMDLG_FILTERSPEC は FileFilter と同じ並びなので、そのまま作り直す。
std::vector<COMDLG_FILTERSPEC> ToFilterSpecs(const std::vector<FileFilter>& filters) {
    std::vector<COMDLG_FILTERSPEC> specs;
    specs.reserve(filters.size());
    for (const FileFilter& filter : filters) {
        specs.push_back(COMDLG_FILTERSPEC{filter.label, filter.pattern});
    }
    return specs;
}

std::filesystem::path ToPath(IShellItem* item) {
    if (item == nullptr) {
        return {};
    }
    PWSTR raw = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)) || raw == nullptr) {
        return {};
    }
    std::filesystem::path path(raw);
    ::CoTaskMemFree(raw);
    return path;
}

// ダイアログの共通設定。失敗したら nullptr。
// 名前を CreateDialog にすると Windows のマクロ(CreateDialogW)と衝突する。
ComPtr<IFileDialog> CreateFileDialog(const CLSID& clsid, const wchar_t* title,
                                     const std::vector<FileFilter>& filters) {
    ComPtr<IFileDialog> dialog;
    if (FAILED(::CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        TG_LOG_ERROR("ファイルダイアログを作成できません");
        return nullptr;
    }

    if (title != nullptr) {
        dialog->SetTitle(title);
    }

    const std::vector<COMDLG_FILTERSPEC> specs = ToFilterSpecs(filters);
    if (!specs.empty()) {
        dialog->SetFileTypes(static_cast<UINT>(specs.size()), specs.data());
        dialog->SetFileTypeIndex(1);
    }
    return dialog;
}

}  // namespace

std::vector<FileFilter> ImageFileFilters() {
    return {
        {L"画像 (*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.exr)",
         L"*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.exr"},
        {L"OpenEXR (*.exr)", L"*.exr"},
        {L"すべてのファイル (*.*)", L"*.*"},
    };
}

std::vector<FileFilter> HdriFileFilters() {
    return {
        {L"Radiance HDR (*.hdr)", L"*.hdr"},
        {L"すべてのファイル (*.*)", L"*.*"},
    };
}

std::vector<FileFilter> ProjectFileFilters() {
    return {
        {L"terrain-graph プロジェクト (*.tgproj)", L"*.tgproj"},
        {L"material-mixer プロジェクト (*.mmproj)", L"*.mmproj"},
        {L"すべてのファイル (*.*)", L"*.*"},
    };
}

std::vector<FileFilter> MaterialFileFilters() {
    return {
        {L"terrain-graph マテリアル (*.tgmat)", L"*.tgmat"},
        {L"material-mixer マテリアル (*.mmmat)", L"*.mmmat"},
        {L"すべてのファイル (*.*)", L"*.*"},
    };
}

std::filesystem::path ShowOpenFileDialog(const wchar_t* title,
                                         const std::vector<FileFilter>& filters) {
    ComPtr<IFileDialog> dialog = CreateFileDialog(CLSID_FileOpenDialog, title, filters);
    if (!dialog) {
        return {};
    }
    if (FAILED(dialog->Show(::GetActiveWindow()))) {
        return {};  // 取り消し。
    }

    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(&item))) {
        return {};
    }
    return ToPath(item.Get());
}

std::filesystem::path ShowPickFolderDialog(const wchar_t* title,
                                           const std::filesystem::path& initialPath) {
    // 絞り込みは要らない。FOS_PICKFOLDERS でフォルダ選択に切り替える。
    ComPtr<IFileDialog> dialog = CreateFileDialog(CLSID_FileOpenDialog, title, {});
    if (!dialog) {
        return {};
    }

    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS);

    if (!initialPath.empty()) {
        ComPtr<IShellItem> folder;
        if (SUCCEEDED(::SHCreateItemFromParsingName(initialPath.c_str(), nullptr,
                                                    IID_PPV_ARGS(&folder)))) {
            dialog->SetFolder(folder.Get());
        }
    }

    if (FAILED(dialog->Show(::GetActiveWindow()))) {
        return {};  // 取り消し。
    }

    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(&item))) {
        return {};
    }
    return ToPath(item.Get());
}

std::vector<std::filesystem::path> ShowOpenFilesDialog(const wchar_t* title,
                                                       const std::vector<FileFilter>& filters) {
    ComPtr<IFileDialog> dialog = CreateFileDialog(CLSID_FileOpenDialog, title, filters);
    if (!dialog) {
        return {};
    }

    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_ALLOWMULTISELECT);

    if (FAILED(dialog->Show(::GetActiveWindow()))) {
        return {};  // 取り消し。
    }

    ComPtr<IFileOpenDialog> openDialog;
    if (FAILED(dialog.As(&openDialog))) {
        return {};
    }
    ComPtr<IShellItemArray> items;
    if (FAILED(openDialog->GetResults(&items))) {
        return {};
    }

    DWORD count = 0;
    items->GetCount(&count);

    std::vector<std::filesystem::path> paths;
    paths.reserve(count);
    for (DWORD i = 0; i < count; ++i) {
        ComPtr<IShellItem> item;
        if (FAILED(items->GetItemAt(i, &item))) {
            continue;
        }
        std::filesystem::path path = ToPath(item.Get());
        if (!path.empty()) {
            paths.push_back(std::move(path));
        }
    }
    return paths;
}

std::filesystem::path ShowSaveFileDialog(const wchar_t* title,
                                         const std::vector<FileFilter>& filters,
                                         const wchar_t* defaultExtension,
                                         const std::filesystem::path& initialPath) {
    ComPtr<IFileDialog> dialog = CreateFileDialog(CLSID_FileSaveDialog, title, filters);
    if (!dialog) {
        return {};
    }

    if (defaultExtension != nullptr) {
        dialog->SetDefaultExtension(defaultExtension);
    }
    if (!initialPath.empty()) {
        dialog->SetFileName(initialPath.filename().wstring().c_str());
    }

    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_OVERWRITEPROMPT);

    if (FAILED(dialog->Show(::GetActiveWindow()))) {
        return {};  // 取り消し。
    }

    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(&item))) {
        return {};
    }
    return ToPath(item.Get());
}

}  // namespace tg
