#include "rhi/ComputeQueue.h"

#include "core/Log.h"
#include "rhi/Device.h"

namespace tg::rhi {
namespace {

uint64_t AlignUp(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

}  // namespace

bool ComputeQueue::Create(Device& device, uint64_t uploadBytes, const wchar_t* debugName) {
    Destroy(device);

    ID3D12Device* d3d = device.GetDevice();
    if (d3d == nullptr) {
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    if (!TG_CHECK_HR(d3d->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_queue)))) {
        return false;
    }
    m_queue->SetName(debugName);

    if (!TG_CHECK_HR(d3d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE,
                                                 IID_PPV_ARGS(&m_allocator)))) {
        return false;
    }
    if (!TG_CHECK_HR(d3d->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, m_allocator.Get(),
                                            nullptr, IID_PPV_ARGS(&m_commandList)))) {
        return false;
    }
    // 作った直後は記録状態。Reset で始める作法に揃えるため閉じておく。
    if (!TG_CHECK_HR(m_commandList->Close())) {
        return false;
    }

    if (!TG_CHECK_HR(d3d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)))) {
        return false;
    }
    m_fenceEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (m_fenceEvent == nullptr) {
        TG_LOG_ERROR("コンピュートキューのフェンスイベントを作れませんでした");
        return false;
    }

    m_uploadBytes = AlignUp(uploadBytes, 256);
    if (!device.Allocator().CreateUploadBuffer(m_uploadBytes, debugName, m_upload)) {
        return false;
    }
    void* mapped = nullptr;
    const D3D12_RANGE readRange = {0, 0};
    if (!TG_CHECK_HR(m_upload.resource->Map(0, &readRange, &mapped))) {
        return false;
    }
    m_uploadMapped = static_cast<uint8_t*>(mapped);
    m_uploadOffset = 0;
    m_uploadExhausted = false;
    m_submittedValue = 0;
    return true;
}

void ComputeQueue::Destroy(Device& device) {
    // 走っている仕事の途中でバッファやフェンスを消さない。
    Wait();
    if (m_recording) {
        Abort();
    }
    m_uploadMapped = nullptr;
    device.DeferRelease(m_upload);
    if (m_fenceEvent != nullptr) {
        ::CloseHandle(m_fenceEvent);
        m_fenceEvent = nullptr;
    }
    m_fence.Reset();
    m_commandList.Reset();
    m_allocator.Reset();
    m_queue.Reset();
    m_uploadBytes = 0;
    m_uploadOffset = 0;
    m_uploadExhausted = false;
    m_submittedValue = 0;
}

ID3D12GraphicsCommandList* ComputeQueue::Begin(Device& device) {
    if (!IsValid() || m_recording || IsBusy()) {
        return nullptr;
    }
    // 前回の仕事は終わっている。アロケータも定数の置き場も巻き戻せる。
    if (!TG_CHECK_HR(m_allocator->Reset())) {
        return nullptr;
    }
    if (!TG_CHECK_HR(m_commandList->Reset(m_allocator.Get(), nullptr))) {
        return nullptr;
    }
    m_uploadOffset = 0;
    m_uploadExhausted = false;
    m_recording = true;

    ID3D12DescriptorHeap* heaps[] = {device.SrvHeap().Heap()};
    m_commandList->SetDescriptorHeaps(1, heaps);
    return m_commandList.Get();
}

bool ComputeQueue::Submit(Device& device) {
    if (!m_recording) {
        return false;
    }
    m_recording = false;
    if (!TG_CHECK_HR(m_commandList->Close())) {
        return false;
    }

    // グラフィックスキューが、いま記録中のフレームを流し終えるまで待つ。
    // このフレームの中で遷移させたリソース（書き込み先を UAV へ、など）を、
    // その状態で受け取るため。フェンスは EndFrame でこの値を立てる。
    if (!TG_CHECK_HR(m_queue->Wait(device.FrameFence(), device.NextFenceValue()))) {
        return false;
    }
    ID3D12CommandList* lists[] = {m_commandList.Get()};
    m_queue->ExecuteCommandLists(1, lists);

    const uint64_t value = m_submittedValue + 1;
    if (!TG_CHECK_HR(m_queue->Signal(m_fence.Get(), value))) {
        return false;
    }
    m_submittedValue = value;
    return true;
}

void ComputeQueue::Abort() {
    if (!m_recording) {
        return;
    }
    m_recording = false;
    // 開いたままでは次の Reset ができない。閉じるだけで実行はしない。
    TG_CHECK_HR(m_commandList->Close());
}

bool ComputeQueue::IsBusy() const {
    if (!m_fence || m_submittedValue == 0) {
        return false;
    }
    return m_fence->GetCompletedValue() < m_submittedValue;
}

void ComputeQueue::Wait() {
    if (!IsBusy() || m_fenceEvent == nullptr) {
        return;
    }
    if (!TG_CHECK_HR(m_fence->SetEventOnCompletion(m_submittedValue, m_fenceEvent))) {
        return;
    }
    ::WaitForSingleObjectEx(m_fenceEvent, INFINITE, FALSE);
}

UploadAllocation ComputeQueue::Allocate(uint64_t size, uint64_t alignment) {
    UploadAllocation result;
    if (m_uploadMapped == nullptr || size == 0) {
        return result;
    }
    const uint64_t offset = AlignUp(m_uploadOffset, alignment);
    if (offset + size > m_uploadBytes) {
        // 1 回だけ知らせる。呼び出し側は広げてやり直す。
        if (!m_uploadExhausted) {
            TG_LOG_WARN("コンピュートキューの定数の置き場を使い切りました（%llu KB）",
                        static_cast<unsigned long long>(m_uploadBytes / 1024));
        }
        m_uploadExhausted = true;
        return result;
    }
    result.cpu = m_uploadMapped + offset;
    result.gpuAddress = m_upload.GpuAddress() + offset;
    result.resource = m_upload.resource.Get();
    result.offset = offset;
    result.size = size;
    m_uploadOffset = offset + size;
    return result;
}

}  // namespace tg::rhi
