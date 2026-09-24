#include "renderer/Impostor.h"

#include <directx/d3dx12.h>
#include <pix3.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "core/ImageIo.h"
#include "core/Log.h"
#include "core/PathUtf8.h"
#include "renderer/Mesh.h"

namespace tg::renderer {
namespace {
constexpr float kPi = 3.14159265358979f;
constexpr uint32_t kMaxAtlasSize = 8192;

uint32_t MipCount(uint32_t size) {
    uint32_t count = 1;
    while (size > 1) { size >>= 1; ++count; }
    return count;
}

// ImpostorBake.hlsl と一致させる。
struct BakeConstants {
    uint32_t baseColorIndex, normalIndex, roughnessIndex, mapChannels;
    float baseColorTint[3]; float roughnessValue;
    float colorAdjust[2]; float brightness; float alphaCutoff;
    float center[3]; float radius;
    uint32_t frame[2]; uint32_t frames; uint32_t fullSphere;
    uint32_t flipNormalGreen; uint32_t padding[3];
};
static_assert(sizeof(BakeConstants) == 96);
struct DilateConstants {
    uint32_t colorIn, normalIn, colorOut, normalOut;
    uint32_t width, height, tileSize, padding;
};

// 最終のアトラス。ミップ連鎖を作るため、全ミップ COPY_DEST で作りミップごとのビューを張る。
bool CreateAtlas(rhi::Device& device, uint32_t size, const wchar_t* name, rhi::GpuTexture& texture) {
    rhi::TextureDesc desc;
    desc.width = desc.height = size;
    desc.mipLevels = MipCount(size);
    desc.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.allowUnorderedAccess = true;
    desc.createMipUavs = true;
    desc.createMipSrvs = true;
    desc.initialState = D3D12_RESOURCE_STATE_COPY_DEST;
    desc.debugName = name;
    return device.Allocator().CreateTexture2D(desc, texture);
}

// ミップを作り、作り終えたミップごとのビューを返す。
bool FinishAtlas(rhi::Device& device, rhi::PipelineCache& cache, rhi::GpuTexture& texture) {
    if (!compositor::TextureLibrary::GenerateMips(device, cache, texture)) return false;
    device.DeferFreeMipViews(texture);
    return true;
}

// ミップ 0 へ RGBA8 を書き込む（全ミップ COPY_DEST のまま）。
bool UploadAtlas(rhi::Device& device, rhi::GpuTexture& texture, const LdrImage& image) {
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows = 0;
    UINT64 rowBytes = 0, totalBytes = 0;
    const auto desc = texture.resource->GetDesc();
    device.GetDevice()->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows, &rowBytes, &totalBytes);
    rhi::GpuBuffer staging;
    if (!device.Allocator().CreateUploadBuffer(totalBytes, L"ImpostorStaging", staging)) return false;
    void* mapped = nullptr;
    const D3D12_RANGE none{0, 0};
    if (!TG_CHECK_HR(staging.resource->Map(0, &none, &mapped))) {
        device.DeferRelease(staging);
        return false;
    }
    for (UINT row = 0; row < rows; ++row)
        std::memcpy(static_cast<uint8_t*>(mapped) + footprint.Offset + size_t(row) * footprint.Footprint.RowPitch,
                    image.pixels.data() + size_t(row) * image.RowPitchInBytes(), size_t(rowBytes));
    staging.resource->Unmap(0, nullptr);
    const bool uploaded = device.ExecuteImmediate([&](ID3D12GraphicsCommandList* list) {
        const CD3DX12_TEXTURE_COPY_LOCATION destination(texture.resource.Get(), 0);
        const CD3DX12_TEXTURE_COPY_LOCATION source(staging.resource.Get(), footprint);
        list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    });
    device.DeferRelease(staging);
    return uploaded;
}

// ミップ 0 を PNG へ書き出す。テクスチャは読み取り状態で受け取り、読み取り状態へ戻す。
bool SaveAtlas(rhi::Device& device, rhi::GpuTexture& texture, const std::filesystem::path& path) {
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows = 0;
    UINT64 rowBytes = 0, totalBytes = 0;
    const auto desc = texture.resource->GetDesc();
    device.GetDevice()->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows, &rowBytes, &totalBytes);
    rhi::GpuBuffer readback;
    if (!device.Allocator().CreateReadbackBuffer(totalBytes, L"ImpostorReadback", readback)) return false;
    const auto readState = texture.state;
    const bool copied = device.ExecuteImmediate([&](ID3D12GraphicsCommandList* list) {
        rhi::TransitionIfNeeded(list, texture, D3D12_RESOURCE_STATE_COPY_SOURCE);
        const CD3DX12_TEXTURE_COPY_LOCATION destination(readback.resource.Get(), footprint);
        const CD3DX12_TEXTURE_COPY_LOCATION source(texture.resource.Get(), 0);
        list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        rhi::TransitionIfNeeded(list, texture, readState);
    });
    bool saved = false;
    void* mapped = nullptr;
    const D3D12_RANGE range{0, static_cast<SIZE_T>(totalBytes)};
    if (copied && TG_CHECK_HR(readback.resource->Map(0, &range, &mapped))) {
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        saved = SaveRgba8Png(path, texture.width, texture.height, footprint.Footprint.RowPitch,
                             static_cast<const uint8_t*>(mapped) + footprint.Offset);
        const D3D12_RANGE written{0, 0};
        readback.resource->Unmap(0, &written);
    }
    device.DeferRelease(readback);
    return saved;
}
}  // namespace

void ImpostorPaths(const ModelAsset& model, std::filesystem::path& colorPath, std::filesystem::path& normalPath) {
    const auto directory = (model.assetPath.empty() ? model.path : model.assetPath).parent_path();
    colorPath = directory / FromUtf8(model.name + "_Impostor_C.png");
    normalPath = directory / FromUtf8(model.name + "_Impostor_N.png");
}

void ImpostorLibrary::Destroy(rhi::Device& device) {
    for (auto& [id, entry] : m_entries) {
        device.DeferRelease(entry.color);
        device.DeferRelease(entry.normal);
    }
    m_entries.clear();
    m_failed.clear();
}

const ImpostorTextures* ImpostorLibrary::Find(uint64_t model) const {
    const auto found = m_entries.find(model);
    return found == m_entries.end() ? nullptr : &found->second;
}

void ImpostorLibrary::Remove(rhi::Device& device, uint64_t model) {
    const auto found = m_entries.find(model);
    if (found == m_entries.end()) return;
    device.DeferRelease(found->second.color);
    device.DeferRelease(found->second.normal);
    m_entries.erase(found);
}

bool ImpostorLibrary::Sync(rhi::Device& device, rhi::PipelineCache& cache, const ModelAsset& model) {
    const auto& impostor = model.impostor;
    if (!impostor.baked) {
        Remove(device, model.id);
        m_failed.erase(model.id);
        return true;
    }
    if (const auto* entry = Find(model.id); entry && entry->colorPath == impostor.colorPath &&
        entry->normalPath == impostor.normalPath && entry->settings == impostor.bakedSettings)
        return true;
    // 読めなかった画像を毎フレーム読み直さない。パスが変われば試し直す。
    if (const auto failed = m_failed.find(model.id); failed != m_failed.end() && failed->second == impostor.colorPath)
        return false;
    const uint32_t size = impostor.bakedSettings.frames * impostor.bakedSettings.frameSize;
    LdrImage color, normal;
    if (!LoadLdrImage(impostor.colorPath, color) || !LoadLdrImage(impostor.normalPath, normal) ||
        color.width != size || color.height != size || normal.width != size || normal.height != size) {
        TG_LOG_WARN("インポスターの画像を読み込めません（無いか、大きさが設定と違います）: %s",
                    ToUtf8Display(impostor.colorPath).c_str());
        m_failed[model.id] = impostor.colorPath;
        Remove(device, model.id);
        return false;
    }
    ImpostorTextures entry;
    entry.settings = impostor.bakedSettings;
    entry.center = impostor.center;
    entry.radius = impostor.radius;
    entry.colorPath = impostor.colorPath;
    entry.normalPath = impostor.normalPath;
    const bool loaded = CreateAtlas(device, size, L"ImpostorColor", entry.color) &&
                        CreateAtlas(device, size, L"ImpostorNormal", entry.normal) &&
                        UploadAtlas(device, entry.color, color) && UploadAtlas(device, entry.normal, normal) &&
                        FinishAtlas(device, cache, entry.color) && FinishAtlas(device, cache, entry.normal);
    if (!loaded) {
        device.DeferRelease(entry.color);
        device.DeferRelease(entry.normal);
        m_failed[model.id] = impostor.colorPath;
        return false;
    }
    Remove(device, model.id);
    m_failed.erase(model.id);
    m_entries.emplace(model.id, std::move(entry));
    return true;
}

bool ImpostorLibrary::Bake(rhi::Device& device, rhi::PipelineCache& cache, const ModelAsset& model,
                           const compositor::MaterialLibrary& materials,
                           const compositor::TextureLibrary& textures,
                           const std::filesystem::path& colorPath, const std::filesystem::path& normalPath,
                           ModelImpostor& result, std::string& error) {
    if (!model.geometry || model.geometry->lods.empty()) {
        error = "モデルの形状が読み込まれていません";
        return false;
    }
    ImpostorSettings settings = model.impostor.settings;
    settings.frames = std::clamp<uint32_t>(settings.frames, 2, 32);
    settings.frameSize = std::clamp<uint32_t>(settings.frameSize, 32, 2048);
    const uint32_t frames = settings.frames, frameSize = settings.frameSize, size = frames * frameSize;
    if (size > kMaxAtlasSize) {
        error = "画像が大きすぎます（方向数 × 解像度は 8192 まで）";
        return false;
    }
    // 形状全体を囲む球。どの方向から撮ってもマスに収まる。
    using namespace DirectX;
    const auto& geometry = *model.geometry;
    const XMVECTOR lo = XMLoadFloat3(&geometry.minimum), hi = XMLoadFloat3(&geometry.maximum);
    XMFLOAT3 center;
    XMStoreFloat3(&center, XMVectorScale(XMVectorAdd(lo, hi), 0.5f));
    const float radius = std::max(0.5f * XMVectorGetX(XMVector3Length(XMVectorSubtract(hi, lo))), 1e-3f);

    rhi::GraphicsPipelineDesc desc;
    desc.shaderPath = L"ImpostorBake.hlsl";
    desc.vertexEntry = L"VsBake";
    desc.pixelEntry = L"PsBake";
    desc.layout = rhi::VertexLayout::MeshStandard;
    desc.rtvFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.rtvFormat1 = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.dsvFormat = DXGI_FORMAT_D32_FLOAT;
    desc.cullMode = D3D12_CULL_MODE_NONE;
    auto* bake = cache.GetGraphics(desc);
    auto* dilate = cache.GetCompute(L"ImpostorBake.hlsl", L"CsDilate");
    if (!bake || !dilate) {
        error = "焼き込みのシェーダを用意できません";
        return false;
    }

    // 焼き込みは LOD0。パーツごとにメッシュを作り、終わったら返す。
    const auto& parts = geometry.lods[0].parts;
    std::vector<Mesh> meshes(parts.size());
    rhi::GpuTexture rawColor, rawNormal, depth;
    rhi::GpuBuffer constantsBuffer;
    ImpostorTextures entry;
    const auto release = [&]() {
        for (auto& mesh : meshes) mesh.Release(device);
        device.DeferRelease(rawColor);
        device.DeferRelease(rawNormal);
        device.DeferRelease(depth);
        device.DeferRelease(constantsBuffer);
    };
    const auto fail = [&](const char* message) {
        release();
        device.DeferRelease(entry.color);
        device.DeferRelease(entry.normal);
        error = message;
        return false;
    };
    for (size_t i = 0; i < parts.size(); ++i)
        if (!meshes[i].Create(device, parts[i].mesh, L"ImpostorBakeMesh")) return fail("メッシュを作れません");

    rhi::TextureDesc target;
    target.width = target.height = size;
    target.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    target.allowRenderTarget = true;
    target.clearColor[3] = 0.0f;
    target.debugName = L"ImpostorBakeColor";
    if (!device.Allocator().CreateTexture2D(target, rawColor)) return fail("描画先を作れません");
    target.debugName = L"ImpostorBakeNormal";
    if (!device.Allocator().CreateTexture2D(target, rawNormal)) return fail("描画先を作れません");
    rhi::TextureDesc depthDesc;
    depthDesc.width = depthDesc.height = size;
    depthDesc.format = DXGI_FORMAT_D32_FLOAT;
    depthDesc.allowDepthStencil = true;
    depthDesc.createSrv = false;
    depthDesc.debugName = L"ImpostorBakeDepth";
    if (!device.Allocator().CreateTexture2D(depthDesc, depth)) return fail("深度を作れません");
    if (!CreateAtlas(device, size, L"ImpostorColor", entry.color) ||
        !CreateAtlas(device, size, L"ImpostorNormal", entry.normal))
        return fail("アトラスを作れません");

    // マスとパーツごとの定数。CBV は 256 バイト境界。
    constexpr uint64_t kStride = 256;
    const uint64_t drawCount = uint64_t(frames) * frames * parts.size();
    if (!device.Allocator().CreateUploadBuffer(drawCount * kStride, L"ImpostorBakeConstants", constantsBuffer))
        return fail("定数を用意できません");
    std::vector<bool> drawable(parts.size());
    {
        void* mapped = nullptr;
        const D3D12_RANGE none{0, 0};
        if (!TG_CHECK_HR(constantsBuffer.resource->Map(0, &none, &mapped))) return fail("定数を用意できません");
        auto* bytes = static_cast<uint8_t*>(mapped);
        for (size_t part = 0; part < parts.size(); ++part) {
            const auto slot = parts[part].slot;
            const auto* material = slot < model.materials.size() ? materials.Find(model.materials[slot]) : nullptr;
            const compositor::MaterialAsset fallback;
            const auto& asset = material ? *material : fallback;
            BakeConstants constants{};
            constants.baseColorIndex = textures.SrvIndex(asset.baseColor, true);
            constants.normalIndex = textures.SrvIndex(asset.normal, false);
            constants.roughnessIndex = textures.SrvIndex(asset.roughness.texture, false);
            constants.mapChannels = compositor::PackMaterialChannels(asset);
            constants.baseColorTint[0] = asset.baseColorTint.x;
            constants.baseColorTint[1] = asset.baseColorTint.y;
            constants.baseColorTint[2] = asset.baseColorTint.z;
            constants.roughnessValue = asset.roughnessValue;
            constants.colorAdjust[0] = asset.hueShiftDegrees * (kPi / 180.0f);
            constants.colorAdjust[1] = asset.saturation;
            constants.brightness = asset.brightness;
            constants.alphaCutoff = constants.baseColorIndex != compositor::kInvalidTextureIndex ? asset.alphaCutoff : 0.0f;
            constants.center[0] = center.x; constants.center[1] = center.y; constants.center[2] = center.z;
            constants.radius = radius;
            constants.frames = frames;
            constants.fullSphere = settings.fullSphere ? 1u : 0u;
            constants.flipNormalGreen = asset.flipNormalGreen ? 1u : 0u;
            drawable[part] = meshes[part].IndexCount() > 0;
            for (uint32_t j = 0; j < frames; ++j)
                for (uint32_t i = 0; i < frames; ++i) {
                    constants.frame[0] = i;
                    constants.frame[1] = j;
                    std::memcpy(bytes + ((uint64_t(j) * frames + i) * parts.size() + part) * kStride, &constants, sizeof(constants));
                }
        }
        constantsBuffer.resource->Unmap(0, nullptr);
    }

    const bool baked = device.ExecuteImmediate([&](ID3D12GraphicsCommandList* list) {
        PIXBeginEvent(list, PIX_COLOR(200, 170, 90), "ImpostorBake");
        rhi::TransitionIfNeeded(list, rawColor, D3D12_RESOURCE_STATE_RENDER_TARGET);
        rhi::TransitionIfNeeded(list, rawNormal, D3D12_RESOURCE_STATE_RENDER_TARGET);
        rhi::TransitionIfNeeded(list, depth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        const float clear[4] = {0, 0, 0, 0};
        list->ClearRenderTargetView(rawColor.rtv.cpu, clear, 0, nullptr);
        list->ClearRenderTargetView(rawNormal.rtv.cpu, clear, 0, nullptr);
        list->ClearDepthStencilView(depth.dsv.cpu, D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr);
        const D3D12_CPU_DESCRIPTOR_HANDLE targets[2] = {rawColor.rtv.cpu, rawNormal.rtv.cpu};
        list->OMSetRenderTargets(2, targets, FALSE, &depth.dsv.cpu);
        list->SetGraphicsRootSignature(cache.GlobalRootSignature());
        list->SetPipelineState(bake);
        for (uint32_t j = 0; j < frames; ++j)
            for (uint32_t i = 0; i < frames; ++i) {
                const D3D12_VIEWPORT viewport{float(i * frameSize), float(j * frameSize), float(frameSize), float(frameSize), 0, 1};
                const D3D12_RECT scissor{LONG(i * frameSize), LONG(j * frameSize), LONG((i + 1) * frameSize), LONG((j + 1) * frameSize)};
                list->RSSetViewports(1, &viewport);
                list->RSSetScissorRects(1, &scissor);
                for (size_t part = 0; part < parts.size(); ++part) {
                    if (!drawable[part]) continue;
                    list->SetGraphicsRootConstantBufferView(
                        1, constantsBuffer.GpuAddress() + ((uint64_t(j) * frames + i) * parts.size() + part) * kStride);
                    meshes[part].Draw(list);
                }
            }
        // 抜けた画素へ色を広げ、最終のアトラスのミップ 0 へ書く。
        constexpr auto kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        rhi::TransitionIfNeeded(list, rawColor, kRead);
        rhi::TransitionIfNeeded(list, rawNormal, kRead);
        rhi::TransitionMip(list, entry.color, 0, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        rhi::TransitionMip(list, entry.normal, 0, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        const DilateConstants dilateConstants{rawColor.SrvIndex(), rawNormal.SrvIndex(), entry.color.MipUavIndex(0),
                                              entry.normal.MipUavIndex(0), size, size, frameSize, 0};
        list->SetComputeRootSignature(cache.GlobalRootSignature());
        list->SetPipelineState(dilate);
        list->SetComputeRoot32BitConstants(0, sizeof(dilateConstants) / sizeof(uint32_t), &dilateConstants, 0);
        list->Dispatch(rhi::DispatchCount(size), rhi::DispatchCount(size), 1);
        const auto barrier = CD3DX12_RESOURCE_BARRIER::UAV(nullptr);
        list->ResourceBarrier(1, &barrier);
        // ミップ生成は全ミップ COPY_DEST を前提にするので戻しておく。
        rhi::TransitionMip(list, entry.color, 0, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        rhi::TransitionMip(list, entry.normal, 0, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        PIXEndEvent(list);
    });
    release();
    if (!baked) return fail("焼き込みに失敗しました");
    if (!FinishAtlas(device, cache, entry.color) || !FinishAtlas(device, cache, entry.normal))
        return fail("ミップを作れません");
    if (!SaveAtlas(device, entry.color, colorPath) || !SaveAtlas(device, entry.normal, normalPath))
        return fail("画像を保存できません");

    result = model.impostor;
    result.settings = settings;
    result.baked = true;
    result.bakedSettings = settings;
    result.center = center;
    result.radius = radius;
    result.colorPath = colorPath;
    result.normalPath = normalPath;
    entry.settings = settings;
    entry.center = center;
    entry.radius = radius;
    entry.colorPath = colorPath;
    entry.normalPath = normalPath;
    Remove(device, model.id);
    m_failed.erase(model.id);
    m_entries.emplace(model.id, std::move(entry));
    TG_LOG_INFO("インポスターを作りました: %s（%u×%u 方向、%u px）", model.name.c_str(), frames, frames, frameSize);
    return true;
}
}  // namespace tg::renderer
