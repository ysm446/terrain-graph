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
    // 別のキュー（合成の評価を流すコンピュートキュー）で走っている仕事の完了条件。
    // これ以降に積むものは、フレームのフェンスに加えてこの条件も満たすまで解放しない。
    // フレームの外で走る仕事が、参照中のテクスチャを消されないようにするため。
    void SetAuxiliaryFence(ComPtr<ID3D12Fence> fence, uint64_t value);

    // fenceValue は「この値が完了したら解放してよい」という値。
    void Push(ComPtr<IUnknown> object, uint64_t fenceValue);

    // ディスクリプタの遅延解放。heap は解放時まで生存していること。
    void Push(DescriptorHeap* heap, const DescriptorHandle& handle, uint64_t fenceValue);

    // 完了済みフェンス値を渡し、解放可能なものを解放する。
    // 補助フェンスの条件が付いたものは、そちらの完了も確かめる。
    void Collect(uint64_t completedFenceValue);

    // フェンス値に関係なく全て解放する。GPU 待機後の終了処理でのみ呼ぶ。
    void Flush();

    size_t PendingCount() const { return m_entries.size() + m_descriptorEntries.size(); }

private:
    // 補助フェンスの条件。fence が null なら条件なし。
    struct Guard {
        ComPtr<ID3D12Fence> fence;
        uint64_t value = 0;

        bool Passed() const {
            return !fence || fence->GetCompletedValue() >= value;
        }
    };
    struct Entry {
        uint64_t fenceValue = 0;
        Guard guard;
        ComPtr<IUnknown> object;
    };
    struct DescriptorEntry {
        uint64_t fenceValue = 0;
        Guard guard;
        DescriptorHeap* heap = nullptr;
        DescriptorHandle handle;
    };

    // いま積むものに付ける補助フェンスの条件。
    Guard m_guard;
    std::vector<Entry> m_entries;
    std::vector<DescriptorEntry> m_descriptorEntries;
};

}  // namespace tg::rhi
