#include "compositor/MaterialLibrary.h"

#include "core/Log.h"
#include "graph/SurfacePresetGraph.h"
#include <cstring>

#include <pix3.h>

#include <algorithm>

namespace tg::compositor {
namespace {

using rhi::DispatchCount;
// サムネイルの一辺。一覧で並べる大きさに対して十分で、VRAM も食わない。
constexpr uint32_t kThumbnailSize = 128;
// サムネイルの中でマップを何回並べるか。1 未満なら並べず、拡大して一部だけを写す。
//
// **0.25（マップの 1/4 だけを拡大して写す）。** 素材の判別は 84 px の
// マテリアル一覧だけでなく、レイヤー一覧の行（32 px）でもできる必要がある。
// 1 枚まるごと収めても行では模様が潰れるため、粒を大きく写すほうを採る。
// サムネイルは「何が写っているか」を見分けるためのもので、
// マップ全体を見せる場所ではない（それはテクスチャ一覧の役目）。
constexpr float kThumbnailUvScale = 0.25f;

// GPU 側の ThumbnailConstants と一致させること。
struct ThumbnailConstants {
    uint32_t outputIndex;
    uint32_t size;
    uint32_t baseColorIndex;
    uint32_t normalIndex;

    uint32_t roughnessIndex;
    uint32_t metallicIndex;
    uint32_t aoIndex;
    uint32_t heightIndex;

    float baseColorTint[3];
    float roughnessValue;

    float metallicValue;
    float aoValue;
    float uvScale;
    uint32_t mapChannels;

    // ベースカラーの調整。**合成と同じ値を渡すこと**（違うとサムネイルと本番で色が変わる）。
    float colorAdjust[2];  // 色相（ラジアン）, 彩度
    float brightness;      // 明度（倍率）
    float pad0;
    LayerMaterialGpu layerMaterial;
};

}  // namespace

uint32_t PackMaterialChannels(const MaterialAsset& asset) {
    // 並びは TG_CHANNEL_SLOT_* と一致させること。
    return PackChannel(asset.roughness.channel, 0) | PackChannel(asset.metallic.channel, 1) |
           PackChannel(asset.ambientOcclusion.channel, 2) | PackChannel(asset.height.channel, 3);
}

namespace {

}  // namespace

LayerMaterialGpu MaterialLibrary::CompileLayerMaterial(const MaterialAsset& asset, const TextureLibrary& textures, std::string& error) const {
    LayerMaterialGpu result;
    error.clear();
    if (!asset.layerMaterial) return result;
    std::vector<graph::PresetMaterial> layers;
    if (!graph::CompilePresetMaterials(*asset.layerMaterial, layers, error) || layers.size() > 4) return result;
    result.blendRange = asset.layerMaterial->layerBlendRange;
    result.displacementMeters = asset.layerMaterial->displacementMeters;
    for (const auto& layer : layers) {
        const auto* source = Find(layer.material);
        if ((layer.material && !source) || (source && source->layerMaterial)) { error = "合成材質の入力には通常のPBR素材を指定してください"; return {}; }
        if (layer.mask && (layer.mask->shape == graph::RoadMaskShape::WheelTracks || layer.mask->shape == graph::RoadMaskShape::EdgeFalloff)) {
            error = "轍・道路端のマスクには道路の座標が必要です。Surfaceでは定数またはノイズを使用してください"; return {};
        }
        auto& g = result.slots[result.count++];
        std::fill(std::begin(g.textures0), std::end(g.textures0), kInvalidTextureIndex);
        std::fill(std::begin(g.textures1), std::end(g.textures1), kInvalidTextureIndex);
        g.color[0] = source ? source->baseColorTint.x : layer.baseColor[0];
        g.color[1] = source ? source->baseColorTint.y : layer.baseColor[1];
        g.color[2] = source ? source->baseColorTint.z : layer.baseColor[2];
        g.color[3] = layer.uvRepeatMeters;
        g.surface[0] = source ? source->roughnessValue : layer.roughness;
        g.surface[1] = source ? source->metallicValue : layer.metallic;
        g.surface[2] = source ? source->ambientOcclusionValue : layer.ambientOcclusion;
        g.surface[3] = layer.worldUv ? 1.0f : 0.0f;
        g.adjust[0] = source ? source->hueShiftDegrees * 0.01745329252f : 0;
        g.adjust[1] = source ? source->saturation : 1;
        g.adjust[2] = source ? source->brightness : 1;
        g.adjust[3] = source && source->flipNormalGreen ? 1.0f : 0.0f;
        if (source) {
            g.textures0[0] = textures.SrvIndex(source->baseColor, true); g.textures0[1] = textures.SrvIndex(source->normal, false);
            g.textures0[2] = textures.SrvIndex(source->roughness.texture, false); g.textures0[3] = textures.SrvIndex(source->metallic.texture, false);
            g.textures1[0] = textures.SrvIndex(source->ambientOcclusion.texture, false); g.textures1[1] = textures.SrvIndex(source->height.texture, false);
            g.textures1[2] = PackMaterialChannels(*source);
        } else g.textures1[2] = 0;
        g.textures1[3] = layer.enabled ? 1 : 0;
        g.mask[0] = -1;
        if (layer.mask) {
            const auto& m = *layer.mask;
            g.mask[0] = static_cast<float>(m.shape); g.mask[1] = m.noiseScaleMeters;
            g.mask[2] = m.threshold; g.mask[3] = m.softness;
            g.breakup[0] = m.strength; g.breakup[1] = m.breakupAmount;
            g.breakup[2] = m.breakupScaleMeters; g.breakup[3] = static_cast<float>(m.seed);
            if (m.invert) g.textures1[3] |= 2;
        }
        g.blend[0] = static_cast<float>(layer.blendMode); g.blend[1] = static_cast<float>(layer.heightGate);
        g.blend[2] = layer.heightGateThreshold; g.blend[3] = layer.heightGateSoftness;
    }
    return result;
}

void MaterialLibrary::Destroy(rhi::Device& device) {
    for (MaterialAsset& asset : m_entries) {
        device.DeferRelease(asset.thumbnail);
    }
    m_entries.clear();
}

void MaterialLibrary::Clear(rhi::Device& device) {
    Destroy(device);
}

MaterialAssetId MaterialLibrary::Add(const std::string& name) {
    MaterialAsset asset;
    asset.id = m_nextId++;
    asset.name = name;
    m_entries.push_back(std::move(asset));
    return m_entries.back().id;
}

MaterialAsset& MaterialLibrary::RestoreAsset(MaterialAssetId id, const std::string& name) {
    if (MaterialAsset* existing = FindMutable(id); existing != nullptr) {
        return *existing;
    }

    MaterialAsset asset;
    asset.id = id;
    asset.name = name;
    m_entries.push_back(std::move(asset));

    // 次に払い出す ID を戻した ID より先へ進めておく。
    // そうしないと、この後の Add が同じ番号を配って衝突する。
    m_nextId = std::max(m_nextId, id + 1);

    // 並びは ID 順に保つ。一覧の見え方が undo のたびに入れ替わらないようにする。
    std::sort(m_entries.begin(), m_entries.end(),
              [](const MaterialAsset& a, const MaterialAsset& b) { return a.id < b.id; });
    return *FindMutable(id);
}

MaterialAssetId MaterialLibrary::Duplicate(const MaterialAsset& source) {
    MaterialAsset asset = source;
    asset.id = m_nextId++;
    asset.name = source.name + " のコピー";
    asset.assetPath.clear(); asset.assetUid.clear();
    // サムネイルは共有しない。作り直させる。
    asset.thumbnail = rhi::GpuTexture{};
    asset.thumbnailDirty = true;
    m_entries.push_back(std::move(asset));
    return m_entries.back().id;
}

void MaterialLibrary::Remove(rhi::Device& device, MaterialAssetId id) {
    const auto it = std::find_if(m_entries.begin(), m_entries.end(),
                                 [id](const MaterialAsset& asset) { return asset.id == id; });
    if (it == m_entries.end()) {
        return;
    }
    device.DeferRelease(it->thumbnail);
    m_entries.erase(it);
}

const MaterialAsset* MaterialLibrary::Find(MaterialAssetId id) const {
    if (id == kNoMaterialAsset) {
        return nullptr;
    }
    for (const MaterialAsset& asset : m_entries) {
        if (asset.id == id) {
            return &asset;
        }
    }
    return nullptr;
}

MaterialAsset* MaterialLibrary::FindMutable(MaterialAssetId id) {
    return const_cast<MaterialAsset*>(Find(id));
}

void MaterialLibrary::AssignOrdTexture(MaterialAssetId id, TextureId texture) {
    MaterialAsset* asset = FindMutable(id);
    if (asset == nullptr) {
        return;
    }
    // Megascans の `_ORD` は O = Occlusion(R) / R = Roughness(G) / D = Displacement(B)。
    asset->ambientOcclusion = MapSlot{texture, TextureChannel::R};
    asset->roughness = MapSlot{texture, TextureChannel::G};
    asset->height = MapSlot{texture, TextureChannel::B};
    asset->thumbnailDirty = true;
}

void MaterialLibrary::MarkThumbnailDirty(MaterialAssetId id) {
    if (MaterialAsset* asset = FindMutable(id); asset != nullptr) {
        asset->thumbnailDirty = true;
    }
}

D3D12_GPU_DESCRIPTOR_HANDLE MaterialLibrary::ThumbnailHandle(MaterialAssetId id) const {
    const MaterialAsset* asset = Find(id);
    if (asset == nullptr || !asset->thumbnail.IsValid()) {
        return D3D12_GPU_DESCRIPTOR_HANDLE{0};
    }
    return asset->thumbnail.srv.gpu;
}

void MaterialLibrary::ProcessPendingWork(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                                         const TextureLibrary& textures, bool buildThumbnails) {
    const bool sourceDirty = std::any_of(m_entries.begin(), m_entries.end(), [](const auto& a) { return a.thumbnailDirty && !a.layerMaterial; });
    for (auto& asset : m_entries) if (asset.layerMaterial) {
        if (sourceDirty) asset.thumbnailDirty = true;
        if (asset.thumbnailDirty) {
            const auto previousError = asset.layerError;
            asset.layerGpu = CompileLayerMaterial(asset, textures, asset.layerError);
            if (!asset.layerError.empty() && asset.layerError != previousError) TG_LOG_ERROR("%s: %s", asset.name.c_str(), asset.layerError.c_str());
        }
    }
    if (buildThumbnails) {
        BuildPendingThumbnails(device, pipelineCache, textures, nullptr);
    }
}

void MaterialLibrary::RenderPendingThumbnails(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                                              ID3D12GraphicsCommandList* commandList,
                                              const TextureLibrary& textures) {
    if (commandList == nullptr) {
        return;
    }
    BuildPendingThumbnails(device, pipelineCache, textures, commandList);
}

void MaterialLibrary::BuildPendingThumbnails(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                                             const TextureLibrary& textures,
                                             ID3D12GraphicsCommandList* commandList) {
    const bool anyDirty = std::any_of(m_entries.begin(), m_entries.end(),
                                      [](const MaterialAsset& a) { return a.thumbnailDirty; });
    if (!anyDirty) {
        return;
    }

    for (MaterialAsset& asset : m_entries) {
        if (!asset.thumbnailDirty) {
            continue;
        }
        if (!BuildThumbnail(device, pipelineCache, textures, asset, commandList)) {
            // 失敗を繰り返さないよう、要求は落とす。
            TG_LOG_WARN("マテリアル「%s」のサムネイルを作れませんでした", asset.name.c_str());
        }
        asset.thumbnailDirty = false;
    }
}

bool MaterialLibrary::BuildThumbnail(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                                     const TextureLibrary& textures, MaterialAsset& asset,
                                     ID3D12GraphicsCommandList* commandList) {
    ID3D12PipelineState* pipeline =
        pipelineCache.GetCompute(L"MaterialThumbnail.hlsl", L"CsMain");
    if (pipeline == nullptr) {
        return false;
    }

    if (!asset.thumbnail.IsValid()) {
        rhi::TextureDesc desc;
        desc.width = kThumbnailSize;
        desc.height = kThumbnailSize;
        desc.format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.allowUnorderedAccess = true;
        // 初回は Discard で初期化してから書く（TextureLibrary::BuildPreview と同じ理由）。
        // D3D12MA の配置リソースは解放跡のメモリを再利用するため、初期化せずに
        // UAV で書くと GPU ベースバリデーションが「レイアウト COMMON のまま書いた」と
        // 報告する。Discard は直接キューでは RENDER_TARGET 状態を要求するので
        // RTV フラグも付ける。
        desc.allowRenderTarget = true;
        desc.createSrv = true;
        desc.initialState = D3D12_RESOURCE_STATE_COMMON;
        desc.debugName = L"MaterialThumbnail";
        if (!device.Allocator().CreateTexture2D(desc, asset.thumbnail)) {
            return false;
        }
    }

    ThumbnailConstants constants = {};
    constants.layerMaterial = asset.layerGpu;
    constants.outputIndex = asset.thumbnail.UavIndex();
    constants.size = kThumbnailSize;
    // ベースカラーだけ sRGB として読む。それ以外はリニア。
    constants.baseColorIndex = textures.SrvIndex(asset.baseColor, true);
    constants.normalIndex = textures.SrvIndex(asset.normal, false);
    constants.roughnessIndex = textures.SrvIndex(asset.roughness.texture, false);
    constants.metallicIndex = textures.SrvIndex(asset.metallic.texture, false);
    constants.aoIndex = textures.SrvIndex(asset.ambientOcclusion.texture, false);
    constants.heightIndex = textures.SrvIndex(asset.height.texture, false);
    constants.mapChannels = PackMaterialChannels(asset);
    constants.baseColorTint[0] = asset.baseColorTint.x;
    constants.baseColorTint[1] = asset.baseColorTint.y;
    constants.baseColorTint[2] = asset.baseColorTint.z;
    constants.roughnessValue = asset.roughnessValue;
    constants.metallicValue = asset.metallicValue;
    constants.aoValue = asset.ambientOcclusionValue;
    constants.uvScale = asset.layerMaterial ? 8.0f : kThumbnailUvScale;
    constants.colorAdjust[0] = asset.hueShiftDegrees * (3.14159265358979f / 180.0f);
    constants.colorAdjust[1] = asset.saturation;
    constants.brightness = asset.brightness;

    rhi::GpuTexture& thumbnail = asset.thumbnail;
    const auto record = [&](ID3D12GraphicsCommandList* list) {
        PIXBeginEvent(list, PIX_COLOR(120, 200, 200), "MaterialThumbnail");

        // 作った直後（COMMON）は Discard で初期化してから UAV へ。
        // 中身はディスパッチが全画素を書き潰すので、初期化は Discard で十分。
        if (thumbnail.state == D3D12_RESOURCE_STATE_COMMON) {
            rhi::TransitionIfNeeded(list, thumbnail, D3D12_RESOURCE_STATE_RENDER_TARGET);
            list->DiscardResource(thumbnail.resource.Get(), nullptr);
        }
        rhi::TransitionIfNeeded(list, thumbnail, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        list->SetComputeRootSignature(pipelineCache.GlobalRootSignature());
        list->SetPipelineState(pipeline);
        const auto cb = device.Upload().Allocate(sizeof(constants), 256);
        if (!cb.IsValid()) { PIXEndEvent(list); return; }
        std::memcpy(cb.cpu, &constants, sizeof(constants));
        list->SetComputeRootConstantBufferView(1, cb.gpuAddress);
        list->Dispatch(DispatchCount(kThumbnailSize), DispatchCount(kThumbnailSize), 1);

        // ImGui から SRV として読むので、ピクセルシェーダ可視の状態へ移す。
        // 同じコマンドリストのこの後に ImGui の描画が積まれるので、順序は保たれる。
        rhi::TransitionIfNeeded(list, thumbnail, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        PIXEndEvent(list);
    };

    // フレームの中ならそのまま積む。**GPU 待機を挟まない**のはこちらの経路。
    if (commandList != nullptr) {
        record(commandList);
        return true;
    }
    return device.ExecuteImmediate(record);
}

}  // namespace tg::compositor
