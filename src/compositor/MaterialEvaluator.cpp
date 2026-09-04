#include "compositor/MaterialEvaluator.h"

#include "core/Log.h"

#include <pix3.h>

#include <algorithm>
#include <cstring>
#include <vector>

using namespace DirectX;

namespace tg::compositor {
namespace {

using rhi::DispatchCount;

constexpr DXGI_FORMAT kBaseColorFormat = DXGI_FORMAT_R11G11B10_FLOAT;
constexpr DXGI_FORMAT kNormalFormat = DXGI_FORMAT_R16G16_FLOAT;
constexpr DXGI_FORMAT kSurfaceFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
// Height は R32。0〜1 の全幅が標高差なので、R16 では 600 m の地形で 1 ULP が約 0.3 m になり、
// 合成後の Height から作り直す法線（ブラー / 堆積 / 積雪 / 河川の後）が階段になる。
constexpr DXGI_FORMAT kHeightFormat = DXGI_FORMAT_R32_FLOAT;
// マスクは 0〜1 の被覆率なので R16 で足りる（op の結果、水面の被覆 / 水深）。
constexpr DXGI_FORMAT kMaskFormat = DXGI_FORMAT_R16_FLOAT;
// 非同期評価の定数の置き場。レイヤーごとにタイル数ぶんの定数と、加工の反復用の
// 数十本が載る。使い切ったら次の評価で倍に広げる。
constexpr uint64_t kAsyncUploadBytes = 2ull * 1024 * 1024;

constexpr uint32_t kFlagMaskInvert = 0x1u;
constexpr uint32_t kFlagBaseLayer = 0x2u;
// レイヤーの種類。シェーダの TG_FLAG_KIND_* と一致させること。
constexpr uint32_t kFlagKindShape = 0x4u;
constexpr uint32_t kFlagKindLiquid = 0x8u;
// 下地に沿わせる（サーフェスのみ）。シェーダの TG_FLAG_WRAP と一致させること。
constexpr uint32_t kFlagWrap = 0x10u;
// 法線マップの緑を反転して読む（OpenGL 規約の素材）。
constexpr uint32_t kFlagFlipNormalGreen = 0x20u;

// レイヤー一覧に出すマスクサムネイルの一辺。行の高さに対して十分な細かさがあればよい。
constexpr uint32_t kMaskThumbnailSize = 64;
// マスクは 1 チャンネルだが、R8 のまま ImGui へ渡すと赤一色で描かれる。
// 灰色として見せたいので RGB へ同じ値を書く。
constexpr DXGI_FORMAT kMaskThumbnailFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

// GPU 側の LayerConstants と一致させること。
struct LayerConstants {
    uint32_t outputIndices[4];
    uint32_t tile[4];
    uint32_t resolution[2];
    uint32_t channelMask;
    uint32_t flags;

    float baseColor[4];
    float surfaceParams[4];
    float blendParams[4];
    float maskParams[4];
    float heightNoise[4];
    float maskNoise[4];
    uint32_t textureIndices0[4];
    uint32_t textureIndices1[4];
    float maskCurve[4];
    uint32_t noiseTypes[4];
    uint32_t paintParams[4];
    uint32_t mapChannels[4];
    float colorAdjust[4];  // 色相（ラジアン）, 彩度, 未使用 x2
};

// GPU 側の SedimentConstants と一致させること。
struct CrumblingConstants {
    uint32_t indices0[4];  // 積む先 UAV, Height SRV, 発生マスク SRV, Height UAV
    uint32_t indices1[4];  // 合成解像度, 粒子の試行回数, 歩数, 岩片の形
    uint32_t indices2[4];  // マスクの出力 UAV, 出力の種類, 未使用 x2
    float params0[4];      // 最小サイズ（テクセル）, 最大サイズ, gravity, spread
    float params1[4];      // 岩片の高さの最大（正規化）, シード, テクセル（m）, 標高差（m）
    float params2[4];      // 岩屑の量, 未使用 x3
};

// GPU 側の ScatterConstants と一致させること。
struct ScatterConstants {
    uint32_t indices0[4];  // Height SRV, 差分 UAV, 配置マスク SRV, パック済み UAV
    uint32_t indices1[4];  // 合成解像度, 形, 向きの決め方, 探索半径
    uint32_t indices2[4];  // Height UAV, マスクの出力 UAV, 出力の種類, 未使用
    float params0[4];      // 散布の間隔（m）, 置く確率, 最小サイズ, 最大サイズ（散布セル）
    float params1[4];      // 高さ（正規化）, 高さのばらつき, 向きのばらつき, 細長さのばらつき
    float params2[4];      // 届く範囲（散布セル）, シード, テクセル（m）, 標高差（m）
    float params3[4];      // なめらかさ, 未使用 x3
};

struct SedimentConstants {
    uint32_t indices0[4];  // UAV: 基盤, 土砂, 流出, 元の高さ
    uint32_t indices1[4];  // SRV: 基盤, 土砂, 元の高さ, 合成の Height
    uint32_t indices2[4];  // グリッドの一辺, 地形を土砂として扱うか, Height の UAV, 合成解像度
    uint32_t indices3[4];  // 最大値の UAV, マスクの UAV, 未使用 x2
    float params[4];       // 安息角ぶんの落差, 1 反復あたりの供給量, マスクのコントラスト, 未使用
};

// GPU 側の SnowConstants と一致させること。
struct SnowConstants {
    uint32_t indices0[4];  // UAV: 基盤, 積雪厚, 流出, ならしの作業用
    uint32_t indices1[4];  // SRV: 基盤, 積雪厚, ならしの作業用, 合成の Height
    uint32_t indices2[4];  // グリッドの一辺, Height の UAV, 合成解像度, マスクの UAV
    uint32_t indices3[4];  // 歩幅, ならしの向き, ならしの半径, 未使用
    float params0[4];      // 安息角ぶんの落差, 1 段あたりの供給量, 流動率, ならしの強さ
    float params1[4];      // マスクのしきい値, マスクのぼかし, 未使用 x2
};

// GPU 側の RiverConstants と一致させること。
struct RiverConstants {
    uint32_t indices0[4];  // UAV: heights, scratch, surface, weights0
    uint32_t indices1[4];  // UAV: weights1, accumA, accumB, width
    uint32_t indices2[4];  // UAV: jfaA, jfaB, maxScratch / SRV: 種マスク
    uint32_t indices3[4];  // UAV: waterLevel, distance, ground, lakeDepth
    uint32_t indices4[4];  // UAV: halfWidth, waterFine, depthFine / SRV: 合成の Height
    uint32_t indices5[4];  // UAV: 合成の Height, マスクの出力 / グリッドの一辺, 合成解像度
    uint32_t indices6[4];  // ヤコビの向き, JFA の歩幅, ぼかし半径, JFA の読み側
    uint32_t indices7[4];  // マスクのチャンネル, 水を張る, Height へ書く, 合成の Normal の UAV
    uint32_t indices8[4];  // SRV: heights, waterLevel, distance, ground
    uint32_t indices9[4];  // SRV: lakeDepth, halfWidth, waterFine, depthFine
    float params0[4];      // 集中度, しきい値（セル数）, 最小勾配（1 セルあたり）, 未使用
    float params1[4];      // 主流の半幅（セル）, 最小の半幅（セル）, 幅の伸び, 河床の深さ
    float params2[4];      // 岸の幅（セル）, 岸の硬さ, 河原の広がり（m）, 河原の比高（m）
    float params3[4];      // 河原のぼかし, セルの大きさ（m）, 標高差（m）, 湖とみなす深さ
    float params4[4];      // 水際のぼかし, 主流の半幅（m）, 岸の幅（m）, 標高差 / 一辺
};

// パスの線分を 1 回のディスパッチで流す上限。シェーダの TG_PATH_MAX_SEGMENTS と一致させること。
// 3 float4 × 256 = 12 KB。定数バッファの 64 KB には十分に収まる。
constexpr uint32_t kMaxPathSegments = 256;

// GPU 側の PathMaskConstants と一致させること。
struct PathMaskConstants {
    uint32_t indices[4];  // 出力 UAV, 出力の一辺, 線分数, 前の結果と max
    float params[4];      // 一辺（m）, ガンマ, 反転, 未使用
    float segments[kMaxPathSegments * 3][4];
};

// CPU へ写すハイトの一辺。パスの投影と表示に使うだけなので粗くてよい
// （2048 m の地形で 4 m。点の幅は数十 m）。
constexpr uint32_t kHeightfieldReadbackResolution = 512;

// GPU 側の MaskOpConstants と一致させること。
struct MaskOpConstants {
    uint32_t indices[4];  // 出力 UAV, 入力 A SRV, 入力 B SRV, 素材 SRV
    uint32_t params0[4];  // 出力の一辺, 読むチャンネル, 反転 / ブレンドの種類, 未使用
    float params1[4];
    float params2[4];
};

// GPU 側の FluvialConstants と一致させること。
struct FluvialConstants {
    uint32_t indices0[4];  // heights, heightsScratch, weights0, weights1
    uint32_t indices1[4];  // accumA, accumB, mask, maxScratch
    uint32_t indices2[4];  // 入力 Height の SRV, グリッドの一辺, ヤコビの向き, ぼかし半径
    float params0[4];      // 集中度, しきい値（セル数）, ガンマ, やわらかさ
    float params1[4];      // 川縁の強さ, 出力カーブ, 未使用 x2
};

// GPU 側の BlurConstants と一致させること。
struct BlurConstants {
    uint32_t sourceIndex;
    uint32_t outputIndex;
    uint32_t axis;  // 0 = 水平 / 1 = 垂直
    float radiusTexels;

    uint32_t tile[4];
    uint32_t resolution[2];
    float strength;
    float heightPerSize;

    // どこをぼかすか。**既定はマスク無し。**
    // 法線の作り直し（CsNormalFromHeight）は堆積・積雪・河川なども使うので、
    // ゼロのままだとディスクリプタ 0 をマスクとして読んでしまう。
    uint32_t maskIndex = kInvalidTextureIndex;
    uint32_t pad0[3];
};

// GPU 側の MaskConstants と一致させること。
struct MaskConstants {
    uint32_t heightIndex;
    uint32_t outputIndex;
    uint32_t source;
    float derivedScale;

    uint32_t tile[4];
    uint32_t resolution[2];
    float pad0[2];
};

// --- 焼き直しの要否を決めるハッシュ -----------------------------------------
//
// マスクの op は「入力が前回と同じなら焼き直さない」。その判定に使う。
// FNV-1a。速度も衝突耐性もこの用途には十分で、依存も増えない。
uint64_t HashBytes(uint64_t seed, const void* data, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    uint64_t hash = seed;
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 0x100000001b3ull;
    }
    return hash;
}

// **Height に効く値だけ**を混ぜる。色やラフネスを変えても、
// それを読むマスク（川筋 / 傾斜）は焼き直さずに済ませたいため。
uint64_t HashHeightState(uint64_t seed, const MaterialLayer& layer) {
    uint64_t hash = HashBytes(seed, &layer.kind, sizeof(layer.kind));
    hash = HashBytes(hash, &layer.enabled, sizeof(layer.enabled));
    hash = HashBytes(hash, &layer.channelMask, sizeof(layer.channelMask));
    hash = HashBytes(hash, &layer.heightSource, sizeof(layer.heightSource));
    hash = HashBytes(hash, &layer.heightBase, sizeof(layer.heightBase));
    hash = HashBytes(hash, &layer.heightGain, sizeof(layer.heightGain));
    hash = HashBytes(hash, &layer.heightNoise, sizeof(layer.heightNoise));
    hash = HashBytes(hash, &layer.heightTexture, sizeof(layer.heightTexture));
    hash = HashBytes(hash, &layer.blendRange, sizeof(layer.blendRange));
    hash = HashBytes(hash, &layer.wrapToUnderlying, sizeof(layer.wrapToUnderlying));
    hash = HashBytes(hash, &layer.uvScale, sizeof(layer.uvScale));
    hash = HashBytes(hash, &layer.material, sizeof(layer.material));
    hash = HashBytes(hash, &layer.blur, sizeof(layer.blur));
    hash = HashBytes(hash, &layer.sediment, sizeof(layer.sediment));
    hash = HashBytes(hash, &layer.crumbling, sizeof(layer.crumbling));
    hash = HashBytes(hash, &layer.snow, sizeof(layer.snow));
    hash = HashBytes(hash, &layer.river, sizeof(layer.river));
    hash = HashBytes(hash, &layer.droplet, sizeof(layer.droplet));
    hash = HashBytes(hash, &layer.maskOnly, sizeof(layer.maskOnly));
    // マスクは「どこに載せるか」を決めるので Height にも効く。
    hash = HashBytes(hash, &layer.mask.source, sizeof(layer.mask.source));
    hash = HashBytes(hash, &layer.mask.constant, sizeof(layer.mask.constant));
    hash = HashBytes(hash, &layer.mask.noise, sizeof(layer.mask.noise));
    hash = HashBytes(hash, &layer.mask.derivedScale, sizeof(layer.mask.derivedScale));
    hash = HashBytes(hash, &layer.mask.contrast, sizeof(layer.mask.contrast));
    hash = HashBytes(hash, &layer.mask.levelsLow, sizeof(layer.mask.levelsLow));
    hash = HashBytes(hash, &layer.mask.levelsHigh, sizeof(layer.mask.levelsHigh));
    hash = HashBytes(hash, &layer.mask.invert, sizeof(layer.mask.invert));
    hash = HashBytes(hash, &layer.mask.paint, sizeof(layer.mask.paint));
    hash = HashBytes(hash, &layer.mask.texture, sizeof(layer.mask.texture));
    hash = HashBytes(hash, &layer.mask.maskOp, sizeof(layer.mask.maskOp));
    return hash;
}

// op のパラメータ。種類ごとに使う構造体だけを混ぜる。
uint64_t HashMaskOpParams(uint64_t seed, const MaskOp& op) {
    switch (op.kind) {
        case MaskOpKind::Image:
            return HashBytes(seed, &op.map, sizeof(op.map));
        case MaskOpKind::Fluvial:
            return HashBytes(seed, &op.fluvial, sizeof(op.fluvial));
        case MaskOpKind::Slope:
            return HashBytes(seed, &op.slope, sizeof(op.slope));
        case MaskOpKind::Levels:
            return HashBytes(seed, &op.levels, sizeof(op.levels));
        case MaskOpKind::Blend:
            return HashBytes(seed, &op.blend, sizeof(op.blend));
        case MaskOpKind::Blur:
            return HashBytes(seed, &op.blur, sizeof(op.blur));
        case MaskOpKind::Sediment:
            return HashBytes(seed, &op.sedimentMask, sizeof(op.sedimentMask));
        case MaskOpKind::Crumbling:
            return HashBytes(seed, &op.crumblingMask, sizeof(op.crumblingMask));
        case MaskOpKind::Scatter:
            return HashBytes(seed, &op.scatterMask, sizeof(op.scatterMask));
        case MaskOpKind::Noise:
            return HashBytes(seed, &op.noise, sizeof(op.noise));
        case MaskOpKind::Curvature:
            return HashBytes(seed, &op.curvature, sizeof(op.curvature));
        case MaskOpKind::Snow:
            return HashBytes(seed, &op.snowMask, sizeof(op.snowMask));
        case MaskOpKind::Height:
            return HashBytes(seed, &op.height, sizeof(op.height));
        case MaskOpKind::River:
            return HashBytes(seed, &op.riverMask, sizeof(op.riverMask));
        case MaskOpKind::Droplet:
            return HashBytes(seed, &op.dropletMask, sizeof(op.dropletMask));
        case MaskOpKind::Path: {
            // 線分列の中身まで混ぜる（点を 1 つ動かしただけでも焼き直すため）。
            uint64_t hash = HashBytes(seed, &op.pathMask, sizeof(op.pathMask));
            const size_t count = op.pathSegments.size();
            hash = HashBytes(hash, &count, sizeof(count));
            if (!op.pathSegments.empty()) {
                hash = HashBytes(hash, op.pathSegments.data(),
                                 op.pathSegments.size() * sizeof(PathSegment));
            }
            return hash;
        }
        default:
            return seed;
    }
}

bool CreateChannelTexture(rhi::Device& device, uint32_t resolution, DXGI_FORMAT format,
                          const wchar_t* debugName, rhi::GpuTexture& outTexture) {
    rhi::TextureDesc desc;
    desc.width = resolution;
    desc.height = resolution;
    desc.format = format;
    desc.allowUnorderedAccess = true;
    desc.createSrv = true;
    desc.initialState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    desc.debugName = debugName;
    return device.Allocator().CreateTexture2D(desc, outTexture);
}

bool CreateTextureSet(rhi::Device& device, uint32_t resolution, MaterialTextureSet& set) {
    return CreateChannelTexture(device, resolution, kBaseColorFormat, L"MaterialBaseColor",
                                set.baseColor) &&
           CreateChannelTexture(device, resolution, kNormalFormat, L"MaterialNormal",
                                set.normal) &&
           CreateChannelTexture(device, resolution, kSurfaceFormat, L"MaterialSurface",
                                set.surface) &&
           CreateChannelTexture(device, resolution, kHeightFormat, L"MaterialHeight",
                                set.height);
}

void ReleaseTextureSet(rhi::Device& device, MaterialTextureSet& set) {
    device.DeferRelease(set.baseColor);
    device.DeferRelease(set.normal);
    device.DeferRelease(set.surface);
    device.DeferRelease(set.height);
}

}  // namespace

float CpuHeightfield::Sample(float u, float v) const {
    if (!IsValid()) {
        return 0.5f;
    }
    // テクセル中心が (i + 0.5) / n に当たる（DownsampleHeight の矩形の中心）。
    const float last = static_cast<float>(resolution - 1);
    const float fx = std::clamp(u * static_cast<float>(resolution) - 0.5f, 0.0f, last);
    const float fy = std::clamp(v * static_cast<float>(resolution) - 0.5f, 0.0f, last);
    const uint32_t x0 = static_cast<uint32_t>(fx);
    const uint32_t y0 = static_cast<uint32_t>(fy);
    const uint32_t x1 = std::min(x0 + 1, resolution - 1);
    const uint32_t y1 = std::min(y0 + 1, resolution - 1);
    const float tx = fx - static_cast<float>(x0);
    const float ty = fy - static_cast<float>(y0);
    const auto at = [&](uint32_t x, uint32_t y) {
        return values[static_cast<size_t>(y) * resolution + x];
    };
    const float top = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * tx;
    const float bottom = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * tx;
    return top + (bottom - top) * ty;
}

uint64_t HashStackHeightState(const MaterialStack& stack) {
    uint64_t hash = 0xcbf29ce484222325ull;
    const float sizeMeters = stack.SizeMeters();
    const float heightMeters = stack.HeightMeters();
    hash = HashBytes(hash, &sizeMeters, sizeof(sizeMeters));
    hash = HashBytes(hash, &heightMeters, sizeof(heightMeters));
    for (const MaterialLayer& layer : stack.Layers()) {
        hash = HashHeightState(hash, layer);
    }
    for (const MaskOp& op : stack.MaskOps()) {
        hash = HashBytes(hash, &op.kind, sizeof(op.kind));
        hash = HashBytes(hash, &op.inputA, sizeof(op.inputA));
        hash = HashBytes(hash, &op.inputB, sizeof(op.inputB));
        hash = HashBytes(hash, &op.heightSourceLayer, sizeof(op.heightSourceLayer));
        hash = HashMaskOpParams(hash, op);
    }
    return hash;
}

bool MaterialEvaluator::ReadbackHeight(rhi::Device& device, CpuHeightfield& out) {
    rhi::GpuTexture& height = TexturesMutable().height;
    if (!height.IsValid() || m_resolution == 0) {
        return false;
    }
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT rowCount = 0;
    UINT64 rowBytes = 0;
    UINT64 totalBytes = 0;
    const D3D12_RESOURCE_DESC desc = height.resource->GetDesc();
    device.GetDevice()->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rowCount, &rowBytes,
                                              &totalBytes);
    rhi::GpuBuffer readback;
    if (!device.Allocator().CreateReadbackBuffer(totalBytes, L"HeightReadbackNow", readback)) {
        return false;
    }
    const bool executed = device.ExecuteImmediate([&](ID3D12GraphicsCommandList* commandList) {
        PIXBeginEvent(commandList, PIX_COLOR(120, 140, 160), "HeightReadbackNow");
        TransitionIfNeeded(commandList, height, D3D12_RESOURCE_STATE_COPY_SOURCE);
        const CD3DX12_TEXTURE_COPY_LOCATION destination(readback.resource.Get(), footprint);
        const CD3DX12_TEXTURE_COPY_LOCATION source(height.resource.Get(), 0);
        commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        PIXEndEvent(commandList);
    });
    if (!executed) {
        device.DeferRelease(readback);
        return false;
    }
    void* mapped = nullptr;
    const D3D12_RANGE readRange = {0, static_cast<SIZE_T>(totalBytes)};
    if (!TG_CHECK_HR(readback.resource->Map(0, &readRange, &mapped))) {
        device.DeferRelease(readback);
        return false;
    }
    out.resolution = m_resolution;
    out.values.resize(static_cast<size_t>(m_resolution) * m_resolution);
    const auto* base = static_cast<const uint8_t*>(mapped) + footprint.Offset;
    const uint32_t rows = std::min<uint32_t>(rowCount, m_resolution);
    for (uint32_t y = 0; y < rows; ++y) {
        std::memcpy(out.values.data() + static_cast<size_t>(y) * m_resolution,
                    base + static_cast<size_t>(y) * footprint.Footprint.RowPitch,
                    sizeof(float) * m_resolution);
    }
    const D3D12_RANGE writtenRange = {0, 0};
    readback.resource->Unmap(0, &writtenRange);
    device.DeferRelease(readback);
    return true;
}

bool MaterialEvaluator::Create(rhi::Device& device, uint32_t resolution, bool asynchronous) {
    // 走っている評価が前の組を読んでいるかもしれない。作り直す前に必ず待つ。
    WaitForEvaluation();
    ReleaseTextures(device);
    m_asyncInFlight = false;
    m_hasResult = false;
    m_asynchronous = asynchronous;

    if (!CreateTextureSet(device, resolution, m_textures) ||
        !CreateChannelTexture(device, resolution, kHeightFormat, L"MaterialScratch",
                              m_scratch)) {
        return false;
    }
    if (asynchronous) {
        // 表側。描画はこちらを読み、評価は裏側（m_textures）へ書く。
        if (!CreateTextureSet(device, resolution, m_frontTextures)) {
            return false;
        }
        if (!m_compute.IsValid() &&
            !m_compute.Create(device, kAsyncUploadBytes, L"MaterialEvaluatorCompute")) {
            // キューが作れなくても同期で評価はできる。落とさずに続ける。
            TG_LOG_WARN("合成の評価用のコンピュートキューを作れませんでした。同期で評価します");
            m_compute.Destroy(device);
        }
    }

    m_resolution = resolution;
    m_evaluatedRevision = 0;
    return true;
}

void MaterialEvaluator::ReleaseFluvialResources(rhi::Device& device) {
    device.DeferRelease(m_fluvial.heights);
    device.DeferRelease(m_fluvial.heightsScratch);
    device.DeferRelease(m_fluvial.weights0);
    device.DeferRelease(m_fluvial.weights1);
    device.DeferRelease(m_fluvial.accumA);
    device.DeferRelease(m_fluvial.accumB);
    device.DeferRelease(m_fluvial.maxScratch);
    m_fluvial.workResolution = 0;
}

// 川筋の作業リソースは**使うときだけ**作る。1 組を順に使い回すので、
// 一番大きいグリッドに合わせて作れば足りる（小さい川筋は左上だけを使う）。
bool MaterialEvaluator::EnsureFluvialResources(rhi::Device& device, uint32_t workResolution) {
    if (m_fluvial.workResolution >= workResolution && m_fluvial.IsValid()) {
        return true;
    }
    ReleaseFluvialResources(device);

    const bool ok =
        CreateChannelTexture(device, workResolution, DXGI_FORMAT_R32_FLOAT, L"FluvialHeights",
                             m_fluvial.heights) &&
        CreateChannelTexture(device, workResolution, DXGI_FORMAT_R32_FLOAT,
                             L"FluvialHeightsScratch", m_fluvial.heightsScratch) &&
        CreateChannelTexture(device, workResolution, DXGI_FORMAT_R32G32B32A32_FLOAT,
                             L"FluvialWeights0", m_fluvial.weights0) &&
        CreateChannelTexture(device, workResolution, DXGI_FORMAT_R32G32B32A32_FLOAT,
                             L"FluvialWeights1", m_fluvial.weights1) &&
        CreateChannelTexture(device, workResolution, DXGI_FORMAT_R32_FLOAT, L"FluvialAccumA",
                             m_fluvial.accumA) &&
        CreateChannelTexture(device, workResolution, DXGI_FORMAT_R32_FLOAT, L"FluvialAccumB",
                             m_fluvial.accumB) &&
        CreateChannelTexture(device, 1, DXGI_FORMAT_R32_UINT, L"FluvialMax",
                             m_fluvial.maxScratch);
    if (!ok) {
        TG_LOG_WARN("川筋マスクの作業リソースを作れませんでした（%u^2）", workResolution);
        ReleaseFluvialResources(device);
        return false;
    }
    m_fluvial.workResolution = workResolution;
    return true;
}

// マスクの op ごとの結果テクスチャ。**中間結果を残す**ので、合流（Blend）や
// 同じマスクの使い回しができる。川筋だけは自前のグリッド、それ以外は合成解像度。
bool MaterialEvaluator::EnsureMaskOpTextures(rhi::Device& device, const MaskProgram& ops) {
    while (m_maskOpTextures.size() > ops.size()) {
        device.DeferRelease(m_maskOpTextures.back());
        m_maskOpTextures.pop_back();
        m_maskOpResolutions.pop_back();
        m_maskOpHashes.pop_back();
    }
    m_maskOpTextures.resize(ops.size());
    m_maskOpResolutions.resize(ops.size(), 0);
    m_maskOpHashes.resize(ops.size(), 0);

    bool ok = true;
    for (size_t i = 0; i < ops.size(); ++i) {
        const uint32_t resolution = (ops[i].kind == MaskOpKind::Fluvial)
                                        ? std::clamp(ops[i].fluvial.resolution, 64u, 2048u)
                                        : m_resolution;
        if (m_maskOpTextures[i].IsValid() && m_maskOpResolutions[i] == resolution) {
            continue;
        }
        device.DeferRelease(m_maskOpTextures[i]);
        if (!CreateChannelTexture(device, resolution, kMaskFormat, L"MaskOp",
                                  m_maskOpTextures[i])) {
            TG_LOG_WARN("マスクの結果テクスチャを作れませんでした（%u^2）", resolution);
            ok = false;
            continue;
        }
        m_maskOpResolutions[i] = resolution;
        // 作り直したら中身は空。ハッシュを無効にして必ず焼き直す。
        m_maskOpHashes[i] = 0;
    }
    return ok;
}

void MaterialEvaluator::Destroy(rhi::Device& device) {
    // コンピュートキューがまだ作業用テクスチャを読んでいるかもしれない。先に待つ。
    WaitForEvaluation();
    m_compute.Destroy(device);
    m_asyncInFlight = false;
    m_hasResult = false;
    ReleaseHeightfieldResources(device);
    ReleaseFluvialResources(device);
    device.DeferRelease(m_maskHeightRange);
    ReleaseSedimentResources(device);
    ReleaseCrumblingResources(device);
    ReleaseSnowResources(device);
    ReleaseRiverResources(device);
    ReleaseDropletResources(device);
    ReleaseScatterResources(device);
    for (rhi::GpuTexture& texture : m_maskOpTextures) {
        device.DeferRelease(texture);
    }
    m_maskOpTextures.clear();
    m_maskOpResolutions.clear();
    m_maskOpHashes.clear();
    ReleaseTextures(device);
    for (rhi::GpuTexture& thumbnail : m_maskThumbnails) {
        device.DeferRelease(thumbnail);
    }
    m_maskThumbnails.clear();
    m_resolution = 0;
    m_evaluatedRevision = 0;
}

// マスクサムネイルは合成解像度に依らないので、Create / Resize では作り直さない。
void MaterialEvaluator::EnsureMaskThumbnails(rhi::Device& device, size_t layerCount) {
    // 減ったぶんは捨てる。GPU がまだ見ているかもしれないので Defer を通す。
    while (m_maskThumbnails.size() > layerCount) {
        device.DeferRelease(m_maskThumbnails.back());
        m_maskThumbnails.pop_back();
    }

    while (m_maskThumbnails.size() < layerCount) {
        rhi::TextureDesc desc;
        desc.width = kMaskThumbnailSize;
        desc.height = kMaskThumbnailSize;
        desc.format = kMaskThumbnailFormat;
        desc.allowUnorderedAccess = true;
        desc.createSrv = true;
        desc.initialState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        desc.debugName = L"LayerMaskThumbnail";

        rhi::GpuTexture thumbnail;
        if (!device.Allocator().CreateTexture2D(desc, thumbnail)) {
            TG_LOG_WARN("レイヤーのマスクサムネイルを作れませんでした");
            return;
        }
        m_maskThumbnails.push_back(std::move(thumbnail));
    }
}

D3D12_GPU_DESCRIPTOR_HANDLE MaterialEvaluator::MaskThumbnailHandle(size_t layerIndex) const {
    if (layerIndex >= m_maskThumbnails.size() || !m_maskThumbnails[layerIndex].IsValid()) {
        return D3D12_GPU_DESCRIPTOR_HANDLE{0};
    }
    return m_maskThumbnails[layerIndex].srv.gpu;
}

void MaterialEvaluator::ReleaseTextures(rhi::Device& device) {
    ReleaseTextureSet(device, m_textures);
    ReleaseTextureSet(device, m_frontTextures);
    device.DeferRelease(m_scratch);
}

bool MaterialEvaluator::Resize(rhi::Device& device, uint32_t resolution) {
    if (resolution == m_resolution) {
        return true;
    }
    // 作り直す前に GPU の参照が切れるのを待つ（コンピュートキューの評価も含む）。
    device.WaitForGpu();
    return Create(device, resolution, m_asynchronous);
}

bool MaterialEvaluator::IsEvaluating() const {
    return m_asyncInFlight && m_compute.IsBusy();
}

void MaterialEvaluator::WaitForEvaluation() {
    m_compute.Wait();
}

rhi::UploadAllocation MaterialEvaluator::AllocateConstants(rhi::Device& device, uint64_t size) {
    if (m_recordingAsync) {
        return m_compute.Allocate(size, 256);
    }
    return device.Upload().Allocate(size, 256);
}

std::vector<TileRect> MaterialEvaluator::MakeTiles() const {
    std::vector<TileRect> tiles;
    for (uint32_t y = 0; y < m_resolution; y += m_tileSize) {
        for (uint32_t x = 0; x < m_resolution; x += m_tileSize) {
            TileRect tile;
            tile.x = x;
            tile.y = y;
            tile.width = std::min(m_tileSize, m_resolution - x);
            tile.height = std::min(m_tileSize, m_resolution - y);
            tiles.push_back(tile);
        }
    }
    return tiles;
}

// メッシュの描画から読めるようにする。Height は頂点 / ドメインシェーダ
// （ディスプレイスメント）からも読まれるため、NON_PIXEL も含める。
// 状態の食い違いを避けるため 4 枚とも同じ状態に揃える。
void MaterialEvaluator::TransitionForDisplay(ID3D12GraphicsCommandList* commandList,
                                             MaterialTextureSet& set) {
    constexpr D3D12_RESOURCE_STATES kDisplayReadState =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    TransitionIfNeeded(commandList, set.baseColor, kDisplayReadState);
    TransitionIfNeeded(commandList, set.normal, kDisplayReadState);
    TransitionIfNeeded(commandList, set.surface, kDisplayReadState);
    TransitionIfNeeded(commandList, set.height, kDisplayReadState);
}
// マスクの op を 1 つ焼く。入力（他の op の結果）は既に SRV になっている。
bool MaterialEvaluator::RunMaskOp(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                                  ID3D12GraphicsCommandList* commandList,
                                  const MaskProgram& ops, size_t index,
                                  const MaterialStack& stack, const TextureLibrary& textures) {
    if (index >= ops.size() || index >= m_maskOpTextures.size() ||
        !m_maskOpTextures[index].IsValid()) {
        return false;
    }
    const MaskOp& op = ops[index];
    rhi::GpuTexture& target = m_maskOpTextures[index];

    // 川筋だけは反復が要るので専用のパイプライン。
    if (op.kind == MaskOpKind::Fluvial) {
        return ApplyFluvialMask(device, pipelineCache, commandList, op, stack, target);
    }
    // 堆積の厚みは、直前に走った堆積レイヤーの作業用テクスチャから焼く。
    if (op.kind == MaskOpKind::Sediment) {
        return ApplySedimentMask(device, pipelineCache, commandList, op, stack, target);
    }
    // 崩落も同じく、直前に走った崩落レイヤーの作業用テクスチャから焼く。
    if (op.kind == MaskOpKind::Crumbling) {
        return ApplyCrumblingMask(device, pipelineCache, commandList, op, target);
    }
    // 散布も同じく、直前に走った散布レイヤーの作業用テクスチャから焼く。
    if (op.kind == MaskOpKind::Scatter) {
        return ApplyScatterMask(device, pipelineCache, commandList, op, target);
    }
    // 積雪の被覆も、直前に走った積雪レイヤーの積雪厚から焼く。
    if (op.kind == MaskOpKind::Snow) {
        return ApplySnowMask(device, pipelineCache, commandList, op, stack, target);
    }
    // 河川の水面 / 河原 / 水深も、直前に走った河川レイヤーの作業用テクスチャから焼く。
    if (op.kind == MaskOpKind::River) {
        return ApplyRiverMask(device, pipelineCache, commandList, op, stack, target);
    }
    // 水滴侵食の流量 / 堆積も、直前に走った水滴侵食レイヤーの作業用テクスチャから焼く。
    if (op.kind == MaskOpKind::Droplet) {
        return ApplyDropletMask(device, pipelineCache, commandList, op, target);
    }
    // パスは線分の列を定数で受ける専用のシェーダ。
    if (op.kind == MaskOpKind::Path) {
        return ApplyPathMask(device, pipelineCache, commandList, op, stack, target);
    }
    // ぼかしは近傍を読むので、1 ディスパッチでは書けない（水平 / 垂直に分ける）。
    if (op.kind == MaskOpKind::Blur) {
        return ApplyMaskBlur(device, pipelineCache, commandList, op, stack,
                             m_maskOpResolutions[index], target);
    }

    const wchar_t* entry = L"CsImage";
    switch (op.kind) {
        case MaskOpKind::Noise:  entry = L"CsNoise"; break;
        case MaskOpKind::Slope:  entry = L"CsSlope"; break;
        case MaskOpKind::Height: entry = L"CsHeight"; break;
        case MaskOpKind::Curvature: entry = L"CsCurvature"; break;
        case MaskOpKind::Levels: entry = L"CsLevels"; break;
        case MaskOpKind::Blend:  entry = L"CsBlend"; break;
        default: break;
    }
    ID3D12PipelineState* pipeline = pipelineCache.GetCompute(L"CompositeMaskOps.hlsl", entry);
    if (pipeline == nullptr) {
        return false;
    }

    const uint32_t resolution = m_maskOpResolutions[index];
    MaskOpConstants constants = {};
    constants.indices[0] = target.UavIndex();
    constants.indices[1] = kInvalidTextureIndex;
    constants.indices[2] = kInvalidTextureIndex;
    constants.indices[3] = kInvalidTextureIndex;
    if (op.inputA >= 0 && static_cast<size_t>(op.inputA) < m_maskOpTextures.size()) {
        constants.indices[1] = m_maskOpTextures[op.inputA].SrvIndex();
    }
    if (op.inputB >= 0 && static_cast<size_t>(op.inputB) < m_maskOpTextures.size()) {
        constants.indices[2] = m_maskOpTextures[op.inputB].SrvIndex();
    }
    constants.params0[0] = resolution;

    switch (op.kind) {
        case MaskOpKind::Image: {
            constants.indices[3] = textures.SrvIndex(op.map.texture, false);
            constants.params0[1] = static_cast<uint32_t>(op.map.channel);
            break;
        }
        case MaskOpKind::Noise: {
            constants.params0[1] = static_cast<uint32_t>(op.noise.type);
            constants.params0[3] = static_cast<uint32_t>(std::clamp(op.noise.octaves, 1, 8));
            constants.params1[0] = op.noise.scale;
            constants.params1[1] = op.noise.amount;
            constants.params1[2] = op.noise.offset;
            break;
        }
        case MaskOpKind::Curvature: {
            constants.indices[3] = m_textures.height.SrvIndex();
            constants.params0[1] = static_cast<uint32_t>(op.curvature.mode);
            // 感度は m。ハイト 0〜1 の全幅が標高差なので、その比へ直す。
            const float heightMeters =
                (stack.HeightMeters() > 0.0f) ? stack.HeightMeters() : 1.0f;
            constants.params1[0] =
                std::max(op.curvature.sensitivityMeters, 1e-4f) / heightMeters;
            constants.params1[1] = std::clamp(op.curvature.threshold, 0.0f, 0.99f);
            constants.params1[2] = std::clamp(op.curvature.gamma, 0.05f, 8.0f);
            // 比べる周りの広さ（テクセル）。
            const float sizeMeters = (stack.SizeMeters() > 0.0f) ? stack.SizeMeters() : 1.0f;
            const float cellMeters = sizeMeters / static_cast<float>(std::max(1u, resolution));
            constants.params2[0] =
                std::clamp(op.curvature.detailMeters / std::max(cellMeters, 1e-6f), 1.0f, 64.0f);
            break;
        }
        case MaskOpKind::Height: {
            constants.indices[3] = m_textures.height.SrvIndex();
            constants.params0[1] = op.height.useFullRange ? 1u : 0u;
            constants.params0[2] = op.height.invert ? 1u : 0u;
            // 標高は m。ハイト 0〜1 の全幅が標高差なので、その比へ直す。
            const float heightMeters =
                (stack.HeightMeters() > 0.0f) ? stack.HeightMeters() : 1.0f;
            const float low = std::min(op.height.minMeters, op.height.maxMeters);
            const float high = std::max(op.height.minMeters, op.height.maxMeters);
            constants.params1[0] = low / heightMeters;
            constants.params1[1] = high / heightMeters;
            constants.params1[2] = std::clamp(op.height.gamma, 0.05f, 8.0f);
            constants.params1[3] = std::max(op.height.featherMeters, 0.0f) / heightMeters;
            // 全範囲のときだけ、最低 / 最高をためる 1 枚を使う。
            if (op.height.useFullRange && EnsureMaskHeightRange(device)) {
                constants.indices[2] = m_maskHeightRange.UavIndex();
            } else {
                constants.params0[1] = 0u;
            }
            break;
        }
        case MaskOpKind::Slope: {
            constants.indices[3] = m_textures.height.SrvIndex();
            constants.params0[2] = op.slope.invert ? 1u : 0u;
            constants.params1[0] = op.slope.minDegrees;
            constants.params1[1] = op.slope.maxDegrees;
            constants.params1[2] = op.slope.gamma;
            // 実寸の勾配にするための比（標高差 / 一辺）。法線と同じ考え方。
            constants.params1[3] = (stack.SizeMeters() > 0.0f)
                                       ? (stack.HeightMeters() / stack.SizeMeters())
                                       : 0.0f;
            // 「最大ディテール」は**勾配を測る距離**（テクセル）にする。
            const float sizeMeters = (stack.SizeMeters() > 0.0f) ? stack.SizeMeters() : 1.0f;
            const float cellMeters = sizeMeters / static_cast<float>(std::max(1u, resolution));
            constants.params2[0] =
                std::clamp(op.slope.detailMeters / std::max(cellMeters, 1e-6f), 1.0f, 64.0f);
            break;
        }
        case MaskOpKind::Levels: {
            constants.params0[2] = op.levels.invert ? 1u : 0u;
            constants.params1[0] = op.levels.blackPoint;
            constants.params1[1] = op.levels.whitePoint;
            constants.params1[2] = op.levels.gamma;
            break;
        }
        case MaskOpKind::Blend: {
            constants.params0[2] = static_cast<uint32_t>(op.blend.mode);
            constants.params1[0] = op.blend.intensity;
            break;
        }
        default: break;
    }

    const rhi::UploadAllocation cb = AllocateConstants(device, sizeof(MaskOpConstants));
    if (!cb.IsValid()) {
        return false;
    }
    std::memcpy(cb.cpu, &constants, sizeof(constants));

    PIXBeginEvent(commandList, PIX_COLOR(200, 200, 120), "CompositeMaskOp");
    // 画像やノイズは合成の Height を触らないが、下地から作る op は読むので
    // SRV にしておく。**種類はここ 1 か所で判定する**（傾斜だけを見ていて
    // 曲率が漏れていた）。
    const bool readsHeight = (op.kind == MaskOpKind::Slope) ||
                             (op.kind == MaskOpKind::Curvature) ||
                             (op.kind == MaskOpKind::Height);
    if (readsHeight) {
        TransitionIfNeeded(commandList, m_textures.height,
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    TransitionIfNeeded(commandList, target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    commandList->SetComputeRootConstantBufferView(1, cb.gpuAddress);

    // 標高マスクの「全範囲」は、地形の最低 / 最高を先に集計してから焼く。
    if (op.kind == MaskOpKind::Height && constants.params0[1] != 0u) {
        const auto rangePass = [&](const wchar_t* entry) {
            return pipelineCache.GetCompute(L"CompositeMaskOps.hlsl", entry);
        };
        ID3D12PipelineState* clearPass = rangePass(L"CsHeightRangeClear");
        ID3D12PipelineState* reducePass = rangePass(L"CsHeightRangeReduce");
        if (clearPass == nullptr || reducePass == nullptr) {
            PIXEndEvent(commandList);
            return false;
        }
        const D3D12_RESOURCE_BARRIER uav = CD3DX12_RESOURCE_BARRIER::UAV(nullptr);
        commandList->SetPipelineState(clearPass);
        commandList->Dispatch(1, 1, 1);
        commandList->ResourceBarrier(1, &uav);
        commandList->SetPipelineState(reducePass);
        commandList->Dispatch(DispatchCount(resolution), DispatchCount(resolution), 1);
        commandList->ResourceBarrier(1, &uav);
    }

    commandList->SetPipelineState(pipeline);
    commandList->Dispatch(DispatchCount(resolution), DispatchCount(resolution), 1);

    // 下流の op と合成パスは SRV で読む。Height は次のレイヤーが書き換える。
    TransitionIfNeeded(commandList, target, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (readsHeight) {
        TransitionIfNeeded(commandList, m_textures.height, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    PIXEndEvent(commandList);
    return true;
}

// 標高マスクの「全範囲」で使う 1 枚。2 テクセルしか使わないが、
// テクスチャの作成は正方形しか用意していないので 2x2 で取る。
bool MaterialEvaluator::EnsureMaskHeightRange(rhi::Device& device) {
    if (m_maskHeightRange.IsValid()) {
        return true;
    }
    if (!CreateChannelTexture(device, 2, DXGI_FORMAT_R32_UINT, L"MaskHeightRange",
                              m_maskHeightRange)) {
        TG_LOG_WARN("標高マスクの集計用テクスチャを作れませんでした");
        return false;
    }
    return true;
}

// パスの足跡。線分は kMaxPathSegments 本ずつ流し、2 回目以降は前の結果と max を取る。
// ガンマと反転は最後のバッチだけに掛ける（途中で掛けると max の意味が変わる）。
bool MaterialEvaluator::ApplyPathMask(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                                      ID3D12GraphicsCommandList* commandList, const MaskOp& op,
                                      const MaterialStack& stack, rhi::GpuTexture& target) {
    if (!target.IsValid()) {
        return false;
    }
    ID3D12PipelineState* pipeline =
        pipelineCache.GetCompute(L"CompositeMaskPath.hlsl", L"CsPath");
    if (pipeline == nullptr) {
        return false;
    }
    const uint32_t resolution = m_resolution;
    const float sizeMeters = (stack.SizeMeters() > 0.0f) ? stack.SizeMeters() : 1.0f;
    const size_t total = op.pathSegments.size();
    const size_t batchCount =
        std::max<size_t>(1, (total + kMaxPathSegments - 1) / kMaxPathSegments);

    PIXBeginEvent(commandList, PIX_COLOR(170, 150, 220), "CompositeMaskPath");
    TransitionIfNeeded(commandList, target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    commandList->SetPipelineState(pipeline);

    for (size_t batch = 0; batch < batchCount; ++batch) {
        const size_t begin = batch * kMaxPathSegments;
        const size_t end = std::min(total, begin + kMaxPathSegments);
        const bool last = (batch + 1 == batchCount);

        const rhi::UploadAllocation cb = AllocateConstants(device, sizeof(PathMaskConstants));
        if (!cb.IsValid()) {
            PIXEndEvent(commandList);
            return false;
        }
        // 線分の配列は大きいので、定数は置き場の上で直接組む（スタックに 12 KB 積まない）。
        auto* constants = static_cast<PathMaskConstants*>(cb.cpu);
        constants->indices[0] = target.UavIndex();
        constants->indices[1] = resolution;
        constants->indices[2] = static_cast<uint32_t>(end - begin);
        constants->indices[3] = (batch == 0) ? 0u : 1u;
        constants->params[0] = sizeMeters;
        constants->params[1] = last ? std::clamp(op.pathMask.gamma, 0.05f, 8.0f) : 1.0f;
        constants->params[2] = (last && op.pathMask.invert) ? 1.0f : 0.0f;
        constants->params[3] = 0.0f;
        for (size_t i = begin; i < end; ++i) {
            const PathSegment& segment = op.pathSegments[i];
            float* slot = constants->segments[(i - begin) * 3];
            slot[0] = segment.ax;
            slot[1] = segment.ay;
            slot[2] = segment.bx;
            slot[3] = segment.by;
            slot[4] = segment.widthA;
            slot[5] = segment.widthB;
            slot[6] = segment.featherA;
            slot[7] = segment.featherB;
            slot[8] = segment.intensityA;
            slot[9] = segment.intensityB;
            slot[10] = 0.0f;
            slot[11] = 0.0f;
        }

        commandList->SetComputeRootConstantBufferView(1, cb.gpuAddress);
        commandList->Dispatch(DispatchCount(resolution), DispatchCount(resolution), 1);
        // 次のバッチは前の結果を読む。
        const D3D12_RESOURCE_BARRIER uav = CD3DX12_RESOURCE_BARRIER::UAV(target.resource.Get());
        commandList->ResourceBarrier(1, &uav);
    }

    TransitionIfNeeded(commandList, target, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    PIXEndEvent(commandList);
    return true;
}

// --- CPU 側のハイト ------------------------------------------------------------

void MaterialEvaluator::ReleaseHeightfieldResources(rhi::Device& device) {
    device.DeferRelease(m_heightfieldTexture);
    device.DeferRelease(m_heightfieldReadback);
    m_heightfieldReadbackBytes = 0;
    m_heightfieldRowPitch = 0;
    m_heightfieldPending = false;
    m_heightfieldFence = nullptr;
    m_heightfieldFenceValue = 0;
}

bool MaterialEvaluator::EnsureHeightfieldResources(rhi::Device& device) {
    if (m_heightfieldTexture.IsValid() && m_heightfieldReadback.IsValid()) {
        return true;
    }
    ReleaseHeightfieldResources(device);
    if (!CreateChannelTexture(device, kHeightfieldReadbackResolution, DXGI_FORMAT_R32_FLOAT,
                              L"HeightfieldReadbackTexture", m_heightfieldTexture)) {
        return false;
    }
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT rowCount = 0;
    UINT64 rowBytes = 0;
    UINT64 totalBytes = 0;
    const D3D12_RESOURCE_DESC desc = m_heightfieldTexture.resource->GetDesc();
    device.GetDevice()->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rowCount, &rowBytes,
                                              &totalBytes);
    if (!device.Allocator().CreateReadbackBuffer(totalBytes, L"HeightfieldReadback",
                                                 m_heightfieldReadback)) {
        ReleaseHeightfieldResources(device);
        return false;
    }
    m_heightfieldReadbackBytes = totalBytes;
    m_heightfieldRowPitch = footprint.Footprint.RowPitch;
    return true;
}

// 評価の末尾で呼ぶ。Height を縮小し、読み戻しバッファへコピーする。
// コンピュートキューでも記録できる操作だけを使う（COPY_SOURCE への遷移とコピー）。
void MaterialEvaluator::RecordHeightfieldReadback(rhi::Device& device,
                                                  rhi::PipelineCache& pipelineCache,
                                                  ID3D12GraphicsCommandList* commandList) {
    // 前回の読み戻しをまだ写していなくても、今回で上書きしてよい
    // （評価は 1 本ずつ流れるので、写す時点で最新の結果が入っている）。
    if (!EnsureHeightfieldResources(device)) {
        return;
    }
    ID3D12PipelineState* pipeline =
        pipelineCache.GetCompute(L"CompositeBlur.hlsl", L"CsDownsample");
    if (pipeline == nullptr) {
        return;
    }
    BlurConstants constants = {};
    constants.sourceIndex = m_textures.height.SrvIndex();
    constants.outputIndex = m_heightfieldTexture.UavIndex();
    constants.resolution[0] = kHeightfieldReadbackResolution;
    constants.resolution[1] = kHeightfieldReadbackResolution;
    const rhi::UploadAllocation cb = AllocateConstants(device, sizeof(BlurConstants));
    if (!cb.IsValid()) {
        return;
    }
    std::memcpy(cb.cpu, &constants, sizeof(constants));

    PIXBeginEvent(commandList, PIX_COLOR(120, 140, 160), "HeightfieldReadback");
    TransitionIfNeeded(commandList, m_textures.height,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    TransitionIfNeeded(commandList, m_heightfieldTexture, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    commandList->SetPipelineState(pipeline);
    commandList->SetComputeRootConstantBufferView(1, cb.gpuAddress);
    commandList->Dispatch(DispatchCount(kHeightfieldReadbackResolution),
                          DispatchCount(kHeightfieldReadbackResolution), 1);
    TransitionIfNeeded(commandList, m_heightfieldTexture, D3D12_RESOURCE_STATE_COPY_SOURCE);

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    const D3D12_RESOURCE_DESC desc = m_heightfieldTexture.resource->GetDesc();
    device.GetDevice()->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr,
                                              nullptr);
    const CD3DX12_TEXTURE_COPY_LOCATION destination(m_heightfieldReadback.resource.Get(),
                                                    footprint);
    const CD3DX12_TEXTURE_COPY_LOCATION source(m_heightfieldTexture.resource.Get(), 0);
    commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    PIXEndEvent(commandList);

    // 完了を待つフェンス。非同期ならコンピュートキューの次の値、同期ならこのフレームの値。
    if (m_recordingAsync) {
        m_heightfieldFence = m_compute.Fence();
        m_heightfieldFenceValue = m_compute.SubmittedValue() + 1;
    } else {
        m_heightfieldFence = device.FrameFence();
        m_heightfieldFenceValue = device.NextFenceValue();
    }
    m_heightfieldPending = true;
}

void MaterialEvaluator::CollectHeightfieldReadback() {
    if (!m_heightfieldPending || m_heightfieldFence == nullptr ||
        !m_heightfieldReadback.IsValid()) {
        return;
    }
    if (m_heightfieldFence->GetCompletedValue() < m_heightfieldFenceValue) {
        return;
    }
    m_heightfieldPending = false;
    void* mapped = nullptr;
    const D3D12_RANGE readRange = {0, static_cast<SIZE_T>(m_heightfieldReadbackBytes)};
    if (!TG_CHECK_HR(m_heightfieldReadback.resource->Map(0, &readRange, &mapped))) {
        return;
    }
    const uint32_t resolution = kHeightfieldReadbackResolution;
    m_heightfield.resolution = resolution;
    m_heightfield.values.resize(static_cast<size_t>(resolution) * resolution);
    const auto* base = static_cast<const uint8_t*>(mapped);
    for (uint32_t y = 0; y < resolution; ++y) {
        std::memcpy(m_heightfield.values.data() + static_cast<size_t>(y) * resolution,
                    base + static_cast<size_t>(y) * m_heightfieldRowPitch,
                    sizeof(float) * resolution);
    }
    const D3D12_RANGE writtenRange = {0, 0};
    m_heightfieldReadback.resource->Unmap(0, &writtenRange);
}

// マスクをぼかす（terrain-editor の Mask Blur の移植）。
//
//   種入れ: 入力のマスクを結果へ写す
//   反復 × { 水平: 結果 → 作業用 / 垂直: 作業用 → 結果（強さで混ぜる） }
//
// **あちらは箱ぼかしの繰り返しだが、こちらは分離型ガウス。**
// ハイトのぼかし（ApplyHeightBlur）と同じ「半径（m）/ 強さ / 反復」で
// 同じ効き方をするほうが、2 つのぼかしを行き来したときに戸惑わない。
//
// **作業用は m_scratch を借りる。** ハイトのぼかしと同時には走らないので、
// 専用の 1 枚を増やす必要がない。
//
// 種入れを挟むのは、反復が「結果を読んで結果へ書く」形で回るため。
// 入力の op は解像度が違うことがある（川筋は自前のグリッド）ので、
// 写すときだけ UV で引いて、以降は同じ解像度どうしで回す。
bool MaterialEvaluator::ApplyMaskBlur(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                                      ID3D12GraphicsCommandList* commandList, const MaskOp& op,
                                      const MaterialStack& stack, uint32_t resolution,
                                      rhi::GpuTexture& target) {
    // 作業用は合成解像度で取ってある。ぼかしの op も合成解像度で焼くので必ず一致する。
    if (!target.IsValid() || !m_scratch.IsValid() || resolution != m_resolution ||
        resolution == 0) {
        return false;
    }
    const auto pipeline = [&](const wchar_t* entry) {
        return pipelineCache.GetCompute(L"CompositeMaskOps.hlsl", entry);
    };
    ID3D12PipelineState* seedPass = pipeline(L"CsMaskBlurSeed");
    ID3D12PipelineState* blurPass = pipeline(L"CsMaskBlur");
    if (seedPass == nullptr || blurPass == nullptr) {
        return false;
    }

    rhi::GpuTexture* input = nullptr;
    if (op.inputA >= 0 && static_cast<size_t>(op.inputA) < m_maskOpTextures.size() &&
        m_maskOpTextures[static_cast<size_t>(op.inputA)].IsValid()) {
        input = &m_maskOpTextures[static_cast<size_t>(op.inputA)];
    }
    const uint32_t inputIndex = (input != nullptr) ? input->SrvIndex() : kInvalidTextureIndex;

    // 半径は実寸（m）。合成解像度を変えても効きが変わらないようテクセルへ直す。
    const float sizeMeters = (stack.SizeMeters() > 0.0f) ? stack.SizeMeters() : 1.0f;
    const float radiusTexels =
        op.blur.radiusMeters / sizeMeters * static_cast<float>(resolution);
    const float strength = std::clamp(op.blur.strength, 0.0f, 1.0f);
    const int iterations = std::clamp(op.blur.iterations, 1, 16);
    // テクセル 1 つに満たない半径や強さ 0 は何も変えない。種入れだけで終える
    // （ノードを素通りさせる。ここで失敗にすると下流が定数へ落ちてしまう）。
    const bool passthrough = (radiusTexels < 0.5f) || (strength <= 0.0f);

    const auto dispatch = [&](ID3D12PipelineState* pass, uint32_t outputIndex,
                              uint32_t sourceIndex, uint32_t axis) {
        MaskOpConstants constants = {};
        constants.indices[0] = outputIndex;
        constants.indices[1] = sourceIndex;
        constants.indices[2] = kInvalidTextureIndex;
        constants.indices[3] = kInvalidTextureIndex;
        constants.params0[0] = resolution;
        constants.params0[1] = axis;
        constants.params2[0] = radiusTexels;
        constants.params2[1] = strength;

        const rhi::UploadAllocation cb = AllocateConstants(device, sizeof(MaskOpConstants));
        if (!cb.IsValid()) {
            return false;
        }
        std::memcpy(cb.cpu, &constants, sizeof(constants));
        commandList->SetComputeRootConstantBufferView(1, cb.gpuAddress);
        commandList->SetPipelineState(pass);
        commandList->Dispatch(DispatchCount(resolution), DispatchCount(resolution), 1);
        return true;
    };

    PIXBeginEvent(commandList, PIX_COLOR(200, 180, 120), "CompositeMaskBlur");

    if (input != nullptr) {
        TransitionIfNeeded(commandList, *input, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    TransitionIfNeeded(commandList, target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    bool complete = dispatch(seedPass, target.UavIndex(), inputIndex, 0u);

    for (int iteration = 0; complete && !passthrough && iteration < iterations; ++iteration) {
        // 水平: 結果（読み取り）→ 作業用。中間結果なので混ぜない。
        TransitionIfNeeded(commandList, target, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        TransitionIfNeeded(commandList, m_scratch, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        complete = dispatch(blurPass, m_scratch.UavIndex(), target.SrvIndex(), 0u);
        // 垂直: 作業用（読み取り）→ 結果。ここで元のマスクと強さで混ぜる。
        TransitionIfNeeded(commandList, m_scratch,
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        TransitionIfNeeded(commandList, target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        complete = complete && dispatch(blurPass, target.UavIndex(), m_scratch.SrvIndex(), 1u);
    }

    // 下流の op と合成パスは SRV で読む。
    TransitionIfNeeded(commandList, target, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    PIXEndEvent(commandList);
    return complete;
}

bool MaterialEvaluator::ApplyFluvialMask(rhi::Device& device,
                                        rhi::PipelineCache& pipelineCache,
                                        ID3D12GraphicsCommandList* commandList, const MaskOp& op,
                                        const MaterialStack& stack, rhi::GpuTexture& target) {
    const FluvialParams& params = op.fluvial;
    const uint32_t resolution = std::clamp(params.resolution, 64u, 2048u);
    if (!target.IsValid() || !EnsureFluvialResources(device, resolution)) {
        return false;
    }

    const auto pipeline = [&](const wchar_t* entry) {
        return pipelineCache.GetCompute(L"CompositeFluvial.hlsl", entry);
    };
    ID3D12PipelineState* samplePass = pipeline(L"CsSampleHeight");
    ID3D12PipelineState* blurHPass = pipeline(L"CsBlurH");
    ID3D12PipelineState* blurVPass = pipeline(L"CsBlurV");
    ID3D12PipelineState* pitFillPass = pipeline(L"CsPitFill");
    ID3D12PipelineState* commitPass = pipeline(L"CsCommit");
    ID3D12PipelineState* weightsPass = pipeline(L"CsWeights");
    ID3D12PipelineState* accumInitPass = pipeline(L"CsAccumInit");
    ID3D12PipelineState* accumIterPass = pipeline(L"CsAccumIter");
    ID3D12PipelineState* maxClearPass = pipeline(L"CsMaxClear");
    ID3D12PipelineState* maxReducePass = pipeline(L"CsMaxReduce");
    ID3D12PipelineState* toMaskPass = pipeline(L"CsToMask");
    if (samplePass == nullptr || blurHPass == nullptr || blurVPass == nullptr ||
        pitFillPass == nullptr || commitPass == nullptr || weightsPass == nullptr ||
        accumInitPass == nullptr || accumIterPass == nullptr || maxClearPass == nullptr ||
        maxReducePass == nullptr || toMaskPass == nullptr) {
        return false;
    }

    FluvialConstants constants = {};
    constants.indices0[0] = m_fluvial.heights.UavIndex();
    constants.indices0[1] = m_fluvial.heightsScratch.UavIndex();
    constants.indices0[2] = m_fluvial.weights0.UavIndex();
    constants.indices0[3] = m_fluvial.weights1.UavIndex();
    constants.indices1[0] = m_fluvial.accumA.UavIndex();
    constants.indices1[1] = m_fluvial.accumB.UavIndex();
    constants.indices1[2] = target.UavIndex();
    constants.indices1[3] = m_fluvial.maxScratch.UavIndex();
    constants.indices2[0] = m_textures.height.SrvIndex();
    constants.indices2[1] = resolution;
    constants.indices2[2] = 0;

    // 「最大ディテール」（m）をセル数へ直す。1 セルが何 m かは地形の一辺で決まる。
    const float sizeMeters = (stack.SizeMeters() > 0.0f) ? stack.SizeMeters() : 1.0f;
    const float cellMeters = sizeMeters / static_cast<float>(std::max(1u, resolution - 1u));
    const float detailMeters =
        std::clamp(params.detailMeters, cellMeters, sizeMeters * 0.5f);
    constants.indices2[3] = static_cast<uint32_t>(
        std::clamp(static_cast<int>(std::lround(detailMeters / cellMeters)), 1, 64));

    const float cellCount = static_cast<float>(resolution) * static_cast<float>(resolution);
    constants.params0[0] = std::clamp(params.concentration, 0.1f, 16.0f);
    // しきい値は割合で持っているのでセル数へ直す。
    constants.params0[1] = std::clamp(params.threshold, 0.0f, 1.0f) * cellCount;
    constants.params0[2] = std::clamp(params.gamma, 0.05f, 8.0f);
    constants.params0[3] = std::clamp(params.softness, 0.001f, 4.0f);
    constants.params1[0] = std::clamp(params.edgePower, 0.1f, 8.0f);
    constants.params1[1] = static_cast<float>(params.curve);

    // 定数はヤコビの向き以外変わらないので、**2 本だけ**確保して使い回す。
    // 反復のたびに確保すると、アップロードリングを 1 フレームで食い潰す。
    const auto upload = [&](uint32_t direction, D3D12_GPU_VIRTUAL_ADDRESS& outAddress) {
        FluvialConstants copy = constants;
        copy.indices2[2] = direction;
        const rhi::UploadAllocation cb = AllocateConstants(device, sizeof(FluvialConstants));
        if (!cb.IsValid()) {
            return false;
        }
        std::memcpy(cb.cpu, &copy, sizeof(copy));
        outAddress = cb.gpuAddress;
        return true;
    };
    D3D12_GPU_VIRTUAL_ADDRESS constantsA = 0;
    D3D12_GPU_VIRTUAL_ADDRESS constantsB = 0;
    if (!upload(0u, constantsA) || !upload(1u, constantsB)) {
        return false;
    }

    PIXBeginEvent(commandList, PIX_COLOR(80, 140, 200), "CompositeFluvial");

    // 入力の Height は読み取り専用に、出力のマスクは書き込みに。
    TransitionIfNeeded(commandList, m_textures.height,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    TransitionIfNeeded(commandList, target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    const uint32_t groups = DispatchCount(resolution);
    const auto barrier = [&]() {
        const D3D12_RESOURCE_BARRIER uav = CD3DX12_RESOURCE_BARRIER::UAV(nullptr);
        commandList->ResourceBarrier(1, &uav);
    };
    const auto run = [&](ID3D12PipelineState* pipelineState, uint32_t groupCount) {
        commandList->SetPipelineState(pipelineState);
        commandList->Dispatch(groupCount, groupCount, 1);
        barrier();
    };

    commandList->SetComputeRootConstantBufferView(1, constantsA);
    run(samplePass, groups);

    // 「最大ディテール」が 1 セル以下ならぼかす意味がない。
    if (constants.indices2[3] > 1u) {
        run(blurHPass, groups);
        run(blurVPass, groups);
    }

    // 窪み埋め。反復回数は固定（terrain-editor と同じ 8 回）。
    // 上げても消えるのは深い窪みだけで、川筋の形はほとんど変わらない。
    constexpr int kPitFillIterations = 8;
    for (int i = 0; i < kPitFillIterations; ++i) {
        run(pitFillPass, groups);
        run(commitPass, groups);
    }

    run(weightsPass, groups);
    run(accumInitPass, groups);

    // ヤコビ反復。情報は 1 反復につき 1 セルずつ下流へ進むので、
    // 収束に要るのはおおよそ最長流路長。安全側で 2 × 解像度 回まわす。
    // **偶数回まわすと結果は AccumA に残る**（マスク変換はそこを読む）。
    const int accumIterations = static_cast<int>(resolution) * 2;
    for (int i = 0; i < accumIterations; ++i) {
        commandList->SetComputeRootConstantBufferView(1, (i % 2 == 0) ? constantsA : constantsB);
        run(accumIterPass, groups);
    }
    commandList->SetComputeRootConstantBufferView(1, constantsA);

    // しきい値カーブは正規化しないので、最大値の集計は要らない。
    if (params.curve != FluvialCurve::Threshold) {
        run(maxClearPass, 1);
        run(maxReducePass, groups);
    }
    run(toMaskPass, groups);

    // 下流の op と合成パスはマスクを SRV で読む。Height は次のレイヤーが書き換える。
    TransitionIfNeeded(commandList, target, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    TransitionIfNeeded(commandList, m_textures.height, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    PIXEndEvent(commandList);
    return true;
}

void MaterialEvaluator::ReleaseCrumblingResources(rhi::Device& device) {
    device.DeferRelease(m_crumbling.packed);
    m_crumbling.resolution = 0;
}

bool MaterialEvaluator::EnsureCrumblingResources(rhi::Device& device, uint32_t resolution) {
    if (m_crumbling.IsValid() && m_crumbling.resolution == resolution) {
        return true;
    }
    ReleaseCrumblingResources(device);
    if (!CreateChannelTexture(device, resolution, DXGI_FORMAT_R32_UINT, L"CrumblingPacked",
                              m_crumbling.packed)) {
        ReleaseCrumblingResources(device);
        return false;
    }
    m_crumbling.resolution = resolution;
    return true;
}

void MaterialEvaluator::ReleaseScatterResources(rhi::Device& device) {
    device.DeferRelease(m_scatter.packed);
    device.DeferRelease(m_scatter.delta);
    m_scatter.resolution = 0;
}

bool MaterialEvaluator::EnsureScatterResources(rhi::Device& device, uint32_t resolution) {
    if (m_scatter.IsValid() && m_scatter.resolution == resolution) {
        return true;
    }
    ReleaseScatterResources(device);
    if (!CreateChannelTexture(device, resolution, DXGI_FORMAT_R32_UINT, L"ScatterPacked",
                              m_scatter.packed) ||
        !CreateChannelTexture(device, resolution, DXGI_FORMAT_R32_FLOAT, L"ScatterDelta",
                              m_scatter.delta)) {
        ReleaseScatterResources(device);
        return false;
    }
    m_scatter.resolution = resolution;
    return true;
}

void MaterialEvaluator::ReleaseSedimentResources(rhi::Device& device) {
    device.DeferRelease(m_sediment.bedrock);
    device.DeferRelease(m_sediment.sediment);
    device.DeferRelease(m_sediment.outgoing);
    device.DeferRelease(m_sediment.original);
    device.DeferRelease(m_sediment.maxScratch);
    m_sediment.resolution = 0;
}

// 堆積の作業リソースは**使うときだけ**作る。グリッドが変わったら作り直す。
bool MaterialEvaluator::EnsureSedimentResources(rhi::Device& device, uint32_t resolution) {
    if (m_sediment.resolution == resolution && m_sediment.IsValid()) {
        return true;
    }
    ReleaseSedimentResources(device);

    const bool ok =
        CreateChannelTexture(device, resolution, DXGI_FORMAT_R32_FLOAT, L"SedimentBedrock",
                             m_sediment.bedrock) &&
        CreateChannelTexture(device, resolution, DXGI_FORMAT_R32_FLOAT, L"SedimentSediment",
                             m_sediment.sediment) &&
        CreateChannelTexture(device, resolution, DXGI_FORMAT_R32G32B32A32_FLOAT,
                             L"SedimentOutgoing", m_sediment.outgoing) &&
        CreateChannelTexture(device, resolution, DXGI_FORMAT_R32_FLOAT, L"SedimentOriginal",
                             m_sediment.original) &&
        CreateChannelTexture(device, 1, DXGI_FORMAT_R32_UINT, L"SedimentMax",
                             m_sediment.maxScratch);
    if (!ok) {
        TG_LOG_WARN("堆積の作業リソースを作れませんでした（%u^2）", resolution);
        ReleaseSedimentResources(device);
        return false;
    }
    m_sediment.resolution = resolution;
    return true;
}

// ぼかし / 堆積で Height を書き換えたあと、そこから法線を作り直す。
// **Height だけを変えると形と陰影が食い違う。**
void MaterialEvaluator::RebuildNormalsFromHeight(rhi::Device& device,
                                                 ID3D12PipelineState* pipeline,
                                                 ID3D12GraphicsCommandList* commandList,
                                                 const MaterialStack& stack) {
    if (pipeline == nullptr) {
        return;
    }
    BlurConstants constants = {};
    constants.sourceIndex = m_textures.height.SrvIndex();
    constants.outputIndex = m_textures.normal.UavIndex();
    constants.axis = 0;
    constants.radiusTexels = 1.0f;
    constants.tile[0] = 0;
    constants.tile[1] = 0;
    constants.tile[2] = m_resolution;
    constants.tile[3] = m_resolution;
    constants.resolution[0] = m_resolution;
    constants.resolution[1] = m_resolution;
    constants.strength = 1.0f;
    constants.heightPerSize = (stack.SizeMeters() > 0.0f)
                                  ? (stack.HeightMeters() / stack.SizeMeters())
                                  : 0.0f;

    const rhi::UploadAllocation cb = AllocateConstants(device, sizeof(BlurConstants));
    if (!cb.IsValid()) {
        return;
    }
    std::memcpy(cb.cpu, &constants, sizeof(constants));

    TransitionIfNeeded(commandList, m_textures.height,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    commandList->SetPipelineState(pipeline);
    commandList->SetComputeRootConstantBufferView(1, cb.gpuAddress);
    commandList->Dispatch(DispatchCount(m_resolution), DispatchCount(m_resolution), 1);
    TransitionIfNeeded(commandList, m_textures.height, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    const D3D12_RESOURCE_BARRIER barriers[] = {
        CD3DX12_RESOURCE_BARRIER::UAV(m_textures.normal.resource.Get()),
    };
    commandList->ResourceBarrier(_countof(barriers), barriers);
}

// 堆積。terrain-editor の Sediment を移植したもの。
//
//   setup →（反復ごとに）供給 → 滑らせ（掃引 1 / 掃引 2）を macro × 安定化 回
//        → 差分を Height へ足し戻す → 法線を作り直す
//
// **合成解像度とは別のグリッド**で回し、動いたぶんだけを足し戻すので、
// 合成解像度の細部は残る。
bool MaterialEvaluator::ApplySediment(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                                      ID3D12GraphicsCommandList* commandList,
                                      const MaterialLayer& layer, const MaterialStack& stack) {
    const MaterialLayer::SedimentSettings& params = layer.sediment;
    const uint32_t resolution = std::clamp(params.resolution, 64u, 2048u);
    if (!EnsureSedimentResources(device, resolution)) {
        return false;
    }

    const auto pipeline = [&](const wchar_t* entry) {
        return pipelineCache.GetCompute(L"CompositeSediment.hlsl", entry);
    };
    ID3D12PipelineState* setupPass = pipeline(L"CsSetup");
    ID3D12PipelineState* emitPass = pipeline(L"CsEmit");
    ID3D12PipelineState* sweep1Pass = pipeline(L"CsSweep1");
    ID3D12PipelineState* sweep2Pass = pipeline(L"CsSweep2");
    ID3D12PipelineState* applyPass = pipeline(L"CsApply");
    ID3D12PipelineState* normalPass =
        pipelineCache.GetCompute(L"CompositeBlur.hlsl", L"CsNormalFromHeight");
    if (setupPass == nullptr || emitPass == nullptr || sweep1Pass == nullptr ||
        sweep2Pass == nullptr || applyPass == nullptr) {
        return false;
    }

    // --- パラメータを正規化ハイトの単位へ直す ------------------------------
    // ハイトは 0〜1 で、その全幅が標高差（m）。安息角も供給量も m で持っているので、
    // ここで割って正規化ハイトへ揃える。
    const float sizeMeters = (stack.SizeMeters() > 0.0f) ? stack.SizeMeters() : 1.0f;
    const float heightMeters = (stack.HeightMeters() > 0.0f) ? stack.HeightMeters() : 1.0f;
    const float cellMeters = sizeMeters / static_cast<float>(std::max(1u, resolution - 1u));

    // 粘性は角度の二乗カーブ（0% で 0 度、20% で約 3 度、100% で 80 度）。
    const float viscosity = std::clamp(params.viscosity, 0.0f, 1.0f);
    const float talusDegrees = viscosity * viscosity * 80.0f;
    const float talus =
        std::tan(talusDegrees * 3.14159265358979323846f / 180.0f) * cellMeters / heightMeters;

    const int iterations = std::clamp(params.iterations, 1, 200);
    const int stabilization = std::clamp(params.stabilization, 1, 8);
    // 「1 反復あたりの沈降距離」はセル数（＝滑らせる回数）に直す。
    const int macroPasses = std::clamp(
        static_cast<int>(std::ceil(params.detailMeters / std::max(cellMeters, 1e-6f))), 1, 64);
    const int emissionEnd =
        std::max(1, static_cast<int>(std::ceil(static_cast<float>(iterations) *
                                               std::clamp(params.emissionTime, 0.0f, 1.0f))));
    const float emissionPerIteration =
        (params.emissionMeters / heightMeters) / static_cast<float>(emissionEnd);

    SedimentConstants constants = {};
    constants.indices0[0] = m_sediment.bedrock.UavIndex();
    constants.indices0[1] = m_sediment.sediment.UavIndex();
    constants.indices0[2] = m_sediment.outgoing.UavIndex();
    constants.indices0[3] = m_sediment.original.UavIndex();
    constants.indices1[0] = m_sediment.bedrock.SrvIndex();
    constants.indices1[1] = m_sediment.sediment.SrvIndex();
    constants.indices1[2] = m_sediment.original.SrvIndex();
    constants.indices1[3] = m_textures.height.SrvIndex();
    constants.indices2[0] = resolution;
    constants.indices2[1] = params.convertTerrain ? 1u : 0u;
    constants.indices2[2] = m_textures.height.UavIndex();
    constants.indices2[3] = m_resolution;
    constants.indices3[0] = m_sediment.maxScratch.UavIndex();
    constants.params[0] = talus;

    // 定数は「供給あり / なし」の 2 本だけ確保して使い回す。
    // 反復のたびに確保すると、アップロードリングを 1 フレームで食い潰す。
    const auto upload = [&](float emission, D3D12_GPU_VIRTUAL_ADDRESS& outAddress) {
        SedimentConstants copy = constants;
        copy.params[1] = emission;
        const rhi::UploadAllocation cb = AllocateConstants(device, sizeof(SedimentConstants));
        if (!cb.IsValid()) {
            return false;
        }
        std::memcpy(cb.cpu, &copy, sizeof(copy));
        outAddress = cb.gpuAddress;
        return true;
    };
    D3D12_GPU_VIRTUAL_ADDRESS slideConstants = 0;
    D3D12_GPU_VIRTUAL_ADDRESS emitConstants = 0;
    if (!upload(0.0f, slideConstants) || !upload(emissionPerIteration, emitConstants)) {
        return false;
    }

    PIXBeginEvent(commandList, PIX_COLOR(180, 150, 110), "CompositeSediment");

    // 入力の Height は読み取り専用（setup が読む）。
    TransitionIfNeeded(commandList, m_textures.height,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    const uint32_t groups = DispatchCount(resolution);
    const auto barrier = [&]() {
        const D3D12_RESOURCE_BARRIER uav = CD3DX12_RESOURCE_BARRIER::UAV(nullptr);
        commandList->ResourceBarrier(1, &uav);
    };
    const auto run = [&](ID3D12PipelineState* pipelineState, uint32_t groupCount) {
        commandList->SetPipelineState(pipelineState);
        commandList->Dispatch(groupCount, groupCount, 1);
        barrier();
    };

    commandList->SetComputeRootConstantBufferView(1, slideConstants);
    run(setupPass, groups);

    for (int iteration = 0; iteration < iterations; ++iteration) {
        if (iteration < emissionEnd && emissionPerIteration > 0.0f) {
            commandList->SetComputeRootConstantBufferView(1, emitConstants);
            run(emitPass, groups);
            commandList->SetComputeRootConstantBufferView(1, slideConstants);
        }
        // 粗いスケールから細かいスケールまで、同じ滑らせを繰り返す。
        for (int pass = 0; pass < macroPasses * stabilization; ++pass) {
            run(sweep1Pass, groups);
            run(sweep2Pass, groups);
        }
    }

    // 動いたぶんを合成の Height へ足し戻す。ここだけ合成解像度で回す。
    // **Mask だけが目的のときは足し戻さない**（Result を繋いでいない）。
    // 厚みは作業用テクスチャに残るので、マスクはこの後で焼ける。
    if (!layer.maskOnly) {
        TransitionIfNeeded(commandList, m_sediment.bedrock,
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        TransitionIfNeeded(commandList, m_sediment.sediment,
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        TransitionIfNeeded(commandList, m_sediment.original,
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        TransitionIfNeeded(commandList, m_textures.height,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        run(applyPass, DispatchCount(m_resolution));

        // 次に使うときは書き込みへ戻す。
        TransitionIfNeeded(commandList, m_sediment.bedrock,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        TransitionIfNeeded(commandList, m_sediment.sediment,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        TransitionIfNeeded(commandList, m_sediment.original,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    // 次のレイヤーは Height を UAV として書く。**Mask だけのときも戻す**
    // （setup が読むために SRV へ遷移させたまま渡すと、GPU ベースバリデーションが
    // 次の合成パスの UAV 書き込みを「レイアウト不一致」として報告する）。
    TransitionIfNeeded(commandList, m_textures.height, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    PIXEndEvent(commandList);

    // 形が変わったので、法線も作り直す。足し戻していないなら形は変わっていない。
    if (!layer.maskOnly) {
        RebuildNormalsFromHeight(device, normalPass, commandList, stack);
    }
    return true;
}

// 直前の堆積レイヤーが残した厚みを、マスクとして焼く。
//
// **堆積レイヤーを合成し終えた直後にしか呼ばない**（作業用テクスチャは
// 次の堆積レイヤーで上書きされるため）。段取りは op の heightSourceLayer が
// 決めていて、ちょうどその位置で走る。
bool MaterialEvaluator::ApplySedimentMask(rhi::Device& device,
                                          rhi::PipelineCache& pipelineCache,
                                          ID3D12GraphicsCommandList* commandList,
                                          const MaskOp& op, const MaterialStack& stack,
                                          rhi::GpuTexture& target) {
    if (!m_sediment.IsValid() || !target.IsValid()) {
        return false;
    }
    const auto pipeline = [&](const wchar_t* entry) {
        return pipelineCache.GetCompute(L"CompositeSediment.hlsl", entry);
    };
    ID3D12PipelineState* clearPass = pipeline(L"CsMaskMaxClear");
    ID3D12PipelineState* reducePass = pipeline(L"CsMaskMaxReduce");
    ID3D12PipelineState* maskPass = pipeline(L"CsMask");
    if (clearPass == nullptr || reducePass == nullptr || maskPass == nullptr) {
        return false;
    }

    SedimentConstants constants = {};
    // 厚みは「基盤 + 土砂 − 元の高さ」なので 3 枚とも読む。
    constants.indices1[0] = m_sediment.bedrock.SrvIndex();
    constants.indices1[1] = m_sediment.sediment.SrvIndex();
    constants.indices1[2] = m_sediment.original.SrvIndex();
    constants.indices2[0] = m_sediment.resolution;
    constants.indices2[3] = m_resolution;
    constants.indices3[0] = m_sediment.maxScratch.UavIndex();
    constants.indices3[1] = target.UavIndex();
    constants.params[2] = std::clamp(op.sedimentMask.contrast, 0.0f, 1.0f);
    // 基準の厚みは実寸（m）で持つ。ハイト 0〜1 の全幅が標高差なので、その比へ直す。
    // 0（＝一番厚い所で正規化）はそのまま 0 で渡す。
    const float heightMeters = stack.HeightMeters();
    constants.params[3] = (op.sedimentMask.thicknessMeters > 0.0f && heightMeters > 0.0f)
                              ? (op.sedimentMask.thicknessMeters / heightMeters)
                              : 0.0f;

    const rhi::UploadAllocation cb = AllocateConstants(device, sizeof(SedimentConstants));
    if (!cb.IsValid()) {
        return false;
    }
    std::memcpy(cb.cpu, &constants, sizeof(constants));

    PIXBeginEvent(commandList, PIX_COLOR(190, 170, 120), "CompositeSedimentMask");
    // 作業用の 3 枚は読むだけ。
    TransitionIfNeeded(commandList, m_sediment.bedrock,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    TransitionIfNeeded(commandList, m_sediment.sediment,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    TransitionIfNeeded(commandList, m_sediment.original,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    TransitionIfNeeded(commandList, target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    commandList->SetComputeRootConstantBufferView(1, cb.gpuAddress);

    const auto barrier = [&]() {
        const D3D12_RESOURCE_BARRIER uav = CD3DX12_RESOURCE_BARRIER::UAV(nullptr);
        commandList->ResourceBarrier(1, &uav);
    };
    // 実寸で正規化するなら最大値は要らない。集計のパスごと飛ばす。
    if (constants.params[3] <= 0.0f) {
        commandList->SetPipelineState(clearPass);
        commandList->Dispatch(1, 1, 1);
        barrier();
        commandList->SetPipelineState(reducePass);
        commandList->Dispatch(DispatchCount(m_sediment.resolution),
                              DispatchCount(m_sediment.resolution), 1);
        barrier();
    }

    commandList->SetPipelineState(maskPass);
    commandList->Dispatch(DispatchCount(m_resolution), DispatchCount(m_resolution), 1);
    barrier();

    // 次に使うときは書き込みへ戻す。
    TransitionIfNeeded(commandList, m_sediment.bedrock, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    TransitionIfNeeded(commandList, m_sediment.sediment, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    TransitionIfNeeded(commandList, m_sediment.original, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    TransitionIfNeeded(commandList, target, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    PIXEndEvent(commandList);
    return true;
}

void MaterialEvaluator::ReleaseSnowResources(rhi::Device& device) {
    device.DeferRelease(m_snow.base);
    device.DeferRelease(m_snow.thickness);
    device.DeferRelease(m_snow.outflow);
    device.DeferRelease(m_snow.scratch);
    m_snow.resolution = 0;
}

// 積雪の作業リソースも**使うときだけ**作る。グリッドが変わったら作り直す。
bool MaterialEvaluator::EnsureSnowResources(rhi::Device& device, uint32_t resolution) {
    if (m_snow.resolution == resolution && m_snow.IsValid()) {
        return true;
    }
    ReleaseSnowResources(device);

    const bool ok =
        CreateChannelTexture(device, resolution, DXGI_FORMAT_R32_FLOAT, L"SnowBase",
                             m_snow.base) &&
        CreateChannelTexture(device, resolution, DXGI_FORMAT_R32_FLOAT, L"SnowThickness",
                             m_snow.thickness) &&
        CreateChannelTexture(device, resolution, DXGI_FORMAT_R32G32_FLOAT, L"SnowOutflow",
                             m_snow.outflow) &&
        CreateChannelTexture(device, resolution, DXGI_FORMAT_R32G32_FLOAT, L"SnowScratch",
                             m_snow.scratch);
    if (!ok) {
        TG_LOG_WARN("積雪の作業リソースを作れませんでした（%u^2）", resolution);
        ReleaseSnowResources(device);
        return false;
    }
    m_snow.resolution = resolution;
    return true;
}

// 積雪。terrain-editor の Snow を移植したもの。
//
//   setup →（段ごとに）供給 → 滑らせ（流出 → 流入）を 安定化 回
//        → 雪面をならす（横 / 縦）→ 積雪厚を Height へ足し戻す → 法線を作り直す
//
// **滑らせは歩幅を粗いほうから細かいほうへ落としていく。** 1 セルずつしか
// 動かさないと、広い斜面で雪が下まで届くのに段数が要る。terrain-editor と
// 同じく、`最大ディテール` から 1 セルまで半分ずつ縮めながら滑らせる。
bool MaterialEvaluator::ApplySnow(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                                  ID3D12GraphicsCommandList* commandList,
                                  const MaterialLayer& layer, const MaterialStack& stack) {
    const MaterialLayer::SnowSettings& params = layer.snow;
    const uint32_t resolution = std::clamp(params.resolution, 64u, 2048u);
    if (!EnsureSnowResources(device, resolution)) {
        return false;
    }

    const auto pipeline = [&](const wchar_t* entry) {
        return pipelineCache.GetCompute(L"CompositeSnow.hlsl", entry);
    };
    ID3D12PipelineState* setupPass = pipeline(L"CsSetup");
    ID3D12PipelineState* emitPass = pipeline(L"CsEmit");
    ID3D12PipelineState* flowPass = pipeline(L"CsFlow");
    ID3D12PipelineState* gatherPass = pipeline(L"CsGather");
    ID3D12PipelineState* smoothHorizontalPass = pipeline(L"CsSmoothHorizontal");
    ID3D12PipelineState* smoothVerticalPass = pipeline(L"CsSmoothVertical");
    ID3D12PipelineState* applyPass = pipeline(L"CsApply");
    ID3D12PipelineState* normalPass =
        pipelineCache.GetCompute(L"CompositeBlur.hlsl", L"CsNormalFromHeight");
    if (setupPass == nullptr || emitPass == nullptr || flowPass == nullptr ||
        gatherPass == nullptr || smoothHorizontalPass == nullptr ||
        smoothVerticalPass == nullptr || applyPass == nullptr) {
        return false;
    }

    // --- パラメータを正規化ハイトの単位へ直す ------------------------------
    // ハイトは 0〜1 で、その全幅が標高差（m）。安息角も積雪量も m で持っているので、
    // ここで割って正規化ハイトへ揃える（堆積と同じ扱い）。
    const float sizeMeters = (stack.SizeMeters() > 0.0f) ? stack.SizeMeters() : 1.0f;
    const float heightMeters = (stack.HeightMeters() > 0.0f) ? stack.HeightMeters() : 1.0f;
    const float cellMeters = sizeMeters / static_cast<float>(std::max(1u, resolution - 1u));

    // 安息角は**雪面の角度**。1 セル進む間に許す落差へ直す。
    const float degrees = std::clamp(params.motionSlopeDegrees, 0.0f, 89.9f);
    const float talus =
        std::tan(degrees * 3.14159265358979323846f / 180.0f) * cellMeters / heightMeters;

    const int iterations = std::clamp(params.iterations, 1, 256);
    const int settlingPasses = std::clamp(params.settlingPasses, 1, 16);
    const float emissionTime = std::clamp(params.emissionTime, 0.0f, 1.0f);
    const int emissionEnd =
        (emissionTime <= 0.0f)
            ? 1
            : std::clamp(
                  static_cast<int>(std::ceil(static_cast<float>(iterations) * emissionTime)), 1,
                  iterations);
    const float emissionPerIteration = (std::max(0.0f, params.emissionMeters) / heightMeters) /
                                       static_cast<float>(emissionEnd);

    // 歩幅（雪が移動先を探す距離）はセル数で持つ。1 セルより細かくは探せない。
    const float detailMeters =
        std::clamp(params.detailMeters, cellMeters, std::max(cellMeters, sizeMeters * 0.5f));
    const int maxStride = std::clamp(
        static_cast<int>(std::round(detailMeters / std::max(cellMeters, 1e-6f))), 1, 64);
    int strideLevels = 0;
    for (int stride = maxStride; stride > 1; stride = std::max(1, stride / 2)) {
        ++strideLevels;
    }

    SnowConstants constants = {};
    constants.indices0[0] = m_snow.base.UavIndex();
    constants.indices0[1] = m_snow.thickness.UavIndex();
    constants.indices0[2] = m_snow.outflow.UavIndex();
    constants.indices0[3] = m_snow.scratch.UavIndex();
    constants.indices1[0] = m_snow.base.SrvIndex();
    constants.indices1[1] = m_snow.thickness.SrvIndex();
    constants.indices1[2] = m_snow.scratch.SrvIndex();
    constants.indices1[3] = m_textures.height.SrvIndex();
    constants.indices2[0] = resolution;
    constants.indices2[1] = m_textures.height.UavIndex();
    constants.indices2[2] = m_resolution;
    constants.indices3[2] = static_cast<uint32_t>(std::clamp(maxStride, 1, 32));
    constants.params0[0] = talus;
    constants.params0[2] = std::clamp(params.transportRate, 0.0f, 1.0f);
    constants.params0[3] = std::clamp(params.surfaceSmoothing, 0.0f, 1.0f);
    constants.params1[0] = std::max(0.0f, params.maskThresholdMeters) / heightMeters;
    constants.params1[1] = std::max(0.0f, params.maskFeatherMeters) / heightMeters;

    // 定数は**段の中で使う組み合わせぶんだけ**確保して使い回す。段ごとに確保すると、
    // アップロードリングを 1 フレームで食い潰す（堆積で踏んだのと同じ）。
    const auto upload = [&](float emission, uint32_t stride, uint32_t smoothDirection,
                            D3D12_GPU_VIRTUAL_ADDRESS& outAddress) {
        SnowConstants copy = constants;
        copy.params0[1] = emission;
        copy.indices3[0] = stride;
        copy.indices3[1] = smoothDirection;
        const rhi::UploadAllocation cb = AllocateConstants(device, sizeof(SnowConstants));
        if (!cb.IsValid()) {
            return false;
        }
        std::memcpy(cb.cpu, &copy, sizeof(copy));
        outAddress = cb.gpuAddress;
        return true;
    };
    D3D12_GPU_VIRTUAL_ADDRESS setupConstants = 0;
    D3D12_GPU_VIRTUAL_ADDRESS emitConstants = 0;
    if (!upload(0.0f, 1u, 0u, setupConstants) ||
        !upload(emissionPerIteration, 1u, 0u, emitConstants)) {
        return false;
    }
    // 滑らせの歩幅は段の中で粗い順に決まっていて、どの段でも同じ並びになる。
    std::vector<D3D12_GPU_VIRTUAL_ADDRESS> slideConstants(static_cast<size_t>(settlingPasses));
    for (int pass = 0; pass < settlingPasses; ++pass) {
        const int level = (settlingPasses <= 1)
                              ? strideLevels
                              : (pass * strideLevels) / std::max(1, settlingPasses - 1);
        int stride = maxStride;
        for (int step = 0; step < level; ++step) {
            stride = std::max(1, stride / 2);
        }
        if (!upload(0.0f, static_cast<uint32_t>(std::max(1, stride)), 0u,
                    slideConstants[static_cast<size_t>(pass)])) {
            return false;
        }
    }
    D3D12_GPU_VIRTUAL_ADDRESS smoothHorizontalConstants = 0;
    D3D12_GPU_VIRTUAL_ADDRESS smoothVerticalConstants = 0;
    if (!upload(0.0f, 1u, 0u, smoothHorizontalConstants) ||
        !upload(0.0f, 1u, 1u, smoothVerticalConstants)) {
        return false;
    }

    PIXBeginEvent(commandList, PIX_COLOR(190, 200, 215), "CompositeSnow");

    // 入力の Height は読み取り専用（setup が読む）。
    TransitionIfNeeded(commandList, m_textures.height,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    const uint32_t groups = DispatchCount(resolution);
    const auto barrier = [&]() {
        const D3D12_RESOURCE_BARRIER uav = CD3DX12_RESOURCE_BARRIER::UAV(nullptr);
        commandList->ResourceBarrier(1, &uav);
    };
    const auto run = [&](ID3D12PipelineState* pipelineState, uint32_t groupCount) {
        commandList->SetPipelineState(pipelineState);
        commandList->Dispatch(groupCount, groupCount, 1);
        barrier();
    };

    commandList->SetComputeRootConstantBufferView(1, setupConstants);
    run(setupPass, groups);

    const bool transports = constants.params0[2] > 0.0f;
    for (int iteration = 0; iteration < iterations; ++iteration) {
        if (iteration < emissionEnd && emissionPerIteration > 0.0f) {
            commandList->SetComputeRootConstantBufferView(1, emitConstants);
            run(emitPass, groups);
        }
        if (!transports) {
            continue;
        }
        for (int pass = 0; pass < settlingPasses; ++pass) {
            commandList->SetComputeRootConstantBufferView(
                1, slideConstants[static_cast<size_t>(pass)]);
            run(flowPass, groups);
            run(gatherPass, groups);
        }
    }

    // 積もった雪面だけをならす。0 のときは 1 往復ぶん丸ごと省く。
    if (constants.params0[3] > 0.0f) {
        commandList->SetComputeRootConstantBufferView(1, smoothHorizontalConstants);
        run(smoothHorizontalPass, groups);
        commandList->SetComputeRootConstantBufferView(1, smoothVerticalConstants);
        run(smoothVerticalPass, groups);
    }

    // 積雪厚を合成の Height へ足す。ここだけ合成解像度で回す。
    // **Mask だけが目的のときは足し戻さない**（Result を繋いでいない）。
    // 厚みは作業用テクスチャに残るので、マスクはこの後で焼ける。
    if (!layer.maskOnly) {
        TransitionIfNeeded(commandList, m_snow.base,
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        TransitionIfNeeded(commandList, m_snow.thickness,
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        TransitionIfNeeded(commandList, m_textures.height,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        commandList->SetComputeRootConstantBufferView(1, setupConstants);
        run(applyPass, DispatchCount(m_resolution));

        // 次に使うときは書き込みへ戻す。
        TransitionIfNeeded(commandList, m_snow.base, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        TransitionIfNeeded(commandList, m_snow.thickness,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    // 次のレイヤーは Height を UAV として書く。Mask だけのときも戻す（堆積と同じ）。
    TransitionIfNeeded(commandList, m_textures.height, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    PIXEndEvent(commandList);

    // 形が変わったので、法線も作り直す。足し戻していないなら形は変わっていない。
    if (!layer.maskOnly) {
        RebuildNormalsFromHeight(device, normalPass, commandList, stack);
    }
    return true;
}

// 直前の積雪レイヤーが残した積雪厚を、被覆のマスクとして焼く。
//
// **積雪レイヤーを合成し終えた直後にしか呼ばない**（作業用テクスチャは
// 次の積雪レイヤーで上書きされるため）。段取りは堆積 / 崩落と同じ。
bool MaterialEvaluator::ApplySnowMask(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                                      ID3D12GraphicsCommandList* commandList, const MaskOp& op,
                                      const MaterialStack& stack, rhi::GpuTexture& target) {
    if (!m_snow.IsValid() || !target.IsValid()) {
        return false;
    }
    ID3D12PipelineState* maskPass = pipelineCache.GetCompute(L"CompositeSnow.hlsl", L"CsMask");
    if (maskPass == nullptr) {
        return false;
    }

    const float heightMeters = (stack.HeightMeters() > 0.0f) ? stack.HeightMeters() : 1.0f;

    SnowConstants constants = {};
    constants.indices1[1] = m_snow.thickness.SrvIndex();
    constants.indices2[0] = m_snow.resolution;
    constants.indices2[2] = m_resolution;
    constants.indices2[3] = target.UavIndex();
    // しきい値もぼかしも実寸（m）。ハイト 0〜1 の全幅が標高差なので、その比へ直す。
    constants.params1[0] = std::max(0.0f, op.snowMask.thresholdMeters) / heightMeters;
    constants.params1[1] = std::max(0.0f, op.snowMask.featherMeters) / heightMeters;

    const rhi::UploadAllocation cb = AllocateConstants(device, sizeof(SnowConstants));
    if (!cb.IsValid()) {
        return false;
    }
    std::memcpy(cb.cpu, &constants, sizeof(constants));

    PIXBeginEvent(commandList, PIX_COLOR(200, 210, 225), "CompositeSnowMask");
    TransitionIfNeeded(commandList, m_snow.thickness,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    TransitionIfNeeded(commandList, target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    commandList->SetComputeRootConstantBufferView(1, cb.gpuAddress);
    commandList->SetPipelineState(maskPass);
    commandList->Dispatch(DispatchCount(m_resolution), DispatchCount(m_resolution), 1);
    const D3D12_RESOURCE_BARRIER uav = CD3DX12_RESOURCE_BARRIER::UAV(nullptr);
    commandList->ResourceBarrier(1, &uav);

    // 次に使うときは書き込みへ戻す。
    TransitionIfNeeded(commandList, m_snow.thickness, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    TransitionIfNeeded(commandList, target, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    PIXEndEvent(commandList);
    return true;
}

void MaterialEvaluator::ReleaseRiverResources(rhi::Device& device) {
    device.DeferRelease(m_river.heights);
    device.DeferRelease(m_river.scratch);
    device.DeferRelease(m_river.surface);
    device.DeferRelease(m_river.weights0);
    device.DeferRelease(m_river.weights1);
    device.DeferRelease(m_river.accumA);
    device.DeferRelease(m_river.accumB);
    device.DeferRelease(m_river.width);
    device.DeferRelease(m_river.jfaA);
    device.DeferRelease(m_river.jfaB);
    device.DeferRelease(m_river.maxScratch);
    device.DeferRelease(m_river.waterLevel);
    device.DeferRelease(m_river.distance);
    device.DeferRelease(m_river.ground);
    device.DeferRelease(m_river.lakeDepth);
    device.DeferRelease(m_river.halfWidth);
    device.DeferRelease(m_river.waterFine);
    device.DeferRelease(m_river.depthFine);
    m_river.resolution = 0;
    m_river.fineResolution = 0;
}

// 河川の作業リソースも**使うときだけ**作る。解析グリッドか合成解像度が変わったら作り直す。
// 川筋（Fluvial）と共有しないのは、マスクの op が河川レイヤーの後で焼くまで
// 中身を残しておく必要があるため（同じ位置で焼く川筋の op に上書きされると困る）。
bool MaterialEvaluator::EnsureRiverResources(rhi::Device& device, uint32_t resolution,
                                             uint32_t fineResolution) {
    if (m_river.resolution == resolution && m_river.fineResolution == fineResolution &&
        m_river.IsValid()) {
        return true;
    }
    ReleaseRiverResources(device);

    const auto grid = [&](DXGI_FORMAT format, const wchar_t* name, rhi::GpuTexture& texture) {
        return CreateChannelTexture(device, resolution, format, name, texture);
    };
    const bool ok =
        grid(DXGI_FORMAT_R32_FLOAT, L"RiverHeights", m_river.heights) &&
        grid(DXGI_FORMAT_R32_FLOAT, L"RiverScratch", m_river.scratch) &&
        grid(DXGI_FORMAT_R32_FLOAT, L"RiverSurface", m_river.surface) &&
        grid(DXGI_FORMAT_R32G32B32A32_FLOAT, L"RiverWeights0", m_river.weights0) &&
        grid(DXGI_FORMAT_R32G32B32A32_FLOAT, L"RiverWeights1", m_river.weights1) &&
        grid(DXGI_FORMAT_R32_FLOAT, L"RiverAccumA", m_river.accumA) &&
        grid(DXGI_FORMAT_R32_FLOAT, L"RiverAccumB", m_river.accumB) &&
        grid(DXGI_FORMAT_R32_FLOAT, L"RiverWidth", m_river.width) &&
        grid(DXGI_FORMAT_R32G32B32A32_FLOAT, L"RiverJfaA", m_river.jfaA) &&
        grid(DXGI_FORMAT_R32G32B32A32_FLOAT, L"RiverJfaB", m_river.jfaB) &&
        CreateChannelTexture(device, 1, DXGI_FORMAT_R32_UINT, L"RiverMax", m_river.maxScratch) &&
        grid(DXGI_FORMAT_R32_FLOAT, L"RiverWaterLevel", m_river.waterLevel) &&
        grid(DXGI_FORMAT_R32_FLOAT, L"RiverDistance", m_river.distance) &&
        grid(DXGI_FORMAT_R32_FLOAT, L"RiverGround", m_river.ground) &&
        grid(DXGI_FORMAT_R32_FLOAT, L"RiverLakeDepth", m_river.lakeDepth) &&
        grid(DXGI_FORMAT_R32_FLOAT, L"RiverHalfWidth", m_river.halfWidth) &&
        CreateChannelTexture(device, fineResolution, kMaskFormat, L"RiverWaterFine",
                             m_river.waterFine) &&
        CreateChannelTexture(device, fineResolution, kMaskFormat, L"RiverDepthFine",
                             m_river.depthFine);
    if (!ok) {
        TG_LOG_WARN("河川の作業リソースを作れませんでした（%u^2）", resolution);
        ReleaseRiverResources(device);
        return false;
    }
    m_river.resolution = resolution;
    m_river.fineResolution = fineResolution;
    return true;
}

// 河川。設計は docs/reference/river-node.md。
//
//   解析グリッドへ落とす → ローパス → 窪み埋め = 水面高（2 × 解像度 回）
//   → 埋めた面の上で MFD 重み → 流量（2 × 解像度 回）→ 幅 → JFA → 水面 / 距離 / 掘り
//   → 合成解像度へ書き戻す（掘りは差分、水面は置き換え）→ 法線を作り直す
//
// **窪み埋めと水面の単調化は同じ計算**（Planchon–Darboux）なので 1 回で済ませる。
// Mask Fluvial の 8 回固定の窪み埋めだと大きな盆地で流量が消え、湖の下流で
// 川が途切れる。ここでは O(解像度) 回まわして盆地を出口まで埋める。
bool MaterialEvaluator::ApplyRiver(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                                   ID3D12GraphicsCommandList* commandList,
                                   const MaterialLayer& layer, const MaterialStack& stack,
                                   uint32_t seedIndex) {
    const MaterialLayer::RiverSettings& params = layer.river;
    const uint32_t resolution = std::clamp(params.resolution, 64u, 2048u);
    if (!EnsureRiverResources(device, resolution, m_resolution)) {
        return false;
    }

    const auto pipeline = [&](const wchar_t* entry) {
        return pipelineCache.GetCompute(L"CompositeRiver.hlsl", entry);
    };
    ID3D12PipelineState* samplePass = pipeline(L"CsSample");
    ID3D12PipelineState* blurHPass = pipeline(L"CsBlurH");
    ID3D12PipelineState* blurVPass = pipeline(L"CsBlurV");
    ID3D12PipelineState* fillInitPass = pipeline(L"CsFillInit");
    ID3D12PipelineState* fillIterPass = pipeline(L"CsFillIter");
    ID3D12PipelineState* weightsPass = pipeline(L"CsWeights");
    ID3D12PipelineState* accumInitPass = pipeline(L"CsAccumInit");
    ID3D12PipelineState* accumIterPass = pipeline(L"CsAccumIter");
    ID3D12PipelineState* maxClearPass = pipeline(L"CsMaxClear");
    ID3D12PipelineState* maxReducePass = pipeline(L"CsMaxReduce");
    ID3D12PipelineState* widthPass = pipeline(L"CsWidth");
    ID3D12PipelineState* jfaPass = pipeline(L"CsJfaStep");
    ID3D12PipelineState* resolvePass = pipeline(L"CsResolve");
    ID3D12PipelineState* applyPass = pipeline(L"CsApply");
    ID3D12PipelineState* waterNormalPass = pipeline(L"CsWaterNormal");
    ID3D12PipelineState* normalPass =
        pipelineCache.GetCompute(L"CompositeBlur.hlsl", L"CsNormalFromHeight");
    if (samplePass == nullptr || blurHPass == nullptr || blurVPass == nullptr ||
        fillInitPass == nullptr || fillIterPass == nullptr || weightsPass == nullptr ||
        accumInitPass == nullptr || accumIterPass == nullptr || maxClearPass == nullptr ||
        maxReducePass == nullptr || widthPass == nullptr || jfaPass == nullptr ||
        resolvePass == nullptr || applyPass == nullptr || waterNormalPass == nullptr ||
        normalPass == nullptr) {
        return false;
    }

    // --- パラメータを正規化ハイト / セル数へ直す --------------------------------
    // ハイトは 0〜1 で、その全幅が標高差（m）。距離は 1 セルが何 m かで決まる。
    const float sizeMeters = (stack.SizeMeters() > 0.0f) ? stack.SizeMeters() : 1.0f;
    const float heightMeters = (stack.HeightMeters() > 0.0f) ? stack.HeightMeters() : 1.0f;
    const float cellMeters = sizeMeters / static_cast<float>(std::max(1u, resolution - 1u));

    const float detailMeters =
        std::clamp(params.detailMeters, cellMeters, std::max(cellMeters, sizeMeters * 0.5f));
    const uint32_t blurRadius = static_cast<uint32_t>(
        std::clamp(static_cast<int>(std::lround(detailMeters / cellMeters)), 1, 64));
    const float cellCount = static_cast<float>(resolution) * static_cast<float>(resolution);
    // 最小勾配は無次元。1 セル進む間の落差（正規化ハイト）へ直す。
    // 0 のままだと平坦面で MFD の重みが全部 0 になり流量が止まるので、下限を入れる。
    const float slopePerCell = std::max(0.0f, params.minSlope) * cellMeters / heightMeters;
    const float epsilon = std::max(slopePerCell, 1e-6f);
    const float minHalfCells = std::max(0.0f, params.minWidthMeters) / (2.0f * cellMeters);
    const float mainHalfCells =
        std::max(std::max(0.0f, params.mainWidthMeters) / (2.0f * cellMeters), minHalfCells);

    RiverConstants constants = {};
    constants.indices0[0] = m_river.heights.UavIndex();
    constants.indices0[1] = m_river.scratch.UavIndex();
    constants.indices0[2] = m_river.surface.UavIndex();
    constants.indices0[3] = m_river.weights0.UavIndex();
    constants.indices1[0] = m_river.weights1.UavIndex();
    constants.indices1[1] = m_river.accumA.UavIndex();
    constants.indices1[2] = m_river.accumB.UavIndex();
    constants.indices1[3] = m_river.width.UavIndex();
    constants.indices2[0] = m_river.jfaA.UavIndex();
    constants.indices2[1] = m_river.jfaB.UavIndex();
    constants.indices2[2] = m_river.maxScratch.UavIndex();
    constants.indices2[3] = seedIndex;
    constants.indices3[0] = m_river.waterLevel.UavIndex();
    constants.indices3[1] = m_river.distance.UavIndex();
    constants.indices3[2] = m_river.ground.UavIndex();
    constants.indices3[3] = m_river.lakeDepth.UavIndex();
    constants.indices4[0] = m_river.halfWidth.UavIndex();
    constants.indices4[1] = m_river.waterFine.UavIndex();
    constants.indices4[2] = m_river.depthFine.UavIndex();
    constants.indices4[3] = m_textures.height.SrvIndex();
    constants.indices5[0] = m_textures.height.UavIndex();
    constants.indices5[1] = kInvalidTextureIndex;
    constants.indices5[2] = resolution;
    constants.indices5[3] = m_resolution;
    constants.indices6[2] = blurRadius;
    constants.indices7[1] = params.fillWater ? 1u : 0u;
    constants.indices7[3] = m_textures.normal.UavIndex();
    constants.indices8[0] = m_river.heights.SrvIndex();
    constants.indices8[1] = m_river.waterLevel.SrvIndex();
    constants.indices8[2] = m_river.distance.SrvIndex();
    constants.indices8[3] = m_river.ground.SrvIndex();
    constants.indices9[0] = m_river.lakeDepth.SrvIndex();
    constants.indices9[1] = m_river.halfWidth.SrvIndex();
    constants.indices9[2] = m_river.waterFine.SrvIndex();
    constants.indices9[3] = m_river.depthFine.SrvIndex();
    constants.params0[0] = std::clamp(params.concentration, 0.1f, 16.0f);
    constants.params0[1] = std::clamp(params.threshold, 0.0f, 1.0f) * cellCount;
    constants.params0[2] = epsilon;
    constants.params1[0] = mainHalfCells;
    constants.params1[1] = minHalfCells;
    constants.params1[2] = std::clamp(params.widthExponent, 0.0f, 2.0f);
    constants.params1[3] = std::max(0.0f, params.bedDepthMeters) / heightMeters;
    constants.params2[0] = std::max(0.0f, params.bankWidthMeters) / cellMeters;
    constants.params2[1] = std::clamp(params.bankHardness, 0.0f, 1.0f);
    constants.params2[2] = std::max(0.0f, params.shoreWidthMeters);
    constants.params2[3] = std::max(0.0f, params.shoreHeightMeters);
    constants.params3[0] = std::clamp(params.shoreFeather, 0.0f, 1.0f);
    constants.params3[1] = cellMeters;
    constants.params3[2] = heightMeters;
    // 湖とみなす深さと水際のぼかしは、標高差に依らず実寸で固定する（5 cm / 2 cm）。
    constants.params3[3] = 0.05f / heightMeters;
    constants.params4[0] = 0.02f / heightMeters;
    constants.params4[1] = mainHalfCells * cellMeters;
    constants.params4[2] = std::max(0.0f, params.bankWidthMeters);
    constants.params4[3] = heightMeters / sizeMeters;

    // 定数は組み合わせぶんだけ確保して使い回す（反復のたびに確保すると
    // アップロードリングを 1 フレームで食い潰す。川筋 / 堆積と同じ）。
    const auto upload = [&](uint32_t direction, uint32_t jfaStep, uint32_t jfaRead,
                            uint32_t writeHeight, D3D12_GPU_VIRTUAL_ADDRESS& outAddress) {
        RiverConstants copy = constants;
        copy.indices6[0] = direction;
        copy.indices6[1] = jfaStep;
        copy.indices6[3] = jfaRead;
        copy.indices7[2] = writeHeight;
        const rhi::UploadAllocation cb = AllocateConstants(device, sizeof(RiverConstants));
        if (!cb.IsValid()) {
            return false;
        }
        std::memcpy(cb.cpu, &copy, sizeof(copy));
        outAddress = cb.gpuAddress;
        return true;
    };
    D3D12_GPU_VIRTUAL_ADDRESS constantsA = 0;
    D3D12_GPU_VIRTUAL_ADDRESS constantsB = 0;
    if (!upload(0u, 0u, 0u, 0u, constantsA) || !upload(1u, 0u, 0u, 0u, constantsB)) {
        return false;
    }
    // JFA の歩幅は 解像度/2 から 1 まで半分ずつ。最後に歩幅 1 をもう 1 回掛けて
    // 取りこぼしを拾う（JFA+1）。
    std::vector<uint32_t> jfaSteps;
    for (uint32_t step = resolution / 2u; step >= 1u; step /= 2u) {
        jfaSteps.push_back(step);
    }
    jfaSteps.push_back(1u);
    std::vector<D3D12_GPU_VIRTUAL_ADDRESS> jfaConstants(jfaSteps.size());
    for (size_t i = 0; i < jfaSteps.size(); ++i) {
        if (!upload(static_cast<uint32_t>(i % 2), jfaSteps[i], 0u, 0u, jfaConstants[i])) {
            return false;
        }
    }
    // 偶数回なら結果は A に、奇数回なら B に残る。
    const uint32_t jfaRead = (jfaSteps.size() % 2 == 0) ? 0u : 1u;
    D3D12_GPU_VIRTUAL_ADDRESS resolveConstants = 0;
    D3D12_GPU_VIRTUAL_ADDRESS applyConstants = 0;
    if (!upload(0u, 0u, jfaRead, 0u, resolveConstants) ||
        !upload(0u, 0u, jfaRead, layer.maskOnly ? 0u : 1u, applyConstants)) {
        return false;
    }

    PIXBeginEvent(commandList, PIX_COLOR(70, 130, 180), "CompositeRiver");

    // 入力の Height は読み取り専用（sample が読む）。
    TransitionIfNeeded(commandList, m_textures.height,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    const uint32_t groups = DispatchCount(resolution);
    const auto barrier = [&]() {
        const D3D12_RESOURCE_BARRIER uav = CD3DX12_RESOURCE_BARRIER::UAV(nullptr);
        commandList->ResourceBarrier(1, &uav);
    };
    const auto run = [&](ID3D12PipelineState* pipelineState, uint32_t groupCount) {
        commandList->SetPipelineState(pipelineState);
        commandList->Dispatch(groupCount, groupCount, 1);
        barrier();
    };

    commandList->SetComputeRootConstantBufferView(1, constantsA);
    run(samplePass, groups);
    if (blurRadius > 1u) {
        run(blurHPass, groups);
        run(blurVPass, groups);
    }

    // 窪み埋め = 水面高。情報は 1 反復に 1 セル進むので、累積と同じ回数まわす。
    // **偶数回まわすと結果は surface に残る。**
    run(fillInitPass, groups);
    const int jacobiIterations = static_cast<int>(resolution) * 2;
    for (int i = 0; i < jacobiIterations; ++i) {
        commandList->SetComputeRootConstantBufferView(1, (i % 2 == 0) ? constantsA : constantsB);
        run(fillIterPass, groups);
    }

    commandList->SetComputeRootConstantBufferView(1, constantsA);
    run(weightsPass, groups);
    run(accumInitPass, groups);
    for (int i = 0; i < jacobiIterations; ++i) {
        commandList->SetComputeRootConstantBufferView(1, (i % 2 == 0) ? constantsA : constantsB);
        run(accumIterPass, groups);
    }
    commandList->SetComputeRootConstantBufferView(1, constantsA);
    run(maxClearPass, 1);
    run(maxReducePass, groups);
    run(widthPass, groups);

    for (size_t i = 0; i < jfaSteps.size(); ++i) {
        commandList->SetComputeRootConstantBufferView(1, jfaConstants[i]);
        run(jfaPass, groups);
    }
    commandList->SetComputeRootConstantBufferView(1, resolveConstants);
    run(resolvePass, groups);

    // 合成解像度へ書き戻す。**Mask だけが目的のときは Height に書かない**が、
    // 水面の被覆と水深（合成解像度）はここでしか焼けないので、パスは必ず通す。
    TransitionIfNeeded(commandList, m_river.heights,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    TransitionIfNeeded(commandList, m_river.waterLevel,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    TransitionIfNeeded(commandList, m_river.distance,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    TransitionIfNeeded(commandList, m_river.ground,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    TransitionIfNeeded(commandList, m_river.lakeDepth,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    TransitionIfNeeded(commandList, m_textures.height, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    commandList->SetComputeRootConstantBufferView(1, applyConstants);
    run(applyPass, DispatchCount(m_resolution));

    // 形が変わったので、法線も作り直す。書き戻していないなら形は変わっていない。
    // 水面の法線だけは合成の Height ではなく解析グリッドの水面高（平らな面）から作る
    // （Height が R16 だった頃は階段で縞になった。R32 になった今も水面高から作るほうが素直）。
    if (!layer.maskOnly) {
        RebuildNormalsFromHeight(device, normalPass, commandList, stack);
        commandList->SetComputeRootConstantBufferView(1, applyConstants);
        run(waterNormalPass, DispatchCount(m_resolution));
    }

    // 次に使うときは書き込みへ戻す。
    TransitionIfNeeded(commandList, m_river.heights, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    TransitionIfNeeded(commandList, m_river.waterLevel, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    TransitionIfNeeded(commandList, m_river.distance, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    TransitionIfNeeded(commandList, m_river.ground, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    TransitionIfNeeded(commandList, m_river.lakeDepth, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    PIXEndEvent(commandList);
    return true;
}

// 直前の河川レイヤーが残した水面 / 河原 / 水深を、マスクとして焼く。
//
// **河川レイヤーを合成し終えた直後にしか呼ばない**（作業用テクスチャは
// 次の河川レイヤーで上書きされるため）。段取りは堆積 / 崩落 / 積雪と同じ。
bool MaterialEvaluator::ApplyRiverMask(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                                       ID3D12GraphicsCommandList* commandList, const MaskOp& op,
                                       const MaterialStack& stack, rhi::GpuTexture& target) {
    if (!m_river.IsValid() || !target.IsValid() || m_river.fineResolution != m_resolution) {
        return false;
    }
    ID3D12PipelineState* maskPass = pipelineCache.GetCompute(L"CompositeRiver.hlsl", L"CsMask");
    if (maskPass == nullptr) {
        return false;
    }

    const float sizeMeters = (stack.SizeMeters() > 0.0f) ? stack.SizeMeters() : 1.0f;
    const float heightMeters = (stack.HeightMeters() > 0.0f) ? stack.HeightMeters() : 1.0f;
    const float cellMeters =
        sizeMeters / static_cast<float>(std::max(1u, m_river.resolution - 1u));

    RiverConstants constants = {};
    constants.indices5[1] = target.UavIndex();
    constants.indices5[2] = m_river.resolution;
    constants.indices5[3] = m_resolution;
    constants.indices7[0] = std::min(op.riverMask.channel, 2u);
    constants.indices8[1] = m_river.waterLevel.SrvIndex();
    constants.indices8[2] = m_river.distance.SrvIndex();
    constants.indices8[3] = m_river.ground.SrvIndex();
    constants.indices9[1] = m_river.halfWidth.SrvIndex();
    constants.indices9[2] = m_river.waterFine.SrvIndex();
    constants.indices9[3] = m_river.depthFine.SrvIndex();
    constants.params2[2] = std::max(0.0f, op.riverMask.shoreWidthMeters);
    constants.params2[3] = std::max(0.0f, op.riverMask.shoreHeightMeters);
    constants.params3[0] = std::clamp(op.riverMask.shoreFeather, 0.0f, 1.0f);
    constants.params3[1] = cellMeters;
    constants.params3[2] = heightMeters;
    // 河原の広がりを縮める基準は主流の半幅（m）。ApplyRiver と同じ式で出す。
    constants.params4[1] =
        std::max(op.riverMask.mainWidthMeters, op.riverMask.minWidthMeters) * 0.5f;

    const rhi::UploadAllocation cb = AllocateConstants(device, sizeof(RiverConstants));
    if (!cb.IsValid()) {
        return false;
    }
    std::memcpy(cb.cpu, &constants, sizeof(constants));

    PIXBeginEvent(commandList, PIX_COLOR(90, 150, 200), "CompositeRiverMask");
    rhi::GpuTexture* inputs[] = {&m_river.waterLevel, &m_river.distance, &m_river.ground,
                                 &m_river.halfWidth,  &m_river.waterFine, &m_river.depthFine};
    for (rhi::GpuTexture* texture : inputs) {
        TransitionIfNeeded(commandList, *texture, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    TransitionIfNeeded(commandList, target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    commandList->SetComputeRootConstantBufferView(1, cb.gpuAddress);
    commandList->SetPipelineState(maskPass);
    commandList->Dispatch(DispatchCount(m_resolution), DispatchCount(m_resolution), 1);
    const D3D12_RESOURCE_BARRIER uav = CD3DX12_RESOURCE_BARRIER::UAV(nullptr);
    commandList->ResourceBarrier(1, &uav);

    // 次に使うときは書き込みへ戻す。
    for (rhi::GpuTexture* texture : inputs) {
        TransitionIfNeeded(commandList, *texture, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    TransitionIfNeeded(commandList, target, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    PIXEndEvent(commandList);
    return true;
}

// 崩落レイヤー 1 枚ぶん。発生源のマスクから岩片を生み、斜面を下らせて積む。
//
// **合成解像度でそのまま回す。** 岩片は m 単位の小さな形なので、堆積のように
// 粗いグリッドで回すと形にならない。歩行は 1 スレッド 1 粒子で、
// 読むのは合成の Height（この時点までの合成結果）だけ。

// 水滴侵食の定数。シェーダ側の DropletConstants と一致させること。
struct DropletConstants {
    uint32_t indices0[4];
    uint32_t indices1[4];
    uint32_t indices2[4];
    uint32_t indices3[4];
    uint32_t indices4[4];
    uint32_t indices5[4];
    float params0[4];
    float params1[4];
    float params2[4];
};

void MaterialEvaluator::ReleaseDropletResources(rhi::Device& device) {
    device.DeferRelease(m_droplet.heights);
    device.DeferRelease(m_droplet.source);
    device.DeferRelease(m_droplet.original);
    device.DeferRelease(m_droplet.delta);
    device.DeferRelease(m_droplet.flow);
    device.DeferRelease(m_droplet.deposit);
    device.DeferRelease(m_droplet.maxScratch);
    m_droplet.resolution = 0;
}

// 水滴侵食の作業リソースも**使うときだけ**作る。解析グリッドが変わったら作り直す。
bool MaterialEvaluator::EnsureDropletResources(rhi::Device& device, uint32_t resolution) {
    if (m_droplet.resolution == resolution && m_droplet.IsValid()) {
        return true;
    }
    ReleaseDropletResources(device);
    const auto grid = [&](DXGI_FORMAT format, const wchar_t* name, rhi::GpuTexture& texture) {
        return CreateChannelTexture(device, resolution, format, name, texture);
    };
    const bool ok = grid(DXGI_FORMAT_R32_FLOAT, L"DropletHeights", m_droplet.heights) &&
                    grid(DXGI_FORMAT_R32_FLOAT, L"DropletSource", m_droplet.source) &&
                    grid(DXGI_FORMAT_R32_FLOAT, L"DropletOriginal", m_droplet.original) &&
                    grid(DXGI_FORMAT_R32_SINT, L"DropletDelta", m_droplet.delta) &&
                    grid(DXGI_FORMAT_R32_SINT, L"DropletFlow", m_droplet.flow) &&
                    grid(DXGI_FORMAT_R32_SINT, L"DropletDeposit", m_droplet.deposit) &&
                    CreateChannelTexture(device, 1, DXGI_FORMAT_R32_UINT, L"DropletMax",
                                         m_droplet.maxScratch);
    if (!ok) {
        TG_LOG_WARN("水滴侵食の作業リソースを作れませんでした（%u^2）", resolution);
        ReleaseDropletResources(device);
        return false;
    }
    m_droplet.resolution = resolution;
    return true;
}

// 水滴侵食。terrain-editor の Droplet Erosion（GPU 版）の移植。段取りはシェーダの頭。
//
//   元の高さを控える → レベルごとに（落とす / 拡大 → 流量と堆積を 0 に →
//   反復ごとに 差分を 0 に → 水滴を流す → 差分を飽和つきで足す）
//   → 最終レベルの差分を合成解像度へ足し戻す → 法線を作り直す
bool MaterialEvaluator::ApplyDroplet(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                                     ID3D12GraphicsCommandList* commandList,
                                     const MaterialLayer& layer, const MaterialStack& stack) {
    const MaterialLayer::DropletSettings& params = layer.droplet;
    const uint32_t resolution = std::clamp(params.resolution, 64u, 2048u);
    if (!EnsureDropletResources(device, resolution)) {
        return false;
    }
    const auto pipeline = [&](const wchar_t* entry) {
        return pipelineCache.GetCompute(L"CompositeDroplet.hlsl", entry);
    };
    ID3D12PipelineState* originalPass = pipeline(L"CsOriginal");
    ID3D12PipelineState* initPass = pipeline(L"CsInit");
    ID3D12PipelineState* copyToSrcPass = pipeline(L"CsCopyToSrc");
    ID3D12PipelineState* upsamplePass = pipeline(L"CsUpsample");
    ID3D12PipelineState* clearLevelPass = pipeline(L"CsClearLevel");
    ID3D12PipelineState* clearIterPass = pipeline(L"CsClearIter");
    ID3D12PipelineState* tracePass = pipeline(L"CsTrace");
    ID3D12PipelineState* applyPass = pipeline(L"CsApply");
    ID3D12PipelineState* resolvePass = pipeline(L"CsResolve");
    ID3D12PipelineState* normalPass =
        pipelineCache.GetCompute(L"CompositeBlur.hlsl", L"CsNormalFromHeight");
    if (originalPass == nullptr || initPass == nullptr || copyToSrcPass == nullptr ||
        upsamplePass == nullptr || clearLevelPass == nullptr || clearIterPass == nullptr ||
        tracePass == nullptr || applyPass == nullptr || resolvePass == nullptr ||
        normalPass == nullptr) {
        return false;
    }

    const float sizeMeters = (stack.SizeMeters() > 0.0f) ? stack.SizeMeters() : 1.0f;
    const float heightMeters = (stack.HeightMeters() > 0.0f) ? stack.HeightMeters() : 1.0f;

    // マルチグリッドのレベル（64² から倍々。最後は解析グリッドそのもの）。
    std::vector<uint32_t> levels;
    if (params.multigrid) {
        for (uint32_t r = std::clamp(64u, 16u, resolution); r < resolution; r *= 2u) {
            levels.push_back(r);
        }
    }
    levels.push_back(resolution);
    const bool singleLevel = levels.size() <= 1;

    // 水滴の総数は密度 × セル数。粗いレベルは面積の比で減らす（密度を保つ）。
    const double density = std::max(0.0f, params.dropletDensity);
    const int targetParticles = std::clamp(
        static_cast<int>(std::lround(density * static_cast<double>(resolution) *
                                     static_cast<double>(resolution))),
        1, 2000000);
    const int iterations = std::clamp(params.iterations, 1, 200);
    const float evaporation = std::clamp(params.evaporationPerMeter, 0.0f, 1.0f);
    // 1 反復で 1 セルが動ける上限（セルの大きさに対する割合）と、端のフェードの幅（m）。
    constexpr float kDeltaCapFactor = 0.20f;
    constexpr float kEdgeFadeMeters = 24.0f;

    DropletConstants base = {};
    base.indices0[0] = m_droplet.heights.UavIndex();
    base.indices0[1] = m_droplet.source.UavIndex();
    base.indices0[2] = m_droplet.original.UavIndex();
    base.indices0[3] = m_droplet.delta.UavIndex();
    base.indices1[0] = m_droplet.flow.UavIndex();
    base.indices1[1] = m_droplet.deposit.UavIndex();
    base.indices1[2] = m_textures.height.SrvIndex();
    base.indices1[3] = m_textures.height.UavIndex();
    base.indices3[0] = static_cast<uint32_t>(params.seed);
    base.indices3[3] = m_resolution;
    base.indices4[3] = resolution;
    base.indices5[0] = m_droplet.heights.SrvIndex();
    base.indices5[1] = m_droplet.original.SrvIndex();
    base.indices5[2] = m_droplet.flow.SrvIndex();
    base.indices5[3] = m_droplet.deposit.SrvIndex();
    base.params0[1] = std::clamp(params.inertia, 0.0f, 0.99f);
    base.params0[2] = std::max(0.01f, params.sedimentCapacity);
    base.params0[3] = std::max(0.0001f, params.minSlope);
    base.params1[0] = std::clamp(params.erosionStrength, 0.0f, 1.0f);
    base.params1[1] = std::clamp(params.depositionStrength, 0.0f, 1.0f);
    base.params1[3] = std::max(0.0f, params.gravity);
    base.params2[2] = heightMeters;
    base.params2[3] = layer.maskOnly ? 0.0f : 1.0f;

    const auto upload = [&](const DropletConstants& constants,
                            D3D12_GPU_VIRTUAL_ADDRESS& outAddress) {
        const rhi::UploadAllocation cb = AllocateConstants(device, sizeof(DropletConstants));
        if (!cb.IsValid()) {
            return false;
        }
        std::memcpy(cb.cpu, &constants, sizeof(constants));
        outAddress = cb.gpuAddress;
        return true;
    };
    const auto barrier = [&]() {
        const D3D12_RESOURCE_BARRIER uav = CD3DX12_RESOURCE_BARRIER::UAV(nullptr);
        commandList->ResourceBarrier(1, &uav);
    };
    const auto run = [&](ID3D12PipelineState* pipelineState, uint32_t groupCount) {
        commandList->SetPipelineState(pipelineState);
        commandList->Dispatch(groupCount, groupCount, 1);
        barrier();
    };

    PIXBeginEvent(commandList, PIX_COLOR(90, 150, 200), "CompositeDroplet");

    // 入力の Height は読み取り専用（落とすときに読む）。
    TransitionIfNeeded(commandList, m_textures.height,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    TransitionIfNeeded(commandList, m_droplet.heights, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    TransitionIfNeeded(commandList, m_droplet.original, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    TransitionIfNeeded(commandList, m_droplet.flow, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    TransitionIfNeeded(commandList, m_droplet.deposit, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // 元の高さ（最終解像度）。差分を出すために控える。
    {
        DropletConstants constants = base;
        constants.indices2[0] = resolution;
        D3D12_GPU_VIRTUAL_ADDRESS address = 0;
        if (!upload(constants, address)) {
            PIXEndEvent(commandList);
            return false;
        }
        commandList->SetComputeRootConstantBufferView(1, address);
        run(originalPass, DispatchCount(resolution));
    }

    for (size_t levelIndex = 0; levelIndex < levels.size(); ++levelIndex) {
        const uint32_t n = levels[levelIndex];
        const float cellMeters = sizeMeters / static_cast<float>(n);
        // このレベルの水滴の数。粗いレベルは面積の比で減らす（最低 1000）。
        int levelParticles = targetParticles;
        if (n < resolution) {
            const double ratio = (static_cast<double>(n) * n) /
                                 (static_cast<double>(resolution) * resolution);
            levelParticles = std::max(
                1000, static_cast<int>(static_cast<double>(targetParticles) * ratio));
        }
        const int particlesPerIteration = std::max(1, levelParticles / iterations);
        // 歩数は移動距離をセルの大きさで割ったもの（解像度に依らず同じ距離を進む）。
        const int steps = std::clamp(
            static_cast<int>(std::lround(params.travelMeters / std::max(cellMeters, 1e-3f))),
            1, 8192);

        DropletConstants level = base;
        level.indices2[0] = n;
        level.indices2[1] = (levelIndex > 0) ? levels[levelIndex - 1] : n;
        level.indices2[2] = static_cast<uint32_t>(particlesPerIteration);
        level.indices2[3] = static_cast<uint32_t>(steps);
        level.indices3[1] = singleLevel ? 0u : static_cast<uint32_t>(levelIndex + 1);
        level.params0[0] = cellMeters;
        // 蒸発は 1 m あたりで持ち、1 歩（1 セル）ぶんに直す。
        level.params1[2] = std::pow(std::max(0.0f, 1.0f - evaporation), cellMeters);
        level.params2[0] = kDeltaCapFactor * cellMeters;
        level.params2[1] = std::clamp(kEdgeFadeMeters / std::max(cellMeters, 1e-3f), 2.0f,
                                      static_cast<float>(n) * 0.25f);
        D3D12_GPU_VIRTUAL_ADDRESS levelAddress = 0;
        if (!upload(level, levelAddress)) {
            PIXEndEvent(commandList);
            return false;
        }
        commandList->SetComputeRootConstantBufferView(1, levelAddress);
        const uint32_t groups = DispatchCount(n);
        if (levelIndex == 0) {
            run(initPass, groups);
        } else {
            run(copyToSrcPass, DispatchCount(levels[levelIndex - 1]));
            run(upsamplePass, groups);
        }
        run(clearLevelPass, groups);

        for (int iteration = 0; iteration < iterations; ++iteration) {
            DropletConstants constants = level;
            constants.indices3[2] = static_cast<uint32_t>(iteration);
            D3D12_GPU_VIRTUAL_ADDRESS address = 0;
            if (!upload(constants, address)) {
                PIXEndEvent(commandList);
                return false;
            }
            commandList->SetComputeRootConstantBufferView(1, address);
            run(clearIterPass, groups);
            commandList->SetPipelineState(tracePass);
            commandList->Dispatch((static_cast<uint32_t>(particlesPerIteration) + 63u) / 64u, 1,
                                  1);
            barrier();
            run(applyPass, groups);
        }
    }

    // 合成解像度へ足し戻す。**Mask だけが目的のときは Height に書かない。**
    if (!layer.maskOnly) {
        TransitionIfNeeded(commandList, m_droplet.heights,
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        TransitionIfNeeded(commandList, m_droplet.original,
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        TransitionIfNeeded(commandList, m_textures.height, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        run(resolvePass, DispatchCount(m_resolution));
        // 形が変わったので、法線も作り直す。
        RebuildNormalsFromHeight(device, normalPass, commandList, stack);
        TransitionIfNeeded(commandList, m_droplet.heights, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        TransitionIfNeeded(commandList, m_droplet.original, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    // 次のレイヤーは Height を UAV として書く。Mask だけのときも戻す。
    TransitionIfNeeded(commandList, m_textures.height, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    PIXEndEvent(commandList);
    return true;
}

// 直前の水滴侵食レイヤーが残した流量 / 堆積を、マスクとして焼く。
bool MaterialEvaluator::ApplyDropletMask(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                                         ID3D12GraphicsCommandList* commandList,
                                         const MaskOp& op, rhi::GpuTexture& target) {
    if (!m_droplet.IsValid() || !target.IsValid()) {
        return false;
    }
    const auto pipeline = [&](const wchar_t* entry) {
        return pipelineCache.GetCompute(L"CompositeDroplet.hlsl", entry);
    };
    ID3D12PipelineState* clearPass = pipeline(L"CsMaskMaxClear");
    ID3D12PipelineState* reducePass = pipeline(L"CsMaskMaxReduce");
    ID3D12PipelineState* maskPass = pipeline(L"CsMask");
    if (clearPass == nullptr || reducePass == nullptr || maskPass == nullptr) {
        return false;
    }

    DropletConstants constants = {};
    constants.indices3[3] = m_resolution;
    constants.indices4[0] = m_droplet.maxScratch.UavIndex();
    constants.indices4[1] = target.UavIndex();
    constants.indices4[2] = (op.dropletMask.channel == 0u) ? 0u : 1u;
    constants.indices4[3] = m_droplet.resolution;
    constants.indices5[2] = m_droplet.flow.SrvIndex();
    constants.indices5[3] = m_droplet.deposit.SrvIndex();
    const rhi::UploadAllocation cb = AllocateConstants(device, sizeof(DropletConstants));
    if (!cb.IsValid()) {
        return false;
    }
    std::memcpy(cb.cpu, &constants, sizeof(constants));

    PIXBeginEvent(commandList, PIX_COLOR(90, 150, 200), "CompositeDropletMask");
    TransitionIfNeeded(commandList, m_droplet.flow, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    TransitionIfNeeded(commandList, m_droplet.deposit,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    TransitionIfNeeded(commandList, m_droplet.maxScratch, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    TransitionIfNeeded(commandList, target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    commandList->SetComputeRootConstantBufferView(1, cb.gpuAddress);
    const auto barrier = [&]() {
        const D3D12_RESOURCE_BARRIER uav = CD3DX12_RESOURCE_BARRIER::UAV(nullptr);
        commandList->ResourceBarrier(1, &uav);
    };
    commandList->SetPipelineState(clearPass);
    commandList->Dispatch(1, 1, 1);
    barrier();
    commandList->SetPipelineState(reducePass);
    commandList->Dispatch(DispatchCount(m_droplet.resolution), DispatchCount(m_droplet.resolution),
                          1);
    barrier();
    commandList->SetPipelineState(maskPass);
    commandList->Dispatch(DispatchCount(m_resolution), DispatchCount(m_resolution), 1);
    barrier();

    // 次に使うときは書き込みへ戻す。
    TransitionIfNeeded(commandList, m_droplet.flow, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    TransitionIfNeeded(commandList, m_droplet.deposit, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    TransitionIfNeeded(commandList, target, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    PIXEndEvent(commandList);
    return true;
}

bool MaterialEvaluator::ApplyCrumbling(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                                       ID3D12GraphicsCommandList* commandList,
                                       const MaterialLayer& layer, const MaterialStack& stack,
                                       uint32_t emissionIndex) {
    if (!EnsureCrumblingResources(device, m_resolution)) {
        return false;
    }
    const auto pipeline = [&](const wchar_t* entry) {
        return pipelineCache.GetCompute(L"CompositeCrumbling.hlsl", entry);
    };
    ID3D12PipelineState* clearPass = pipeline(L"CsClear");
    ID3D12PipelineState* scatterPass = pipeline(L"CsScatter");
    ID3D12PipelineState* resolvePass = pipeline(L"CsResolve");
    ID3D12PipelineState* normalPass =
        pipelineCache.GetCompute(L"CompositeBlur.hlsl", L"CsNormalFromHeight");
    if (clearPass == nullptr || scatterPass == nullptr || resolvePass == nullptr) {
        return false;
    }

    const MaterialLayer::CrumblingSettings& params = layer.crumbling;
    const float sizeMeters = (stack.SizeMeters() > 0.0f) ? stack.SizeMeters() : 1.0f;
    const float heightMeters = (stack.HeightMeters() > 0.0f) ? stack.HeightMeters() : 1.0f;
    const float texelMeters = sizeMeters / static_cast<float>(std::max(1u, m_resolution));
    const float amount = std::clamp(params.amount, 0.0f, 1.0f);

    // 岩片の直径は m で持つ。テクセル数へ直す（1 テクセルより小さい岩は描けない）。
    const float minMeters = std::clamp(params.sizeMinMeters, 0.01f, 10000.0f);
    const float maxMeters = std::clamp(std::max(params.sizeMaxMeters, minMeters), 0.01f, 10000.0f);
    const float minTexels = std::max(0.5f, minMeters / std::max(texelMeters, 1e-6f));
    const float maxTexels = std::max(minTexels, maxMeters / std::max(texelMeters, 1e-6f));

    // 試行回数。**受け入れるかどうかは発生源マスクの明るさ**で決まる。
    //
    // **地形の面積に比例させる。** terrain-editor は解像度によらない固定数だが、
    // 実寸で地形を扱うここでは、同じ量でも 1km 四方と 4km 四方で岩屑の密度が
    // 変わってしまう。1km 四方あたりの数として持ち、面積を掛ける。
    const float areaKm2 = (sizeMeters / 1000.0f) * (sizeMeters / 1000.0f);
    const int attempts = std::clamp(
        static_cast<int>(std::round((256.0f + amount * 12000.0f) * areaKm2)), 0, 200000);
    // 岩片の高さの最大。パックの分母になるので、実際に出る最大と揃えること。
    const float maxDebrisMeters = maxMeters * (0.10f + 0.18f * amount) * 1.35f;

    CrumblingConstants constants = {};
    constants.indices0[0] = m_crumbling.packed.UavIndex();
    constants.indices0[1] = m_textures.height.SrvIndex();
    constants.indices0[2] = emissionIndex;
    constants.indices0[3] = m_textures.height.UavIndex();
    constants.indices1[0] = m_resolution;
    constants.indices1[1] = static_cast<uint32_t>(attempts);
    constants.indices1[2] = static_cast<uint32_t>(std::clamp(params.physicsCount, 0, 512));
    constants.indices1[3] = static_cast<uint32_t>(params.style);
    constants.params0[0] = minTexels;
    constants.params0[1] = maxTexels;
    constants.params0[2] = std::clamp(params.gravity, 0.0f, 1.0f);
    constants.params0[3] = std::clamp(params.spread, 0.0f, 1.0f);
    constants.params1[0] = maxDebrisMeters / heightMeters;
    constants.params1[1] = static_cast<float>(params.seed);
    constants.params1[2] = texelMeters;
    constants.params1[3] = heightMeters;
    constants.params2[0] = amount;

    const rhi::UploadAllocation cb = AllocateConstants(device, sizeof(CrumblingConstants));
    if (!cb.IsValid()) {
        return false;
    }
    std::memcpy(cb.cpu, &constants, sizeof(constants));

    PIXBeginEvent(commandList, PIX_COLOR(150, 130, 110), "CompositeCrumbling");
    commandList->SetComputeRootConstantBufferView(1, cb.gpuAddress);
    const auto barrier = [&]() {
        const D3D12_RESOURCE_BARRIER uav = CD3DX12_RESOURCE_BARRIER::UAV(nullptr);
        commandList->ResourceBarrier(1, &uav);
    };

    TransitionIfNeeded(commandList, m_crumbling.packed, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    commandList->SetPipelineState(clearPass);
    commandList->Dispatch(DispatchCount(m_resolution), DispatchCount(m_resolution), 1);
    barrier();

    if (attempts > 0 && amount > 0.0f) {
        // 歩行は Height を読むだけ。読み取り専用にしてから走らせる。
        TransitionIfNeeded(commandList, m_textures.height,
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        commandList->SetPipelineState(scatterPass);
        commandList->Dispatch((static_cast<uint32_t>(attempts) + 63u) / 64u, 1, 1);
        barrier();

        // 積んだぶんを足し戻す。ここからは Height へ書く。
        // **Mask だけが目的のときは足し戻さない**（Result を繋いでいない）。
        // 岩屑は作業用テクスチャに残るので、マスクはこの後で焼ける。
        if (!layer.maskOnly) {
            TransitionIfNeeded(commandList, m_textures.height,
                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            commandList->SetPipelineState(resolvePass);
            commandList->Dispatch(DispatchCount(m_resolution), DispatchCount(m_resolution), 1);
            barrier();
        }
    }
    // 次のレイヤーは Height を UAV として書く。Mask だけのときも戻す（堆積と同じ）。
    TransitionIfNeeded(commandList, m_textures.height, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    PIXEndEvent(commandList);

    // 形が変わったので、法線も作り直す。足し戻していないなら形は変わっていない。
    if (normalPass != nullptr && !layer.maskOnly) {
        RebuildNormalsFromHeight(device, normalPass, commandList, stack);
    }
    return true;
}

// 直前の崩落レイヤーが積んだ岩屑を、マスクとして焼く。
bool MaterialEvaluator::ApplyCrumblingMask(rhi::Device& device,
                                           rhi::PipelineCache& pipelineCache,
                                           ID3D12GraphicsCommandList* commandList,
                                           const MaskOp& op, rhi::GpuTexture& target) {
    if (!m_crumbling.IsValid() || !target.IsValid()) {
        return false;
    }
    ID3D12PipelineState* maskPass =
        pipelineCache.GetCompute(L"CompositeCrumbling.hlsl", L"CsMask");
    if (maskPass == nullptr) {
        return false;
    }

    CrumblingConstants constants = {};
    constants.indices0[0] = m_crumbling.packed.SrvIndex();
    constants.indices1[0] = m_crumbling.resolution;
    constants.indices2[0] = target.UavIndex();
    constants.indices2[1] = (op.crumblingMask.channel == 0u) ? 0u : 1u;

    const rhi::UploadAllocation cb = AllocateConstants(device, sizeof(CrumblingConstants));
    if (!cb.IsValid()) {
        return false;
    }
    std::memcpy(cb.cpu, &constants, sizeof(constants));

    PIXBeginEvent(commandList, PIX_COLOR(150, 130, 110), "CompositeCrumblingMask");
    TransitionIfNeeded(commandList, m_crumbling.packed,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    TransitionIfNeeded(commandList, target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    commandList->SetComputeRootConstantBufferView(1, cb.gpuAddress);
    commandList->SetPipelineState(maskPass);
    commandList->Dispatch(DispatchCount(m_crumbling.resolution),
                          DispatchCount(m_crumbling.resolution), 1);
    const D3D12_RESOURCE_BARRIER uav = CD3DX12_RESOURCE_BARRIER::UAV(nullptr);
    commandList->ResourceBarrier(1, &uav);

    // 次に使うときは書き込みへ戻す。
    TransitionIfNeeded(commandList, m_crumbling.packed, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    TransitionIfNeeded(commandList, target, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    PIXEndEvent(commandList);
    return true;
}

// 散布。terrain-editor の Scatter の移植。段取りはシェーダの頭。
//
//   近くの散布点から形を決める（差分とパック済みの形 / 乱数を書く）
//   → 差分を Height へ足す → 法線を作り直す
bool MaterialEvaluator::ApplyScatter(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                                     ID3D12GraphicsCommandList* commandList,
                                     const MaterialLayer& layer, const MaterialStack& stack,
                                     uint32_t placementIndex) {
    if (!EnsureScatterResources(device, m_resolution)) {
        return false;
    }
    const auto pipeline = [&](const wchar_t* entry) {
        return pipelineCache.GetCompute(L"CompositeScatter.hlsl", entry);
    };
    ID3D12PipelineState* scatterPass = pipeline(L"CsScatter");
    ID3D12PipelineState* resolvePass = pipeline(L"CsResolve");
    ID3D12PipelineState* normalPass =
        pipelineCache.GetCompute(L"CompositeBlur.hlsl", L"CsNormalFromHeight");
    if (scatterPass == nullptr || resolvePass == nullptr) {
        return false;
    }

    const MaterialLayer::ScatterSettings& params = layer.scatter;
    const float sizeMeters = (stack.SizeMeters() > 0.0f) ? stack.SizeMeters() : 1.0f;
    const float heightMeters = (stack.HeightMeters() > 0.0f) ? stack.HeightMeters() : 1.0f;
    const float texelMeters = sizeMeters / static_cast<float>(std::max(1u, m_resolution));

    // 大きさは m で持つ。散布の格子（間隔 = density）を単位に直して渡す。
    // terrain-editor と同じ換算にしてあるので、同じ設定なら同じ分布になる。
    const float density = std::clamp(params.densityMeters, 0.1f, 10000.0f);
    const float minMeters = std::clamp(params.sizeMinMeters, 0.1f, 2000.0f);
    const float maxMeters = std::clamp(std::max(params.sizeMaxMeters, minMeters), 0.1f, 2000.0f);
    const float sizeMinCells = minMeters / density;
    const float sizeMaxCells = maxMeters / density;
    const float aspectVariation = std::clamp(params.aspectVariation, 0.0f, 1.0f);
    // 届く範囲は「一番大きい個体を一番細長くしたとき」の半径。
    // 探索するマスの数はここから決まる（遠すぎるマスは形が届かない）。
    const float maxReach = (sizeMaxCells * 0.5f) * std::pow(2.0f, aspectVariation);
    const int searchRadius =
        std::clamp(static_cast<int>(std::ceil(maxReach - 0.05f)), 1, 32);

    ScatterConstants constants = {};
    constants.indices0[0] = m_textures.height.SrvIndex();
    constants.indices0[1] = m_scatter.delta.UavIndex();
    constants.indices0[2] = placementIndex;
    constants.indices0[3] = m_scatter.packed.UavIndex();
    constants.indices1[0] = m_resolution;
    constants.indices1[1] = static_cast<uint32_t>(params.shape);
    constants.indices1[2] = static_cast<uint32_t>(params.orientation);
    constants.indices1[3] = static_cast<uint32_t>(searchRadius);
    constants.indices2[0] = m_textures.height.UavIndex();
    constants.params0[0] = density;
    constants.params0[1] = std::clamp(params.coverage, 0.0f, 1.0f);
    constants.params0[2] = sizeMinCells;
    constants.params0[3] = sizeMaxCells;
    // 高さは m。Height は正規化なので、標高差で割ってから渡す。
    constants.params1[0] = std::max(params.heightMeters, 0.0f) / heightMeters;
    constants.params1[1] = std::clamp(params.heightJitter, 0.0f, 1.0f);
    constants.params1[2] = std::clamp(params.rotationVariation, 0.0f, 1.0f);
    constants.params1[3] = aspectVariation;
    constants.params2[0] = maxReach;
    constants.params2[1] = static_cast<float>(params.seed);
    constants.params2[2] = texelMeters;
    constants.params2[3] = heightMeters;
    constants.params3[0] = std::clamp(params.smoothness, 0.0f, 1.0f);

    const rhi::UploadAllocation cb = AllocateConstants(device, sizeof(ScatterConstants));
    if (!cb.IsValid()) {
        return false;
    }
    std::memcpy(cb.cpu, &constants, sizeof(constants));

    PIXBeginEvent(commandList, PIX_COLOR(120, 170, 110), "CompositeScatter");
    commandList->SetComputeRootConstantBufferView(1, cb.gpuAddress);
    const auto barrier = [&]() {
        const D3D12_RESOURCE_BARRIER uav = CD3DX12_RESOURCE_BARRIER::UAV(nullptr);
        commandList->ResourceBarrier(1, &uav);
    };

    // 形を決めるパスは Height を読むだけ。読み取り専用にしてから走らせる。
    TransitionIfNeeded(commandList, m_textures.height,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    TransitionIfNeeded(commandList, m_scatter.packed, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    TransitionIfNeeded(commandList, m_scatter.delta, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    commandList->SetPipelineState(scatterPass);
    commandList->Dispatch(DispatchCount(m_resolution), DispatchCount(m_resolution), 1);
    barrier();

    // 差分を足し戻す。**Mask だけが目的のときは足さない**（Result を繋いでいない）。
    // 形はパック済みのテクスチャに残るので、マスクはこの後で焼ける。
    if (!layer.maskOnly) {
        TransitionIfNeeded(commandList, m_textures.height, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        commandList->SetPipelineState(resolvePass);
        commandList->Dispatch(DispatchCount(m_resolution), DispatchCount(m_resolution), 1);
        barrier();
    }
    // 次のレイヤーは Height を UAV として書く。Mask だけのときも戻す（崩落と同じ）。
    TransitionIfNeeded(commandList, m_textures.height, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    PIXEndEvent(commandList);

    // 形が変わったので、法線も作り直す。足し戻していないなら形は変わっていない。
    if (normalPass != nullptr && !layer.maskOnly) {
        RebuildNormalsFromHeight(device, normalPass, commandList, stack);
    }
    return true;
}

// 直前の散布レイヤーが置いた形 / 乱数を、マスクとして焼く。
bool MaterialEvaluator::ApplyScatterMask(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                                         ID3D12GraphicsCommandList* commandList,
                                         const MaskOp& op, rhi::GpuTexture& target) {
    if (!m_scatter.IsValid() || !target.IsValid()) {
        return false;
    }
    ID3D12PipelineState* maskPass = pipelineCache.GetCompute(L"CompositeScatter.hlsl", L"CsMask");
    if (maskPass == nullptr) {
        return false;
    }

    ScatterConstants constants = {};
    constants.indices0[3] = m_scatter.packed.SrvIndex();
    constants.indices1[0] = m_scatter.resolution;
    constants.indices2[1] = target.UavIndex();
    constants.indices2[2] = (op.scatterMask.channel == 0u) ? 0u : 1u;

    const rhi::UploadAllocation cb = AllocateConstants(device, sizeof(ScatterConstants));
    if (!cb.IsValid()) {
        return false;
    }
    std::memcpy(cb.cpu, &constants, sizeof(constants));

    PIXBeginEvent(commandList, PIX_COLOR(120, 170, 110), "CompositeScatterMask");
    TransitionIfNeeded(commandList, m_scatter.packed,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    TransitionIfNeeded(commandList, target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    commandList->SetComputeRootConstantBufferView(1, cb.gpuAddress);
    commandList->SetPipelineState(maskPass);
    commandList->Dispatch(DispatchCount(m_scatter.resolution),
                          DispatchCount(m_scatter.resolution), 1);
    const D3D12_RESOURCE_BARRIER uav = CD3DX12_RESOURCE_BARRIER::UAV(nullptr);
    commandList->ResourceBarrier(1, &uav);

    // 次に使うときは書き込みへ戻す。
    TransitionIfNeeded(commandList, m_scatter.packed, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    TransitionIfNeeded(commandList, target, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    PIXEndEvent(commandList);
    return true;
}

bool MaterialEvaluator::ApplyHeightBlur(rhi::Device& device,
                                       ID3D12GraphicsCommandList* commandList,
                                       ID3D12PipelineState* blurPipeline,
                                       ID3D12PipelineState* normalPipeline,
                                       const MaterialLayer& layer, const MaterialStack& stack,
                                       const std::vector<TileRect>& tiles,
                                       uint32_t maskIndex) {
    // 半径は実寸（m）で持っている。合成解像度を変えても効きが変わらないように
    // するため、ここでテクセル数へ直す。
    const float sizeMeters = (stack.SizeMeters() > 0.0f) ? stack.SizeMeters() : 1.0f;
    const float radiusTexels =
        layer.blur.radiusMeters / sizeMeters * static_cast<float>(m_resolution);
    const float strength = std::clamp(layer.blur.strength, 0.0f, 1.0f);
    const int iterations = std::clamp(layer.blur.iterations, 1, 16);
    // テクセル 1 つに満たない半径や強さ 0 は何も変えない。パスごと省く。
    if (radiusTexels < 0.5f || strength <= 0.0f) {
        return true;
    }

    const float heightPerSize = (stack.SizeMeters() > 0.0f)
                                    ? (stack.HeightMeters() / stack.SizeMeters())
                                    : 0.0f;

    bool complete = true;
    PIXBeginEvent(commandList, PIX_COLOR(120, 160, 220), "CompositeBlur");

    // 1 パスぶん。**全タイルを回してから**呼び出し側が次のパスへ進むこと。
    const auto dispatchPass = [&](ID3D12PipelineState* pipeline, uint32_t sourceIndex,
                                  uint32_t outputIndex, uint32_t axis, float passStrength,
                                  uint32_t passMaskIndex) {
        commandList->SetPipelineState(pipeline);
        for (const TileRect& tile : tiles) {
            BlurConstants constants = {};
            constants.sourceIndex = sourceIndex;
            constants.outputIndex = outputIndex;
            constants.axis = axis;
            constants.radiusTexels = radiusTexels;
            constants.tile[0] = tile.x;
            constants.tile[1] = tile.y;
            constants.tile[2] = tile.width;
            constants.tile[3] = tile.height;
            constants.resolution[0] = m_resolution;
            constants.resolution[1] = m_resolution;
            constants.strength = passStrength;
            constants.heightPerSize = heightPerSize;
            constants.maskIndex = passMaskIndex;

            const rhi::UploadAllocation cb = AllocateConstants(device, sizeof(BlurConstants));
            if (!cb.IsValid()) {
                complete = false;
                break;
            }
            std::memcpy(cb.cpu, &constants, sizeof(constants));

            commandList->SetComputeRootConstantBufferView(1, cb.gpuAddress);
            commandList->Dispatch(DispatchCount(tile.width), DispatchCount(tile.height), 1);
        }
    };

    for (int iteration = 0; iteration < iterations; ++iteration) {
        // 水平: Height（読み取り）→ 作業用。中間結果なので混ぜない。
        TransitionIfNeeded(commandList, m_textures.height,
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        TransitionIfNeeded(commandList, m_scratch,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        // マスクは混ぜる垂直パスで掛ける。水平パスは中間結果なので効かせない。
        dispatchPass(blurPipeline, m_textures.height.SrvIndex(), m_scratch.UavIndex(),
                     0u, 1.0f, kInvalidTextureIndex);

        // 垂直: 作業用（読み取り）→ Height。ここで元の高さと強さで混ぜる。
        TransitionIfNeeded(commandList, m_scratch,
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        TransitionIfNeeded(commandList, m_textures.height,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        dispatchPass(blurPipeline, m_scratch.SrvIndex(), m_textures.height.UavIndex(),
                     1u, strength, maskIndex);
    }

    // ぼかした形から法線を作り直す。**Height だけをぼかすと形と陰影が食い違う。**
    // ここにもマスクを渡す。渡さないと、高さが変わっていない所まで法線が
    // Height から作り直され、素材の法線ディテールが全面で消える。
    TransitionIfNeeded(commandList, m_textures.height,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    dispatchPass(normalPipeline, m_textures.height.SrvIndex(), m_textures.normal.UavIndex(), 0u,
                 1.0f, maskIndex);

    // 次のレイヤーは Height を UAV として書き換えるので、状態を戻しておく。
    TransitionIfNeeded(commandList, m_textures.height, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const D3D12_RESOURCE_BARRIER barriers[] = {
        CD3DX12_RESOURCE_BARRIER::UAV(m_textures.normal.resource.Get()),
    };
    commandList->ResourceBarrier(_countof(barriers), barriers);

    PIXEndEvent(commandList);
    return complete;
}

bool MaterialEvaluator::Evaluate(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                                 ID3D12GraphicsCommandList* commandList,
                                 const MaterialStack& stack, const TextureLibrary& textures,
                                 const MaterialLibrary& materials,
                                 const PaintMaskStore& paintMasks,
                                 const std::vector<TileRect>& tiles) {
    if (!m_textures.IsValid() || tiles.empty()) {
        return false;
    }

    ID3D12PipelineState* layerPipeline =
        pipelineCache.GetCompute(L"CompositeLayer.hlsl", L"CsMain");
    ID3D12PipelineState* maskPipeline = pipelineCache.GetCompute(L"CompositeMask.hlsl", L"CsMain");
    // ブラー（分離型ガウス）と、ぼかした後の法線の作り直し。
    ID3D12PipelineState* blurPipeline =
        pipelineCache.GetCompute(L"CompositeBlur.hlsl", L"CsBlur");
    ID3D12PipelineState* blurNormalPipeline =
        pipelineCache.GetCompute(L"CompositeBlur.hlsl", L"CsNormalFromHeight");
    // マスクのサムネイルは合成と同じ定数を使うので、同じシェーダの別エントリ。
    ID3D12PipelineState* thumbnailPipeline =
        pipelineCache.GetCompute(L"CompositeLayer.hlsl", L"CsMaskThumbnail");
    if (layerPipeline == nullptr || maskPipeline == nullptr) {
        return false;
    }

    // 有効なレイヤーが 1 枚も無いときは npos。合成はしないが、
    // マスクのサムネイルはレイヤーの有無に関わらず作り直す。
    const size_t baseIndex = stack.FirstEnabledIndex();

    bool complete = true;

    PIXBeginEvent(commandList, PIX_COLOR(220, 140, 60), "CompositeStack");

    commandList->SetComputeRootSignature(pipelineCache.GlobalRootSignature());

    TransitionIfNeeded(commandList, m_textures.baseColor, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    TransitionIfNeeded(commandList, m_textures.normal, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    TransitionIfNeeded(commandList, m_textures.surface, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    TransitionIfNeeded(commandList, m_textures.height, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    EnsureMaskThumbnails(device, stack.Layers().size());
    for (rhi::GpuTexture& thumbnail : m_maskThumbnails) {
        TransitionIfNeeded(commandList, thumbnail, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    // --- マスクの段取り -----------------------------------------------------
    // マスクはノードグラフを落とした op の列（`MaskProgram`）で来る。
    // 高さを読む op（川筋 / 傾斜）は「レイヤー列のどこまで合成した Height を
    // 使うか」を持つので、**その位置を通過した時点**で焼く。
    // それ以外（画像 / レベル / 合成）は入力が揃った時点で焼ける。
    const MaskProgram& maskOps = stack.MaskOps();
    std::vector<int> maskOpComputeAfter(maskOps.size(), -1);
    std::vector<uint64_t> maskOpHash(maskOps.size(), 0);
    std::vector<bool> maskOpDone(maskOps.size(), false);
    bool maskOpsReady = maskOps.empty();
    if (!maskOps.empty()) {
        maskOpsReady = EnsureMaskOpTextures(device, maskOps);
        if (!maskOpsReady) {
            complete = false;
        }

        // 「そのレイヤーまで合成した結果」の巡回ハッシュ。**Height に効く値だけ**
        // を混ぜるので、色やラフネスを触っても川筋は焼き直さずに済む。
        std::vector<uint64_t> heightStateHash(stack.Layers().size() + 1, 0xcbf29ce484222325ull);
        for (size_t i = 0; i < stack.Layers().size(); ++i) {
            heightStateHash[i + 1] = HashHeightState(heightStateHash[i], stack.Layers()[i]);
        }
        // 実寸はマスクの効き方（傾斜の角度、川筋の半径）に入るのでハッシュに混ぜる。
        const float sizeMeters = stack.SizeMeters();
        const float heightMeters = stack.HeightMeters();
        uint64_t scaleHash = HashBytes(0xcbf29ce484222325ull, &sizeMeters, sizeof(float));
        scaleHash = HashBytes(scaleHash, &heightMeters, sizeof(float));

        for (size_t i = 0; i < maskOps.size(); ++i) {
            const MaskOp& op = maskOps[i];
            int after = -1;
            uint64_t hash = HashBytes(0xcbf29ce484222325ull, &op.kind, sizeof(op.kind));
            hash = HashMaskOpParams(hash, op);
            hash = HashBytes(hash, &scaleHash, sizeof(scaleHash));
            hash = HashBytes(hash, &m_maskOpResolutions[i], sizeof(uint32_t));
            if (op.kind == MaskOpKind::Fluvial || op.kind == MaskOpKind::Slope ||
                op.kind == MaskOpKind::Curvature || op.kind == MaskOpKind::Sediment ||
                op.kind == MaskOpKind::Crumbling || op.kind == MaskOpKind::Snow ||
                op.kind == MaskOpKind::Height || op.kind == MaskOpKind::River ||
                op.kind == MaskOpKind::Droplet) {
                after = std::max(after, op.heightSourceLayer);
                const size_t layerCount = std::min<size_t>(
                    stack.Layers().size(),
                    (op.heightSourceLayer >= 0) ? (op.heightSourceLayer + 1) : 0);
                hash = HashBytes(hash, &heightStateHash[layerCount], sizeof(uint64_t));
            }
            for (const int input : {op.inputA, op.inputB}) {
                if (input >= 0 && static_cast<size_t>(input) < maskOps.size()) {
                    after = std::max(after, maskOpComputeAfter[input]);
                    hash = HashBytes(hash, &maskOpHash[input], sizeof(uint64_t));
                }
            }
            // 下地より前は何も合成されていない。そこまで戻ることはできない。
            if (after >= 0 && baseIndex != static_cast<size_t>(-1)) {
                after = std::max(after, static_cast<int>(baseIndex));
            }
            maskOpComputeAfter[i] = after;
            maskOpHash[i] = hash;

            // **入力が前回と同じなら焼き直さない。** 川筋のように重い op を、
            // 無関係な編集のたびに走らせないための仕組み。
            if (maskOpsReady && m_maskOpHashes[i] == hash && m_maskOpTextures[i].IsValid()) {
                maskOpDone[i] = true;
            }
        }
    }

    // 指定した位置（-1 はループ前）で焼ける op を、添字の順に走らせる。
    // op は自分より前だけを入力にするので、この順で必ず入力が揃う。
    const auto runMaskOps = [&](int afterLayerIndex) {
        if (!maskOpsReady) {
            return;
        }
        for (size_t i = 0; i < maskOps.size(); ++i) {
            if (maskOpDone[i] || maskOpComputeAfter[i] != afterLayerIndex) {
                continue;
            }
            if (RunMaskOp(device, pipelineCache, commandList, maskOps, i, stack, textures)) {
                maskOpDone[i] = true;
                m_maskOpHashes[i] = maskOpHash[i];
            } else {
                // 焼けなければマスクは定数へ落ちる（絵は出るが模様は出ない）。
                m_maskOpHashes[i] = 0;
                complete = false;
            }
        }
    };
    runMaskOps(-1);

    m_evaluatedLayerCount = 0;
    m_evaluatedTileCount = static_cast<uint32_t>(tiles.size());

    // レイヤー優先で回す。中間結果由来のマスクは近傍を参照するため、
    // タイル優先で回すと隣のタイルの未評価の値を読んでしまう。
    for (size_t layerIndex = 0; layerIndex < stack.Layers().size(); ++layerIndex) {
        const MaterialLayer& layer = stack.Layers()[layerIndex];
        const bool isBaseLayer = (layerIndex == baseIndex);

        // ブラーと堆積は合成しない**加工**。下地のハイトを書き換えて法線を作り直す。
        // 下に合成済みのレイヤーが無いと相手がいないので、その場合は素通り。
        if (IsHeightOperationKind(layer.kind)) {
            const bool hasUnderlying = (baseIndex != static_cast<size_t>(-1)) &&
                                       (layerIndex > baseIndex);
            // 崩落と河川は Mask 入力を**発生源 / 川の出どころ**として使うので、先に引いておく。
            // 段取り上、ここへ来る時点でその op は焼き終わっている。
            uint32_t inputMaskIndex = kInvalidTextureIndex;
            if (layer.mask.source == MaskSource::Node && layer.mask.maskOp >= 0 &&
                static_cast<size_t>(layer.mask.maskOp) < maskOps.size() &&
                maskOpDone[static_cast<size_t>(layer.mask.maskOp)]) {
                inputMaskIndex =
                    m_maskOpTextures[static_cast<size_t>(layer.mask.maskOp)].SrvIndex();
            }
            if (layer.enabled && hasUnderlying && layer.kind == LayerKind::Crumbling) {
                if (!ApplyCrumbling(device, pipelineCache, commandList, layer, stack,
                                    inputMaskIndex)) {
                    complete = false;
                }
                ++m_evaluatedLayerCount;
            } else if (layer.enabled && hasUnderlying && layer.kind == LayerKind::Scatter) {
                if (!ApplyScatter(device, pipelineCache, commandList, layer, stack,
                                  inputMaskIndex)) {
                    complete = false;
                }
                ++m_evaluatedLayerCount;
            } else if (layer.enabled && hasUnderlying && layer.kind == LayerKind::River) {
                if (!ApplyRiver(device, pipelineCache, commandList, layer, stack,
                                inputMaskIndex)) {
                    complete = false;
                }
                ++m_evaluatedLayerCount;
            } else if (layer.enabled && hasUnderlying && layer.kind == LayerKind::Droplet) {
                if (!ApplyDroplet(device, pipelineCache, commandList, layer, stack)) {
                    complete = false;
                }
                ++m_evaluatedLayerCount;
            } else if (layer.enabled && hasUnderlying && layer.kind == LayerKind::Sediment) {
                if (!ApplySediment(device, pipelineCache, commandList, layer, stack)) {
                    complete = false;
                }
                ++m_evaluatedLayerCount;
            } else if (layer.enabled && hasUnderlying && layer.kind == LayerKind::Snow) {
                if (!ApplySnow(device, pipelineCache, commandList, layer, stack)) {
                    complete = false;
                }
                ++m_evaluatedLayerCount;
            } else if (layer.enabled && hasUnderlying && blurPipeline != nullptr &&
                       blurNormalPipeline != nullptr) {
                if (!ApplyHeightBlur(device, commandList, blurPipeline, blurNormalPipeline,
                                     layer, stack, tiles, inputMaskIndex)) {
                    complete = false;
                }
                ++m_evaluatedLayerCount;
            }
            // ぼかした後の Height を見るマスクがあれば、ここで焼く。
            runMaskOps(static_cast<int>(layerIndex));
            continue;
        }
        // 下地のレイヤーには合成する相手がいないので、中間結果由来のマスクは使えない。
        const bool useDerivedMask =
            layer.enabled && !isBaseLayer && IsDerivedMaskSource(layer.mask.source);
        // ノードのマスクは**段取りで既に焼いてある**（op の結果テクスチャ）。
        const bool useNodeMask = layer.enabled && !isBaseLayer &&
                                 layer.mask.source == MaskSource::Node &&
                                 layer.mask.maskOp >= 0 &&
                                 static_cast<size_t>(layer.mask.maskOp) < maskOps.size() &&
                                 maskOpDone[static_cast<size_t>(layer.mask.maskOp)];

        if (useDerivedMask) {
            PIXBeginEvent(commandList, PIX_COLOR(200, 200, 80), "CompositeMask");

            // Height を読み取り専用にしてからマスクを計算する。
            TransitionIfNeeded(commandList, m_textures.height,
                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            TransitionIfNeeded(commandList, m_scratch,
                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

            commandList->SetPipelineState(maskPipeline);
            for (const TileRect& tile : tiles) {
                MaskConstants constants = {};
                constants.heightIndex = m_textures.height.SrvIndex();
                constants.outputIndex = m_scratch.UavIndex();
                constants.source = static_cast<uint32_t>(layer.mask.source);
                constants.derivedScale = layer.mask.derivedScale;
                constants.tile[0] = tile.x;
                constants.tile[1] = tile.y;
                constants.tile[2] = tile.width;
                constants.tile[3] = tile.height;
                constants.resolution[0] = m_resolution;
                constants.resolution[1] = m_resolution;

                const rhi::UploadAllocation cb =
                    AllocateConstants(device, sizeof(MaskConstants));
                if (!cb.IsValid()) {
                    complete = false;
                    break;
                }
                std::memcpy(cb.cpu, &constants, sizeof(constants));

                commandList->SetComputeRootConstantBufferView(1, cb.gpuAddress);
                commandList->Dispatch(DispatchCount(tile.width), DispatchCount(tile.height), 1);
            }

            // マスクを読み取りに、Height を書き込みに戻す。
            TransitionIfNeeded(commandList, m_scratch,
                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            TransitionIfNeeded(commandList, m_textures.height,
                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

            PIXEndEvent(commandList);
        }

        // タイルごとに変わるのは矩形だけなので、定数はレイヤー 1 枚につき 1 回組む。
        // マスクのサムネイルも同じ値を使う（差し替えるのは出力・解像度・矩形だけ）。
        LayerConstants constants = {};
        constants.outputIndices[0] = m_textures.baseColor.UavIndex();
        constants.outputIndices[1] = m_textures.normal.UavIndex();
        constants.outputIndices[2] = m_textures.surface.UavIndex();
        constants.outputIndices[3] = m_textures.height.UavIndex();

        constants.resolution[0] = m_resolution;
        constants.resolution[1] = m_resolution;

        // シェイプは形（Height と、それに追従する Normal）だけを書く。
        // BaseColor / Surface を書かせると「地形の起伏」の役割からはみ出す。
        uint32_t channelMask = layer.channelMask;
        if (layer.kind == LayerKind::Shape) {
            channelMask = ChannelBit(Channel::Normal) | ChannelBit(Channel::Height);
        }
        // 一番下のレイヤーは下地なので、必ず全チャンネルを埋める。
        // そうしないと未初期化のテクセルが残る。
        constants.channelMask = isBaseLayer ? kAllChannelBits : channelMask;

        constants.flags = 0;
        if (layer.mask.invert) {
            constants.flags |= kFlagMaskInvert;
        }
        if (isBaseLayer) {
            constants.flags |= kFlagBaseLayer;
        }
        if (layer.kind == LayerKind::Shape) {
            constants.flags |= kFlagKindShape;
        } else if (layer.kind == LayerKind::Liquid) {
            constants.flags |= kFlagKindLiquid;
        } else if (layer.wrapToUnderlying) {
            constants.flags |= kFlagWrap;
        }
        // マップはレイヤーが参照するマテリアルから引く。
        const MaterialAsset* material = materials.Find(layer.material);

        // 法線マップの規約はマテリアルごと。マップが無ければ関係ない。
        if (material != nullptr && material->flipNormalGreen) {
            constants.flags |= kFlagFlipNormalGreen;
        }

        // 定数もマテリアルが持っているほうを優先する。
        // マテリアル側とレイヤー側の両方が掛かると、どちらが効いているか分からない。
        const DirectX::XMFLOAT3 baseColor =
            (material != nullptr) ? material->baseColorTint : layer.baseColor;
        constants.baseColor[0] = baseColor.x;
        constants.baseColor[1] = baseColor.y;
        constants.baseColor[2] = baseColor.z;

        // 色相 / 彩度は**マテリアルだけ**が持つ（レイヤー側には無い）。
        // 度で持ち、シェーダへはラジアンで渡す。
        constants.colorAdjust[0] =
            (material != nullptr)
                ? material->hueShiftDegrees * (3.14159265358979f / 180.0f)
                : 0.0f;
        constants.colorAdjust[1] = (material != nullptr) ? material->saturation : 1.0f;

        constants.surfaceParams[0] =
            (material != nullptr) ? material->roughnessValue : layer.roughness;
        constants.surfaceParams[1] =
            (material != nullptr) ? material->metallicValue : layer.metallic;
        constants.surfaceParams[2] = (material != nullptr)
                                         ? material->ambientOcclusionValue
                                         : layer.ambientOcclusion;
        constants.surfaceParams[3] = layer.heightBase;

        constants.blendParams[0] = layer.blendRange;
        // 法線は実寸の勾配から作る。ハイト 0〜1 の全幅が標高差（m）、
        // 出力 UV 0〜1 が地形の一辺（m）なので、その比を渡す。
        constants.blendParams[1] =
            (stack.SizeMeters() > 0.0f) ? (stack.HeightMeters() / stack.SizeMeters()) : 0.0f;
        constants.blendParams[2] = layer.uvScale;
        constants.blendParams[3] = static_cast<float>(layer.heightSource);

        constants.maskParams[0] = layer.mask.constant;
        constants.maskParams[1] = layer.mask.levelsLow;
        constants.maskParams[2] = layer.mask.levelsHigh;
        constants.maskParams[3] = static_cast<float>(layer.mask.source);

        constants.heightNoise[0] = layer.heightNoise.scale;
        // ハイトはノイズの amount ではなく heightGain を使う。
        constants.heightNoise[1] = layer.heightGain;
        constants.heightNoise[2] = static_cast<float>(layer.heightNoise.octaves);
        constants.heightNoise[3] = layer.heightNoise.offset;

        constants.maskNoise[0] = layer.mask.noise.scale;
        constants.maskNoise[1] = layer.mask.noise.amount;
        constants.maskNoise[2] = static_cast<float>(layer.mask.noise.octaves);
        constants.maskNoise[3] = layer.mask.noise.offset;

        // ベースカラーだけ sRGB として読む。それ以外はリニア。
        constants.textureIndices0[0] =
            (material != nullptr) ? textures.SrvIndex(material->baseColor, true)
                                  : kInvalidTextureIndex;
        constants.textureIndices0[1] =
            (material != nullptr) ? textures.SrvIndex(material->normal, false)
                                  : kInvalidTextureIndex;
        constants.textureIndices0[2] =
            (material != nullptr) ? textures.SrvIndex(material->roughness.texture, false)
                                  : kInvalidTextureIndex;
        constants.textureIndices0[3] =
            (material != nullptr) ? textures.SrvIndex(material->metallic.texture, false)
                                  : kInvalidTextureIndex;
        constants.textureIndices1[0] =
            (material != nullptr)
                ? textures.SrvIndex(material->ambientOcclusion.texture, false)
                : kInvalidTextureIndex;
        // ハイトはマテリアルのハイトマップを優先し、マテリアルが無ければ
        // レイヤー直結のハイトマップ（シェイプ用）を使う。
        constants.textureIndices1[1] =
            (material != nullptr) ? textures.SrvIndex(material->height.texture, false)
                                  : textures.SrvIndex(layer.heightTexture.texture, false);
        // マスク用テクスチャはレイヤー固有。マテリアルのマップとは用途が別。
        constants.textureIndices1[2] = textures.SrvIndex(layer.mask.texture.texture, false);

        // スカラーのマップは「どのチャンネルを読むか」も渡す。
        // Megascans の _ORD のように 1 枚へ詰めたテクスチャに対応するため。
        constants.mapChannels[0] =
            ((material != nullptr) ? PackMaterialChannels(*material)
                                   : PackChannel(layer.heightTexture.channel, 3)) |
            PackChannel(layer.mask.texture.channel, 4);
        // マスクのテクスチャ。ノードのマスクは op の結果、
        // 中間結果由来（傾斜 / 曲率 / 窪み）は直前のパスが書いた作業用。
        constants.textureIndices1[3] = kInvalidTextureIndex;
        if (useNodeMask) {
            constants.textureIndices1[3] =
                m_maskOpTextures[static_cast<size_t>(layer.mask.maskOp)].SrvIndex();
        } else if (useDerivedMask) {
            constants.textureIndices1[3] = m_scratch.SrvIndex();
        }

        // derivedScale は CompositeMask 側で適用済み。二重適用しないため渡さない。
        constants.maskCurve[0] = layer.mask.contrast;

        constants.noiseTypes[0] = static_cast<uint32_t>(layer.heightNoise.type);
        constants.noiseTypes[1] = static_cast<uint32_t>(layer.mask.noise.type);

        constants.paintParams[0] = (layer.mask.source == MaskSource::Paint)
                                       ? paintMasks.SrvIndex(layer.mask.paint)
                                       : kInvalidTextureIndex;

        // 無効なレイヤーは合成しない。サムネイルだけは一覧のために作る。
        if (layer.enabled) {
            commandList->SetPipelineState(layerPipeline);

            for (const TileRect& tile : tiles) {
                LayerConstants tileConstants = constants;
                tileConstants.tile[0] = tile.x;
                tileConstants.tile[1] = tile.y;
                tileConstants.tile[2] = tile.width;
                tileConstants.tile[3] = tile.height;

                const rhi::UploadAllocation cb =
                    AllocateConstants(device, sizeof(LayerConstants));
                if (!cb.IsValid()) {
                    complete = false;
                    break;
                }
                std::memcpy(cb.cpu, &tileConstants, sizeof(tileConstants));

                commandList->SetComputeRootConstantBufferView(1, cb.gpuAddress);
                commandList->Dispatch(DispatchCount(tile.width), DispatchCount(tile.height), 1);
            }

            // 次のレイヤーは前のレイヤーの結果を読むので、必ず区切る。
            const D3D12_RESOURCE_BARRIER barriers[] = {
                CD3DX12_RESOURCE_BARRIER::UAV(m_textures.baseColor.resource.Get()),
                CD3DX12_RESOURCE_BARRIER::UAV(m_textures.normal.resource.Get()),
                CD3DX12_RESOURCE_BARRIER::UAV(m_textures.surface.resource.Get()),
                CD3DX12_RESOURCE_BARRIER::UAV(m_textures.height.resource.Get()),
            };
            commandList->ResourceBarrier(_countof(barriers), barriers);

            ++m_evaluatedLayerCount;
        }

        // --- マスクのサムネイル ---------------------------------------------
        // 中間結果由来のマスクはこのレイヤーを合成する直前の下地からしか作れない。
        // 一覧側で後から焼き直せないので、合成ループの中でここに置く。
        if (thumbnailPipeline != nullptr && layerIndex < m_maskThumbnails.size() &&
            m_maskThumbnails[layerIndex].IsValid()) {
            LayerConstants thumbnailConstants = constants;
            thumbnailConstants.outputIndices[0] = m_maskThumbnails[layerIndex].UavIndex();
            thumbnailConstants.resolution[0] = kMaskThumbnailSize;
            thumbnailConstants.resolution[1] = kMaskThumbnailSize;
            thumbnailConstants.tile[0] = 0;
            thumbnailConstants.tile[1] = 0;
            thumbnailConstants.tile[2] = kMaskThumbnailSize;
            thumbnailConstants.tile[3] = kMaskThumbnailSize;

            const rhi::UploadAllocation cb =
                AllocateConstants(device, sizeof(LayerConstants));
            if (!cb.IsValid()) {
                complete = false;
            } else {
                std::memcpy(cb.cpu, &thumbnailConstants, sizeof(thumbnailConstants));
                commandList->SetPipelineState(thumbnailPipeline);
                commandList->SetComputeRootConstantBufferView(1, cb.gpuAddress);
                commandList->Dispatch(DispatchCount(kMaskThumbnailSize),
                                      DispatchCount(kMaskThumbnailSize), 1);
            }
        }

        // このレイヤーまで合成し終えた Height を見るマスクを焼く。
        // **上のレイヤーが使うので、ここで焼いておかないと間に合わない。**
        runMaskOps(static_cast<int>(layerIndex));
    }

    // 読み取りへ。**ここでは NON_PIXEL までしか遷移させない。** コンピュートキューでは
    // PIXEL_SHADER_RESOURCE を含む遷移を記録できないため、描画から読める状態への
    // 遷移はグラフィックス側（TransitionForDisplay）で行う。書き出しはコンピュートと
    // コピーで読むだけなので、この状態で足りる。
    constexpr D3D12_RESOURCE_STATES kOutputReadState =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    TransitionIfNeeded(commandList, m_textures.baseColor, kOutputReadState);
    TransitionIfNeeded(commandList, m_textures.normal, kOutputReadState);
    TransitionIfNeeded(commandList, m_textures.surface, kOutputReadState);
    TransitionIfNeeded(commandList, m_textures.height, kOutputReadState);

    // サムネイルも同じ理由で NON_PIXEL まで。ImGui（ピクセルシェーダ）から読むなら
    // グラフィックス側で PIXEL へ遷移させること（いまは読み手が無い）。
    for (rhi::GpuTexture& thumbnail : m_maskThumbnails) {
        TransitionIfNeeded(commandList, thumbnail, kOutputReadState);
    }

    // プレビュー用の評価器だけ、Height を CPU へ写す（パスの編集に使う）。
    if (m_asynchronous && complete) {
        RecordHeightfieldReadback(device, pipelineCache, commandList);
    }

    PIXEndEvent(commandList);
    return complete;
}

// 毎フレームの駆動。
//
//   1. 終わった評価があれば回収する（裏側を表側へ入れ替え、描画で読める状態へ）。
//   2. スタックが変わっていて、評価が走っていなければ次を投入する。
//
// 走っている最中に何度編集されても、投入は 1 本ずつ。終わった時点で最新の版を
// 評価し直すので、途中の版は飛ばされる（GPU の仕事は途中で止められない）。
void MaterialEvaluator::Update(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                               ID3D12GraphicsCommandList* commandList,
                               const MaterialStack& stack, const TextureLibrary& textures,
                               const MaterialLibrary& materials,
                               const PaintMaskStore& paintMasks) {
    if (!m_textures.IsValid()) {
        return;
    }
    // CPU 側のハイトの読み戻しは、評価の回収とは別に毎フレーム見る
    // （同期評価のときはフレームのフェンスで終わるため）。
    CollectHeightfieldReadback();

    // --- 回収 -----------------------------------------------------------------
    if (m_asyncInFlight && !m_compute.IsBusy()) {
        m_asyncInFlight = false;
        // 裏側に新しい結果が入った。表側と入れ替える。古い表側は次の評価先になる。
        // まだ描画中のフレームが古い表側を読んでいるかもしれないが、次の評価は
        // 投入時のフレームを GPU 側で待ってから走るので、書き込みが追い越すことはない。
        std::swap(m_textures, m_frontTextures);
        m_evaluatedRevision = m_asyncRevision;
        m_hasResult = true;
        TransitionForDisplay(commandList, m_frontTextures);
    }

    if (m_evaluatedRevision == stack.Revision() || m_asyncInFlight) {
        return;
    }

    const std::vector<TileRect> tiles = MakeTiles();
    const bool canRunAsync = m_asynchronous && m_compute.IsValid() && m_frontTextures.IsValid();

    // --- 同期評価 ---------------------------------------------------------------
    // 結果がまだ 1 つも無いとき（起動直後、解像度変更の直後）はその場で評価する。
    // 前回の絵を出しておけないので、非同期にすると最初のフレームがゴミになる。
    // キューが作れなかったときもここ（従来どおりフレームの中で評価する）。
    if (!canRunAsync || !m_hasResult) {
        // 途中で定数バッファが確保できなかった場合などは「評価済み」にせず、
        // 次のフレームで評価し直す（タイルの継ぎ目が残ったまま確定するのを防ぐ）。
        if (Evaluate(device, pipelineCache, commandList, stack, textures, materials, paintMasks,
                     tiles)) {
            m_evaluatedRevision = stack.Revision();
            if (m_frontTextures.IsValid()) {
                std::swap(m_textures, m_frontTextures);
            }
            m_hasResult = true;
            TransitionForDisplay(commandList, m_frontTextures.IsValid() ? m_frontTextures
                                                                        : m_textures);
        }
        return;
    }

    // --- 非同期評価 -------------------------------------------------------------
    // 前回の記録で定数の置き場を使い切っていたら、倍に広げてから記録する。
    if (m_compute.UploadExhausted()) {
        const uint64_t bytes = m_compute.UploadBytes() * 2;
        TG_LOG_INFO("合成の評価の定数の置き場を %llu KB へ広げます",
                    static_cast<unsigned long long>(bytes / 1024));
        m_compute.Destroy(device);
        if (!m_compute.Create(device, bytes, L"MaterialEvaluatorCompute")) {
            m_compute.Destroy(device);
            return;
        }
    }

    // 評価先（裏側）は前回まで描画が読んでいた組。PIXEL を含む状態からの遷移は
    // コンピュートキューでは記録できないので、**このフレームのリストで** UAV へ戻す。
    // キューはこのフレームの完了を待ってから走るので、順序は保たれる。
    TransitionIfNeeded(commandList, m_textures.baseColor, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    TransitionIfNeeded(commandList, m_textures.normal, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    TransitionIfNeeded(commandList, m_textures.surface, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    TransitionIfNeeded(commandList, m_textures.height, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    ID3D12GraphicsCommandList* computeList = m_compute.Begin(device);
    if (computeList == nullptr) {
        return;
    }

    // 記録が途中で失敗したら投入しない。焼いたことにしたマスクの op は実際には
    // 走らないので、ハッシュを記録前の値へ戻す（戻さないと次回スキップされる）。
    const std::vector<uint64_t> savedMaskOpHashes = m_maskOpHashes;
    m_recordingAsync = true;
    const bool recorded = Evaluate(device, pipelineCache, computeList, stack, textures,
                                   materials, paintMasks, tiles);
    m_recordingAsync = false;
    if (!recorded) {
        m_compute.Abort();
        if (m_maskOpHashes.size() == savedMaskOpHashes.size()) {
            m_maskOpHashes = savedMaskOpHashes;
        } else {
            std::fill(m_maskOpHashes.begin(), m_maskOpHashes.end(), 0ull);
        }
        return;
    }
    if (!m_compute.Submit(device)) {
        std::fill(m_maskOpHashes.begin(), m_maskOpHashes.end(), 0ull);
        return;
    }
    // 評価が参照しているテクスチャ（素材、ペイント、作業用）を、評価が終わる前に
    // 解放しないよう Device に知らせる。
    device.SetAuxiliaryFence(m_compute.Fence(), m_compute.SubmittedValue());
    m_asyncInFlight = true;
    m_asyncRevision = stack.Revision();
}

}  // namespace tg::compositor
