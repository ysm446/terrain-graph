#include "compositor/TextureLibrary.h"

#include "core/ImageIo.h"
#include "core/PathUtf8.h"
#include "core/Log.h"

#include <pix3.h>

#include <DirectXPackedVector.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>

namespace tg::compositor {
namespace {

// 表示用テクスチャの一辺。一覧のサムネイル（72）だけでなく、
// 「選択中」の拡大プレビューにも使うので、拡大に耐える大きさにしてある。
constexpr uint32_t kPreviewSize = 512;

uint32_t MipCountFor(uint32_t width, uint32_t height) {
    uint32_t size = std::max(width, height);
    uint32_t count = 1;
    while (size > 1) {
        size >>= 1;
        ++count;
    }
    return count;
}

using rhi::DispatchCount;

bool IsExrPath(const std::filesystem::path& path) {
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension == ".exr";
}

// EXR の float RGBA を half RGBA へ詰め直す。
// 8bit だとハイトの階段が見えるので、EXR は 16bit float のまま持つ。
std::vector<uint8_t> ConvertToHalf4(const HdrImage& image) {
    const size_t texelCount = static_cast<size_t>(image.width) * image.height;
    std::vector<uint8_t> packed(texelCount * 4 * sizeof(uint16_t));
    auto* destination = reinterpret_cast<DirectX::PackedVector::HALF*>(packed.data());
    for (size_t i = 0; i < texelCount * 4; ++i) {
        destination[i] = DirectX::PackedVector::XMConvertFloatToHalf(image.pixels[i]);
    }
    return packed;
}

}  // namespace

void TextureLibrary::Destroy(rhi::Device& device) {
    for (LibraryTexture& entry : m_entries) {
        ReleaseChannelViews(device, entry);
        device.DeferRelease(entry.preview);
        // float は sRGB 用の SRV を別に張っていない（linear と同じものを指す）。
        // 二重解放しないよう、別に張ったときだけ返す。
        if (!entry.isFloat && entry.srgbSrvIndex != kInvalidTextureIndex) {
            device.DeferFree(device.SrvHeap(), device.SrvHeap().At(entry.srgbSrvIndex));
        }
        device.DeferRelease(entry.texture);
    }
    m_entries.clear();
}

const LibraryTexture* TextureLibrary::Find(TextureId id) const {
    if (id == kNoTexture) {
        return nullptr;
    }
    const auto it = std::find_if(m_entries.begin(), m_entries.end(),
                                 [id](const LibraryTexture& entry) { return entry.id == id; });
    return (it != m_entries.end()) ? &(*it) : nullptr;
}

LibraryTexture* TextureLibrary::FindMutable(TextureId id) {
    return const_cast<LibraryTexture*>(Find(id));
}

TextureId TextureLibrary::FindByPath(const std::filesystem::path& path) const {
    // 同じ画像を別の書き方（相対 / 絶対、大文字小文字）で指していても 1 枚に寄せる。
    std::error_code error;
    const std::filesystem::path normalized = std::filesystem::weakly_canonical(path, error);
    const std::filesystem::path& key = error ? path : normalized;

    for (const LibraryTexture& entry : m_entries) {
        std::error_code entryError;
        const std::filesystem::path entryNormalized =
            std::filesystem::weakly_canonical(entry.path, entryError);
        const std::filesystem::path& entryKey = entryError ? entry.path : entryNormalized;
        if (_wcsicmp(entryKey.c_str(), key.c_str()) == 0) {
            return entry.id;
        }
    }
    return kNoTexture;
}

void TextureLibrary::Clear(rhi::Device& device) {
    Destroy(device);
}

uint32_t TextureLibrary::SrvIndex(TextureId id, bool srgb) const {
    const LibraryTexture* entry = Find(id);
    if (entry == nullptr) {
        return kInvalidTextureIndex;
    }
    return srgb ? entry->srgbSrvIndex : entry->linearSrvIndex;
}

void TextureLibrary::Remove(rhi::Device& device, TextureId id) {
    const auto it = std::find_if(m_entries.begin(), m_entries.end(),
                                 [id](const LibraryTexture& entry) { return entry.id == id; });
    if (it == m_entries.end()) {
        return;
    }

    ReleaseChannelViews(device, *it);
    device.DeferRelease(it->preview);
    // Destroy と同じ理由で、別に張ったときだけ返す。
    if (!it->isFloat && it->srgbSrvIndex != kInvalidTextureIndex) {
        device.DeferFree(device.SrvHeap(), device.SrvHeap().At(it->srgbSrvIndex));
    }
    device.DeferRelease(it->texture);
    m_entries.erase(it);
}

TextureId TextureLibrary::Load(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                               const std::filesystem::path& path) {
    // 同じ画像を二重に持たない。プロジェクトやマテリアルの読み込みでは、
    // 複数のマップが同じファイル（Megascans の _ORD など）を指すのが普通。
    if (const TextureId existing = FindByPath(path); existing != kNoTexture) {
        return existing;
    }

    // EXR は 16bit float のまま持つ。8bit へ落とすとハイトに階段が出る。
    // それ以外（PNG / TGA / JPG）は 8bit で読む。
    const bool isFloat = IsExrPath(path);

    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> pixels;  // 出力フォーマットへ詰め直したもの
    size_t sourceRowPitch = 0;

    if (isFloat) {
        HdrImage image;
        if (!LoadExrImage(path, image)) {
            return kNoTexture;
        }
        width = image.width;
        height = image.height;
        pixels = ConvertToHalf4(image);
        sourceRowPitch = static_cast<size_t>(width) * 4 * sizeof(uint16_t);
    } else {
        LdrImage image;
        if (!LoadLdrImage(path, image)) {
            return kNoTexture;
        }
        width = image.width;
        height = image.height;
        pixels = std::move(image.pixels);
        sourceRowPitch = static_cast<size_t>(width) * 4;
    }

    LibraryTexture entry;
    entry.path = path;
    // 名前は UTF-8 で持つ（string() の ACP 変換は日本語名を壊す）。
    entry.name = ToUtf8Display(path.filename());
    entry.isFloat = isFloat;

    // ここから先の失敗では、確保済みのリソースとディスクリプタを必ず返す。
    // 返し漏れると、読み込み失敗を繰り返すだけで SRV ヒープが枯渇していく。
    const auto failCleanup = [&]() -> TextureId {
        if (!entry.isFloat && entry.srgbSrvIndex != kInvalidTextureIndex &&
            entry.srgbSrvIndex != entry.linearSrvIndex) {
            device.DeferFree(device.SrvHeap(), device.SrvHeap().At(entry.srgbSrvIndex));
        }
        device.DeferRelease(entry.preview);
        device.DeferRelease(entry.texture);
        return kNoTexture;
    };

    rhi::TextureDesc desc;
    desc.width = width;
    desc.height = height;
    desc.mipLevels = MipCountFor(width, height);
    if (isFloat) {
        // float は 1 つのビューで足りる。中身はすでにリニア。
        desc.format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    } else {
        // sRGB / リニアの両方の SRV を張れるよう TYPELESS で作る。
        desc.format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
        desc.srvFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.uavFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    }
    desc.allowUnorderedAccess = true;
    desc.createMipUavs = true;
    desc.createMipSrvs = true;
    desc.createSrv = true;
    desc.initialState = D3D12_RESOURCE_STATE_COPY_DEST;
    desc.debugName = L"LibraryTexture";
    if (!device.Allocator().CreateTexture2D(desc, entry.texture)) {
        return kNoTexture;
    }
    entry.linearSrvIndex = entry.texture.SrvIndex();

    if (isFloat) {
        // EXR の中身はリニアなので、sRGB として読む必要がない。同じ SRV を指す。
        entry.srgbSrvIndex = entry.linearSrvIndex;
    } else {
        // sRGB 用の SRV を追加で張る。
        const rhi::DescriptorHandle srgbHandle = device.SrvHeap().Allocate();
        if (!srgbHandle.IsValid()) {
            return failCleanup();
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC srgbSrvDesc = {};
        srgbSrvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        srgbSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srgbSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srgbSrvDesc.Texture2D.MipLevels = desc.mipLevels;
        device.GetDevice()->CreateShaderResourceView(entry.texture.resource.Get(), &srgbSrvDesc,
                                                     srgbHandle.cpu);
        entry.srgbSrvIndex = srgbHandle.index;
    }

    // ミップ 0 をアップロードする。
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT rowCount = 0;
    UINT64 rowSizeInBytes = 0;
    UINT64 totalBytes = 0;
    const D3D12_RESOURCE_DESC resourceDesc = entry.texture.resource->GetDesc();
    device.GetDevice()->GetCopyableFootprints(&resourceDesc, 0, 1, 0, &footprint, &rowCount,
                                              &rowSizeInBytes, &totalBytes);

    rhi::GpuBuffer staging;
    if (!device.Allocator().CreateUploadBuffer(totalBytes, L"TextureStaging", staging)) {
        return failCleanup();
    }

    void* mapped = nullptr;
    const D3D12_RANGE readRange = {0, 0};
    if (!TG_CHECK_HR(staging.resource->Map(0, &readRange, &mapped))) {
        device.DeferRelease(staging);
        return failCleanup();
    }
    auto* destination = static_cast<uint8_t*>(mapped) + footprint.Offset;
    for (uint32_t row = 0; row < rowCount; ++row) {
        std::memcpy(destination + static_cast<size_t>(row) * footprint.Footprint.RowPitch,
                    pixels.data() + static_cast<size_t>(row) * sourceRowPitch,
                    static_cast<size_t>(rowSizeInBytes));
    }
    staging.resource->Unmap(0, nullptr);

    const bool uploaded = device.ExecuteImmediate([&](ID3D12GraphicsCommandList* commandList) {
        PIXBeginEvent(commandList, PIX_COLOR(160, 200, 120), "UploadTexture");
        const CD3DX12_TEXTURE_COPY_LOCATION destinationLocation(entry.texture.resource.Get(), 0);
        const CD3DX12_TEXTURE_COPY_LOCATION sourceLocation(staging.resource.Get(), footprint);
        commandList->CopyTextureRegion(&destinationLocation, 0, 0, 0, &sourceLocation, nullptr);
        PIXEndEvent(commandList);
    });
    device.Defer(staging.resource);
    device.Defer(staging.allocation);
    if (!uploaded) {
        return failCleanup();
    }

    // アップロード直後はミップ 0 のみ COPY_DEST。残りは作成時の状態のまま。
    entry.texture.state = D3D12_RESOURCE_STATE_COPY_DEST;
    if (!GenerateMips(device, pipelineCache, entry.texture)) {
        return failCleanup();
    }

    // リニアなテクスチャは、そのまま一覧へ出すと極端に暗い。表示用に焼き直す。
    if (isFloat) {
        BuildPreview(device, pipelineCache, entry);
    }

    // チャンネルを分けて見る SRV は、描く相手が決まってから張る。
    CreateChannelViews(device, entry);

    entry.id = m_nextId++;
    m_entries.push_back(std::move(entry));
    return m_entries.back().id;
}

// 1 チャンネルだけを灰色で読む SRV を 4 本張る。
//
// リソースは一覧に描くのと同じもの（表示用があればそれ、無ければ元のテクスチャ）。
// どちらも RGBA8 の UNORM なので、書式は共通でよい。
void TextureLibrary::CreateChannelViews(rhi::Device& device, LibraryTexture& entry) {
    // float は表示用（RGBA8）の焼き直しが前提。それが無いのに元の float リソースへ
    // UNORM の SRV を張ると、フォーマット互換違反になる。
    if (entry.isFloat && !entry.preview.IsValid()) {
        return;
    }
    const rhi::GpuTexture& source = entry.preview.IsValid() ? entry.preview : entry.texture;
    if (!source.IsValid()) {
        return;
    }

    for (int channel = 0; channel < 4; ++channel) {
        const rhi::DescriptorHandle handle = device.SrvHeap().Allocate();
        if (!handle.IsValid()) {
            return;
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        // 選んだチャンネルを RGB の 3 つへ配り、アルファは 1 に固定する。
        // 5 は「定数 1」を意味する（4 なら定数 0）。
        srvDesc.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
            channel, channel, channel, 5);
        srvDesc.Texture2D.MipLevels = source.mipLevels;
        device.GetDevice()->CreateShaderResourceView(source.resource.Get(), &srvDesc, handle.cpu);
        entry.channelSrv[channel] = handle;
    }
}

void TextureLibrary::ReleaseChannelViews(rhi::Device& device, LibraryTexture& entry) {
    for (rhi::DescriptorHandle& handle : entry.channelSrv) {
        if (handle.IsValid()) {
            device.DeferFree(device.SrvHeap(), handle);
            handle = rhi::DescriptorHandle{};
        }
    }
}

bool TextureLibrary::BuildPreview(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                                  LibraryTexture& entry) {
    ID3D12PipelineState* pipeline = pipelineCache.GetCompute(L"TexturePreview.hlsl", L"CsMain");
    if (pipeline == nullptr) {
        return false;
    }

    rhi::TextureDesc desc;
    desc.width = kPreviewSize;
    desc.height = kPreviewSize;
    desc.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.allowUnorderedAccess = true;
    // Discard で初期化するために RTV フラグも付ける（Discard は直接キューでは
    // RENDER_TARGET 状態を要求する）。D3D12MA の配置リソースは解放跡の
    // メモリを再利用するため、Clear / Discard / Copy で初期化してから使わないと
    // GPU ベースバリデーションが「レイアウト COMMON のまま UAV で書いた」と
    // 報告する（PreviewOutput と同じ NOT_ZEROED ヒープの規則）。
    desc.allowRenderTarget = true;
    desc.createSrv = true;
    desc.initialState = D3D12_RESOURCE_STATE_COMMON;
    desc.debugName = L"TexturePreview";
    if (!device.Allocator().CreateTexture2D(desc, entry.preview)) {
        return false;
    }

    // 縮小率に見合ったミップを読む。ミップ 0 のままだとサムネイルがざらつく。
    const uint32_t longest = std::max(entry.texture.width, entry.texture.height);
    float sourceMip = 0.0f;
    for (uint32_t size = longest; size > kPreviewSize; size >>= 1) {
        sourceMip += 1.0f;
    }
    sourceMip = std::min(sourceMip, static_cast<float>(entry.texture.mipLevels - 1));

    struct PreviewConstants {
        uint32_t sourceIndex;
        uint32_t outputIndex;
        uint32_t size;
        float sourceMip;
    };
    const PreviewConstants constants{entry.linearSrvIndex, entry.preview.UavIndex(), kPreviewSize,
                                     sourceMip};

    rhi::GpuTexture& preview = entry.preview;
    const bool executed = device.ExecuteImmediate([&](ID3D12GraphicsCommandList* commandList) {
        PIXBeginEvent(commandList, PIX_COLOR(160, 200, 120), "TexturePreview");
        // 中身はディスパッチが全画素を書き潰すので、初期化は Discard で十分。
        rhi::TransitionIfNeeded(commandList, preview, D3D12_RESOURCE_STATE_RENDER_TARGET);
        commandList->DiscardResource(preview.resource.Get(), nullptr);
        rhi::TransitionIfNeeded(commandList, preview, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        commandList->SetComputeRootSignature(pipelineCache.GlobalRootSignature());
        commandList->SetPipelineState(pipeline);
        commandList->SetComputeRoot32BitConstants(0, sizeof(constants) / sizeof(uint32_t),
                                                  &constants, 0);
        commandList->Dispatch(DispatchCount(kPreviewSize), DispatchCount(kPreviewSize), 1);

        // ImGui から SRV として読むので、ピクセルシェーダ可視の状態へ移す。
        const auto toRead = CD3DX12_RESOURCE_BARRIER::Transition(
            preview.resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        commandList->ResourceBarrier(1, &toRead);
        PIXEndEvent(commandList);
    });

    if (!executed) {
        device.DeferRelease(entry.preview);
        return false;
    }
    preview.state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    return true;
}

bool TextureLibrary::GenerateMips(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                                  rhi::GpuTexture& texture) {
    ID3D12PipelineState* pipeline = pipelineCache.GetCompute(L"TextureMips.hlsl", L"CsMain");
    if (pipeline == nullptr) {
        return false;
    }

    constexpr D3D12_RESOURCE_STATES kReadState =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    struct MipConstants {
        uint32_t sourceIndex;
        uint32_t outputIndex;
        uint32_t outputWidth;
        uint32_t outputHeight;
    };

    const bool executed = device.ExecuteImmediate([&](ID3D12GraphicsCommandList* commandList) {
        PIXBeginEvent(commandList, PIX_COLOR(160, 200, 120), "TextureMips");
        commandList->SetComputeRootSignature(pipelineCache.GlobalRootSignature());
        commandList->SetPipelineState(pipeline);

        uint32_t width = texture.width;
        uint32_t height = texture.height;

        for (uint32_t mip = 1; mip < texture.mipLevels; ++mip) {
            // 参照元ミップを読み取り状態へ。ミップ 0 だけは COPY_DEST から遷移する
            // （アップロード先）。それ以降は前の反復で UAV として書いたもの。
            TransitionMip(commandList, texture, mip - 1,
                          (mip == 1) ? D3D12_RESOURCE_STATE_COPY_DEST
                                     : D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                          kReadState);

            // **書き込み先ミップも遷移させる。** テクスチャは全サブリソースが
            // COPY_DEST で作られるので、アップロードしたミップ 0 以外は
            // ここまで一度も遷移していない。これを忘れると UAV への書き込みが
            // COPY_DEST 状態のまま行われ、次の反復の「前の状態」も食い違う。
            TransitionMip(commandList, texture, mip, D3D12_RESOURCE_STATE_COPY_DEST,
                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

            width = std::max<uint32_t>(width >> 1, 1);
            height = std::max<uint32_t>(height >> 1, 1);

            const MipConstants constants{texture.MipSrvIndex(mip - 1), texture.MipUavIndex(mip),
                                         width, height};
            commandList->SetComputeRoot32BitConstants(0, sizeof(constants) / sizeof(uint32_t),
                                                      &constants, 0);
            commandList->Dispatch(DispatchCount(width), DispatchCount(height), 1);

            const auto barrier = CD3DX12_RESOURCE_BARRIER::UAV(texture.resource.Get());
            commandList->ResourceBarrier(1, &barrier);
        }

        // 最後のミップも読み取り状態へ移し、リソース全体を揃える。
        const uint32_t lastMip = texture.mipLevels - 1;
        TransitionMip(commandList, texture, lastMip,
                      (texture.mipLevels == 1) ? D3D12_RESOURCE_STATE_COPY_DEST
                                               : D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                      kReadState);
        PIXEndEvent(commandList);
    });

    if (executed) {
        texture.state = kReadState;
    }
    return executed;
}

}  // namespace tg::compositor
