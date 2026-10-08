#include "renderer/GeneratedMeshes.h"

#include "renderer/Environment.h"

#include <pix3.h>

#include <algorithm>
#include <cstring>

namespace tg::renderer {
namespace {

// GeneratedMesh.hlsl と同じ並び。
struct TrackSpanConstants {
    float start, transition;
    uint32_t material, boundary;  // 材質の枠（0〜3 がこの帯、4〜7 が内側の帯）と境界の枠
};
struct BoundaryConstants {
    uint32_t mask, height, flags, padding;  // flags: bit0 = 模様を U に繰り返す、bit1 = マスクを反転
    float width, repeat, depth, center;
};
constexpr size_t kMaterialSlots = GeneratedMeshItem::kMaxMaterials * 2;
struct GeneratedMeshConstants {
    DirectX::XMFLOAT4X4 viewProjection;  // 転置済み
    float cameraPosition[3];
    float iblIntensity;
    float lightDirection[3];
    float lightIlluminance;
    float lightColor[3];
    uint32_t materialMask;  // bit i: 材質の枠 i に材質があるか（無ければ表の fallbackColor）
    uint32_t irradianceIndex, prefilteredIndex, brdfLutIndex, prefilteredMipCount;
    float fallbackColor[3];
    float fallbackRoughness;
    float ambientLow, ambientHigh, ambientOcclusion;
    uint32_t clearIrradianceIndex;
    uint32_t cloudNoiseIndex, atmosphericMode;
    float roadWidth;
    uint32_t hasInner;
    uint32_t spanCount, innerSpanCount;
    float innerOrigin, innerSign;
    float innerFallbackColor[3];
    float innerFallbackRoughness;
    uint32_t cutoutOpacity, cutoutChannel, cutoutBaseColor;
    float cutoutThreshold, cutoutUvScale, cutoutValue, cutoutPadding[2];
    TrackSpanConstants spans[GeneratedMeshItem::kMaxSpans];
    TrackSpanConstants innerSpans[GeneratedMeshItem::kMaxSpans];
    BoundaryConstants boundaries[GeneratedMeshItem::kMaxBoundaries];
    SceneShadowData shadows;
    AtmosphereSettings atmosphere;
    compositor::LayerMaterialGpu materials[kMaterialSlots];
};
static_assert(sizeof(GeneratedMeshConstants) == 240 + 128 * 3 + 384 + 352 + 672 * kMaterialSlots);

// 材質の表を定数へ写す。slotOffset は材質の枠の始まり（この帯 0、内側の帯 4）。
uint32_t WriteTrack(const GeneratedMeshItem::Track& track, uint32_t slotOffset, TrackSpanConstants* spans,
                    uint32_t& materialMask, compositor::LayerMaterialGpu* materials) {
    const size_t materialCount = std::min(track.materials.size(), GeneratedMeshItem::kMaxMaterials);
    for (size_t i = 0; i < materialCount; ++i) {
        if (!track.materials[i].hasMaterial) continue;
        materialMask |= 1u << (slotOffset + i);
        materials[slotOffset + i] = track.materials[i].gpu;
    }
    const size_t count = std::min(track.spans.size(), GeneratedMeshItem::kMaxSpans);
    for (size_t i = 0; i < count; ++i) {
        const auto& span = track.spans[i];
        spans[i] = {span.startMeters, span.transitionMeters,
                    slotOffset + std::min<uint32_t>(span.material, static_cast<uint32_t>(materialCount ? materialCount - 1 : 0)),
                    span.boundary};
    }
    return static_cast<uint32_t>(std::max<size_t>(count, 1));
}

// 路面に貼る帯（区画線）の深度バイアス。road-material-editor と同じ値。
constexpr int kDecalDepthBias = -2000;
constexpr float kDecalSlopeScaledDepthBias = -2.0f;

// 材質が無いときの路面（アスファルトの目安の灰色）。
constexpr float kFallbackColor[3] = {0.18f, 0.18f, 0.18f};
constexpr float kFallbackRoughness = 0.85f;
// リファレンス表示の線の色（リニア）。地形のクレイと同じく、露出の掛かる明るめの灰色。
constexpr float kReferenceColor[3] = {0.5f, 0.5f, 0.5f};

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
            if (item.geometry != nullptr && !item.geometry->indices.empty())
                entry->mesh.Create(device, *item.geometry, L"GeneratedMesh");
            entry->geometryKey = item.geometryKey;
        }
        entry->look = item;
        entry->look.geometry = nullptr;
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
    // 路面に貼る帯は深度を手前へずらす（影の段では描かない）。
    rhi::GraphicsPipelineDesc decalDesc = desc;
    decalDesc.depthBias = kDecalDepthBias;
    decalDesc.slopeScaledDepthBias = kDecalSlopeScaledDepthBias;
    ID3D12PipelineState* decalPipeline = frame.shadow ? nullptr : pipelineCache.GetGraphics(decalDesc);
    // リファレンス表示は線だけ（影の段では描かない）。
    rhi::GraphicsPipelineDesc referenceDesc = desc;
    referenceDesc.fillMode = D3D12_FILL_MODE_WIREFRAME;
    ID3D12PipelineState* referencePipeline = frame.shadow ? nullptr : pipelineCache.GetGraphics(referenceDesc);

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
    ID3D12PipelineState* current = pipeline;
    for (const auto& entry : m_entries) {
        if (!entry || !entry->look.visible || !entry->mesh.IsValid()) continue;
        const GeneratedMeshItem& look = entry->look;
        if (look.decal && (frame.shadow || decalPipeline == nullptr || look.reference)) continue;
        if (look.reference && (frame.shadow || referencePipeline == nullptr)) continue;
        ID3D12PipelineState* wanted = look.reference ? referencePipeline : look.decal ? decalPipeline : pipeline;
        if (wanted != current) {
            commandList->SetPipelineState(wanted);
            current = wanted;
        }
        const auto cb = device.Upload().Allocate(sizeof(GeneratedMeshConstants), 256);
        if (!cb.IsValid()) break;
        GeneratedMeshConstants constants = base;
        constants.roadWidth = look.roadWidthMeters;
        std::memcpy(constants.fallbackColor, look.track.fallbackColor, sizeof(constants.fallbackColor));
        constants.materialMask = 0;
        constants.spanCount = WriteTrack(look.track, 0, constants.spans, constants.materialMask, constants.materials);
        constants.hasInner = look.hasInner ? 1u : 0u;
        constants.innerSpanCount = look.hasInner
            ? WriteTrack(look.innerTrack, static_cast<uint32_t>(GeneratedMeshItem::kMaxMaterials), constants.innerSpans,
                         constants.materialMask, constants.materials)
            : 0u;
        constants.innerOrigin = look.innerOrigin;
        constants.innerSign = look.innerSign;
        std::memcpy(constants.innerFallbackColor, look.innerTrack.fallbackColor, sizeof(constants.innerFallbackColor));
        constants.innerFallbackRoughness = kFallbackRoughness;
        for (size_t i = 0; i < GeneratedMeshItem::kMaxBoundaries; ++i) {
            BoundaryConstants& out = constants.boundaries[i];
            out = {0xffffffffu, 0xffffffffu, 0, 0, 0, 1, 0, 0.5f};
            if (i >= look.boundaries.size()) continue;
            const auto& boundary = look.boundaries[i];
            out = {boundary.maskIndex, boundary.heightIndex,
                   (boundary.alongU ? 1u : 0u) | (boundary.invertMask ? 2u : 0u), 0,
                   boundary.widthMeters, boundary.repeatMeters, boundary.depthMeters, boundary.heightCenter};
        }
        const auto& cutout = look.cutout;
        constants.cutoutOpacity = cutout.opacityIndex;
        constants.cutoutChannel = cutout.channel;
        constants.cutoutBaseColor = cutout.baseColorIndex;
        constants.cutoutThreshold = cutout.threshold;
        constants.cutoutUvScale = cutout.uvScale;
        constants.cutoutValue = cutout.value;
        if (look.reference) {
            // 材質・境界・切り抜きを外し、単色の陰影で線を引く。
            constants.materialMask = 0;
            constants.spanCount = 1;
            constants.spans[0] = {0, 0, 0, GeneratedMeshItem::kNoBoundary};
            std::memcpy(constants.fallbackColor, kReferenceColor, sizeof(constants.fallbackColor));
            constants.hasInner = 0;
            constants.cutoutThreshold = 0;
        }
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

}  // namespace tg::renderer
