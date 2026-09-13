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
};
static_assert(sizeof(ModelConstants) == 640);

}  // namespace
void ModelPreview::Destroy(rhi::Device& device) {
    for (auto& mesh : m_meshes) mesh.Release(device);
    m_meshes.clear();
    m_geometry.reset();
    m_lod = -1;
    device.DeferRelease(m_output);
    device.DeferRelease(m_depth);
    device.DeferRelease(m_visibleInstances);
    device.DeferRelease(m_indirectArguments);
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
    lod = std::clamp(lod, 0, static_cast<int>(asset.geometry->lods.size()) - 1);
    if (m_geometry == asset.geometry && m_lod == lod) return true;
    for (auto& mesh : m_meshes) mesh.Release(device);
    m_meshes.clear();
    const bool changed = m_geometry != asset.geometry;
    m_geometry = asset.geometry;
    m_lod = -1;
    for (const auto& part : m_geometry->lods[lod].parts) {
        m_meshes.emplace_back();
        if (!m_meshes.back().Create(device, part.mesh, L"ModelPreviewMesh")) return false;
    }
    m_lod = lod;
    if (changed) ResetView();
    return true;
}
bool ModelPreview::CullInstances(rhi::Device& device, rhi::PipelineCache& cache,
                                 ID3D12GraphicsCommandList* list, const ModelInstanceDraw& draw) {
    if (!draw.count || m_meshes.empty()) return false;
    auto* cull = cache.GetCompute(L"InstanceCulling.hlsl", L"CsCull");
    auto* finish = cache.GetCompute(L"InstanceCulling.hlsl", L"CsFinish");
    if (!cull || !finish) return false;
    if (!m_drawSignature) {
        D3D12_INDIRECT_ARGUMENT_DESC argument{};
        argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
        D3D12_COMMAND_SIGNATURE_DESC desc{};
        desc.ByteStride = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
        desc.NumArgumentDescs = 1; desc.pArgumentDescs = &argument;
        if (!TG_CHECK_HR(device.GetDevice()->CreateCommandSignature(&desc,nullptr,IID_PPV_ARGS(&m_drawSignature)))) return false;
    }
    if (m_visibleInstances.sizeInBytes < uint64_t(draw.count)*sizeof(uint32_t)) {
        device.DeferRelease(m_visibleInstances);
        if (!device.Allocator().CreateStructuredBuffer(draw.count,sizeof(uint32_t),L"VisibleInstances",m_visibleInstances,true)) return false;
    }
    const uint32_t parts = static_cast<uint32_t>(m_meshes.size());
    const uint64_t argumentBytes = uint64_t(parts)*sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
    if (m_indirectArguments.sizeInBytes != argumentBytes) {
        device.DeferRelease(m_indirectArguments);
        if (!device.Allocator().CreateStructuredBuffer(parts,sizeof(D3D12_DRAW_INDEXED_ARGUMENTS),L"InstanceArguments",m_indirectArguments,true)) return false;
    }
    const auto transition = [&](rhi::GpuBuffer& buffer, D3D12_RESOURCE_STATES state) {
        if (buffer.state == state) return;
        const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(buffer.resource.Get(),buffer.state,state);
        list->ResourceBarrier(1,&barrier); buffer.state = state;
    };
    const auto upload = device.Upload().Allocate(argumentBytes,16);
    if (!upload.IsValid()) return false;
    auto* arguments = static_cast<D3D12_DRAW_INDEXED_ARGUMENTS*>(upload.cpu);
    for (uint32_t i=0;i<parts;++i) arguments[i] = {m_meshes[i].IndexCount(),0,0,0,0};
    struct CullConstants {
        std::array<DirectX::XMFLOAT4,6> planes;
        uint32_t points, visible, arguments, count;
        uint32_t seed, partCount; float weightStart, weightEnd;
        float scaleMin, scaleMax, modelSize, radius;
        DirectX::XMFLOAT3 camera; float maxDistance;
        float offset; uint32_t usePointSize; uint32_t padding[2]{};
    } constants{};
    static_assert(sizeof(CullConstants)==176);
    constants.planes = InstanceFrustumPlanes(draw.viewProjection);
    constants.points = draw.points; constants.visible = m_visibleInstances.uav.index;
    constants.arguments = m_indirectArguments.uav.index; constants.count = draw.count;
    constants.seed = draw.seed; constants.partCount = parts;
    constants.weightStart = draw.weightStart; constants.weightEnd = draw.weightEnd;
    constants.scaleMin = draw.scaleMin; constants.scaleMax = draw.scaleMax;
    const auto& lo = m_geometry->minimum; const auto& hi = m_geometry->maximum;
    const float x = (hi.x-lo.x)*0.5f, y = hi.y-lo.y, z = (hi.z-lo.z)*0.5f;
    constants.modelSize = std::max({x*2,y,z*2,0.0001f});
    constants.radius = std::sqrt(x*x+y*y+z*z);
    constants.camera = draw.cameraPosition; constants.maxDistance = draw.maxDistance;
    constants.offset = draw.offset; constants.usePointSize = draw.usePointSize;
    const auto cb = device.Upload().Allocate(sizeof(constants),256);
    if (!cb.IsValid()) return false;
    std::memcpy(cb.cpu,&constants,sizeof(constants));
    PIXBeginEvent(list, PIX_COLOR(120,200,200), "CullModelInstances");
    transition(m_indirectArguments,D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyBufferRegion(m_indirectArguments.resource.Get(),0,upload.resource,upload.offset,argumentBytes);
    transition(m_indirectArguments,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    transition(m_visibleInstances,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list->SetComputeRootSignature(cache.GlobalRootSignature());
    list->SetComputeRootConstantBufferView(1,cb.gpuAddress);
    list->SetPipelineState(cull); list->Dispatch((draw.count+63)/64,1,1);
    const auto barrier = CD3DX12_RESOURCE_BARRIER::UAV(nullptr);
    list->ResourceBarrier(1,&barrier);
    if (parts > 1) {
        list->SetPipelineState(finish); list->Dispatch((parts+63)/64,1,1);
        list->ResourceBarrier(1,&barrier);
    }
    transition(m_visibleInstances,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(m_indirectArguments,D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    PIXEndEvent(list);
    return true;
}

void ModelPreview::Render(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                          ID3D12GraphicsCommandList* commandList, const ModelAsset& model,
                          const compositor::MaterialLibrary& materials,
                          const compositor::TextureLibrary& textures,
                          const Environment& environment, float iblIntensity,
                          const LightSettings& light, float exposure, TonemapMode tonemap, const ModelInstanceDraw* instances) {
    if (!m_geometry || m_lod < 0) return;
    rhi::GraphicsPipelineDesc desc;
    desc.shaderPath = L"ModelPreview.hlsl";
    desc.vertexEntry = L"VsMain";
    desc.pixelEntry = L"PsMain";
    desc.layout = rhi::VertexLayout::MeshStandard;
    desc.rtvFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.dsvFormat = DXGI_FORMAT_D32_FLOAT;
    if (instances) {
        desc.rtvFormat = instances->shadow ? DXGI_FORMAT_UNKNOWN : DXGI_FORMAT_R16G16B16A16_FLOAT;
        if (instances->shadow) desc.pixelEntry.clear();
        desc.cullMode = D3D12_CULL_MODE_NONE;
    }
    auto* pipeline = pipelineCache.GetGraphics(desc);
    if (!pipeline) return;
    if (!instances) {
    if (!m_output.IsValid()) {
        rhi::TextureDesc target;
        target.width = kOutputSize;
        target.height = kOutputSize;
        target.allowRenderTarget = true;
        target.clearColor[0] = target.clearColor[1] = target.clearColor[2] = 0.025f;
        target.debugName = L"ModelPreview";
        if (!device.Allocator().CreateTexture2D(target, m_output)) return;
    }
    if (!m_depth.IsValid()) {
        rhi::TextureDesc depth;
        depth.width = kOutputSize;
        depth.height = kOutputSize;
        depth.format = DXGI_FORMAT_D32_FLOAT;
        depth.allowDepthStencil = true;
        depth.createSrv = false;
        depth.debugName = L"ModelPreviewDepth";
        if (!device.Allocator().CreateTexture2D(depth, m_depth)) return;
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
    if (instances && !CullInstances(device, pipelineCache, commandList, *instances)) { PIXEndEvent(commandList); return; }
    commandList->SetGraphicsRootSignature(pipelineCache.GlobalRootSignature());
    commandList->SetPipelineState(pipeline);
    for (size_t i = 0; i < m_meshes.size(); ++i) {
        const auto slot = m_geometry->lods[m_lod].parts[i].slot;
        const auto* material =
            slot < model.materials.size() ? materials.Find(model.materials[slot]) : nullptr;
        const compositor::MaterialAsset fallback;
        const auto& asset = material ? *material : fallback;
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
        constants.flipNormalGreen = asset.flipNormalGreen ? 1u : 0u;

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
            const auto& lo = m_geometry->minimum; const auto& hi = m_geometry->maximum;
            constants.pivot[0] = (lo.x+hi.x)*0.5f; constants.pivot[1] = lo.y;
            constants.pivot[2] = (lo.z+hi.z)*0.5f;
            constants.modelSize = std::max({hi.x-lo.x,hi.y-lo.y,hi.z-lo.z,0.0001f});
            DirectX::XMStoreFloat4x4(&constants.viewProjection,
                DirectX::XMMatrixTranspose(DirectX::XMLoadFloat4x4(&draw.viewProjection)));
            std::memcpy(constants.cameraPosition,&draw.cameraPosition,sizeof(constants.cameraPosition));
        }


        const auto cb = device.Upload().Allocate(sizeof(constants), 256);
        if (!cb.IsValid()) continue;
        std::memcpy(cb.cpu, &constants, sizeof(constants));
        commandList->SetGraphicsRootConstantBufferView(1, cb.gpuAddress);
        if (instances) m_meshes[i].DrawIndirect(commandList, m_drawSignature.Get(),
            m_indirectArguments.resource.Get(), i*sizeof(D3D12_DRAW_INDEXED_ARGUMENTS));
        else m_meshes[i].Draw(commandList);
    }
    if (!instances) rhi::TransitionIfNeeded(commandList, m_output, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    PIXEndEvent(commandList);
}
}  // namespace tg::renderer
