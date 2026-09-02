// アンドゥ履歴のテスト。
//
// **段のまとめ方が要点。** スライダーをドラッグすると毎フレーム変更が飛ぶので、
// 素直に積むと 1 回のドラッグで数百段になる。掴んでいるウィジェットの ID が
// 同じ間は 1 段にまとめる、という規則が効いているかを確かめる。

#include "app/UndoHistory.h"

#include "TestSupport.h"

namespace {

using tg::DocumentSnapshot;
using tg::UndoHistory;
using tg::tests::Check;
using tg::tests::Section;

// 段を見分けるため、グラフのノード数を目印に使う。
DocumentSnapshot MakeSnapshot(size_t nodeCount) {
    DocumentSnapshot snapshot;
    snapshot.graphNodes.resize(nodeCount);
    return snapshot;
}

size_t LayerCount(const DocumentSnapshot& snapshot) {
    return snapshot.graphNodes.size();
}

}  // namespace

void RunUndoHistoryTests() {
    Section("アンドゥ履歴 — 段のまとめ方");
    {
        UndoHistory history;
        // 同じスライダーを掴んだままの 3 フレームぶん。
        history.Push(MakeSnapshot(1), 42);
        history.Push(MakeSnapshot(2), 42);
        history.Push(MakeSnapshot(3), 42);
        Check(history.UndoCount() == 1, "同じウィジェットを掴んでいる間は 1 段にまとまる");

        // 掴みが離れたら次は別の段。
        history.EndEdit();
        history.Push(MakeSnapshot(4), 42);
        Check(history.UndoCount() == 2, "離してから掴み直すと別の段になる");
    }
    {
        UndoHistory history;
        // ボタンのように掴みが無い操作は ID 0 で来る。押すたびに 1 段。
        history.Push(MakeSnapshot(1), 0);
        history.Push(MakeSnapshot(2), 0);
        Check(history.UndoCount() == 2, "掴みの無い操作（ID 0）は毎回 1 段積まれる");
    }
    {
        UndoHistory history;
        history.Push(MakeSnapshot(1), 10);
        history.Push(MakeSnapshot(2), 20);
        Check(history.UndoCount() == 2, "別のウィジェットへ移ると別の段になる");
    }

    Section("アンドゥ履歴 — 戻すとやり直す");
    {
        UndoHistory history;
        history.Push(MakeSnapshot(1), 0);  // 1 枚だった状態を控える
        history.Push(MakeSnapshot(2), 0);  // 2 枚だった状態を控える
        // いまは 3 枚。
        const DocumentSnapshot current = MakeSnapshot(3);

        Check(history.CanUndo(), "積んだあとは戻せる");
        Check(!history.CanRedo(), "積んだ直後はやり直せない");

        const DocumentSnapshot first = history.Undo(current);
        Check(LayerCount(first) == 2, "1 回戻すと直前の状態になる");
        Check(history.CanRedo(), "戻したあとはやり直せる");

        const DocumentSnapshot second = history.Undo(first);
        Check(LayerCount(second) == 1, "もう 1 回戻すとさらに前の状態になる");
        Check(!history.CanUndo(), "積んだぶんを戻し切ると、それ以上は戻せない");

        const DocumentSnapshot back = history.Redo(second);
        Check(LayerCount(back) == 2, "やり直すと戻す前の状態へ進む");
        Check(LayerCount(history.Redo(back)) == 3, "最後までやり直すと元の状態に戻る");
        Check(!history.CanRedo(), "やり直し切ると、それ以上は進めない");
    }
    {
        UndoHistory history;
        history.Push(MakeSnapshot(1), 0);
        history.Undo(MakeSnapshot(2));
        Check(history.CanRedo(), "前提: やり直せる状態");
        // 戻したあとに新しく編集すると、やり直せた先は捨てられる。
        history.Push(MakeSnapshot(9), 0);
        Check(!history.CanRedo(), "戻したあとに編集すると、やり直しは無効になる");
    }

    Section("アンドゥ履歴 — 段数の上限");
    {
        UndoHistory history;
        // 上限より多く積む。古い段から捨てられる。
        for (size_t i = 0; i < UndoHistory::kMaxDepth + 10; ++i) {
            history.Push(MakeSnapshot(i + 1), 0);
        }
        Check(history.UndoCount() == UndoHistory::kMaxDepth, "上限を超えたら古い段から捨てる");

        // いちばん古く残っているのは 11 枚目（1〜10 枚目が押し出された）。
        Check(LayerCount(history.UndoStack().front()) == 11, "捨てられるのは古い段のほう");
    }

    Section("アンドゥ履歴 — 消去");
    {
        UndoHistory history;
        history.Push(MakeSnapshot(1), 0);
        history.Undo(MakeSnapshot(2));
        history.Clear();
        Check(!history.CanUndo() && !history.CanRedo(), "Clear で両側とも空になる");
    }
}
