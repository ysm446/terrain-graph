#pragma once

#include <filesystem>
#include <string>

namespace tg {

// パスの UTF-8 変換。**path::string() はロケール依存（ACP）なので使わない。**
// 用途で 2 つに分ける。
//
// - Display: OS ネイティブの区切り（Windows では '\\'）のまま。UI とログに出す。
// - Portable: 区切りを '/' に揃える。プロジェクトファイルなど、保存する文字列に使う。

inline std::string ToUtf8Display(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

inline std::string ToUtf8Portable(const std::filesystem::path& path) {
    const std::u8string text = path.generic_u8string();
    return std::string(text.begin(), text.end());
}

inline std::filesystem::path FromUtf8(const std::string& text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

}  // namespace tg
