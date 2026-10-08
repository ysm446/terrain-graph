#pragma once

#include <Windows.h>

#include <directx/d3d12.h>
#include <directx/d3dx12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>

namespace tg::rhi {

template <typename T>
using ComPtr = Microsoft::WRL::ComPtr<T>;

// 同時に GPU が処理しうるフレーム数。スワップチェーンのバッファ数と一致させる。
inline constexpr uint32_t kFrameCount = 3;

// alignment は 2 冪であること（呼び出し側で IsPowerOfTwo で確かめる）。
inline uint64_t AlignUp(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

// 0 や非 2 冪を AlignUp に通すと無言で確保が重なり合うので、線形アロケータは必ずこれで弾く。
inline bool IsPowerOfTwo(uint64_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}

inline constexpr DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

// HRESULT を検査し、失敗ならログを出して false を返す。例外は投げない。
bool CheckHr(HRESULT hr, const char* expr, const char* file, int line);

// フェンスが value に達するまで CPU で待つ。既に達していれば待たない。
// event は呼び出し側が持つ自動リセットのイベント。SetEventOnCompletion に失敗したら false。
bool WaitForFence(ID3D12Fence* fence, uint64_t value, HANDLE event);

}  // namespace tg::rhi

#define TG_CHECK_HR(expr) ::tg::rhi::CheckHr((expr), #expr, __FILE__, __LINE__)
