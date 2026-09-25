#include "core/ImageIo.h"

#include "core/PathUtf8.h"

#include "core/Log.h"

#include <algorithm>
#include <cstdarg>
#include <cstdlib>
#include <fstream>
#include <future>
#include <mutex>
#include <semaphore>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

// tinyexr は miniz を同梱の実装から使う。vcpkg 版はライブラリとして提供されるので、
// ここでは実装マクロを定義しない。
#include <tinyexr.h>

namespace tg {
namespace {

// パスを stb / tinyexr のナロー API へ渡さない。ナロー変換（path::string()）は
// ロケール依存で、ACP に無い文字を含むパスが壊れる（ProjectIo 側の方針と同じ）。
// ファイルはワイドパス対応の iostream で読み書きし、画像ライブラリには
// メモリ経由で渡す。

std::vector<uint8_t> ReadFileBytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream.is_open()) {
        return {};
    }
    const std::streamsize size = stream.tellg();
    if (size <= 0) {
        return {};
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    stream.seekg(0);
    stream.read(reinterpret_cast<char*>(bytes.data()), size);
    if (!stream.good()) {
        return {};
    }
    return bytes;
}

bool WriteFileBytes(const std::filesystem::path& path, const void* data, size_t size) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream.is_open()) {
        return false;
    }
    stream.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    return stream.good();
}

bool SavePng(const std::filesystem::path& path, uint32_t width, uint32_t height,
             uint32_t rowPitch, int channels, const uint8_t* pixels) {
    if (pixels == nullptr || width == 0 || height == 0) {
        return false;
    }

    int pngSize = 0;
    unsigned char* png =
        ::stbi_write_png_to_mem(pixels, static_cast<int>(rowPitch), static_cast<int>(width),
                                static_cast<int>(height), channels, &pngSize);
    if (png == nullptr || pngSize <= 0) {
        TG_LOG_ERROR("PNG を書き出せません: %s", ToUtf8Display(path).c_str());
        ::free(png);
        return false;
    }
    const bool written = WriteFileBytes(path, png, static_cast<size_t>(pngSize));
    ::free(png);
    if (!written) {
        TG_LOG_ERROR("PNG を書き出せません: %s", ToUtf8Display(path).c_str());
        return false;
    }

    TG_LOG_INFO("PNG を書き出しました: %s (%u x %u, %d ch)", ToUtf8Display(path).c_str(), width,
                height, channels);
    return true;
}


std::string FormatMessage(const char* fmt, ...) {
    char body[1920];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(body, sizeof(body), fmt, args);
    va_end(args);
    return body;
}

}  // namespace

// 先読みからも呼ぶので無名名前空間の外で宣言する（定義は下）。
bool DecodeLdrImage(const std::filesystem::path& path, LdrImage& outImage, std::string& message);
bool DecodeHdrImage(const std::filesystem::path& path, HdrImage& outImage, std::string& message);
bool DecodeExrImage(const std::filesystem::path& path, HdrImage& outImage, std::string& message);

namespace {

// --- 先読み --------------------------------------------------------------
//
// シーンには数十枚の 2K〜4K の画像があり、1 枚ずつ順に読むと EXR のデコードだけで
// 数秒かかる。先にパスをまとめて渡しておくと裏でデコードし、後の Load* が
// その結果を受け取る。ログは受け取った側（メインスレッド）で出す。
// デコードは stb / tinyexr のメモリ API を別々の入力で呼ぶだけなので並列に走らせてよい。
struct PrefetchedImage {
    bool ok = false;
    bool hdr = false;  // .hdr / .exr は HdrImage、それ以外は LdrImage
    LdrImage ldr;
    HdrImage hdrImage;
    std::string message;
};

std::mutex g_prefetchMutex;
std::unordered_map<std::wstring, std::shared_future<std::shared_ptr<PrefetchedImage>>> g_prefetched;

std::wstring PrefetchKey(const std::filesystem::path& path) {
    std::error_code error;
    const auto absolute = std::filesystem::absolute(path, error);
    return (error ? path : absolute).lexically_normal().wstring();
}

std::wstring LowerExtension(const std::filesystem::path& path) {
    auto ext = path.extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
    return ext;
}

// 先読みした結果があれば取り出す（1 回きり）。hdr は呼び出し側が期待する型。
std::shared_ptr<PrefetchedImage> TakePrefetched(const std::filesystem::path& path, bool hdr) {
    std::shared_future<std::shared_ptr<PrefetchedImage>> future;
    {
        std::lock_guard<std::mutex> lock(g_prefetchMutex);
        const auto found = g_prefetched.find(PrefetchKey(path));
        if (found == g_prefetched.end()) return nullptr;
        future = found->second;
        g_prefetched.erase(found);
    }
    auto image = future.get();
    return image && image->hdr == hdr ? image : nullptr;
}

}  // namespace

void PrefetchImages(const std::vector<std::filesystem::path>& paths) {
    // 同時に走らせる数は CPU のコア数まで。1 枚ずつスレッドを作るが、デコードが
    // 数百 ms なので生成の費用は無視できる。
    static std::counting_semaphore<64> slots(
        static_cast<std::ptrdiff_t>(std::clamp(std::thread::hardware_concurrency(), 2u, 64u)));
    std::lock_guard<std::mutex> lock(g_prefetchMutex);
    for (const auto& path : paths) {
        const auto key = PrefetchKey(path);
        if (key.empty() || g_prefetched.contains(key)) continue;
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error)) continue;
        g_prefetched.emplace(key, std::async(std::launch::async, [path]() {
            slots.acquire();
            auto image = std::make_shared<PrefetchedImage>();
            const auto ext = LowerExtension(path);
            image->hdr = ext == L".hdr" || ext == L".exr";
            if (!image->hdr) image->ok = DecodeLdrImage(path, image->ldr, image->message);
            else if (ext == L".exr") image->ok = DecodeExrImage(path, image->hdrImage, image->message);
            else image->ok = DecodeHdrImage(path, image->hdrImage, image->message);
            slots.release();
            return image;
        }).share());
    }
}

void DiscardPrefetchedImages() {
    std::unordered_map<std::wstring, std::shared_future<std::shared_ptr<PrefetchedImage>>> pending;
    {
        std::lock_guard<std::mutex> lock(g_prefetchMutex);
        pending.swap(g_prefetched);
    }
    // 走っているデコードは終わるまで待つ（結果は捨てる）。
    for (auto& [key, future] : pending) future.wait();
}

namespace {
}  // namespace

bool DecodeLdrImage(const std::filesystem::path& path, LdrImage& outImage, std::string& message) {
    outImage = LdrImage{};

    const std::string utf8Path = ToUtf8Display(path);
    const std::vector<uint8_t> bytes = ReadFileBytes(path);
    if (bytes.empty()) {
        message = FormatMessage("画像を読み込めません: %s (ファイルを開けない)", utf8Path.c_str());
        return false;
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* data = ::stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()),
                                            &width, &height, &channels, 4);
    if (data == nullptr) {
        message = FormatMessage("画像を読み込めません: %s (%s)", utf8Path.c_str(), ::stbi_failure_reason());
        return false;
    }

    outImage.width = static_cast<uint32_t>(width);
    outImage.height = static_cast<uint32_t>(height);
    outImage.pixels.assign(data, data + static_cast<size_t>(width) * height * 4);
    ::stbi_image_free(data);

    message = FormatMessage("画像を読み込みました: %s (%d x %d, %d ch)", utf8Path.c_str(), width, height,
                channels);
    return true;
}

float MedianSkyLuminance(const HdrImage& image) {
    if (image.width == 0 || image.height == 0 || image.pixels.empty()) {
        return 0.0f;
    }

    // 上から 40% を「空」とみなす。地面や被写体を含めると暗くなりすぎる。
    const uint32_t skyRows = std::max<uint32_t>(1, image.height * 2 / 5);
    // 4K でも一瞬で終わるよう間引く。中央値には十分な数を取る。
    const uint32_t stepX = std::max<uint32_t>(1, image.width / 256);
    const uint32_t stepY = std::max<uint32_t>(1, skyRows / 128);

    std::vector<float> samples;
    samples.reserve(static_cast<size_t>(256) * 128);
    for (uint32_t y = 0; y < skyRows; y += stepY) {
        for (uint32_t x = 0; x < image.width; x += stepX) {
            const size_t index = (static_cast<size_t>(y) * image.width + x) * 4;
            const float r = image.pixels[index];
            const float g = image.pixels[index + 1];
            const float b = image.pixels[index + 2];
            samples.push_back(0.2126f * r + 0.7152f * g + 0.0722f * b);
        }
    }
    if (samples.empty()) {
        return 0.0f;
    }

    const size_t middle = samples.size() / 2;
    std::nth_element(samples.begin(), samples.begin() + middle, samples.end());
    return samples[middle];
}

bool DecodeHdrImage(const std::filesystem::path& path, HdrImage& outImage, std::string& message) {
    outImage = HdrImage{};

    const std::string utf8Path = ToUtf8Display(path);
    const std::vector<uint8_t> bytes = ReadFileBytes(path);
    if (bytes.empty()) {
        message = FormatMessage("HDR 画像を読み込めません: %s (ファイルを開けない)", utf8Path.c_str());
        return false;
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    float* data = ::stbi_loadf_from_memory(bytes.data(), static_cast<int>(bytes.size()),
                                           &width, &height, &channels, 4);
    if (data == nullptr) {
        message = FormatMessage("HDR 画像を読み込めません: %s (%s)", utf8Path.c_str(), ::stbi_failure_reason());
        return false;
    }

    outImage.width = static_cast<uint32_t>(width);
    outImage.height = static_cast<uint32_t>(height);
    outImage.pixels.assign(data, data + static_cast<size_t>(width) * height * 4);
    ::stbi_image_free(data);

    message = FormatMessage("HDR 画像を読み込みました: %s (%d x %d, %d ch)", utf8Path.c_str(), width, height,
                channels);
    return true;
}

bool DecodeExrImage(const std::filesystem::path& path, HdrImage& outImage, std::string& message) {
    outImage = HdrImage{};

    const std::string utf8Path = ToUtf8Display(path);
    const std::vector<uint8_t> bytes = ReadFileBytes(path);
    if (bytes.empty()) {
        message = FormatMessage("EXR を読み込めません: %s (ファイルを開けない)", utf8Path.c_str());
        return false;
    }

    float* data = nullptr;
    int width = 0;
    int height = 0;
    const char* error = nullptr;
    // LoadEXRFromMemory は常に RGBA の 4 チャンネルで返す。
    const int result =
        ::LoadEXRFromMemory(&data, &width, &height, bytes.data(), bytes.size(), &error);
    if (result != TINYEXR_SUCCESS) {
        message = FormatMessage("EXR を読み込めません: %s (%s)", utf8Path.c_str(),
                     (error != nullptr) ? error : "原因不明");
        if (error != nullptr) {
            ::FreeEXRErrorMessage(error);
        }
        return false;
    }

    outImage.width = static_cast<uint32_t>(width);
    outImage.height = static_cast<uint32_t>(height);
    outImage.pixels.assign(data, data + static_cast<size_t>(width) * height * 4);
    ::free(data);

    message = FormatMessage("EXR を読み込みました: %s (%d x %d)", utf8Path.c_str(), width, height);
    return true;
}

bool SaveRgba8Png(const std::filesystem::path& path, uint32_t width, uint32_t height,
                  uint32_t rowPitch, const uint8_t* pixels) {
    return SavePng(path, width, height, rowPitch, 4, pixels);
}

bool SaveRgb8Png(const std::filesystem::path& path, uint32_t width, uint32_t height,
                 uint32_t rowPitch, const uint8_t* pixels) {
    return SavePng(path, width, height, rowPitch, 3, pixels);
}

bool SaveGray8Png(const std::filesystem::path& path, uint32_t width, uint32_t height,
                  uint32_t rowPitch, const uint8_t* pixels) {
    return SavePng(path, width, height, rowPitch, 1, pixels);
}

bool SaveExr(const std::filesystem::path& path, uint32_t width, uint32_t height, int channels,
             const float* pixels, bool asHalf) {
    if (pixels == nullptr || width == 0 || height == 0) {
        return false;
    }
    if (channels != 1 && channels != 3 && channels != 4) {
        TG_LOG_ERROR("EXR に書けないチャンネル数です: %d", channels);
        return false;
    }

    unsigned char* buffer = nullptr;
    const char* error = nullptr;
    const int size = ::SaveEXRToMemory(pixels, static_cast<int>(width), static_cast<int>(height),
                                       channels, asHalf ? 1 : 0, &buffer, &error);
    if (size <= 0 || buffer == nullptr) {
        TG_LOG_ERROR("EXR を書き出せません: %s (%s)", ToUtf8Display(path).c_str(),
                     (error != nullptr) ? error : "不明なエラー");
        ::FreeEXRErrorMessage(error);
        ::free(buffer);
        return false;
    }
    ::FreeEXRErrorMessage(error);

    const bool written = WriteFileBytes(path, buffer, static_cast<size_t>(size));
    ::free(buffer);
    if (!written) {
        TG_LOG_ERROR("EXR を書き出せません: %s", ToUtf8Display(path).c_str());
        return false;
    }

    TG_LOG_INFO("EXR を書き出しました: %s (%u x %u, %d ch)", ToUtf8Display(path).c_str(), width,
                height, channels);
    return true;
}

bool LoadLdrImage(const std::filesystem::path& path, LdrImage& outImage) {
    std::string message;
    bool ok = false;
    if (const auto prefetched = TakePrefetched(path, false)) {
        ok = prefetched->ok;
        outImage = std::move(prefetched->ldr);
        message = std::move(prefetched->message);
    } else {
        ok = DecodeLdrImage(path, outImage, message);
    }
    if (ok) TG_LOG_INFO("%s", message.c_str()); else TG_LOG_ERROR("%s", message.c_str());
    return ok;
}

bool LoadHdrImage(const std::filesystem::path& path, HdrImage& outImage) {
    std::string message;
    bool ok = false;
    if (const auto prefetched = TakePrefetched(path, true)) {
        ok = prefetched->ok;
        outImage = std::move(prefetched->hdrImage);
        message = std::move(prefetched->message);
    } else {
        ok = DecodeHdrImage(path, outImage, message);
    }
    if (ok) TG_LOG_INFO("%s", message.c_str()); else TG_LOG_ERROR("%s", message.c_str());
    return ok;
}

bool LoadExrImage(const std::filesystem::path& path, HdrImage& outImage) {
    std::string message;
    bool ok = false;
    if (const auto prefetched = TakePrefetched(path, true)) {
        ok = prefetched->ok;
        outImage = std::move(prefetched->hdrImage);
        message = std::move(prefetched->message);
    } else {
        ok = DecodeExrImage(path, outImage, message);
    }
    if (ok) TG_LOG_INFO("%s", message.c_str()); else TG_LOG_ERROR("%s", message.c_str());
    return ok;
}

}  // namespace tg
