#pragma once

#include "rhi/Common.h"
#include "rhi/GpuResource.h"
#include "rhi/UploadRing.h"

namespace tg::rhi {

class Device;

// フレームとは別に GPU 仕事を流すためのコンピュートキュー。
//
// 合成の評価（河川なら数千回のディスパッチ）をフレームのコマンドリストに載せると、
// その 1 フレームが終わるまで UI が止まる。ここへ流せば、グラフィックスキューは
// 前回の結果を描き続けられる（同時に走らせられる仕事は 1 本だけ）。
//
// **フレームのアップロードリングは使えない。** リングはフレームの完了で巻き戻るが、
// このキューの仕事はそのあとも走っているかもしれない。定数は自前の線形バッファ
// （`Allocate`）から取り、1 本の仕事が終わったら丸ごと巻き戻す。
//
// **コンピュートキューで使える状態には制限がある。** `PIXEL_SHADER_RESOURCE` を含む
// 遷移は記録できない。ここで焼いたものを描画で読むなら、遷移はグラフィックス側で行うこと。
class ComputeQueue {
public:
    bool Create(Device& device, uint64_t uploadBytes, const wchar_t* debugName);
    void Destroy(Device& device);

    // 記録を始める。前回の仕事がまだ走っていれば nullptr。
    // 返るリストにはディスクリプタヒープが設定済み。ルートシグネチャは呼び出し側で。
    ID3D12GraphicsCommandList* Begin(Device& device);
    // 記録を閉じて投入する。**グラフィックスキューがいま記録中のフレームまで
    // 流し終えるのを GPU 側で待ってから**走り始める（このフレームで遷移させた
    // リソースを、そのあとの状態で受け取るため）。
    bool Submit(Device& device);
    // 記録を閉じて捨てる。途中で失敗したときに使う（何も実行されない）。
    void Abort();

    // 投入した仕事がまだ終わっていないか。
    bool IsBusy() const;
    // 投入した仕事の完了を CPU で待つ。
    void Wait();

    // 記録中の仕事が使う定数の置き場。仕事の完了で巻き戻る。
    UploadAllocation Allocate(uint64_t size, uint64_t alignment = 256);
    // 前回の記録で置き場を使い切ったか。呼び出し側は広げて（`Create` し直して）やり直す。
    bool UploadExhausted() const { return m_uploadExhausted; }
    uint64_t UploadBytes() const { return m_uploadBytes; }

    ID3D12Fence* Fence() const { return m_fence.Get(); }
    // 最後に投入した仕事が完了時に立てる値。0 なら未投入。
    uint64_t SubmittedValue() const { return m_submittedValue; }

    bool IsValid() const { return m_queue != nullptr; }

private:
    ComPtr<ID3D12CommandQueue> m_queue;
    ComPtr<ID3D12CommandAllocator> m_allocator;
    ComPtr<ID3D12GraphicsCommandList> m_commandList;
    ComPtr<ID3D12Fence> m_fence;
    HANDLE m_fenceEvent = nullptr;
    uint64_t m_submittedValue = 0;
    bool m_recording = false;

    GpuBuffer m_upload;
    uint8_t* m_uploadMapped = nullptr;
    uint64_t m_uploadBytes = 0;
    uint64_t m_uploadOffset = 0;
    bool m_uploadExhausted = false;
};

}  // namespace tg::rhi
