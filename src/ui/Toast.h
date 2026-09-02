#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace tg::ui {

// 画面右下へ積み上げて出す通知。
//
// 「保存した」「書き出した」のように、**結果をその場で知らせたいが、
// 手を止めさせるほどではない**ものに使う。エラーはステータスバーとログへ流す。
//
// 置き場所と作法は docs/design/design-guide.md の「通知トースト」を参照。
class ToastQueue {
public:
    // revealPath を渡すと、クリックでその場所をエクスプローラで開ける。
    void Push(std::string title, std::string detail = {},
              std::filesystem::path revealPath = {}, float seconds = 6.0f);

    // 右下へ描く。**他のウィンドウより後（フレームの最後）に呼ぶこと。**
    // 先に呼ぶと、後から出したウィンドウの下に隠れる。
    void Draw();

    bool Empty() const { return m_toasts.empty(); }

private:
    struct Toast {
        std::string title;
        std::string detail;
        std::filesystem::path revealPath;
        std::chrono::steady_clock::time_point expiresAt{};
    };

    std::vector<Toast> m_toasts;
};

}  // namespace tg::ui
