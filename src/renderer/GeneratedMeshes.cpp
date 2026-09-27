#include "renderer/GeneratedMeshes.h"

#include "renderer/Environment.h"

#include <pix3.h>

#include <algorithm>
#include <cstring>

namespace tg::renderer {
namespace {

// GeneratedMesh.hlsl と同じ並び。
struct GeneratedMeshConstants {
    DirectX::XMFLOAT4X4 viewProjection;  // 転置済み
    float cameraPosition[3];
    float iblIntensity;
    float lightDirection[3];
    float lightIlluminance;
    float lightColor[3];
    uint32_t hasMaterial;
    uint32_t irradianceIndex, prefilteredIndex, brdfLutIndex, prefilteredMipCount;
    float fallbackColor[3];
    float fallbackRoughness;
    float ambientLow, ambientHigh, ambientOcclusion;
    uint32_t clearIrradianceIndex;
    uint32_t cloudNoiseIndex, atmosphericMode;
    float roadWidth, padding;
    uint32_t boundaryMask, boundaryHeight, boundaryFlags, hasInner;
    float boundaryWidth, boundaryRepeat, boundaryDepth, boundaryCenter;
    float innerOrigin, innerSign, boundaryPadding[2];
    float innerFallbackColor[3];
    float innerFallbackRoughness;
    SceneShadowData shadows;
    AtmosphereSettings atmosphere;
    compositor::LayerMaterialGpu material;
    compositor::LayerMaterialGpu innerMaterial;
};
static_assert(sizeof(GeneratedMeshConstants) == 176 + 64 + 384 + 352 + 672 * 2);

// 材質が無いときの路面（アスファルトの目安の灰色）。
constexpr float kFallbackColor[3] = {0.18f, 0.18f, 0.18f};
constexpr float kFallbackRoughness = 0.85f;

}  // namespace

void GeneratedMeshes::Update(rhi::Device& device, const std::vector<GeneratedMeshItem>& items) {
    std::vector<std::unique_ptr<Entry>> next;
    next.reserve(items.size());
    for (const GeneratedMeshItem& item : items) {
        std::unique_ptr<Entry> entry;
        const auto found = std::find_if(m_entries.begin(), m_entries.end(),
                                        [&](const auto& e) { return e && e->id == item.id; });
        if (found != m_entries.end()) {
            entry = std::move(*found);
        } else {
            entry = std::make_unique<Entry>();
            entry->id = item.id;
        }
        if (!entry->mesh.IsValid() || entry->geometryKey != item.geometryKey) {
            entry->mesh.Release(device);
            entry->vertices = entry->triangles = 0;
            if (item.geometry != nullptr && !item.geometry->indices.empty() &&
                entry->mesh.Create(device, *item.geometry, L"GeneratedMesh")) {
                entry->vertices = item.geometry->vertices.size();
                entry->triangles = item.geometry->indices.size() / 3;
            }
            entry->geometryKey = item.geometryKey;
        }
        entry->hasMaterial = item.hasMaterial;
        entry->material = item.material;
        entry->roadWidthMeters = item.roadWidthMeters;
        std::copy(std::begin(item.fallbackColor), std::end(item.fallbackColor), entry->fallbackColor);
        entry->boundary = item.boundary;
        entry->hasInner = item.hasInner;
        entry->innerMaterial = item.innerMaterial;
        entry->innerOrigin = item.innerOrigin;
        entry->innerSign = item.innerSign;
        std::copy(std::begin(item.innerFallbackColor), std::end(item.innerFallbackColor), entry->innerFallbackColor);
        next.push_back(std::move(entry));
    }
    for (auto& stale : m_entries) {
        if (stale) stale->mesh.Release(device);
    }
    m_entries = std::move(next);
}

uint32_t GeneratedMeshes::Draw(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                               ID3D12GraphicsCommandList* commandList, const GeneratedMeshFrame& frame) const {
    if (m_entries.empty()) return 0;
    // 影は深度だけ（PS なし）。本描画はシーンの HDR へ。路面は上からも下からも見えるので
    // 裏面を落とさない（モデルの配置と同じ）。
    rhi::GraphicsPipelineDesc desc;
    desc.shaderPath = L"GeneratedMesh.hlsl";
    desc.vertexEntry = L"VsMain";
    desc.pixelEntry = frame.shadow ? L"" : L"PsMain";
    desc.layout = rhi::VertexLayout::MeshStandard;
    desc.rtvFormat = frame.shadow ? DXGI_FORMAT_UNKNOWN : DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.dsvFormat = DXGI_FORMAT_D32_FLOAT;
    desc.cullMode = D3D12_CULL_MODE_NONE;
    ID3D12PipelineState* pipeline = pipelineCache.GetGraphics(desc);
    if (pipeline == nullptr) return 0;

    PIXBeginEvent(commandList, PIX_COLOR(200, 180, 120), frame.shadow ? "GeneratedMeshesShadow" : "GeneratedMeshes");
    commandList->SetGraphicsRootSignature(pipelineCache.GlobalRootSignature());
    commandList->SetPipelineState(pipeline);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    GeneratedMeshConstants base = {};
    DirectX::XMStoreFloat4x4(&base.viewProjection,
                             DirectX::XMMatrixTranspose(DirectX::XMLoadFloat4x4(&frame.viewProjection)));
    std::memcpy(base.cameraPosition, &frame.cameraPosition, sizeof(base.cameraPosition));
    std::memcpy(base.lightDirection, &frame.lightDirection, sizeof(base.lightDirection));
    std::memcpy(base.lightColor, &frame.lightColor, sizeof(base.lightColor));
    base.lightIlluminance = frame.lightIlluminance;
    const bool hasEnvironment = frame.environment != nullptr && frame.environment->IsReady();
    base.iblIntensity = hasEnvironment ? frame.iblIntensity : 0.0f;
    base.irradianceIndex = hasEnvironment ? frame.environment->IrradianceSrvIndex() : compositor::kInvalidTextureIndex;
    base.prefilteredIndex = hasEnvironment ? frame.environment->PrefilteredSrvIndex() : compositor::kInvalidTextureIndex;
    base.brdfLutIndex = hasEnvironment ? frame.environment->BrdfLutSrvIndex() : compositor::kInvalidTextureIndex;
    base.prefilteredMipCount = hasEnvironment ? frame.environment->PrefilteredMipCount() : 1;
    std::memcpy(base.fallbackColor, kFallbackColor, sizeof(base.fallbackColor));
    base.fallbackRoughness = kFallbackRoughness;
    base.ambientLow = frame.ambient.low;
    base.ambientHigh = frame.ambient.high;
    base.ambientOcclusion = frame.ambient.occlusion;
    base.clearIrradianceIndex = frame.ambient.clearIrradianceIndex;
    base.cloudNoiseIndex = frame.cloudNoiseIndex;
    base.atmosphericMode = frame.atmosphericMode;
    base.shadows = frame.shadows;
    base.atmosphere = frame.atmosphere;

    uint32_t drawCalls = 0;
    for (const auto& entry : m_entries) {
        if (!entry || !entry->mesh.IsValid()) continue;
        const auto cb = device.Upload().Allocate(sizeof(GeneratedMeshConstants), 256);
        if (!cb.IsValid()) break;
        GeneratedMeshConstants constants = base;
        constants.hasMaterial = entry->hasMaterial ? 1u : 0u;
        constants.material = entry->material;
        constants.roadWidth = entry->roadWidthMeters;
        std::memcpy(constants.fallbackColor, entry->fallbackColor, sizeof(constants.fallbackColor));
        const auto& boundary = entry->boundary;
        constants.boundaryMask = boundary.maskIndex;
        constants.boundaryHeight = boundary.heightIndex;
        constants.boundaryFlags = (boundary.alongU ? 1u : 0u) | (boundary.invertMask ? 2u : 0u);
        constants.boundaryWidth = boundary.widthMeters;
        constants.boundaryRepeat = boundary.repeatMeters;
        constants.boundaryDepth = boundary.depthMeters;
        constants.boundaryCenter = boundary.heightCenter;
        constants.hasInner = entry->hasInner ? 1u : 0u;
        constants.innerMaterial = entry->innerMaterial;
        constants.innerOrigin = entry->innerOrigin;
        constants.innerSign = entry->innerSign;
        std::memcpy(constants.innerFallbackColor, entry->innerFallbackColor, sizeof(constants.innerFallbackColor));
        constants.innerFallbackRoughness = kFallbackRoughness;
        std::memcpy(cb.cpu, &constants, sizeof(constants));
        commandList->SetGraphicsRootConstantBufferView(1, cb.gpuAddress);
        entry->mesh.Draw(commandList);
        ++drawCalls;
    }
    PIXEndEvent(commandList);
    return drawCalls;
}

void GeneratedMeshes::Destroy(rhi::Device& device) {
    for (auto& entry : m_entries) {
        if (entry) entry->mesh.Release(device);
    }
    m_entries.clear();
}

uint64_t GeneratedMeshes::Vertices() const {
    uint64_t total = 0;
    for (const auto& entry : m_entries) if (entry) total += entry->vertices;
    return total;
}

uint64_t GeneratedMeshes::Triangles() const {
    uint64_t total = 0;
    for (const auto& entry : m_entries) if (entry) total += entry->triangles;
    return total;
}

}  // namespace tg::renderer
