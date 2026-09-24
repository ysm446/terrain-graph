#include "renderer/ModelPreview.h"

#include <pix3.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include "core/Log.h"
#include "renderer/InstanceCulling.h"
namespace tg::renderer {
namespace {
constexpr uint32_t kOutputSize = 512;
constexpr float kPi = 3.14159265358979f;
// ModelPreview.hlsl と一致させる。
struct ModelConstants {
    uint32_t baseColorIndex, normalIndex, roughnessIndex, metallicIndex;
    uint32_t aoIndex, mapChannels, flipNormalGreen, irradianceIndex;
    uint32_t prefilteredIndex, brdfLutIndex, prefilteredMipCount, tonemapMode;
    float baseColorTint[3];
    float roughnessValue;
    float metallicValue, aoValue, colorAdjust[2];
    float brightness, ambientLow, ambientHigh, alphaCutoff;
    float cameraPosition[3];
    float exposure;
    float lightDirection[3];
    float lightIlluminance;
    float lightColor[3];
    float iblIntensity;
    DirectX::XMFLOAT4X4 viewProjection;
    uint32_t points, rows, visibleIndices, seed;
    float weightStart, weightEnd, scaleMin, scaleMax;
    float pivot[3], modelSize;
    float align, offset; uint32_t usePointSize, sceneMode;
    SceneShadowData shadows;
    AtmosphereSettings atmosphere;
    uint32_t cloudNoiseIndex, atmosphericMode, clearIrradianceIndex;
    float ambientOcclusion;
    // 可視リストの区画の先頭（インスタンス描画）。SV_InstanceID は StartInstance を含まないので定数で渡す。
    uint32_t visibleOffset;
    uint32_t lodView;  // 真ならベースカラーのマップを使わず、ティントを段の色にする
    uint32_t padding[2];
};
static_assert(sizeof(ModelConstants) == 1040);

}  // namespace
void ModelPreview::Destroy(rhi::Device& device) {
    for (auto& mesh : m_meshes) mesh.Release(device);
    m_meshes.clear();
    m_geometry.reset();
    m_ready = false;
    m_parts.clear();
    m_lodFirstPart.clear();
    m_segmentFirstArgument.clear();
    device.DeferRelease(m_output);
    device.DeferRelease(m_depth);
    device.DeferRelease(m_visibleInstances);
    device.DeferRelease(m_indirectArguments);
    device.DeferRelease(m_statCounters);
    for (auto& readback : m_statReadback) device.DeferRelease(readback);
    for (auto& fence : m_statFence) fence = 0;
    m_statFrame = m_statCollected = 0;
    m_instanceStats = {};
    device.Defer(m_drawSignature);
    m_drawSignature.Reset();
}
void ModelPreview::ResetView() {
    m_camera.Reset();
    FrameView();
}
void ModelPreview::FocusView() {
    if (!m_geometry) return;
    using namespace DirectX;
    XMFLOAT3 center;
    XMStoreFloat3(&center, XMVectorScale(XMVectorAdd(XMLoadFloat3(&m_geometry->minimum),
                                                   XMLoadFloat3(&m_geometry->maximum)), 0.5f));
    m_camera.Focus(center);
}
void ModelPreview::FrameView() {
    if (!m_geometry) return;
    using namespace DirectX;
    auto lo = XMLoadFloat3(&m_geometry->minimum), hi = XMLoadFloat3(&m_geometry->maximum);
    XMFLOAT3 center;
    XMStoreFloat3(&center, XMVectorScale(XMVectorAdd(lo, hi), 0.5f));
    const float radius =
        std::max(0.0001f, XMVectorGetX(XMVector3Length(XMVectorSubtract(hi, lo))) * 0.5f);
    m_camera.SetViewportSize(kOutputSize, kOutputSize);
    m_camera.SetSceneRadius(radius);
    m_camera.Frame(center, radius);
}
bool ModelPreview::Prepare(rhi::Device& device, const ModelAsset& asset, int lod) {
    if (!asset.geometry) {
        if (m_geometry) Destroy(device);
        return false;
    }
    const size_t lodCount = asset.geometry->lods.size();
    const bool all = lod == kAllLods;
    if (!all) lod = std::clamp(lod, 0, static_cast<int>(lodCount) - 1);
    if (m_ready && m_geometry == asset.geometry && m_requestedLod == lod) return true;
    for (auto& mesh : m_meshes) mesh.Release(device);
    m_meshes.clear();
    m_parts.clear();
    m_lodFirstPart.clear();
    m_segmentFirstArgument.clear();
    const bool changed = m_geometry != asset.geometry;
    m_geometry = asset.geometry;
    m_ready = false;
    m_requestedLod = lod;
    m_firstLod = all ? 0 : static_cast<size_t>(lod);
    const size_t lastLod = all ? std::min(lodCount, kMaxInstanceLods) : m_firstLod + 1;
    for (size_t level = m_firstLod; level < lastLod; ++level) {
        m_lodFirstPart.push_back(m_meshes.size());
        for (const auto& part : m_geometry->lods[level].parts) {
            m_meshes.emplace_back();
            if (!m_meshes.back().Create(device, part.mesh, L"ModelPreviewMesh")) return false;
            m_parts.push_back({static_cast<uint32_t>(level - m_firstLod), part.slot});
        }
    }
    m_lodFirstPart.push_back(m_meshes.size());
    // 区画ごとの描画引数。区画 s の段は s % L、各段のパーツ数ぶん並べる。
    const size_t lods = LodCount();
    uint32_t argument = 0;
    for (size_t segment = 0; segment < SegmentCount(); ++segment) {
        const size_t level = segment % lods;
        m_segmentFirstArgument.push_back(argument);
        argument += static_cast<uint32_t>(m_lodFirstPart[level + 1] - m_lodFirstPart[level]);
    }
    m_ready = true;
    if (changed) ResetView();
    return true;
}
bool ModelPreview::CullInstances(rhi::Device& device, rhi::PipelineCache& cache,
                                 ID3D12GraphicsCommandList* list, const ModelAsset& model,
                                 const ModelInstanceDraw& draw) {
    if (!draw.count || m_meshes.empty()) return false;
    auto* cull = cache.GetCompute(L"InstanceCulling.hlsl", L"CsCull");
    auto* finish = cache.GetCompute(L"InstanceCulling.hlsl", L"CsFinish");
    auto* accumulate = cache.GetCompute(L"InstanceCulling.hlsl", L"CsAccumulate");
    if (!cull || !finish || !accumulate) return false;
    if (!m_statCounters.IsValid() &&
        !device.Allocator().CreateStructuredBuffer(kStatSlots,sizeof(uint32_t),L"InstanceStats",m_statCounters,true)) return false;
    const uint32_t frame = device.FrameIndex();
    if (!m_statReadback[frame].IsValid() &&
        !device.Allocator().CreateReadbackBuffer(kStatSlots*sizeof(uint32_t),L"InstanceStatsReadback",m_statReadback[frame])) return false;
    if (!m_drawSignature) {
        D3D12_INDIRECT_ARGUMENT_DESC argument{};
        argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
        D3D12_COMMAND_SIGNATURE_DESC desc{};
        desc.ByteStride = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
        desc.NumArgumentDescs = 1; desc.pArgumentDescs = &argument;
        if (!TG_CHECK_HR(device.GetDevice()->CreateCommandSignature(&desc,nullptr,IID_PPV_ARGS(&m_drawSignature)))) return false;
    }
    const size_t lods = LodCount(), segments = SegmentCount();
    // 区画ごとに候補数ぶんの枠を取る。要素は（元ID, 切り替えの進み具合）。
    const uint64_t visibleCount = uint64_t(draw.count) * segments;
    constexpr uint32_t kVisibleStride = sizeof(uint32_t) * 2;
    if (m_visibleInstances.sizeInBytes < visibleCount * kVisibleStride) {
        device.DeferRelease(m_visibleInstances);
        if (!device.Allocator().CreateStructuredBuffer(static_cast<uint32_t>(visibleCount),kVisibleStride,L"VisibleInstances",m_visibleInstances,true)) return false;
    }
    uint32_t argumentCount = 0;
    for (size_t segment = 0; segment < segments; ++segment) {
        const size_t level = segment % lods;
        argumentCount += static_cast<uint32_t>(m_lodFirstPart[level + 1] - m_lodFirstPart[level]);
    }
    const uint64_t argumentBytes = uint64_t(argumentCount)*sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
    if (m_indirectArguments.sizeInBytes != argumentBytes) {
        device.DeferRelease(m_indirectArguments);
        if (!device.Allocator().CreateStructuredBuffer(argumentCount,sizeof(D3D12_DRAW_INDEXED_ARGUMENTS),L"InstanceArguments",m_indirectArguments,true)) return false;
    }
    const auto transition = [&](rhi::GpuBuffer& buffer, D3D12_RESOURCE_STATES state) {
        if (buffer.state == state) return;
        const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(buffer.resource.Get(),buffer.state,state);
        list->ResourceBarrier(1,&barrier); buffer.state = state;
    };
    const auto upload = device.Upload().Allocate(argumentBytes,16);
    if (!upload.IsValid()) return false;
    auto* arguments = static_cast<D3D12_DRAW_INDEXED_ARGUMENTS*>(upload.cpu);
    for (size_t segment = 0, index = 0; segment < segments; ++segment) {
        const size_t level = segment % lods;
        for (size_t part = m_lodFirstPart[level]; part < m_lodFirstPart[level + 1]; ++part)
            arguments[index++] = {m_meshes[part].IndexCount(),0,0,0,0};
    }
    struct CullConstants {
        std::array<DirectX::XMFLOAT4,6> planes;
        uint32_t points, visible, arguments, count;
        uint32_t seed, argumentCount; float weightStart, weightEnd;
        float scaleMin, scaleMax, modelSize, radius;
        DirectX::XMFLOAT3 camera; float maxDistance;
        float offset; uint32_t usePointSize; uint32_t lodCount; float fadeBand;
        float lodStart[kMaxInstanceLods];
        uint32_t segmentFirst[kMaxInstanceLods * 2];
        uint32_t segmentCount, statCounters, statOffset; uint32_t padding{};
    } constants{};
    static_assert(sizeof(CullConstants)==240);
    constants.planes = InstanceFrustumPlanes(draw.viewProjection);
    constants.points = draw.points; constants.visible = m_visibleInstances.uav.index;
    constants.arguments = m_indirectArguments.uav.index; constants.count = draw.count;
    constants.seed = draw.seed; constants.argumentCount = argumentCount;
    constants.weightStart = draw.weightStart; constants.weightEnd = draw.weightEnd;
    constants.scaleMin = draw.scaleMin; constants.scaleMax = draw.scaleMax;
    const auto& lo = m_geometry->minimum; const auto& hi = m_geometry->maximum;
    const float x = (hi.x-lo.x)*0.5f, y = hi.y-lo.y, z = (hi.z-lo.z)*0.5f;
    constants.modelSize = std::max({x*2,y,z*2,0.0001f});
    constants.radius = std::sqrt(x*x+y*y+z*z);
    constants.camera = draw.cameraPosition; constants.maxDistance = draw.maxDistance;
    constants.offset = draw.offset; constants.usePointSize = draw.usePointSize;
    constants.lodCount = static_cast<uint32_t>(lods);
    // 影は硬く切り替える（重ね合わせの区画を描かない）。
    constants.fadeBand = draw.shadow ? 0.0f : std::max(draw.fadeBand, 0.0f);
    for (size_t level = 1; level < lods; ++level)
        constants.lodStart[level] = LodStartDistance(model, m_firstLod + level) * std::max(draw.lodBias, 0.0f);
    for (size_t segment = 0; segment < segments; ++segment)
        constants.segmentFirst[segment] = m_segmentFirstArgument[segment];
    constants.segmentCount = static_cast<uint32_t>(segments);
    constants.statCounters = m_statCounters.uav.index;
    constants.statOffset = draw.shadow ? kMaxInstanceLods * 2 : 0;
    const auto cb = device.Upload().Allocate(sizeof(constants),256);
    if (!cb.IsValid()) return false;
    std::memcpy(cb.cpu,&constants,sizeof(constants));
    PIXBeginEvent(list, PIX_COLOR(120,200,200), "CullModelInstances");
    // フレームの最初の使用で集計を 0 に戻す。
    if (m_statFrame != device.NextFenceValue()) {
        const auto zeros = device.Upload().Allocate(kStatSlots*sizeof(uint32_t),16);
        if (!zeros.IsValid()) { PIXEndEvent(list); return false; }
        std::memset(zeros.cpu,0,kStatSlots*sizeof(uint32_t));
        transition(m_statCounters,D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyBufferRegion(m_statCounters.resource.Get(),0,zeros.resource,zeros.offset,kStatSlots*sizeof(uint32_t));
        m_statFrame = device.NextFenceValue();
    }
    transition(m_indirectArguments,D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyBufferRegion(m_indirectArguments.resource.Get(),0,upload.resource,upload.offset,argumentBytes);
    transition(m_indirectArguments,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    transition(m_visibleInstances,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list->SetComputeRootSignature(cache.GlobalRootSignature());
    list->SetComputeRootConstantBufferView(1,cb.gpuAddress);
    list->SetPipelineState(cull); list->Dispatch((draw.count+63)/64,1,1);
    const auto barrier = CD3DX12_RESOURCE_BARRIER::UAV(nullptr);
    list->ResourceBarrier(1,&barrier);
    // 区画の先頭の件数を、同じ区画の他のパーツへ写す。
    if (argumentCount > segments) {
        list->SetPipelineState(finish); list->Dispatch((argumentCount+63)/64,1,1);
        list->ResourceBarrier(1,&barrier);
    }
    // 区画ごとの件数を足し込み、このフレームの読み戻し先へ写す（後の使用で上書きされ、最後が合計）。
    transition(m_statCounters,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list->SetPipelineState(accumulate); list->Dispatch(1,1,1);
    transition(m_statCounters,D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(m_statReadback[frame].resource.Get(),0,m_statCounters.resource.Get(),0,kStatSlots*sizeof(uint32_t));
    m_statFence[frame] = device.NextFenceValue();
    m_statLodCount[frame] = lods;
    transition(m_visibleInstances,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(m_indirectArguments,D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    PIXEndEvent(list);
    return true;
}

void ModelPreview::CollectInstanceStats(rhi::Device& device) {
    // 完了したうち最も新しいフレームを読む。
    const uint64_t completed = device.CompletedFenceValue();
    int newest = -1;
    for (uint32_t i = 0; i < rhi::kFrameCount; ++i)
        if (m_statFence[i] && m_statFence[i] <= completed && m_statFence[i] > m_statCollected &&
            (newest < 0 || m_statFence[i] > m_statFence[newest]))
            newest = static_cast<int>(i);
    if (newest < 0) return;
    m_statCollected = m_statFence[newest];
    const size_t lods = m_statLodCount[newest];
    if (lods != LodCount() || !lods) return;
    uint32_t counts[kStatSlots] = {};
    void* mapped = nullptr;
    const D3D12_RANGE range{0,sizeof(counts)};
    if (!TG_CHECK_HR(m_statReadback[newest].resource->Map(0,&range,&mapped))) return;
    std::memcpy(counts,mapped,sizeof(counts));
    const D3D12_RANGE written{0,0};
    m_statReadback[newest].resource->Unmap(0,&written);
    InstanceStats stats;
    const size_t segments = SegmentCount();
    for (size_t pass = 0; pass < 2; ++pass)
        for (size_t segment = 0; segment < segments; ++segment) {
            const size_t level = segment % lods;
            const uint64_t instances = counts[pass*kMaxInstanceLods*2+segment];
            uint64_t indices = 0;
            for (size_t part = m_lodFirstPart[level]; part < m_lodFirstPart[level+1]; ++part)
                indices += m_meshes[part].IndexCount();
            stats.vertices += instances*indices;
            stats.triangles += instances*(indices/3);
            if (pass == 0 && m_firstLod + level < kMaxInstanceLods) stats.instances[m_firstLod+level] += instances;
        }
    m_instanceStats = stats;
}

uint32_t ModelPreview::Render(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                          ID3D12GraphicsCommandList* commandList, const ModelAsset& model,
                          const compositor::MaterialLibrary& materials,
                          const compositor::TextureLibrary& textures,
                          const Environment& environment, float iblIntensity,
                          const LightSettings& light, float exposure, TonemapMode tonemap, const ModelInstanceDraw* instances) {
    if (!m_geometry || !m_ready) return 0;
    // パーツのマテリアルごとに PSO を選ぶ。アルファ抜きは早期深度テストが効きにくいので、
    // 使うパーツだけ clip 付きの PS にする。影は不透明なら PS なしの深度だけで描く。
    const auto pipelineFor = [&](bool cutout, bool twoSided, bool fade) {
        rhi::GraphicsPipelineDesc desc;
        desc.shaderPath = L"ModelPreview.hlsl";
        desc.vertexEntry = L"VsMain";
        desc.pixelEntry = L"PsMain";
        desc.layout = rhi::VertexLayout::MeshStandard;
        desc.rtvFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.dsvFormat = DXGI_FORMAT_D32_FLOAT;
        desc.cullMode = twoSided ? D3D12_CULL_MODE_NONE : D3D12_CULL_MODE_BACK;
        if (instances) {
            desc.rtvFormat = instances->shadow ? DXGI_FORMAT_UNKNOWN : DXGI_FORMAT_R16G16B16A16_FLOAT;
            if (instances->shadow) desc.pixelEntry = cutout ? L"PsShadow" : L"";
            else if (fade) desc.pixelEntry = L"PsDither";
            desc.cullMode = D3D12_CULL_MODE_NONE;
        }
        return pipelineCache.GetGraphics(desc);
    };
    if (!pipelineFor(false, false, false)) return 0;
    if (!instances) {
    if (!m_output.IsValid()) {
        rhi::TextureDesc target;
        target.width = kOutputSize;
        target.height = kOutputSize;
        target.allowRenderTarget = true;
        target.clearColor[0] = target.clearColor[1] = target.clearColor[2] = 0.025f;
        target.debugName = L"ModelPreview";
        if (!device.Allocator().CreateTexture2D(target, m_output)) return 0;
    }
    if (!m_depth.IsValid()) {
        rhi::TextureDesc depth;
        depth.width = kOutputSize;
        depth.height = kOutputSize;
        depth.format = DXGI_FORMAT_D32_FLOAT;
        depth.allowDepthStencil = true;
        depth.createSrv = false;
        depth.debugName = L"ModelPreviewDepth";
        if (!device.Allocator().CreateTexture2D(depth, m_depth)) return 0;
    }
    PIXBeginEvent(commandList, PIX_COLOR(120, 200, 200), "ModelPreview");
    rhi::TransitionIfNeeded(commandList, m_output, D3D12_RESOURCE_STATE_RENDER_TARGET);
    rhi::TransitionIfNeeded(commandList, m_depth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    const float clear[4] = {0.025f, 0.025f, 0.025f, 1};
    commandList->ClearRenderTargetView(m_output.rtv.cpu, clear, 0, nullptr);
    commandList->ClearDepthStencilView(m_depth.dsv.cpu, D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr);
    commandList->OMSetRenderTargets(1, &m_output.rtv.cpu, FALSE, &m_depth.dsv.cpu);
    const D3D12_VIEWPORT viewport = {0, 0, float(kOutputSize), float(kOutputSize), 0, 1};
    const D3D12_RECT scissor = {0, 0, kOutputSize, kOutputSize};
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);
    } else {
        PIXBeginEvent(commandList, PIX_COLOR(120, 200, 200), "ModelInstances");
    }
    if (instances && !CullInstances(device, pipelineCache, commandList, model, *instances)) { PIXEndEvent(commandList); return 0; }
    commandList->SetGraphicsRootSignature(pipelineCache.GlobalRootSignature());
    ID3D12PipelineState* current = nullptr;
    uint32_t drawCalls = 0;
    // segment / argument はインスタンス描画のときだけ使う。fade は切り替え中の区画。
    const auto drawPart = [&](size_t i, bool fade, size_t segment, size_t argument) {
        const auto slot = m_parts[i].slot;
        const auto* material =
            slot < model.materials.size() ? materials.Find(model.materials[slot]) : nullptr;
        const compositor::MaterialAsset fallback;
        const auto& asset = material ? *material : fallback;
        // アルファはベースカラーのマップから読むので、マップが無ければ抜かない。
        const bool cutout = asset.alphaCutoff > 0.0f &&
                            textures.SrvIndex(asset.baseColor, true) != compositor::kInvalidTextureIndex;
        auto* pipeline = pipelineFor(cutout, asset.twoSided, fade);
        if (!pipeline) return;
        if (pipeline != current) {
            commandList->SetPipelineState(pipeline);
            current = pipeline;
        }
        ModelConstants constants = {};
        // ベースカラーだけ sRGB として読む。それ以外はリニア（サムネイルと同じ）。
        constants.baseColorIndex = textures.SrvIndex(asset.baseColor, true);
        constants.normalIndex = textures.SrvIndex(asset.normal, false);
        constants.roughnessIndex = textures.SrvIndex(asset.roughness.texture, false);
        constants.metallicIndex = textures.SrvIndex(asset.metallic.texture, false);
        constants.aoIndex = textures.SrvIndex(asset.ambientOcclusion.texture, false);
        constants.mapChannels = compositor::PackMaterialChannels(asset);
        constants.baseColorTint[0] = asset.baseColorTint.x;
        constants.baseColorTint[1] = asset.baseColorTint.y;
        constants.baseColorTint[2] = asset.baseColorTint.z;
        constants.roughnessValue = asset.roughnessValue;
        constants.metallicValue = asset.metallicValue;
        constants.aoValue = asset.ambientOcclusionValue;
        constants.colorAdjust[0] = asset.hueShiftDegrees * (kPi / 180.0f);
        constants.colorAdjust[1] = asset.saturation;
        constants.brightness = asset.brightness;
        constants.flipNormalGreen = asset.flipNormalGreen ? 1u : 0u;
        constants.alphaCutoff = cutout ? asset.alphaCutoff : 0.0f;

        const auto position = m_camera.Position();
        std::memcpy(constants.cameraPosition, &position, sizeof(position));
        DirectX::XMStoreFloat4x4(
            &constants.viewProjection,
            DirectX::XMMatrixTranspose(m_camera.ViewMatrix() * m_camera.ProjectionMatrix()));
        const DirectX::XMFLOAT3 lightDirection = light.Direction();
        constants.lightDirection[0] = lightDirection.x;
        constants.lightDirection[1] = lightDirection.y;
        constants.lightDirection[2] = lightDirection.z;
        constants.lightIlluminance = light.illuminance;
        constants.lightColor[0] = light.color.x;
        constants.lightColor[1] = light.color.y;
        constants.lightColor[2] = light.color.z;

        const bool hasEnvironment = environment.IsReady();
        constants.iblIntensity = hasEnvironment ? iblIntensity : 0.0f;
        constants.irradianceIndex =
            hasEnvironment ? environment.IrradianceSrvIndex() : compositor::kInvalidTextureIndex;
        constants.prefilteredIndex = environment.PrefilteredSrvIndex();
        constants.brdfLutIndex = environment.BrdfLutSrvIndex();
        constants.prefilteredMipCount = environment.PrefilteredMipCount();
        constants.exposure = exposure;
        constants.tonemapMode = static_cast<uint32_t>(tonemap);
        if (instances) {
            const auto& draw = *instances;
            constants.points = draw.points; constants.rows = draw.rows;
            constants.visibleIndices = m_visibleInstances.srv.index; constants.seed = draw.seed;
            constants.weightStart = draw.weightStart; constants.weightEnd = draw.weightEnd;
            constants.scaleMin = draw.scaleMin; constants.scaleMax = draw.scaleMax;
            constants.align = draw.align; constants.offset = draw.offset;
            constants.usePointSize = draw.usePointSize; constants.sceneMode = 1;
            constants.shadows = draw.shadows;
            constants.atmosphere = draw.atmosphere;
            constants.cloudNoiseIndex = draw.cloudNoiseIndex;
            constants.atmosphericMode = draw.atmosphericMode;
            constants.clearIrradianceIndex = draw.ambient.clearIrradianceIndex;
            constants.ambientLow = draw.ambient.low;
            constants.ambientHigh = draw.ambient.high;
            constants.ambientOcclusion = draw.ambient.occlusion;
            const auto& lo = m_geometry->minimum; const auto& hi = m_geometry->maximum;
            constants.pivot[0] = (lo.x+hi.x)*0.5f; constants.pivot[1] = lo.y;
            constants.pivot[2] = (lo.z+hi.z)*0.5f;
            constants.visibleOffset = static_cast<uint32_t>(segment * draw.count);
            if (draw.lodView) {
                const size_t lod = std::min<size_t>(m_firstLod + m_parts[i].lod, std::size(kLodDebugColors) - 1);
                constants.lodView = 1;
                constants.baseColorTint[0] = kLodDebugColors[lod].x;
                constants.baseColorTint[1] = kLodDebugColors[lod].y;
                constants.baseColorTint[2] = kLodDebugColors[lod].z;
                constants.colorAdjust[0] = 0.0f;
                constants.colorAdjust[1] = 1.0f;
                constants.brightness = 1.0f;
            }
            constants.modelSize = std::max({hi.x-lo.x,hi.y-lo.y,hi.z-lo.z,0.0001f});
            DirectX::XMStoreFloat4x4(&constants.viewProjection,
                DirectX::XMMatrixTranspose(DirectX::XMLoadFloat4x4(&draw.viewProjection)));
            std::memcpy(constants.cameraPosition,&draw.cameraPosition,sizeof(constants.cameraPosition));
        }


        const auto cb = device.Upload().Allocate(sizeof(constants), 256);
        if (!cb.IsValid()) return;
        std::memcpy(cb.cpu, &constants, sizeof(constants));
        commandList->SetGraphicsRootConstantBufferView(1, cb.gpuAddress);
        if (instances) m_meshes[i].DrawIndirect(commandList, m_drawSignature.Get(),
            m_indirectArguments.resource.Get(), argument*sizeof(D3D12_DRAW_INDEXED_ARGUMENTS));
        else m_meshes[i].Draw(commandList);
        ++drawCalls;
    };
    if (!instances) {
        for (size_t i = 0; i < m_meshes.size(); ++i) drawPart(i, false, 0, 0);
    } else {
        const size_t lods = LodCount();
        for (size_t segment = 0; segment < SegmentCount(); ++segment) {
            const bool fade = segment >= lods;
            // 影と重ね合わせ無しでは切り替え中の区画は空なので描かない。
            if (fade && (instances->shadow || instances->fadeBand <= 0)) continue;
            const size_t level = segment % lods;
            for (size_t part = m_lodFirstPart[level]; part < m_lodFirstPart[level + 1]; ++part)
                drawPart(part, fade, segment,
                         m_segmentFirstArgument[segment] + (part - m_lodFirstPart[level]));
        }
    }
    if (!instances) rhi::TransitionIfNeeded(commandList, m_output, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    PIXEndEvent(commandList);
    return drawCalls;
}
}  // namespace tg::renderer
