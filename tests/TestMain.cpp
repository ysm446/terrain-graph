// テストの入口。領域ごとの関数を順に呼ぶだけ。
//
// 対象は「スクリーンショットでは確認できないもの」。
// UI の相互作用（ドラッグ、ホバー）、アンドゥ履歴の段のまとめ方、
// フレームレート上限の待ち時間。

#include "TestSupport.h"

void RunFrameLimiterTests();
void RunUiInteractionTests();
void RunUndoHistoryTests();

int main() {
    RunUiInteractionTests();
    RunUndoHistoryTests();
    RunFrameLimiterTests();

    std::printf("\n%s\n", (tg::tests::g_failures == 0) ? "すべて成功" : "失敗あり");
    return (tg::tests::g_failures == 0) ? 0 : 1;
}
