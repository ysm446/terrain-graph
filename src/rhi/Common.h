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

inline constexpr DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

// HRESULT を検査し、失敗ならログを出して false を返す。例外は投げない。
bool CheckHr(HRESULT hr, const char* expr, const char* file, int line);

}  // namespace tg::rhi

#define TG_CHECK_HR(expr) ::tg::rhi::CheckHr((expr), #expr, __FILE__, __LINE__)
