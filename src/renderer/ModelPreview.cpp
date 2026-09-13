#include "renderer/ModelPreview.h"

#include <pix3.h>

#include <algorithm>
#include <cmath>
#include <cstring>
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
    uint32_t points, rows, instanceCount, seed;
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
            constants.instanceCount = draw.count; constants.seed = draw.seed;
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
        m_meshes[i].Draw(commandList, false, instances ? instances->count : 1);
    }
    if (!instances) rhi::TransitionIfNeeded(commandList, m_output, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    PIXEndEvent(commandList);
}
}  // namespace tg::renderer
