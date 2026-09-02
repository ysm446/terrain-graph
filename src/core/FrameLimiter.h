#pragma once

#include <Windows.h>

#include <chrono>

namespace tg {

// フレームレートの上限。
//
// **「長く眠る」のではなく「描くのを間引く」。**
// フレームの間ずっとブロックすると、その間メッセージを汲めず、OS からは
// 無反応なウィンドウに見える。クリックしても固まったように感じる。
//
// 使い方は、フレームループの頭で ShouldRender() を呼び、偽なら描かずに
// `continue` してメッセージ処理へ戻る。眠るのは 1 回あたり数ミリ秒までなので、
// 上限を落としていても入力への反応は鈍らない。
class FrameLimiter {
public:
    FrameLimiter() = default;
    ~FrameLimiter();

    FrameLimiter(const FrameLimiter&) = delete;
    FrameLimiter& operator=(const FrameLimiter&) = delete;

    // このフレームを描いてよければ true。
    // 偽のときは締め切りまで（ただし長くても数ミリ秒）眠ってから返る。
    // fps が 0 以下なら常に true（上限なし）。
    bool ShouldRender(int fps);

    // 締め切りを捨てる。**上限が変わる場面で呼ぶ**（背面から前面へ戻ったときなど）。
    // 積み上げた締め切りが残っていると、戻った直後の 1 フレームだけ待たされる。
    void Reset() { m_next = std::chrono::steady_clock::time_point{}; }

private:
    void SleepUpTo(std::chrono::steady_clock::duration duration);

    // 次のフレームを描いてよい時刻。**前回の締め切りから積む**ので、
    // 1 フレームだけ長引いても平均のレートがずれない。
    std::chrono::steady_clock::time_point m_next{};
    HANDLE m_timer = nullptr;
    bool m_timerChecked = false;
};

}  // namespace tg
