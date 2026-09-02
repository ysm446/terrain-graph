#pragma once

#include "rhi/Common.h"

#include <vector>

namespace tg::rhi {

inline constexpr uint32_t kInvalidDescriptorIndex = 0xFFFFFFFFu;

struct DescriptorHandle {
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = {};
    uint32_t index = kInvalidDescriptorIndex;

    bool IsValid() const { return index != kInvalidDescriptorIndex; }
};

// 単一ディスクリプタ単位のフリーリスト型アロケータ。
// ヒープを直接触らず、必ずここを経由して確保・解放する。
class DescriptorHeap {
public:
    bool Create(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t capacity,
                bool shaderVisible);
    void Destroy();

    DescriptorHandle Allocate();
    void Free(const DescriptorHandle& handle);

    DescriptorHandle At(uint32_t index) const;

    ID3D12DescriptorHeap* Heap() const { return m_heap.Get(); }
    uint32_t Capacity() const { return m_capacity; }
    uint32_t DescriptorSize() const { return m_descriptorSize; }

private:
    ComPtr<ID3D12DescriptorHeap> m_heap;
    std::vector<uint32_t> m_freeList;
    // 二重解放の検出用。index が払い出し中かどうか。
    std::vector<bool> m_inUse;
    D3D12_CPU_DESCRIPTOR_HANDLE m_cpuStart = {};
    D3D12_GPU_DESCRIPTOR_HANDLE m_gpuStart = {};
    uint32_t m_capacity = 0;
    uint32_t m_descriptorSize = 0;
    bool m_shaderVisible = false;
};

}  // namespace tg::rhi
