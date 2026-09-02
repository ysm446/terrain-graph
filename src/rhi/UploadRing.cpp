#include "rhi/UploadRing.h"

#include "core/Log.h"

namespace tg::rhi {
namespace {

uint64_t AlignUp(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

}  // namespace

bool UploadRing::Create(ResourceAllocator& allocator, uint64_t bytesPerFrame) {
    // フレーム境界自体を最大要求アライメント（テクスチャコピーの 512）に合わせておく。
    // これが揃っていないと、フレーム 1 以降で絶対オフセットのアライメントが崩れる。
    m_bytesPerFrame = AlignUp(bytesPerFrame, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    if (m_bytesPerFrame == 0 || m_bytesPerFrame > (UINT64_MAX / kFrameCount)) {
        TG_LOG_ERROR("アップロードリングのサイズ指定が不正です");
        return false;
    }

    const uint64_t total = m_bytesPerFrame * kFrameCount;
    if (!allocator.CreateUploadBuffer(total, L"UploadRing", m_buffer)) {
        return false;
    }

    // 常時マップしたままにする。アップロードヒープなので Unmap は不要。
    void* mapped = nullptr;
    const D3D12_RANGE readRange = {0, 0};
    if (!TG_CHECK_HR(m_buffer.resource->Map(0, &readRange, &mapped))) {
        return false;
    }
    m_mapped = static_cast<uint8_t*>(mapped);

    TG_LOG_INFO("アップロードリング: %llu MB (%llu MB x %u フレーム)",
                static_cast<unsigned long long>(total / (1024 * 1024)),
                static_cast<unsigned long long>(m_bytesPerFrame / (1024 * 1024)), kFrameCount);
    return true;
}

void UploadRing::Destroy() {
    m_mapped = nullptr;
    m_buffer = GpuBuffer{};
    m_bytesPerFrame = 0;
    m_frameBase = 0;
    m_offset = 0;
    m_peakBytes = 0;
    m_overflowReported = false;
}

void UploadRing::BeginFrame(uint32_t frameIndex) {
    m_frameBase = m_bytesPerFrame * frameIndex;
    m_offset = 0;
    m_overflowReported = false;
}

UploadAllocation UploadRing::Allocate(uint64_t size, uint64_t alignment) {
    UploadAllocation result;
    if (m_mapped == nullptr || size == 0) {
        return result;
    }
    if (alignment == 0 || (alignment & (alignment - 1)) != 0 ||
        alignment > D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT) {
        // 0 や非 2 冪を通すと AlignUp が壊れ、無言で確保が重なり合う。
        TG_LOG_ERROR("アップロードリングのアライメント指定が不正です (%llu)",
                     static_cast<unsigned long long>(alignment));
        return result;
    }

    // アライメントはバッファ先頭からの絶対オフセット（= GPU 仮想アドレス）に対して満たす。
    const uint64_t alignedOffset = AlignUp(m_frameBase + m_offset, alignment) - m_frameBase;
    if (alignedOffset + size > m_bytesPerFrame) {
        if (!m_overflowReported) {
            const uint64_t remaining =
                (alignedOffset < m_bytesPerFrame) ? m_bytesPerFrame - alignedOffset : 0;
            TG_LOG_ERROR("アップロードリングが不足しました (要求 %llu, 残り %llu)",
                         static_cast<unsigned long long>(size),
                         static_cast<unsigned long long>(remaining));
            m_overflowReported = true;
        }
        return result;
    }

    const uint64_t absolute = m_frameBase + alignedOffset;
    result.cpu = m_mapped + absolute;
    result.gpuAddress = m_buffer.GpuAddress() + absolute;
    result.resource = m_buffer.resource.Get();
    result.offset = absolute;
    result.size = size;

    m_offset = alignedOffset + size;
    if (m_offset > m_peakBytes) {
        m_peakBytes = m_offset;
    }
    return result;
}

}  // namespace tg::rhi
