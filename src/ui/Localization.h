#pragma once

#include <array>
#include <cstddef>

namespace tg::ui {

// UI の表示言語。設定ウィンドウの「言語」で切り替え、settings.json に覚える。
//
// 文字列は呼ぶ場所で日本語と英語を並べて書く（`Tr("ベースカラー", "Base Color")`）。
// 訳を別の表に分けないのは、日本語を直したときに英語が古いまま残らないようにするため。
// **英語のほうが自然な語（AO、GPU、ノード名など）は Tr を通さず 1 つのまま書く。**
// ログ（TG_LOG_*）は開発用なので日本語のまま。
//
// ウィンドウ名は ImGui の ID を兼ねるので、`"ビューポート###Viewport"` のように
// `###` 以降を言語によらない固定の名前にする。ドッキングの指定や検索は `"###Viewport"` で行う。
enum class Language {
    Japanese,
    English,
};

namespace detail {
inline Language g_language = Language::Japanese;
}  // namespace detail

inline void SetLanguage(Language language) { detail::g_language = language; }
inline Language CurrentLanguage() { return detail::g_language; }

// 今の言語の方を返す。どちらも文字列リテラル（寿命の長いもの）を渡すこと。
inline const char* Tr(const char* japanese, const char* english) {
    return (detail::g_language == Language::English) ? english : japanese;
}

// 日本語と英語の対。選択肢の一覧のように、表で持つ所で使う。
struct LocalizedText {
    const char* japanese;
    const char* english;

    const char* Get() const { return Tr(japanese, english); }
};

// 対の表を、今の言語の文字列の並びにする（PropertyCombo などへ渡す）。
template <size_t N>
std::array<const char*, N> Tr(const LocalizedText (&items)[N]) {
    std::array<const char*, N> result{};
    for (size_t i = 0; i < N; ++i) {
        result[i] = items[i].Get();
    }
    return result;
}

}  // namespace tg::ui
