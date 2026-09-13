#pragma once
#include "rhi/Device.h"
namespace tg::rhi {
// フレーム外でRGBA8テクスチャをPNGへ保存。maxSizeは縦横比を保った縮小上限。
bool SaveTextureToPng(Device& device, GpuTexture& texture, const std::filesystem::path& path, uint32_t maxSize = 0);
}
