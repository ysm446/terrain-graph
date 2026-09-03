#include "io/MaterialExport.h"

#include "compositor/MaterialEvaluator.h"
#include "core/ImageIo.h"
#include "core/Log.h"
#include "core/PathUtf8.h"
#include "rhi/GpuResource.h"

#include <DirectXPackedVector.h>
#include <pix3.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace tg::io {
namespace {

using rhi::DispatchCount;
using rhi::TransitionIfNeeded;

// 書き出しのタイルの一辺。プレビューと同じ大きさにして、同じ経路を通す。
constexpr uint32_t kExportTileSize = 512;

// GPU 側の TG_EXPORT_* と並びを合わせること。
enum class ExportMap : uint32_t {
    BaseColor = 0,
    Normal = 1,
    Roughness = 2,
    Metallic = 3,
    AmbientOcclusion = 4,
    Height = 5,
    Ord = 6,
    Orm = 7,
};

// GPU 側の ExportConstants と一致させること。
struct ExportConstants {
    uint32_t outputIndex;
    uint32_t baseColorIndex;
    uint32_t normalIndex;
    uint32_t surfaceIndex;

    uint32_t heightIndex;
    uint32_t resolution;
    uint32_t map;
    uint32_t pad0;
};

// 書き出す 1 枚ぶんの指定。
struct MapRequest {
    ExportMap map = ExportMap::BaseColor;
    const char* suffix = "";
    int channels = 3;  // PNG に書くチャンネル数
};

// テクスチャ 1 枚を読み戻し、行ごとに fn(row, rowBytes) を呼ぶ。
// 読み戻しは GPU 待機を伴うので、フレームの外から呼ぶこと。
template <typename Fn>
bool ReadbackTexture(rhi::Device& device, rhi::GpuTexture& texture, const Fn& fn) {
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT rowCount = 0;
    UINT64 rowSizeInBytes = 0;
    UINT64 totalBytes = 0;
    const D3D12_RESOURCE_DESC resourceDesc = texture.resource->GetDesc();
    device.GetDevice()->GetCopyableFootprints(&resourceDesc, 0, 1, 0, &footprint, &rowCount,
                                              &rowSizeInBytes, &totalBytes);

    rhi::GpuBuffer readback;
    if (!device.Allocator().CreateReadbackBuffer(totalBytes, L"ExportReadback", readback)) {
        TG_LOG_ERROR("書き出し用の読み戻しバッファを作れません (%llu バイト)", totalBytes);
        return false;
    }

    const bool executed = device.ExecuteImmediate([&](ID3D12GraphicsCommandList* commandList) {
        PIXBeginEvent(commandList, PIX_COLOR(200, 160, 80), "ExportReadback");
        TransitionIfNeeded(commandList, texture, D3D12_RESOURCE_STATE_COPY_SOURCE);
        const CD3DX12_TEXTURE_COPY_LOCATION destination(readback.resource.Get(), footprint);
        const CD3DX12_TEXTURE_COPY_LOCATION source(texture.resource.Get(), 0);
        commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        PIXEndEvent(commandList);
    });
    if (!executed) {
        device.DeferRelease(readback);
        return false;
    }

    void* mapped = nullptr;
    const D3D12_RANGE readRange = {0, static_cast<SIZE_T>(totalBytes)};
    if (!TG_CHECK_HR(readback.resource->Map(0, &readRange, &mapped))) {
        device.DeferRelease(readback);
        return false;
    }
    const auto* base = static_cast<const uint8_t*>(mapped) + footprint.Offset;
    fn(base, static_cast<size_t>(footprint.Footprint.RowPitch), rowCount,
       static_cast<size_t>(rowSizeInBytes));

    const D3D12_RANGE writtenRange = {0, 0};
    readback.resource->Unmap(0, &writtenRange);
    device.DeferRelease(readback);
    return true;
}

// 読み戻した RGBA8 から、先頭 channels 本だけを詰め直す。
// 行の余白（RowPitch と実データの差）を落とし、PNG へそのまま渡せる形にする。
std::vector<uint8_t> CompactRgba8(const uint8_t* base, size_t rowPitch, uint32_t resolution,
                                  int channels) {
    std::vector<uint8_t> pixels(static_cast<size_t>(resolution) * resolution *
                                static_cast<size_t>(channels));
    for (uint32_t y = 0; y < resolution; ++y) {
        const uint8_t* source = base + static_cast<size_t>(y) * rowPitch;
        uint8_t* destination =
            pixels.data() + static_cast<size_t>(y) * resolution * static_cast<size_t>(channels);
        for (uint32_t x = 0; x < resolution; ++x) {
            for (int c = 0; c < channels; ++c) {
                destination[static_cast<size_t>(x) * static_cast<size_t>(channels) +
                            static_cast<size_t>(c)] =
                    source[static_cast<size_t>(x) * 4 + static_cast<size_t>(c)];
            }
        }
    }
    return pixels;
}

std::filesystem::path MapPath(const ExportSettings& settings, const char* suffix,
                              const char* extension) {
    return settings.directory /
           FromUtf8(settings.baseName + "_" + suffix + "." + extension);
}

// 書き出すマップの一覧を、設定から組み立てる。
std::vector<MapRequest> BuildRequests(const ExportSettings& settings) {
    std::vector<MapRequest> requests;
    if (settings.baseColor) {
        requests.push_back({ExportMap::BaseColor, "BaseColor", 3});
    }
    if (settings.normal) {
        requests.push_back({ExportMap::Normal, "Normal", 3});
    }
    if (settings.surface) {
        switch (settings.packing) {
            case ExportPacking::Ord:
                requests.push_back({ExportMap::Ord, "ORD", 3});
                break;
            case ExportPacking::Orm:
                requests.push_back({ExportMap::Orm, "ORM", 3});
                break;
            default:
                requests.push_back({ExportMap::Roughness, "Roughness", 1});
                requests.push_back({ExportMap::Metallic, "Metallic", 1});
                requests.push_back({ExportMap::AmbientOcclusion, "AO", 1});
                break;
        }
    }
    // ORD にはハイトが入っているが、8bit なので段差が出る。
    // 「ハイト」を選んでいれば、それとは別に精度のある 1 枚を書く。
    if (settings.height && !settings.heightAsExr) {
        requests.push_back({ExportMap::Height, "Height", 1});
    }
    return requests;
}

// ハイトを EXR で書く。R32_FLOAT をそのまま写す。
bool WriteHeightExr(rhi::Device& device, compositor::MaterialEvaluator& evaluator,
                    const ExportSettings& settings) {
    const uint32_t resolution = evaluator.Resolution();
    std::vector<float> pixels(static_cast<size_t>(resolution) * resolution, 0.0f);

    rhi::GpuTexture& height = evaluator.TexturesMutable().height;
    const bool read = ReadbackTexture(
        device, height,
        [&](const uint8_t* base, size_t rowPitch, uint32_t rowCount, size_t /*rowBytes*/) {
            const uint32_t rows = std::min(rowCount, resolution);
            for (uint32_t y = 0; y < rows; ++y) {
                const auto* source = reinterpret_cast<const float*>(base + y * rowPitch);
                float* destination = pixels.data() + static_cast<size_t>(y) * resolution;
                std::memcpy(destination, source, sizeof(float) * resolution);
            }
        });
    if (!read) {
        return false;
    }
    // 1 チャンネルで書く。ハイトは値そのものが要るので **fp16 には落とさない。**
    return SaveExr(MapPath(settings, "Height", "exr"), resolution, resolution, 1, pixels.data(),
                   false);
}

}  // namespace

uint32_t ExportMaterialTextures(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                                const ExportRefs& refs, const ExportSettings& settings) {
    if (settings.directory.empty() || settings.baseName.empty() || settings.resolution == 0) {
        TG_LOG_ERROR("書き出し先か名前が決まっていません");
        return 0;
    }

    std::error_code error;
    std::filesystem::create_directories(settings.directory, error);

    ID3D12PipelineState* pipeline = pipelineCache.GetCompute(L"ExportPack.hlsl", L"CsMain");
    if (pipeline == nullptr) {
        return 0;
    }

    // **書き出し専用の評価器。** プレビュー側の解像度には触らない。
    compositor::MaterialEvaluator evaluator;
    if (!evaluator.Create(device, settings.resolution)) {
        TG_LOG_ERROR("%u^2 の書き出し用バッファを確保できません（VRAM 不足）",
                     settings.resolution);
        return 0;
    }
    evaluator.SetTileSize(kExportTileSize);

    // 詰め直した結果を受ける 1 枚。すべてのマップで使い回す。
    rhi::GpuTexture packed;
    {
        rhi::TextureDesc desc;
        desc.width = settings.resolution;
        desc.height = settings.resolution;
        desc.format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.allowUnorderedAccess = true;
        desc.createSrv = true;
        desc.initialState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        desc.debugName = L"ExportPacked";
        if (!device.Allocator().CreateTexture2D(desc, packed)) {
            evaluator.Destroy(device);
            return 0;
        }
    }

    // 全体をタイルに分けて評価する。プレビューと同じ経路（レイヤー優先）を通る。
    std::vector<compositor::TileRect> tiles;
    for (uint32_t y = 0; y < settings.resolution; y += kExportTileSize) {
        for (uint32_t x = 0; x < settings.resolution; x += kExportTileSize) {
            compositor::TileRect tile;
            tile.x = x;
            tile.y = y;
            tile.width = std::min(kExportTileSize, settings.resolution - x);
            tile.height = std::min(kExportTileSize, settings.resolution - y);
            tiles.push_back(tile);
        }
    }

    bool evaluated = false;
    const bool submitted = device.ExecuteImmediate([&](ID3D12GraphicsCommandList* commandList) {
        PIXBeginEvent(commandList, PIX_COLOR(200, 160, 80), "ExportEvaluate");
        evaluated = evaluator.Evaluate(device, pipelineCache, commandList, refs.stack,
                                       refs.textures, refs.materials, refs.paintMasks, tiles);
        PIXEndEvent(commandList);
    });
    if (!submitted || !evaluated) {
        TG_LOG_ERROR("書き出し用の合成に失敗しました");
        device.DeferRelease(packed);
        evaluator.Destroy(device);
        return 0;
    }

    const compositor::MaterialTextureSet& source = evaluator.Textures();
    uint32_t written = 0;

    for (const MapRequest& request : BuildRequests(settings)) {
        ExportConstants constants = {};
        constants.outputIndex = packed.UavIndex();
        constants.baseColorIndex = source.baseColor.SrvIndex();
        constants.normalIndex = source.normal.SrvIndex();
        constants.surfaceIndex = source.surface.SrvIndex();
        constants.heightIndex = source.height.SrvIndex();
        constants.resolution = settings.resolution;
        constants.map = static_cast<uint32_t>(request.map);

        const bool packedOk =
            device.ExecuteImmediate([&](ID3D12GraphicsCommandList* commandList) {
                PIXBeginEvent(commandList, PIX_COLOR(200, 160, 80), "ExportPack");
                TransitionIfNeeded(commandList, packed, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                commandList->SetComputeRootSignature(pipelineCache.GlobalRootSignature());
                commandList->SetPipelineState(pipeline);
                commandList->SetComputeRoot32BitConstants(
                    0, sizeof(constants) / sizeof(uint32_t), &constants, 0);
                commandList->Dispatch(DispatchCount(settings.resolution),
                                      DispatchCount(settings.resolution), 1);
                PIXEndEvent(commandList);
            });
        if (!packedOk) {
            continue;
        }

        const std::filesystem::path path = MapPath(settings, request.suffix, "png");
        bool saved = false;
        const bool read = ReadbackTexture(
            device, packed,
            [&](const uint8_t* base, size_t rowPitch, uint32_t /*rowCount*/, size_t /*rowBytes*/) {
                const std::vector<uint8_t> pixels =
                    CompactRgba8(base, rowPitch, settings.resolution, request.channels);
                const auto pitch =
                    static_cast<uint32_t>(settings.resolution * static_cast<uint32_t>(request.channels));
                saved = (request.channels == 1)
                            ? SaveGray8Png(path, settings.resolution, settings.resolution, pitch,
                                           pixels.data())
                            : SaveRgb8Png(path, settings.resolution, settings.resolution, pitch,
                                          pixels.data());
            });
        if (read && saved) {
            ++written;
        }
    }

    if (settings.height && settings.heightAsExr && WriteHeightExr(device, evaluator, settings)) {
        ++written;
    }

    device.DeferRelease(packed);
    evaluator.Destroy(device);

    if (written == 0) {
        TG_LOG_ERROR("書き出せた画像がありません");
    } else {
        TG_LOG_INFO("%u 枚を書き出しました: %s (%u^2)", written,
                    ToUtf8Display(settings.directory).c_str(), settings.resolution);
    }
    return written;
}

}  // namespace tg::io
