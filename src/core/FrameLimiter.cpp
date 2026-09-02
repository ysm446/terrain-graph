#include "core/FrameLimiter.h"

#include <algorithm>
#include <thread>

namespace tg {
namespace {

using Clock = std::chrono::steady_clock;

// 1 回に眠る上限。**これがそのまま入力への反応の遅れになる。**
// 10fps に落としていても 1 秒に 500 回しか起きないので、CPU はほぼ使わない。
constexpr std::chrono::milliseconds kSlice{2};

// 待機タイマーを作る。高分解能版が使えない環境では通常のものへ落とす。
HANDLE CreateTimer() {
#if defined(CREATE_WAITABLE_TIMER_HIGH_RESOLUTION)
    if (HANDLE timer = ::CreateWaitableTimerExW(nullptr, nullptr,
                                                CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                                TIMER_ALL_ACCESS);
        timer != nullptr) {
        return timer;
    }
#endif
    return ::CreateWaitableTimerW(nullptr, FALSE, nullptr);
}

}  // namespace

FrameLimiter::~FrameLimiter() {
    if (m_timer != nullptr) {
        ::CloseHandle(m_timer);
        m_timer = nullptr;
    }
}

bool FrameLimiter::ShouldRender(int fps) {
    if (fps <= 0) {
        m_next = Clock::time_point{};
        return true;
    }

    const auto period = std::chrono::nanoseconds(1'000'000'000LL / fps);
    const Clock::time_point now = Clock::now();

    // 初回、上限を変えた直後、読み込みなどで大きく止まった後は、いまから積み直す。
    // そうしないと「遅れを取り戻す」ために何フレームも間引かずに走ってしまう。
    if (m_next == Clock::time_point{} || m_next + period < now) {
        m_next = now;
    }

    if (now >= m_next) {
        m_next += period;
        return true;
    }

    SleepUpTo(m_next - now);
    return false;
}

void FrameLimiter::SleepUpTo(Clock::duration duration) {
    const auto wait = std::min<Clock::duration>(duration, kSlice);
    if (wait <= Clock::duration::zero()) {
        return;
    }

    if (!m_timerChecked) {
        m_timerChecked = true;
        m_timer = CreateTimer();
    }
    // **Sleep ではなく高分解能の待機タイマーを使う。** 既定の Sleep は 15ms 刻みで、
    // 2ms を頼んでも 15ms 眠ってしまう。プロセス全体の分解能を上げる
    // （timeBeginPeriod）方法は他のアプリにも影響するので採らない。
    if (m_timer == nullptr) {
        std::this_thread::sleep_for(wait);
        return;
    }

    // 負の値は「いまから 100ns 単位で」の相対指定。
    LARGE_INTEGER due = {};
    due.QuadPart = -(std::chrono::duration_cast<std::chrono::nanoseconds>(wait).count() / 100);
    if (::SetWaitableTimer(m_timer, &due, 0, nullptr, nullptr, FALSE)) {
        ::WaitForSingleObject(m_timer, INFINITE);
    } else {
        std::this_thread::sleep_for(wait);
    }
}

}  // namespace tg
