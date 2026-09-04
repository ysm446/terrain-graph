#include "renderer/SkyLibrary.h"

#include "core/ImageIo.h"
#include "core/Log.h"
#include "core/PathUtf8.h"

#include <pix3.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace tg::renderer {
namespace {

using rhi::DispatchCount;
using rhi::TransitionIfNeeded;

// サムネイルの一辺。マテリアルと揃える（一覧で並ぶ大きさが同じになる）。
constexpr uint32_t kThumbnailSize = 128;

// サムネイル用に落とす equirect の大きさ。**元の解像度は要らない。**
// 128 px の円へ 180 度を写すだけなので、これ以上あっても縁で潰れる。
// 小さくしておくと、8K の HDRI でも一時テクスチャが 1 MB 未満で済む。
constexpr uint32_t kThumbnailEquirectWidth = 256;
constexpr uint32_t kThumbnailEquirectHeight = 128;

constexpr uint32_t kInvalidTextureIndex = 0xFFFFFFFFu;

// GPU 側の SkyThumbnailConstants と一致させること。
struct SkyThumbnailConstants {
    uint32_t outputIndex;
    uint32_t size;
    uint32_t equirectIndex;
    float luminanceScale;

    float zenithColor[3];
    float intensity;

    float horizonColor[3];
    float pad0;

    float groundColor[3];
    float pad1;
};

// HDR 画像をサムネイル用の小さな equirect へ落とす。
// 単純な面積平均。太陽のような 1 画素の輝点も、周りへ均されて残る。
HdrImage DownsampleEquirect(const HdrImage& source, uint32_t width, uint32_t height) {
    HdrImage result;
    result.width = width;
    result.height = height;
    result.pixels.assign(static_cast<size_t>(width) * height * 4, 0.0f);

    for (uint32_t y = 0; y < height; ++y) {
        const uint32_t y0 = static_cast<uint32_t>(static_cast<uint64_t>(y) * source.height / height);
        const uint32_t y1 = std::max(
            y0 + 1, static_cast<uint32_t>(static_cast<uint64_t>(y + 1) * source.height / height));
        for (uint32_t x = 0; x < width; ++x) {
            const uint32_t x0 =
                static_cast<uint32_t>(static_cast<uint64_t>(x) * source.width / width);
            const uint32_t x1 = std::max(
                x0 + 1, static_cast<uint32_t>(static_cast<uint64_t>(x + 1) * source.width / width));

            float sum[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            uint32_t count = 0;
            for (uint32_t sy = y0; sy < y1 && sy < source.height; ++sy) {
                for (uint32_t sx = x0; sx < x1 && sx < source.width; ++sx) {
                    const size_t index = (static_cast<size_t>(sy) * source.width + sx) * 4;
                    for (int c = 0; c < 4; ++c) {
                        sum[c] += source.pixels[index + static_cast<size_t>(c)];
                    }
                    ++count;
                }
            }
            const float inverse = (count > 0) ? (1.0f / static_cast<float>(count)) : 0.0f;
            const size_t destination = (static_cast<size_t>(y) * width + x) * 4;
            for (int c = 0; c < 4; ++c) {
                result.pixels[destination + static_cast<size_t>(c)] =
                    sum[static_cast<size_t>(c)] * inverse;
            }
        }
    }
    return result;
}

// HdrImage を 1 枚のテクスチャへ上げる。上げ終わるまで待つ（フレームの外で呼ぶこと）。
bool UploadEquirect(rhi::Device& device, const HdrImage& image, rhi::GpuTexture& outTexture) {
    rhi::TextureDesc desc;
    desc.width = image.width;
    desc.height = image.height;
    desc.format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    desc.createSrv = true;
    desc.initialState = D3D12_RESOURCE_STATE_COPY_DEST;
    desc.debugName = L"SkyThumbnailEquirect";
    if (!device.Allocator().CreateTexture2D(desc, outTexture)) {
        return false;
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT rowCount = 0;
    UINT64 rowSizeInBytes = 0;
    UINT64 totalBytes = 0;
    const D3D12_RESOURCE_DESC resourceDesc = outTexture.resource->GetDesc();
    device.GetDevice()->GetCopyableFootprints(&resourceDesc, 0, 1, 0, &footprint, &rowCount,
                                              &rowSizeInBytes, &totalBytes);

    rhi::GpuBuffer staging;
    if (!device.Allocator().CreateUploadBuffer(totalBytes, L"SkyThumbnailStaging", staging)) {
        return false;
    }

    void* mapped = nullptr;
    const D3D12_RANGE readRange = {0, 0};
    if (!TG_CHECK_HR(staging.resource->Map(0, &readRange, &mapped))) {
        device.DeferRelease(staging);
        return false;
    }
    auto* destination = static_cast<uint8_t*>(mapped) + footprint.Offset;
    const auto* source = reinterpret_cast<const uint8_t*>(image.pixels.data());
    for (uint32_t row = 0; row < rowCount; ++row) {
        std::memcpy(destination + static_cast<size_t>(row) * footprint.Footprint.RowPitch,
                    source + static_cast<size_t>(row) * image.RowPitchInBytes(),
                    static_cast<size_t>(rowSizeInBytes));
    }
    staging.resource->Unmap(0, nullptr);

    const bool executed = device.ExecuteImmediate([&](ID3D12GraphicsCommandList* commandList) {
        PIXBeginEvent(commandList, PIX_COLOR(120, 180, 255), "SkyThumbnailUpload");
        const CD3DX12_TEXTURE_COPY_LOCATION destinationLocation(outTexture.resource.Get(), 0);
        const CD3DX12_TEXTURE_COPY_LOCATION sourceLocation(staging.resource.Get(), footprint);
        commandList->CopyTextureRegion(&destinationLocation, 0, 0, 0, &sourceLocation, nullptr);
        TransitionIfNeeded(commandList, outTexture, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        PIXEndEvent(commandList);
    });
    // 実行に失敗しても、ステージングは必ず GPU の完了後に返す。
    device.DeferRelease(staging);
    return executed;
}

}  // namespace

bool NeedsEnvironmentRebuild(const SkyDefinition& before, const SkyDefinition& after) {
    if (before.source != after.source) {
        return true;
    }
    if (after.source == SkySource::Hdri) {
        return before.hdriPath != after.hdriPath;
    }
    const SkySettings& a = before.procedural;
    const SkySettings& b = after.procedural;
    return std::memcmp(&a, &b, sizeof(SkySettings)) != 0;
}

bool NeedsLuminanceRebuild(const SkyDefinition& before, const SkyDefinition& after) {
    return after.source == SkySource::Hdri && before.skyLuminance != after.skyLuminance;
}

void SkyLibrary::Destroy(rhi::Device& device) {
    for (SkyAsset& asset : m_entries) {
        device.DeferRelease(asset.thumbnail);
    }
    m_entries.clear();
    m_activeId = kNoSkyAsset;
}

SkyAssetId SkyLibrary::Add(const std::string& name) {
    SkyAsset asset;
    asset.id = m_nextId++;
    asset.name = name;
    m_entries.push_back(std::move(asset));
    if (m_activeId == kNoSkyAsset) {
        m_activeId = m_entries.back().id;
    }
    return m_entries.back().id;
}

SkyAssetId SkyLibrary::Duplicate(const SkyAsset& source) {
    // サムネイルは複製せず、作り直しに任せる（GPU リソースは持ち回らない）。
    SkyAsset asset;
    asset.id = m_nextId++;
    asset.name = source.name + " のコピー";
    asset.sky = source.sky;
    m_entries.push_back(std::move(asset));
    return m_entries.back().id;
}

void SkyLibrary::Remove(rhi::Device& device, SkyAssetId id) {
    const auto it = std::find_if(m_entries.begin(), m_entries.end(),
                                 [id](const SkyAsset& asset) { return asset.id == id; });
    if (it == m_entries.end()) {
        return;
    }
    device.DeferRelease(it->thumbnail);
    const auto index = static_cast<size_t>(std::distance(m_entries.begin(), it));
    m_entries.erase(it);

    if (m_activeId == id) {
        // 消したものの隣へ移す。一覧が空になったら既定を作り直す。
        m_activeId = kNoSkyAsset;
        if (!m_entries.empty()) {
            m_activeId = m_entries[std::min(index, m_entries.size() - 1)].id;
        }
    }
    EnsureDefault();
}

void SkyLibrary::Clear(rhi::Device& device) {
    Destroy(device);
}

SkyAssetId SkyLibrary::EnsureDefault() {
    if (m_entries.empty()) {
        m_activeId = Add("既定の空");
        return m_activeId;
    }
    if (Find(m_activeId) == nullptr) {
        m_activeId = m_entries.front().id;
    }
    return m_activeId;
}

const SkyAsset* SkyLibrary::Find(SkyAssetId id) const {
    const auto it = std::find_if(m_entries.begin(), m_entries.end(),
                                 [id](const SkyAsset& asset) { return asset.id == id; });
    return (it != m_entries.end()) ? &(*it) : nullptr;
}

SkyAsset* SkyLibrary::FindMutable(SkyAssetId id) {
    const auto it = std::find_if(m_entries.begin(), m_entries.end(),
                                 [id](const SkyAsset& asset) { return asset.id == id; });
    return (it != m_entries.end()) ? &(*it) : nullptr;
}

void SkyLibrary::SetActive(SkyAssetId id) {
    if (Find(id) != nullptr) {
        m_activeId = id;
    }
}

void SkyLibrary::MarkThumbnailDirty(SkyAssetId id) {
    if (SkyAsset* asset = FindMutable(id); asset != nullptr) {
        asset->thumbnailDirty = true;
    }
}

D3D12_GPU_DESCRIPTOR_HANDLE SkyLibrary::ThumbnailHandle(SkyAssetId id) const {
    const SkyAsset* asset = Find(id);
    if (asset == nullptr || !asset->thumbnail.IsValid()) {
        return D3D12_GPU_DESCRIPTOR_HANDLE{0};
    }
    return asset->thumbnail.srv.gpu;
}

void SkyLibrary::ProcessPendingWork(rhi::Device& device, rhi::PipelineCache& pipelineCache) {
    for (SkyAsset& asset : m_entries) {
        if (!asset.thumbnailDirty) {
            continue;
        }
        if (!BuildThumbnail(device, pipelineCache, asset)) {
            TG_LOG_WARN("天球「%s」のサムネイルを作れませんでした", asset.name.c_str());
        }
        // 失敗しても要求は落とす（同じ失敗を毎フレーム繰り返さない）。
        asset.thumbnailDirty = false;
        return;
    }
}

bool SkyLibrary::BuildThumbnail(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                                SkyAsset& asset) {
    ID3D12PipelineState* pipeline = pipelineCache.GetCompute(L"SkyThumbnail.hlsl", L"CsMain");
    if (pipeline == nullptr) {
        return false;
    }

    rhi::GpuTexture& target = asset.thumbnail;
    if (!target.IsValid()) {
        rhi::TextureDesc desc;
        desc.width = kThumbnailSize;
        desc.height = kThumbnailSize;
        desc.format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.allowUnorderedAccess = true;
        desc.createSrv = true;
        desc.initialState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        desc.debugName = L"SkyThumbnail";
        if (!device.Allocator().CreateTexture2D(desc, target)) {
            return false;
        }
    }

    SkyThumbnailConstants constants = {};
    constants.outputIndex = target.UavIndex();
    constants.size = kThumbnailSize;
    constants.equirectIndex = kInvalidTextureIndex;
    constants.luminanceScale = 1.0f;
    const SkySettings& procedural = asset.sky.procedural;
    constants.zenithColor[0] = procedural.zenithColor.x;
    constants.zenithColor[1] = procedural.zenithColor.y;
    constants.zenithColor[2] = procedural.zenithColor.z;
    constants.horizonColor[0] = procedural.horizonColor.x;
    constants.horizonColor[1] = procedural.horizonColor.y;
    constants.horizonColor[2] = procedural.horizonColor.z;
    constants.groundColor[0] = procedural.groundColor.x;
    constants.groundColor[1] = procedural.groundColor.y;
    constants.groundColor[2] = procedural.groundColor.z;
    constants.intensity = procedural.intensity;

    // HDRI のときは、小さく落とした equirect を一時テクスチャへ上げて読む。
    // **環境本体（Environment）の equirect は使わない。**
    // あちらはビューポートに適用中の 1 枚だけで、一覧の他の天球には無い。
    rhi::GpuTexture equirect;
    if (asset.sky.source == SkySource::Hdri && !asset.sky.hdriPath.empty()) {
        HdrImage image;
        if (!LoadHdrImage(asset.sky.hdriPath, image)) {
            return false;
        }
        // `small` は Windows のヘッダがマクロにしているので、名前に使わない。
        const HdrImage reduced =
            DownsampleEquirect(image, kThumbnailEquirectWidth, kThumbnailEquirectHeight);
        if (!UploadEquirect(device, reduced, equirect)) {
            device.DeferRelease(equirect);
            return false;
        }
        constants.equirectIndex = equirect.SrvIndex();
        constants.luminanceScale =
            SkyLuminanceScale(asset.sky.skyLuminance, MedianSkyLuminance(image));
    }

    const bool executed = device.ExecuteImmediate([&](ID3D12GraphicsCommandList* commandList) {
        PIXBeginEvent(commandList, PIX_COLOR(120, 200, 200), "SkyThumbnail");
        TransitionIfNeeded(commandList, target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        commandList->SetComputeRootSignature(pipelineCache.GlobalRootSignature());
        commandList->SetPipelineState(pipeline);
        commandList->SetComputeRoot32BitConstants(0, sizeof(constants) / sizeof(uint32_t),
                                                  &constants, 0);
        commandList->Dispatch(DispatchCount(kThumbnailSize), DispatchCount(kThumbnailSize),
                              1);

        // ImGui から SRV として読むので、ピクセルシェーダ可視の状態へ移す。
        TransitionIfNeeded(commandList, target, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        PIXEndEvent(commandList);
    });

    // 一時テクスチャは GPU の完了後に返す。
    device.DeferRelease(equirect);
    return executed;
}

}  // namespace tg::renderer
