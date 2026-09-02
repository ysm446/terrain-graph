#include "app/UndoHistory.h"

namespace tg {

void UndoHistory::Push(const DocumentSnapshot& before, uint32_t editId) {
    // 同じウィジェットを掴んだままの変更は 1 段にまとめる。
    // スライダーのドラッグで毎フレーム段が積まれるのを防ぐ。
    if (editId != 0 && editId == m_lastEditId) {
        return;
    }
    m_lastEditId = editId;

    m_undo.push_back(before);
    if (m_undo.size() > kMaxDepth) {
        // 古い段から捨てる。
        m_undo.erase(m_undo.begin());
    }

    // 新しい編集が入ったらリドゥは無効になる。
    m_redo.clear();
}

DocumentSnapshot UndoHistory::Undo(const DocumentSnapshot& current) {
    DocumentSnapshot restored = m_undo.back();
    m_undo.pop_back();
    m_redo.push_back(current);
    // 戻した直後の編集が、直前のドラッグと同じ段にまとめられないようにする。
    m_lastEditId = 0;
    return restored;
}

DocumentSnapshot UndoHistory::Redo(const DocumentSnapshot& current) {
    DocumentSnapshot restored = m_redo.back();
    m_redo.pop_back();
    m_undo.push_back(current);
    m_lastEditId = 0;
    return restored;
}

void UndoHistory::Clear() {
    m_undo.clear();
    m_redo.clear();
    m_lastEditId = 0;
}

}  // namespace tg
