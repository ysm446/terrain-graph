#include "rhi/DeletionQueue.h"

#include <algorithm>

namespace tg::rhi {

void DeletionQueue::Push(ComPtr<IUnknown> object, uint64_t fenceValue) {
    if (!object) {
        return;
    }
    m_entries.push_back(Entry{fenceValue, std::move(object)});
}

void DeletionQueue::Push(DescriptorHeap* heap, const DescriptorHandle& handle,
                         uint64_t fenceValue) {
    if (heap == nullptr || !handle.IsValid()) {
        return;
    }
    m_descriptorEntries.push_back(DescriptorEntry{fenceValue, heap, handle});
}

void DeletionQueue::Collect(uint64_t completedFenceValue) {
    if (!m_entries.empty()) {
        const auto removed = std::remove_if(m_entries.begin(), m_entries.end(),
                                            [completedFenceValue](const Entry& entry) {
                                                return entry.fenceValue <= completedFenceValue;
                                            });
        m_entries.erase(removed, m_entries.end());
    }
    if (!m_descriptorEntries.empty()) {
        const auto removed = std::remove_if(
            m_descriptorEntries.begin(), m_descriptorEntries.end(),
            [completedFenceValue](const DescriptorEntry& entry) {
                if (entry.fenceValue > completedFenceValue) {
                    return false;
                }
                entry.heap->Free(entry.handle);
                return true;
            });
        m_descriptorEntries.erase(removed, m_descriptorEntries.end());
    }
}

void DeletionQueue::Flush() {
    m_entries.clear();
    for (const DescriptorEntry& entry : m_descriptorEntries) {
        entry.heap->Free(entry.handle);
    }
    m_descriptorEntries.clear();
}

}  // namespace tg::rhi
