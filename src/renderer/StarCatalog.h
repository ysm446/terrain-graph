#pragma once
#include <cstdint>
#include <filesystem>
#include <string_view>
#include <vector>

namespace tg::renderer {
// HLSL の StarEntry と同じ配置。direction は赤道座標の単位ベクトル、illuminance は大気圏外照度（lux）、
// color は輝度 1 に正規化した線形 sRGB。
struct StarEntry {
    float direction[3] = {};
    float illuminance = 0;
    float color[3] = {1, 1, 1};
    float padding = 0;
};
// 星表を赤経・赤緯の 1 度格子に並べ替えたもの。シェーダは画素の周辺セルだけを走査する。
struct StarCatalog {
    std::vector<StarEntry> stars;        // セル順
    std::vector<uint32_t> cellOffsets;   // セル数+1。セル c は [cellOffsets[c], cellOffsets[c+1])。
    // "ra_deg,dec_deg,vmag,bv" 形式のテキストを読む。# で始まる行と空行は無視する。
    bool Parse(std::string_view text);
    bool LoadFile(const std::filesystem::path& path);
};
// 視等級 V → 大気圏外照度（lux）。0 等星を 2.54e-6 lux とする。
float StarIlluminance(float magnitude);
// B-V 色指数 → 黒体近似の線形 sRGB（輝度 1 に正規化）。
void StarColor(float colorIndex, float rgb[3]);
}
