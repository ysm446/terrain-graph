#pragma once

#include <filesystem>
#include <vector>

namespace tg {

// ファイル選択ダイアログの絞り込み。表示名と `*.png;*.jpg` 形式のパターンの対。
struct FileFilter {
    const wchar_t* label = nullptr;
    const wchar_t* pattern = nullptr;
};

// よく使う絞り込み。増やすときはここに足す。
std::vector<FileFilter> ImageFileFilters();
std::vector<FileFilter> HdriFileFilters();
std::vector<FileFilter> ProjectFileFilters();
std::vector<FileFilter> MaterialFileFilters();

// ファイルを 1 つ選ぶ。取り消したら空のパスを返す。
//
// COM を使うので、呼び出し前に CoInitializeEx 済みであること
// （Application::Initialize が済ませる）。
std::filesystem::path ShowOpenFileDialog(const wchar_t* title,
                                         const std::vector<FileFilter>& filters);

// 複数のファイルを選ぶ。取り消したら空を返す。
std::vector<std::filesystem::path> ShowOpenFilesDialog(const wchar_t* title,
                                                       const std::vector<FileFilter>& filters);

// フォルダを 1 つ選ぶ。取り消したら空のパスを返す。
// 書き出しのように、1 回で複数のファイルを置く先を決めるときに使う。
std::filesystem::path ShowPickFolderDialog(const wchar_t* title,
                                           const std::filesystem::path& initialPath = {});

// 保存先を選ぶ。取り消したら空のパスを返す。
// defaultExtension は先頭のドットを含めない（"tgproj" など）。
std::filesystem::path ShowSaveFileDialog(const wchar_t* title,
                                         const std::vector<FileFilter>& filters,
                                         const wchar_t* defaultExtension,
                                         const std::filesystem::path& initialPath = {});

}  // namespace tg
