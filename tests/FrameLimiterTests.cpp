// フレームレート上限のテスト。
//
// **時間の話なのでスクリーンショットには写らない。** 上限どおりに待つこと、
// 上限なしでは待たないこと、長く止まった後に取り返そうとしないことを見る。

#include "TestSupport.h"

#include "core/FrameLimiter.h"

#include <chrono>
#include <thread>

namespace {

using tg::tests::Check;
using tg::tests::Section;
using Clock = std::chrono::steady_clock;

// fps の上限で frames 回描くまでの経過ミリ秒と、そのあいだの空回りの回数。
struct RunResult {
    long long elapsedMs = 0;
    int skipped = 0;
};

RunResult Run(int fps, int frames) {
    tg::FrameLimiter limiter;
    RunResult result;
    const Clock::time_point start = Clock::now();
    for (int rendered = 0; rendered < frames;) {
        if (limiter.ShouldRender(fps)) {
            ++rendered;
        } else {
            ++result.skipped;
        }
    }
    result.elapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
    return result;
}

}  // namespace

void RunFrameLimiterTests() {
    Section("フレームレート上限");

    // 20fps で 5 フレーム。1 フレーム目はすぐ描くので、待つのは 4 回ぶん = 200ms。
    // 待機タイマーの精度と OS のスケジューリングを見込んで幅を持たせる。
    // **下限は必ず見る**（間引いていなければ意味がない）。
    const RunResult limited = Run(20, 5);
    Check(limited.elapsedMs >= 180, "上限どおりに間引く（20fps x 5 フレームで 180ms 以上）");
    Check(limited.elapsedMs < 500, "待ちすぎない（同 500ms 未満）");

    // **1 回の眠りは数ミリ秒まで。** 長く眠るとメッセージを汲めず、
    // ウィンドウが固まったように見える。50ms を 2ms 刻みで待つなら 20 回以上空回りする。
    Check(limited.skipped >= 40, "描かない間も細かく起きる（ブロックし続けない）");

    // 0 は上限なし。常に描いてよい。
    const RunResult unlimited = Run(0, 100);
    Check(unlimited.elapsedMs < 50, "上限なし（0）では待たない");
    Check(unlimited.skipped == 0, "上限なし（0）では間引かない");

    // **長く止まった後に取り返そうとしない。** 締め切りを素直に積むと、
    // 読み込みなどで数秒止まった後に何フレームも間引かずに走ってしまう。
    {
        tg::FrameLimiter limiter;
        Check(limiter.ShouldRender(20), "1 フレーム目はすぐ描く");
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        Check(limiter.ShouldRender(20), "長く止まった後の 1 フレーム目もすぐ描く");
        const Clock::time_point start = Clock::now();
        while (!limiter.ShouldRender(20)) {
        }
        const long long elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
        Check(elapsed >= 30, "その次からは上限どおりに間引く");
    }

    // 締め切りを捨てれば、次のフレームはすぐ描ける。
    {
        tg::FrameLimiter limiter;
        limiter.ShouldRender(10);
        limiter.Reset();
        Check(limiter.ShouldRender(10), "Reset のあとはすぐ描ける");
    }
}
