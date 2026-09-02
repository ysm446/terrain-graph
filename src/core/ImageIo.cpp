#include "core/ImageIo.h"

#include "core/PathUtf8.h"

#include "core/Log.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <string>
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

}  // namespace

bool LoadLdrImage(const std::filesystem::path& path, LdrImage& outImage) {
    outImage = LdrImage{};

    const std::string utf8Path = ToUtf8Display(path);
    const std::vector<uint8_t> bytes = ReadFileBytes(path);
    if (bytes.empty()) {
        TG_LOG_ERROR("画像を読み込めません: %s (ファイルを開けない)", utf8Path.c_str());
        return false;
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* data = ::stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()),
                                            &width, &height, &channels, 4);
    if (data == nullptr) {
        TG_LOG_ERROR("画像を読み込めません: %s (%s)", utf8Path.c_str(), ::stbi_failure_reason());
        return false;
    }

    outImage.width = static_cast<uint32_t>(width);
    outImage.height = static_cast<uint32_t>(height);
    outImage.pixels.assign(data, data + static_cast<size_t>(width) * height * 4);
    ::stbi_image_free(data);

    TG_LOG_INFO("画像を読み込みました: %s (%d x %d, %d ch)", utf8Path.c_str(), width, height,
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

bool LoadHdrImage(const std::filesystem::path& path, HdrImage& outImage) {
    outImage = HdrImage{};

    const std::string utf8Path = ToUtf8Display(path);
    const std::vector<uint8_t> bytes = ReadFileBytes(path);
    if (bytes.empty()) {
        TG_LOG_ERROR("HDR 画像を読み込めません: %s (ファイルを開けない)", utf8Path.c_str());
        return false;
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    float* data = ::stbi_loadf_from_memory(bytes.data(), static_cast<int>(bytes.size()),
                                           &width, &height, &channels, 4);
    if (data == nullptr) {
        TG_LOG_ERROR("HDR 画像を読み込めません: %s (%s)", utf8Path.c_str(), ::stbi_failure_reason());
        return false;
    }

    outImage.width = static_cast<uint32_t>(width);
    outImage.height = static_cast<uint32_t>(height);
    outImage.pixels.assign(data, data + static_cast<size_t>(width) * height * 4);
    ::stbi_image_free(data);

    TG_LOG_INFO("HDR 画像を読み込みました: %s (%d x %d, %d ch)", utf8Path.c_str(), width, height,
                channels);
    return true;
}

bool LoadExrImage(const std::filesystem::path& path, HdrImage& outImage) {
    outImage = HdrImage{};

    const std::string utf8Path = ToUtf8Display(path);
    const std::vector<uint8_t> bytes = ReadFileBytes(path);
    if (bytes.empty()) {
        TG_LOG_ERROR("EXR を読み込めません: %s (ファイルを開けない)", utf8Path.c_str());
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
        TG_LOG_ERROR("EXR を読み込めません: %s (%s)", utf8Path.c_str(),
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

    TG_LOG_INFO("EXR を読み込みました: %s (%d x %d)", utf8Path.c_str(), width, height);
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

}  // namespace tg
