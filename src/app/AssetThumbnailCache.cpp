#include "app/AssetThumbnailCache.h"
#include "core/ColorSpace.h"
#include "core/ImageIo.h"
#include "core/Log.h"
#include "core/PathUtf8.h"
#include <pix3.h>
#include "rhi/TextureReadback.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <utility>

namespace tg {
namespace fs = std::filesystem;
namespace {
constexpr uint32_t ThumbnailSize = 128;
constexpr size_t MaxEntries = 128;
std::string Extension(const fs::path& path) {
    auto extension = ToUtf8Portable(path.extension());
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return extension;
}
}
bool AssetThumbnailCache::Supports(const fs::path& path) {
    const auto ext = Extension(path);
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp" ||
           ext == ".exr" || ext == ".hdr" || ext == ".tgmat" || ext == ".tglayer" || ext == ".tgsky" ||
           ext == ".tgmodel" || ext == ".fbx" || ext == ".tgscene" || ext == ".tgboundary";
}
void AssetThumbnailCache::BeginRequests() { m_requests.clear(); ++m_frame; }
D3D12_GPU_DESCRIPTOR_HANDLE AssetThumbnailCache::Request(const fs::path& path) {
    if (!Supports(path)) return {};
    m_requests.push_back(path);
    const auto found = m_entries.find(path);
    if (found == m_entries.end()) return {};
    found->second.lastUsed = m_frame;
    return found->second.texture.srv.gpu;
}
bool AssetThumbnailCache::Failed(const fs::path& path) const {
    const auto found = m_entries.find(path);
    return found != m_entries.end() && found->second.failed;
}
bool AssetThumbnailCache::Stale(const fs::path& path) const {
    const auto found = m_entries.find(path);
    return found != m_entries.end() && found->second.stale;
}
void AssetThumbnailCache::Refresh(const fs::path& path) {
    if (Stale(path)) m_refresh.insert(path);
}
bool AssetThumbnailCache::HasPendingWork() const {
    if (!m_pendingModel.empty()) return true;
    return std::any_of(m_requests.begin(), m_requests.end(), [&](const auto& path) { return !m_entries.contains(path); });
}
void AssetThumbnailCache::ClearScratch(rhi::Device& device) {
    m_modelPreview.Destroy(device);
    m_models.clear();
    m_materials.Destroy(device);
    m_skies.Destroy(device);
    m_textures.Destroy(device);
    m_pendingModel.clear(); m_modelRendered = false;
}
void AssetThumbnailCache::Destroy(rhi::Device& device) {
    ClearScratch(device);
    for (auto& [path, entry] : m_entries) device.DeferRelease(entry.texture);
    m_entries.clear(); m_requests.clear(); m_refresh.clear(); m_root.clear(); m_directory.clear(); m_invalidate = false;
}
void AssetThumbnailCache::Store(rhi::Device& device, const fs::path& path, rhi::GpuTexture texture, bool persist) {
    if (m_entries.size() >= MaxEntries) {
        const auto oldest = std::min_element(m_entries.begin(), m_entries.end(), [](const auto& a, const auto& b) {
            return a.second.lastUsed < b.second.lastUsed;
        });
        device.DeferRelease(oldest->second.texture);
        m_entries.erase(oldest);
    }
    if (persist && texture.IsValid() && !m_diskRecord.image.empty()) {
        std::error_code error;
        fs::create_directories(m_diskRecord.image.parent_path(), error);
        if (!error && rhi::SaveTextureToPng(device, texture, m_diskRecord.image, ThumbnailSize))
            io::CommitThumbnail(m_diskRecord);
    }
    Entry entry;
    entry.failed = !texture.IsValid(); entry.lastUsed = m_frame;
    entry.texture = std::move(texture);
    if (entry.failed && Extension(path) != ".tgscene") TG_LOG_WARN("アセットのサムネイルを生成できません: %s", ToUtf8Display(path).c_str());
    m_entries.emplace(path, std::move(entry));
}
bool AssetThumbnailCache::BuildImage(rhi::Device& device, const fs::path& path, rhi::GpuTexture& output) {
    const auto extension = Extension(path);
    const bool hdr = extension == ".hdr", linear = hdr || extension == ".exr";
    LdrImage ldrImage;
    HdrImage hdrImage;
    if (linear ? !(hdr ? LoadHdrImage(path, hdrImage) : LoadExrImage(path, hdrImage)) : !LoadLdrImage(path, ldrImage)) return false;
    const uint32_t width = linear ? hdrImage.width : ldrImage.width;
    const uint32_t height = linear ? hdrImage.height : ldrImage.height;
    if (!width || !height) return false;
    const float scale = float(ThumbnailSize) / float(std::max(width, height));
    const uint32_t scaledWidth = std::max(1u, uint32_t(float(width) * scale));
    const uint32_t scaledHeight = std::max(1u, uint32_t(float(height) * scale));
    const uint32_t offsetX = (ThumbnailSize - scaledWidth) / 2, offsetY = (ThumbnailSize - scaledHeight) / 2;
    std::vector<uint8_t> pixels(ThumbnailSize * ThumbnailSize * 4, 0);
    // HDRIだけ代表輝度で露出を揃える。EXRの通常テクスチャはリニア→sRGBのみ。
    const float exposure = hdr ? 0.18f / std::max(MedianSkyLuminance(hdrImage), 0.0001f) : 1.0f;
    for (uint32_t y = 0; y < scaledHeight; ++y) for (uint32_t x = 0; x < scaledWidth; ++x) {
        const uint32_t x0 = uint32_t(uint64_t(x) * width / scaledWidth);
        const uint32_t x1 = std::max(x0 + 1, uint32_t(uint64_t(x + 1) * width / scaledWidth));
        const uint32_t y0 = uint32_t(uint64_t(y) * height / scaledHeight);
        const uint32_t y1 = std::max(y0 + 1, uint32_t(uint64_t(y + 1) * height / scaledHeight));
        float sum[4]{};
        for (uint32_t sy = y0; sy < y1; ++sy) for (uint32_t sx = x0; sx < x1; ++sx) {
            const size_t source = (size_t(sy) * width + sx) * 4;
            for (size_t c = 0; c < 4; ++c) {
                const float value = linear ? hdrImage.pixels[source + c] : float(ldrImage.pixels[source + c]) / 255.0f;
                sum[c] += std::isfinite(value) ? std::max(value, 0.0f) : 0.0f;
            }
        }
        const float count = float(uint64_t(x1 - x0) * (y1 - y0));
        const size_t target = (size_t(y + offsetY) * ThumbnailSize + x + offsetX) * 4;
        for (size_t c = 0; c < 4; ++c) {
            float value = sum[c] / count;
            if (linear && c < 3) {
                value *= exposure;
                if (hdr) value = value / (1.0f + value);
                value = LinearToSrgb(value);
            }
            pixels[target + c] = uint8_t(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
        }
    }
    return UploadPixels(device, pixels, output);
}

// 128 x 128 の RGBA8（sRGB）を GPU のテクスチャへ上げる。
bool AssetThumbnailCache::UploadPixels(rhi::Device& device, const std::vector<uint8_t>& pixels, rhi::GpuTexture& output) {
    rhi::TextureDesc desc;
    desc.width = desc.height = ThumbnailSize;
    desc.initialState = D3D12_RESOURCE_STATE_COPY_DEST;
    desc.debugName = L"AssetImageThumbnail";
    if (!device.Allocator().CreateTexture2D(desc, output)) return false;
    const uint32_t rowPitch = ThumbnailSize * 4;
    rhi::GpuBuffer staging;
    if (!device.Allocator().CreateUploadBuffer(pixels.size(), L"AssetThumbnailUpload", staging)) return false;
    void* mapped = nullptr;
    const D3D12_RANGE range{0, 0};
    if (!TG_CHECK_HR(staging.resource->Map(0, &range, &mapped))) { device.DeferRelease(staging); return false; }
    std::memcpy(mapped, pixels.data(), pixels.size());
    staging.resource->Unmap(0, nullptr);
    const bool result = device.ExecuteImmediate([&](auto* list) {
        PIXBeginEvent(list, PIX_COLOR_DEFAULT, "AssetImageThumbnail");
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        footprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, ThumbnailSize, ThumbnailSize, 1, rowPitch};
        const CD3DX12_TEXTURE_COPY_LOCATION source(staging.resource.Get(), footprint), target(output.resource.Get(), 0);
        list->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
        rhi::TransitionIfNeeded(list, output, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        PIXEndEvent(list);
    });
    device.DeferRelease(staging);
    return result;
}
// 境界マテリアル（.tgboundary）のサムネイル。路肩の端を真上から見た絵にする: 左 1/4 が路面（内側の帯）、
// 真ん中 1/2 が境界の幅、右 1/4 が路肩。境界の幅の中はマスク（白が路面）で塗り分け、ハイトで陰影を付ける。
// 横と縦は同じ縮尺（道に沿う向きが縦）。色は材質に依らない目安の固定色（アスファルトと砂利）。
bool AssetThumbnailCache::BuildBoundary(rhi::Device& device, io::ProjectWorkspace& workspace, const fs::path& path,
                                        rhi::GpuTexture& output) {
    nlohmann::json body;
    if (!workspace.ReadAsset(path, "boundary-material-asset", body)) return false;
    const auto number = [&](const char* key, float fallback) {
        return body.contains(key) && body[key].is_number() ? body[key].get<float>() : fallback;
    };
    const auto flag = [&](const char* key) { return body.contains(key) && body[key].is_boolean() && body[key].get<bool>(); };
    const float width = std::clamp(number("width", 0.5f), 0.01f, 20.0f);
    const float repeat = std::clamp(number("repeat", 2.0f), 0.01f, 1000.0f);
    const float depth = std::clamp(number("depth", 0.03f), 0.0f, 1.0f);
    const bool alongU = flag("alongU"), invert = flag("invertMask");
    const auto image = [&](const char* key, LdrImage& out) {
        if (!body.contains(key) || !body[key].is_object()) return false;
        const auto file = workspace.Resolve(body[key]);
        return !file.empty() && LoadLdrImage(file, out) && out.width > 0 && out.height > 0;
    };
    LdrImage mask, height;
    if (!image("mask", mask)) return false;
    const bool hasHeight = image("height", height);
    // R を双線形で読む。横切る向き（across）は端で止め、道に沿う向きは繰り返す。
    const auto sample = [&](const LdrImage& img, float across, float along) {
        float u = alongU ? along : across, v = alongU ? across : along;
        const bool wrapU = alongU, wrapV = !alongU;
        const auto coord = [](float t, uint32_t size, bool wrap, uint32_t& i0, uint32_t& i1, float& f) {
            float x = t * float(size) - 0.5f;
            if (wrap) { x = std::fmod(x, float(size)); if (x < 0) x += float(size); }
            else x = std::clamp(x, 0.0f, float(size - 1));
            const float base = std::floor(x);
            f = x - base;
            i0 = uint32_t(base) % size;
            i1 = wrap ? (i0 + 1) % size : std::min(i0 + 1, size - 1);
        };
        uint32_t x0, x1, y0, y1; float fx, fy;
        coord(u, img.width, wrapU, x0, x1, fx);
        coord(v, img.height, wrapV, y0, y1, fy);
        const auto r = [&](uint32_t x, uint32_t y) { return float(img.pixels[(size_t(y) * img.width + x) * 4]) / 255.0f; };
        return (r(x0, y0) * (1 - fx) + r(x1, y0) * fx) * (1 - fy) + (r(x0, y1) * (1 - fx) + r(x1, y1) * fx) * fy;
    };
    const float span = width * 2.0f;  // 絵の一辺（m）
    const float meterPerPixel = span / float(ThumbnailSize);
    // 目安の色（sRGB）: 路面はアスファルトの暗い灰色、路肩は砂利の明るい黄土色。
    const float road[3] = {0.25f, 0.25f, 0.27f}, shoulder[3] = {0.62f, 0.56f, 0.46f};
    std::vector<uint8_t> pixels(ThumbnailSize * ThumbnailSize * 4, 255);
    for (uint32_t py = 0; py < ThumbnailSize; ++py) for (uint32_t px = 0; px < ThumbnailSize; ++px) {
        const float acrossMeters = (float(px) + 0.5f) * meterPerPixel - width * 0.5f;
        const float alongMeters = (float(ThumbnailSize - 1 - py) + 0.5f) * meterPerPixel;
        const float across = acrossMeters / width, along = alongMeters / repeat;
        float inner = acrossMeters < 0 ? 1.0f : 0.0f;
        float shade = 1.0f;
        if (acrossMeters >= 0 && acrossMeters < width) {
            inner = sample(mask, across, along);
            if (invert) inner = 1 - inner;
            if (hasHeight && depth > 0) {
                // ハイトの勾配（m / m）から法線を作り、左上からの光で陰影を付ける。外側の端へ向かって弱める
                // （描画の GeneratedMesh.hlsl と同じ）。凹凸が浅くても見えるよう、絵では勾配を強調する。
                const float step = meterPerPixel;
                const float dx = (sample(height, (acrossMeters + step) / width, along) -
                                  sample(height, (acrossMeters - step) / width, along)) / (2 * step);
                const float dy = (sample(height, across, (alongMeters + step) / repeat) -
                                  sample(height, across, (alongMeters - step) / repeat)) / (2 * step);
                const float envelope = 1.0f - std::clamp((across - 0.7f) / 0.3f, 0.0f, 1.0f);
                const float gain = 2.0f * depth * envelope * 8.0f;
                const float nx = -dx * gain, ny = -dy * gain;
                const float length = std::sqrt(nx * nx + ny * ny + 1.0f);
                // 光は左上から（-0.5, 0.5, 0.7）。平らな所がちょうど 1 になるよう、平らなときの明るさとの差で陰影を付ける。
                const float flat = 0.7f / std::sqrt(0.99f);
                const float light = (nx * -0.5f + ny * 0.5f + 0.7f) / (length * std::sqrt(0.99f));
                shade = std::clamp(1.0f + 1.2f * (light - flat), 0.4f, 1.3f);
            }
        }
        const size_t target = (size_t(py) * ThumbnailSize + px) * 4;
        for (size_t c = 0; c < 3; ++c) {
            const float value = (road[c] * inner + shoulder[c] * (1 - inner)) * shade;
            pixels[target + c] = uint8_t(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
        }
    }
    return UploadPixels(device, pixels, output);
}

void AssetThumbnailCache::Process(rhi::Device& device, rhi::PipelineCache& pipelines,
                                 io::ProjectWorkspace& workspace, const fs::path& directory,
                                 renderer::PreviewRenderer& renderer) {
    // 項目はフルパスで持つので、フォルダを移っても捨てない（戻ったときに作り直さない）。
    // 捨てるのはルートが変わったときと、保存・移動・削除で中身が変わったときだけ。
    if (m_invalidate || m_root != workspace.Root()) {
        Destroy(device); m_root = workspace.Root();
    }
    m_directory = directory;
    // 作り直しを頼まれた古いサムネイルは、項目を捨てて下の要求の列に乗せ直す。
    for (const auto& path : m_refresh) {
        if (const auto found = m_entries.find(path); found != m_entries.end() && found->second.stale) {
            device.DeferRelease(found->second.texture);
            m_entries.erase(found);
        }
    }
    if (!m_pendingModel.empty()) {
        if (m_modelRendered) {
            Store(device, m_pendingModel, m_modelPreview.TakeOutput());
            ClearScratch(device);
        } else if (m_pendingModel.parent_path() != directory) ClearScratch(device);
        return;
    }
    // 表示中のフォルダを先に、他のフォルダ（削除確認に並ぶ関連ファイルなど）を後に作る。
    auto request = std::find_if(m_requests.begin(), m_requests.end(), [&](const auto& path) {
        return path.parent_path() == directory && !m_entries.contains(path);
    });
    if (request == m_requests.end())
        request = std::find_if(m_requests.begin(), m_requests.end(),
                               [&](const auto& path) { return !m_entries.contains(path); });
    if (request == m_requests.end()) return;
    const auto path = *request;
    const auto extension = Extension(path);
    rhi::GpuTexture thumbnail;
    m_diskRecord = {};
    if (extension == ".tgscene") {
        const auto preview = io::SceneThumbnailPath(workspace, path);
        std::error_code error;
        if (fs::is_regular_file(preview, error)) BuildImage(device, preview, thumbnail);
        Store(device, path, std::move(thumbnail), false);
        // 古いシーンにプレビューが無いのは正常。保存すると生成される。
        m_entries[path].failed = false;
        return;
    }
    m_diskRecord = io::AssetThumbnailRecord(workspace, path);
    // 保存済みのサムネイルがあればそれを出す。**古くても出す**（アセットやその参照先が変わっていても）。
    // 作り直しはアセットを丸ごと読み込むので 1 つ数秒かかり、フォルダを開くたびに画面が止まるため。
    // 作り直すのは、保存済みのものが無いときと、頼まれたとき（Refresh。アセットのクリック）だけ。
    const bool current = io::ThumbnailIsCurrent(m_diskRecord);
    const bool refresh = m_refresh.erase(path) > 0;
    std::error_code imageError;
    if (current || (!refresh && fs::is_regular_file(m_diskRecord.image, imageError))) {
        if (BuildImage(device, m_diskRecord.image, thumbnail)) {
            Store(device, path, std::move(thumbnail), false);
            m_entries[path].stale = !current;
            return;
        }
        device.DeferRelease(thumbnail);
    }
    if (extension == ".tgboundary") {
        if (!BuildBoundary(device, workspace, path, thumbnail)) device.DeferRelease(thumbnail);
        Store(device, path, std::move(thumbnail));
        return;
    }
    if (extension != ".tgmat" && extension != ".tglayer" && extension != ".tgsky" && extension != ".tgmodel" && extension != ".fbx") {
        if (!BuildImage(device, path, thumbnail)) device.DeferRelease(thumbnail);
        Store(device, path, std::move(thumbnail));
        return;
    }
    ClearScratch(device);
    bool loaded = false;
    if (extension == ".fbx") {
        renderer::ModelAsset model;
        loaded = renderer::LoadModel(path, model);
        if (loaded) m_models.push_back(std::move(model));
    } else {
        nlohmann::json header;
        compositor::PaintMaskStore unusedPaint;
        graph::NodeGraph unusedGraph;
        io::ProjectRefs refs{m_textures, m_materials, unusedPaint, m_skies, renderer, unusedGraph, &m_models};
        const auto format = io::ProjectWorkspace::ReadJson(path, header) ? io::ProjectWorkspace::String(header, "format") : "";
        if (extension == ".tgmat" && (format == "terrain-graph.material" || format == "material-mixer.material"))
            loaded = io::LoadMaterial(path, device, pipelines, m_textures, m_materials) != compositor::kNoMaterialAsset;
        else loaded = io::LoadSharedAsset(workspace, path, device, pipelines, refs);
    }
    if (loaded && !m_models.empty()) {
        if (m_modelPreview.Prepare(device, m_models.front(), 0)) { m_pendingModel = path; return; }
    } else if (loaded && !m_materials.Entries().empty()) {
        m_materials.ProcessPendingWork(device, pipelines, m_textures);
        auto* material = m_materials.FindMutable(m_materials.Entries().front().id);
        thumbnail = std::exchange(material->thumbnail, {});
    } else if (loaded && !m_skies.Entries().empty()) {
        m_skies.ProcessPendingWork(device, pipelines);
        thumbnail = std::exchange(m_skies.ActiveMutable()->thumbnail, {});
    }
    Store(device, path, std::move(thumbnail));
    ClearScratch(device);
}
void AssetThumbnailCache::Render(rhi::Device& device, rhi::PipelineCache& pipelines,
                                ID3D12GraphicsCommandList* list, renderer::PreviewRenderer&) {
    if (m_pendingModel.empty() || m_modelRendered || m_models.empty()) return;
    const renderer::Environment environment;
    renderer::LightSettings light;
    m_modelPreview.Render(device, pipelines, list, m_models.front(), m_materials, m_textures,
                          environment, 0.0f, light, 0.00003f, renderer::TonemapMode::Aces);
    m_modelRendered = true;
}
}  // namespace tg
