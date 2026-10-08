#include "rhi/DeletionQueue.h"

#include <algorithm>

namespace tg::rhi {

void DeletionQueue::SetAuxiliaryFence(ComPtr<ID3D12Fence> fence, uint64_t value) {
    if (!fence) {
        return;
    }
    const auto it = std::find_if(m_guards.begin(), m_guards.end(),
                                 [&](const Guard& guard) { return guard.fence == fence; });
    if (it != m_guards.end()) {
        it->value = std::max(it->value, value);
    } else {
        m_guards.push_back(Guard{std::move(fence), value});
    }
    m_currentGuards.reset();
}

void DeletionQueue::ClearAuxiliaryFences() {
    m_guards.clear();
    m_currentGuards.reset();
}

bool DeletionQueue::AllPassed(const GuardSetPtr& guards) {
    if (!guards) {
        return true;
    }
    return std::all_of(guards->begin(), guards->end(),
                       [](const Guard& guard) { return guard.Passed(); });
}

DeletionQueue::GuardSetPtr DeletionQueue::CurrentGuards() {
    // 既に完了している条件は付けない（フェンスの参照を無駄に持たない）。
    const auto passed = std::remove_if(m_guards.begin(), m_guards.end(),
                                       [](const Guard& guard) { return guard.Passed(); });
    if (passed != m_guards.end()) {
        m_guards.erase(passed, m_guards.end());
        m_currentGuards.reset();
    }
    if (m_guards.empty()) {
        return nullptr;
    }
    if (!m_currentGuards) {
        m_currentGuards = std::make_shared<const GuardSet>(m_guards);
    }
    return m_currentGuards;
}

void DeletionQueue::Push(ComPtr<IUnknown> object, uint64_t fenceValue) {
    if (!object) {
        return;
    }
    m_entries.push_back(Entry{fenceValue, CurrentGuards(), std::move(object)});
}

void DeletionQueue::Push(DescriptorHeap* heap, const DescriptorHandle& handle,
                         uint64_t fenceValue) {
    if (heap == nullptr || !handle.IsValid()) {
        return;
    }
    m_descriptorEntries.push_back(DescriptorEntry{fenceValue, CurrentGuards(), heap, handle});
}

void DeletionQueue::Collect(uint64_t completedFenceValue) {
    if (!m_entries.empty()) {
        const auto removed = std::remove_if(m_entries.begin(), m_entries.end(),
                                            [completedFenceValue](const Entry& entry) {
                                                return entry.fenceValue <= completedFenceValue &&
                                                       AllPassed(entry.guards);
                                            });
        m_entries.erase(removed, m_entries.end());
    }
    if (!m_descriptorEntries.empty()) {
        const auto removed = std::remove_if(
            m_descriptorEntries.begin(), m_descriptorEntries.end(),
            [completedFenceValue](const DescriptorEntry& entry) {
                if (entry.fenceValue > completedFenceValue || !AllPassed(entry.guards)) {
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
