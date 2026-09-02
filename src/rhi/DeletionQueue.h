#pragma once

#include "rhi/Common.h"
#include "rhi/DescriptorHeap.h"

#include <vector>

namespace tg::rhi {

// GPU がまだ参照している可能性のあるオブジェクトを、フレーム同期後に解放するためのキュー。
// リソースを直接 Reset せず、必ずここを経由させる。
// ディスクリプタも同様に、フェンス完了後にフリーリストへ返す。
//
// レンダースレッドからのみ触ること（スレッド安全ではない）。
class DeletionQueue {
public:
    // fenceValue は「この値が完了したら解放してよい」という値。
    void Push(ComPtr<IUnknown> object, uint64_t fenceValue);

    // ディスクリプタの遅延解放。heap は解放時まで生存していること。
    void Push(DescriptorHeap* heap, const DescriptorHandle& handle, uint64_t fenceValue);

    // 完了済みフェンス値を渡し、解放可能なものを解放する。
    void Collect(uint64_t completedFenceValue);

    // フェンス値に関係なく全て解放する。GPU 待機後の終了処理でのみ呼ぶ。
    void Flush();

    size_t PendingCount() const { return m_entries.size() + m_descriptorEntries.size(); }

private:
    struct Entry {
        uint64_t fenceValue = 0;
        ComPtr<IUnknown> object;
    };
    struct DescriptorEntry {
        uint64_t fenceValue = 0;
        DescriptorHeap* heap = nullptr;
        DescriptorHandle handle;
    };

    std::vector<Entry> m_entries;
    std::vector<DescriptorEntry> m_descriptorEntries;
};

}  // namespace tg::rhi
