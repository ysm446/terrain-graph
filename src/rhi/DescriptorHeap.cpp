#include "rhi/DescriptorHeap.h"

#include "core/Log.h"

namespace tg::rhi {

bool DescriptorHeap::Create(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type,
                            uint32_t capacity, bool shaderVisible) {
    D3D12_DESCRIPTOR_HEAP_DESC desc = {};
    desc.Type = type;
    desc.NumDescriptors = capacity;
    desc.Flags = shaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
                               : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

    if (!TG_CHECK_HR(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_heap)))) {
        return false;
    }

    m_capacity = capacity;
    m_shaderVisible = shaderVisible;
    m_descriptorSize = device->GetDescriptorHandleIncrementSize(type);
    m_cpuStart = m_heap->GetCPUDescriptorHandleForHeapStart();
    m_gpuStart = shaderVisible ? m_heap->GetGPUDescriptorHandleForHeapStart()
                               : D3D12_GPU_DESCRIPTOR_HANDLE{};

    m_freeList.clear();
    m_freeList.reserve(capacity);
    // 末尾から積むことで、先頭の index から順に払い出される。
    for (uint32_t i = capacity; i > 0; --i) {
        m_freeList.push_back(i - 1);
    }
    m_inUse.assign(capacity, false);
    return true;
}

void DescriptorHeap::Destroy() {
    m_heap.Reset();
    m_freeList.clear();
    m_inUse.clear();
    m_capacity = 0;
    m_descriptorSize = 0;
    m_cpuStart = {};
    m_gpuStart = {};
}

DescriptorHandle DescriptorHeap::Allocate() {
    if (m_freeList.empty()) {
        TG_LOG_ERROR("ディスクリプタヒープが枯渇しました (capacity=%u)", m_capacity);
        return DescriptorHandle{};
    }
    const uint32_t index = m_freeList.back();
    m_freeList.pop_back();
    m_inUse[index] = true;
    return At(index);
}

void DescriptorHeap::Free(const DescriptorHandle& handle) {
    if (!handle.IsValid()) {
        return;
    }
    if (handle.index >= m_capacity) {
        TG_LOG_ERROR("範囲外のディスクリプタを解放しようとしました (index=%u)", handle.index);
        return;
    }
    if (!m_inUse[handle.index]) {
        // 二重解放するとフリーリストに同じスロットが 2 つ積まれ、
        // 以後 2 つの別リソースが同じディスクリプタを共有してしまう。
        TG_LOG_ERROR("解放済みのディスクリプタを再度解放しようとしました (index=%u)",
                     handle.index);
        return;
    }
    m_inUse[handle.index] = false;
    m_freeList.push_back(handle.index);
}

DescriptorHandle DescriptorHeap::At(uint32_t index) const {
    DescriptorHandle handle;
    if (index >= m_capacity) {
        return handle;
    }
    handle.index = index;
    handle.cpu.ptr = m_cpuStart.ptr + static_cast<SIZE_T>(index) * m_descriptorSize;
    if (m_shaderVisible) {
        handle.gpu.ptr = m_gpuStart.ptr + static_cast<UINT64>(index) * m_descriptorSize;
    }
    return handle;
}

}  // namespace tg::rhi
