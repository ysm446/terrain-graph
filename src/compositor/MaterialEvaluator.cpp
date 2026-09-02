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
    for (rhi::GpuTexture& mask : m_fluvial.masks) {
        device.DeferRelease(mask);
    }
    m_fluvial.masks.clear();
    m_fluvial.maskResolutions.clear();
    m_fluvial.workResolution = 0;
}

// 川筋のリソースは**使うときだけ**作る。解像度は合成解像度と独立で、
// 変わったら作り直す（GPU がまだ見ているかもしれないので Defer を通す）。
bool MaterialEvaluator::EnsureFluvialResources(rhi::Device& device, uint32_t workResolution,
                                               const std::vector<uint32_t>& maskResolutions) {
    if (m_fluvial.workResolution < workResolution || !m_fluvial.IsValid()) {
        // 作業用は一番大きいグリッドに合わせる。小さい川筋は左上だけを使う。
        for (rhi::GpuTexture* texture :
             {&m_fluvial.heights, &m_fluvial.heightsScratch, &m_fluvial.weights0,
              &m_fluvial.weights1, &m_fluvial.accumA, &m_fluvial.accumB,
              &m_fluvial.maxScratch}) {
            device.DeferRelease(*texture);
        }
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
    }

    // 出力マスクは川筋 1 本につき 1 枚。解像度が変わった枚だけ作り直す。
    while (m_fluvial.masks.size() > maskResolutions.size()) {
        device.DeferRelease(m_fluvial.masks.back());
        m_fluvial.masks.pop_back();
        m_fluvial.maskResolutions.pop_back();
    }
    m_fluvial.masks.resize(maskResolutions.size());
    m_fluvial.maskResolutions.resize(maskResolutions.size(), 0);
    for (size_t i = 0; i < maskResolutions.size(); ++i) {
        if (m_fluvial.masks[i].IsValid() && m_fluvial.maskResolutions[i] == maskResolutions[i]) {
            continue;
        }
        device.DeferRelease(m_fluvial.masks[i]);
        if (!CreateChannelTexture(device, maskResolutions[i], DXGI_FORMAT_R32_FLOAT,
                                  L"FluvialMask", m_fluvial.masks[i])) {
            TG_LOG_WARN("川筋マスクを作れませんでした（%u^2）", maskResolutions[i]);
            return false;
        }
        m_fluvial.maskResolutions[i] = maskResolutions[i];
    }
    return true;
}

void MaterialEvaluator::Destroy(rhi::Device& device) {
    ReleaseFluvialResources(device);
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
bool MaterialEvaluator::ApplyFluvialMask(rhi::Device& device,
                                        rhi::PipelineCache& pipelineCache,
                                        ID3D12GraphicsCommandList* commandList,
                                        const MaterialLayer& layer,
                                        const MaterialStack& stack, size_t slot) {
    const FluvialParams& params = layer.mask.fluvial;
    const uint32_t resolution = std::clamp(params.resolution, 64u, 2048u);
    if (slot >= m_fluvial.masks.size() || !m_fluvial.masks[slot].IsValid() ||
        m_fluvial.workResolution < resolution) {
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
    constants.indices1[2] = m_fluvial.masks[slot].UavIndex();
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
    TransitionIfNeeded(commandList, m_fluvial.masks[slot],
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

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

    // 合成パスはマスクを SRV で読む。Height は次のレイヤーが書き換える。
    TransitionIfNeeded(commandList, m_fluvial.masks[slot],
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    TransitionIfNeeded(commandList, m_textures.height, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

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

    // --- 川筋マスクの段取り -------------------------------------------------
    // 川筋は**チェーンのどこまで合成した Height から作るか**が決まっている
    // （Mask Fluvial の Base 入力。繋いでいなければ、使うレイヤーの直下）。
    // 使うレイヤーより前に作らないといけないので、先に予定を組む。
    struct FluvialJob {
        size_t layerIndex = 0;    // このマスクを使うレイヤー
        size_t computeAfter = 0;  // このレイヤーまで合成し終えた時点で作る
    };
    std::vector<FluvialJob> fluvialJobs;
    std::vector<uint32_t> fluvialMaskResolutions;
    if (baseIndex != static_cast<size_t>(-1)) {
        for (size_t i = baseIndex + 1; i < stack.Layers().size(); ++i) {
            const MaterialLayer& candidate = stack.Layers()[i];
            if (!candidate.enabled || candidate.mask.source != MaskSource::Fluvial) {
                continue;
            }
            size_t computeAfter = i - 1;
            const int requested = candidate.mask.fluvialSourceIndex;
            if (requested >= 0 && static_cast<size_t>(requested) < i) {
                computeAfter = static_cast<size_t>(requested);
            }
            // 下地より前は何も合成されていない。そこまで戻ることはできない。
            computeAfter = std::max(computeAfter, baseIndex);
            fluvialJobs.push_back({i, computeAfter});
            fluvialMaskResolutions.push_back(
                std::clamp(candidate.mask.fluvial.resolution, 64u, 2048u));
        }
    }
    bool fluvialReady = false;
    if (!fluvialJobs.empty()) {
        const uint32_t workResolution = *std::max_element(fluvialMaskResolutions.begin(),
                                                          fluvialMaskResolutions.end());
        fluvialReady = EnsureFluvialResources(device, workResolution, fluvialMaskResolutions);
        if (!fluvialReady) {
            complete = false;
        }
    }
    std::vector<bool> fluvialDone(fluvialJobs.size(), false);

    // 指定したレイヤーまで合成し終えた時点で作る川筋を、まとめて走らせる。
    const auto runFluvialJobs = [&](size_t afterLayerIndex) {
        if (!fluvialReady) {
            return;
        }
        for (size_t job = 0; job < fluvialJobs.size(); ++job) {
            if (fluvialDone[job] || fluvialJobs[job].computeAfter != afterLayerIndex) {
                continue;
            }
            if (ApplyFluvialMask(device, pipelineCache, commandList,
                                 stack.Layers()[fluvialJobs[job].layerIndex], stack, job)) {
                fluvialDone[job] = true;
            } else {
                // 作れなければマスクは定数へ落ちる（絵は出るが川筋は出ない）。
                complete = false;
            }
        }
    };

    m_evaluatedLayerCount = 0;
    m_evaluatedTileCount = static_cast<uint32_t>(tiles.size());

    // レイヤー優先で回す。中間結果由来のマスクは近傍を参照するため、
    // タイル優先で回すと隣のタイルの未評価の値を読んでしまう。
    for (size_t layerIndex = 0; layerIndex < stack.Layers().size(); ++layerIndex) {
        const MaterialLayer& layer = stack.Layers()[layerIndex];
        const bool isBaseLayer = (layerIndex == baseIndex);

        // ブラーは合成しない**加工**。下地のハイトをならして法線を作り直す。
        // 下に合成済みのレイヤーが無いとぼかす相手がいないので、その場合は素通り。
        if (layer.kind == LayerKind::Blur) {
            const bool hasUnderlying = (baseIndex != static_cast<size_t>(-1)) &&
                                       (layerIndex > baseIndex);
            if (layer.enabled && hasUnderlying && blurPipeline != nullptr &&
                blurNormalPipeline != nullptr) {
                if (!ApplyHeightBlur(device, commandList, blurPipeline, blurNormalPipeline,
                                     layer, stack, tiles)) {
                    complete = false;
                }
                ++m_evaluatedLayerCount;
            }
            // ぼかした後の Height を見る川筋があれば、ここで作る。
            runFluvialJobs(layerIndex);
            continue;
        }
        // 下地のレイヤーには合成する相手がいないので、中間結果由来のマスクは使えない。
        const bool useDerivedMask =
            layer.enabled && !isBaseLayer && IsDerivedMaskSource(layer.mask.source);
        // 川筋は近傍を何度も舐める別のパイプライン。**このループより前の
        // 段取りで、指定された位置まで合成し終えた時点に作ってある。**
        const bool useFluvialMask = useDerivedMask && layer.mask.source == MaskSource::Fluvial;
        size_t fluvialSlot = fluvialJobs.size();
        if (useFluvialMask) {
            for (size_t job = 0; job < fluvialJobs.size(); ++job) {
                if (fluvialJobs[job].layerIndex == layerIndex) {
                    fluvialSlot = job;
                }
            }
        }

        if (!useFluvialMask && useDerivedMask) {
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
        // 中間結果由来のマスク。川筋だけは専用グリッドのテクスチャを読む。
        constants.textureIndices1[3] = kInvalidTextureIndex;
        if (useFluvialMask) {
            if (fluvialSlot < fluvialJobs.size() && fluvialDone[fluvialSlot]) {
                constants.textureIndices1[3] = m_fluvial.masks[fluvialSlot].SrvIndex();
            }
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

        // このレイヤーまで合成し終えた Height を見る川筋を作る。
        // **上のレイヤーが使うので、ここで作っておかないと間に合わない。**
        runFluvialJobs(layerIndex);
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
