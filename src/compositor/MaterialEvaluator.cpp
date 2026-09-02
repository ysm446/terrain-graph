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
constexpr DXGI_FORMAT kHeightFormat = DXGI_FORMAT_R16_FLOAT;

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
};

// GPU 側の SedimentConstants と一致させること。
struct SedimentConstants {
    uint32_t indices0[4];  // UAV: 基盤, 土砂, 流出, 元の高さ
    uint32_t indices1[4];  // SRV: 基盤, 土砂, 元の高さ, 合成の Height
    uint32_t indices2[4];  // グリッドの一辺, 地形を土砂として扱うか, Height の UAV, 合成解像度
    uint32_t indices3[4];  // 最大値の UAV, マスクの UAV, 未使用 x2
    float params[4];       // 安息角ぶんの落差, 1 反復あたりの供給量, マスクのコントラスト, 未使用
};

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
        case MaskOpKind::Sediment:
            return HashBytes(seed, &op.sedimentMask, sizeof(op.sedimentMask));
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

}  // namespace

bool MaterialEvaluator::Create(rhi::Device& device, uint32_t resolution) {
    ReleaseTextures(device);

    if (!CreateChannelTexture(device, resolution, kBaseColorFormat, L"MaterialBaseColor",
                              m_textures.baseColor) ||
        !CreateChannelTexture(device, resolution, kNormalFormat, L"MaterialNormal",
                              m_textures.normal) ||
        !CreateChannelTexture(device, resolution, kSurfaceFormat, L"MaterialSurface",
                              m_textures.surface) ||
        !CreateChannelTexture(device, resolution, kHeightFormat, L"MaterialHeight",
                              m_textures.height) ||
        !CreateChannelTexture(device, resolution, kHeightFormat, L"MaterialScratch",
                              m_textures.scratch)) {
        return false;
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
        if (!CreateChannelTexture(device, resolution, kHeightFormat, L"MaskOp",
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
    ReleaseFluvialResources(device);
    ReleaseSedimentResources(device);
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
    device.DeferRelease(m_textures.baseColor);
    device.DeferRelease(m_textures.normal);
    device.DeferRelease(m_textures.surface);
    device.DeferRelease(m_textures.height);
    device.DeferRelease(m_textures.scratch);
}

bool MaterialEvaluator::Resize(rhi::Device& device, uint32_t resolution) {
    if (resolution == m_resolution) {
        return true;
    }
    // 作り直す前に GPU の参照が切れるのを待つ。
    device.WaitForGpu();
    return Create(device, resolution);
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

    const wchar_t* entry = L"CsImage";
    switch (op.kind) {
        case MaskOpKind::Slope:  entry = L"CsSlope"; break;
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

    const rhi::UploadAllocation cb = device.Upload().Allocate(sizeof(MaskOpConstants), 256);
    if (!cb.IsValid()) {
        return false;
    }
    std::memcpy(cb.cpu, &constants, sizeof(constants));

    PIXBeginEvent(commandList, PIX_COLOR(200, 200, 120), "CompositeMaskOp");
    // 画像を読む op は合成の Height を触らないが、傾斜は読むので SRV にしておく。
    if (op.kind == MaskOpKind::Slope) {
        TransitionIfNeeded(commandList, m_textures.height,
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    TransitionIfNeeded(commandList, target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    commandList->SetPipelineState(pipeline);
    commandList->SetComputeRootConstantBufferView(1, cb.gpuAddress);
    commandList->Dispatch(DispatchCount(resolution), DispatchCount(resolution), 1);

    // 下流の op と合成パスは SRV で読む。Height は次のレイヤーが書き換える。
    TransitionIfNeeded(commandList, target, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (op.kind == MaskOpKind::Slope) {
        TransitionIfNeeded(commandList, m_textures.height, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    PIXEndEvent(commandList);
    return true;
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
        const rhi::UploadAllocation cb = device.Upload().Allocate(sizeof(FluvialConstants), 256);
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

    const rhi::UploadAllocation cb = device.Upload().Allocate(sizeof(BlurConstants), 256);
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
        const rhi::UploadAllocation cb = device.Upload().Allocate(sizeof(SedimentConstants), 256);
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
    TransitionIfNeeded(commandList, m_sediment.bedrock,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    TransitionIfNeeded(commandList, m_sediment.sediment,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    TransitionIfNeeded(commandList, m_sediment.original,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    TransitionIfNeeded(commandList, m_textures.height, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    run(applyPass, DispatchCount(m_resolution));

    // 次に使うときは書き込みへ戻す。
    TransitionIfNeeded(commandList, m_sediment.bedrock, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    TransitionIfNeeded(commandList, m_sediment.sediment, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    TransitionIfNeeded(commandList, m_sediment.original, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    PIXEndEvent(commandList);

    // 形が変わったので、法線も作り直す。
    RebuildNormalsFromHeight(device, normalPass, commandList, stack);
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
    constants.indices0[1] = m_sediment.sediment.UavIndex();
    constants.indices1[1] = m_sediment.sediment.SrvIndex();
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

    const rhi::UploadAllocation cb = device.Upload().Allocate(sizeof(SedimentConstants), 256);
    if (!cb.IsValid()) {
        return false;
    }
    std::memcpy(cb.cpu, &constants, sizeof(constants));

    PIXBeginEvent(commandList, PIX_COLOR(190, 170, 120), "CompositeSedimentMask");
    TransitionIfNeeded(commandList, m_sediment.sediment, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
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

    // マスクを焼くときだけ、厚みは読み取り専用にする。
    TransitionIfNeeded(commandList, m_sediment.sediment,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    commandList->SetPipelineState(maskPass);
    commandList->Dispatch(DispatchCount(m_resolution), DispatchCount(m_resolution), 1);
    barrier();

    TransitionIfNeeded(commandList, m_sediment.sediment, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    TransitionIfNeeded(commandList, target, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    PIXEndEvent(commandList);
    return true;
}

bool MaterialEvaluator::ApplyHeightBlur(rhi::Device& device,
                                       ID3D12GraphicsCommandList* commandList,
                                       ID3D12PipelineState* blurPipeline,
                                       ID3D12PipelineState* normalPipeline,
                                       const MaterialLayer& layer, const MaterialStack& stack,
                                       const std::vector<TileRect>& tiles) {
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
                                  uint32_t outputIndex, uint32_t axis, float passStrength) {
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

            const rhi::UploadAllocation cb = device.Upload().Allocate(sizeof(BlurConstants), 256);
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
        TransitionIfNeeded(commandList, m_textures.scratch,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        dispatchPass(blurPipeline, m_textures.height.SrvIndex(), m_textures.scratch.UavIndex(),
                     0u, 1.0f);

        // 垂直: 作業用（読み取り）→ Height。ここで元の高さと強さで混ぜる。
        TransitionIfNeeded(commandList, m_textures.scratch,
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        TransitionIfNeeded(commandList, m_textures.height,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        dispatchPass(blurPipeline, m_textures.scratch.SrvIndex(), m_textures.height.UavIndex(),
                     1u, strength);
    }

    // ぼかした形から法線を作り直す。**Height だけをぼかすと形と陰影が食い違う。**
    TransitionIfNeeded(commandList, m_textures.height,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    dispatchPass(normalPipeline, m_textures.height.SrvIndex(), m_textures.normal.UavIndex(), 0u,
                 1.0f);

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
                op.kind == MaskOpKind::Sediment) {
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
            if (layer.enabled && hasUnderlying && layer.kind == LayerKind::Sediment) {
                if (!ApplySediment(device, pipelineCache, commandList, layer, stack)) {
                    complete = false;
                }
                ++m_evaluatedLayerCount;
            } else if (layer.enabled && hasUnderlying && blurPipeline != nullptr &&
                       blurNormalPipeline != nullptr) {
                if (!ApplyHeightBlur(device, commandList, blurPipeline, blurNormalPipeline,
                                     layer, stack, tiles)) {
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
            TransitionIfNeeded(commandList, m_textures.scratch,
                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

            commandList->SetPipelineState(maskPipeline);
            for (const TileRect& tile : tiles) {
                MaskConstants constants = {};
                constants.heightIndex = m_textures.height.SrvIndex();
                constants.outputIndex = m_textures.scratch.UavIndex();
                constants.source = static_cast<uint32_t>(layer.mask.source);
                constants.derivedScale = layer.mask.derivedScale;
                constants.tile[0] = tile.x;
                constants.tile[1] = tile.y;
                constants.tile[2] = tile.width;
                constants.tile[3] = tile.height;
                constants.resolution[0] = m_resolution;
                constants.resolution[1] = m_resolution;

                const rhi::UploadAllocation cb =
                    device.Upload().Allocate(sizeof(MaskConstants), 256);
                if (!cb.IsValid()) {
                    complete = false;
                    break;
                }
                std::memcpy(cb.cpu, &constants, sizeof(constants));

                commandList->SetComputeRootConstantBufferView(1, cb.gpuAddress);
                commandList->Dispatch(DispatchCount(tile.width), DispatchCount(tile.height), 1);
            }

            // マスクを読み取りに、Height を書き込みに戻す。
            TransitionIfNeeded(commandList, m_textures.scratch,
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
            constants.textureIndices1[3] = m_textures.scratch.SrvIndex();
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
                    device.Upload().Allocate(sizeof(LayerConstants), 256);
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
                device.Upload().Allocate(sizeof(LayerConstants), 256);
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

    // メッシュの描画から読めるようにする。Height は頂点 / ドメインシェーダ
    // （ディスプレイスメント）からも読まれるため、NON_PIXEL も含める。
    // 状態の食い違いを避けるため 4 枚とも同じ状態に揃える。
    constexpr D3D12_RESOURCE_STATES kOutputReadState =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    TransitionIfNeeded(commandList, m_textures.baseColor, kOutputReadState);
    TransitionIfNeeded(commandList, m_textures.normal, kOutputReadState);
    TransitionIfNeeded(commandList, m_textures.surface, kOutputReadState);
    TransitionIfNeeded(commandList, m_textures.height, kOutputReadState);

    // サムネイルは ImGui が SRV として読む。
    for (rhi::GpuTexture& thumbnail : m_maskThumbnails) {
        TransitionIfNeeded(commandList, thumbnail, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }

    PIXEndEvent(commandList);
    return complete;
}

void MaterialEvaluator::EvaluateIfDirty(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                                        ID3D12GraphicsCommandList* commandList,
                                        const MaterialStack& stack,
                                        const TextureLibrary& textures,
                                        const MaterialLibrary& materials,
                                        const PaintMaskStore& paintMasks) {
    if (m_evaluatedRevision == stack.Revision()) {
        return;
    }

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

    // 途中で定数バッファが確保できなかった場合などは「評価済み」にせず、
    // 次のフレームで評価し直す（タイルの継ぎ目が残ったまま確定するのを防ぐ）。
    if (Evaluate(device, pipelineCache, commandList, stack, textures, materials, paintMasks,
                 tiles)) {
        m_evaluatedRevision = stack.Revision();
    }
}

}  // namespace tg::compositor
