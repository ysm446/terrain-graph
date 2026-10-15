#pragma once

#include <array>
#include <atomic>
#include <cstddef>

namespace tg::ui {

// UI の表示言語。設定ウィンドウの「言語」で切り替え、settings.json に覚える。
//
// 文字列は呼ぶ場所で日本語と英語を並べて書く（`Tr("ベースカラー", "Base Color")`）。
// 訳を別の表に分けないのは、日本語を直したときに英語が古いまま残らないようにするため。
// **英語のほうが自然な語（AO、GPU、ノード名など）は Tr を通さず 1 つのまま書く。**
// ログ（TG_LOG_*）は開発用なので日本語のまま。
// このヘッダは ImGui に依存しないので、graph / compositor / io の側で作る、画面に出る文
// （ノードの説明、評価のエラー文など）にも使ってよい。
//
// ウィンドウ名は ImGui の ID を兼ねるので、`"ビューポート###Viewport"` のように
// `###` 以降を言語によらない固定の名前にする。ドッキングの指定や検索は `"###Viewport"` で行う。
enum class Language {
    Japanese,
    English,
};

namespace detail {
// 書くのは UI のスレッドだけだが、モデルの読み込みなど別スレッドで作る文からも読むので atomic にする。
// 順序の保証は要らない（切り替えの瞬間にどちらの言語になってもよい）。
inline std::atomic<Language> g_language = Language::Japanese;
}  // namespace detail

inline void SetLanguage(Language language) { detail::g_language.store(language, std::memory_order_relaxed); }
inline Language CurrentLanguage() { return detail::g_language.load(std::memory_order_relaxed); }

// 今の言語の方を返す。どちらも文字列リテラル（寿命の長いもの）を渡すこと。
inline const char* Tr(const char* japanese, const char* english) {
    return (CurrentLanguage() == Language::English) ? english : japanese;
}

// ワイド文字列版。ファイルダイアログの題とフィルタ名（Win32 へそのまま渡すもの）に使う。
inline const wchar_t* Tr(const wchar_t* japanese, const wchar_t* english) {
    return (CurrentLanguage() == Language::English) ? english : japanese;
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
