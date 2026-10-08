#include "rhi/Common.h"

#include "core/Log.h"

namespace tg::rhi {

bool CheckHr(HRESULT hr, const char* expr, const char* file, int line) {
    if (SUCCEEDED(hr)) {
        return true;
    }
    TG_LOG_ERROR("%s:%d: %s が 0x%08lX で失敗しました", file, line, expr,
                 static_cast<unsigned long>(hr));
    return false;
}

bool WaitForFence(ID3D12Fence* fence, uint64_t value, HANDLE event) {
    if (fence == nullptr || event == nullptr) {
        return false;
    }
    if (fence->GetCompletedValue() >= value) {
        return true;
    }
    if (!TG_CHECK_HR(fence->SetEventOnCompletion(value, event))) {
        return false;
    }
    ::WaitForSingleObjectEx(event, INFINITE, FALSE);
    return true;
}

}  // namespace tg::rhi
