#pragma once

#include "rhi/GpuResource.h"

namespace tg::rhi {

// アップロードリングから切り出した領域。
struct UploadAllocation {
    void* cpu = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS gpuAddress = 0;
    ID3D12Resource* resource = nullptr;
    uint64_t offset = 0;
    uint64_t size = 0;

    bool IsValid() const { return cpu != nullptr; }
};

// フレームごとに切り替わる線形アロケータ。
// 定数バッファやテクスチャのアップロードは必ずここから確保する。
class UploadRing {
public:
    bool Create(ResourceAllocator& allocator, uint64_t bytesPerFrame);
    void Destroy();

    // フレーム開始時に呼び、そのスロットの使用量をリセットする。
    void BeginFrame(uint32_t frameIndex);

    // alignment は 2 の冪であること。定数バッファなら 256 を指定する。
    UploadAllocation Allocate(uint64_t size, uint64_t alignment = 256);

    uint64_t BytesPerFrame() const { return m_bytesPerFrame; }
    uint64_t UsedBytes() const { return m_offset; }
    uint64_t PeakBytes() const { return m_peakBytes; }

private:
    GpuBuffer m_buffer;
    uint8_t* m_mapped = nullptr;
    uint64_t m_bytesPerFrame = 0;
    uint64_t m_frameBase = 0;
    uint64_t m_offset = 0;
    uint64_t m_peakBytes = 0;
    bool m_overflowReported = false;
};

}  // namespace tg::rhi
