#include "renderer/StarCatalog.h"
#include "../../shaders/NightSkyCoordinates.hlsli"
#include "core/Log.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <sstream>

namespace tg::renderer {
namespace {
constexpr float kPi = 3.14159265358979f;
bool ParseFloat(std::string_view& text, float& value) {
    while (!text.empty() && text.front() == ' ') text.remove_prefix(1);
    const auto result = std::from_chars(text.data(), text.data()+text.size(), value);
    if (result.ec != std::errc{}) return false;
    text.remove_prefix(static_cast<size_t>(result.ptr-text.data()));
    while (!text.empty() && (text.front() == ' ' || text.front() == ',')) text.remove_prefix(1);
    return true;
}
}

float StarIlluminance(float magnitude) {
    return 2.54e-6f*std::pow(10.0f, -0.4f*magnitude);
}

void StarColor(float colorIndex, float rgb[3]) {
    // Ballesteros の B-V → 色温度近似と、Kim らの CIE 1931 黒体軌跡近似。
    const float bv = std::clamp(colorIndex, -0.4f, 2.0f);
    const float temperature = std::clamp(4600.0f*(1.0f/(0.92f*bv+1.7f)+1.0f/(0.92f*bv+0.62f)), 1667.0f, 25000.0f);
    const double t = temperature, t2 = t*t, t3 = t2*t;
    const double x = t <= 4000 ? -0.2661239e9/t3-0.2343589e6/t2+0.8776956e3/t+0.179910
                               : -3.0258469e9/t3+2.1070379e6/t2+0.2226347e3/t+0.240390;
    const double x2 = x*x, x3 = x2*x;
    const double y = t <= 2222 ? -1.1063814*x3-1.34811020*x2+2.18555832*x-0.20219683
                   : t <= 4000 ? -0.9549476*x3-1.37418593*x2+2.09137015*x-0.16748867
                               :  3.0817580*x3-5.87338670*x2+3.75112997*x-0.37001483;
    const double X = x/y, Y = 1.0, Z = (1.0-x-y)/y;
    double r = std::max(0.0,  3.2406*X-1.5372*Y-0.4986*Z);
    double g = std::max(0.0, -0.9689*X+1.8758*Y+0.0415*Z);
    double b = std::max(0.0,  0.0557*X-0.2040*Y+1.0570*Z);
    const double luminance = std::max(1e-6, 0.2126*r+0.7152*g+0.0722*b);
    rgb[0] = static_cast<float>(r/luminance);
    rgb[1] = static_cast<float>(g/luminance);
    rgb[2] = static_cast<float>(b/luminance);
}

bool StarCatalog::Parse(std::string_view text) {
    struct Parsed { StarEntry entry; uint32_t cell; };
    std::vector<Parsed> parsed;
    size_t begin = 0;
    while (begin < text.size()) {
        size_t end = text.find('\n', begin);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(begin, end-begin);
        begin = end+1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
        if (line.empty() || line.front() == '#') continue;
        float ra = 0, dec = 0, magnitude = 0, colorIndex = 0;
        if (!ParseFloat(line, ra) || !ParseFloat(line, dec) || !ParseFloat(line, magnitude)) return false;
        if (!line.empty() && !ParseFloat(line, colorIndex)) return false;
        Parsed item;
        const float alpha = ra*kPi/180.0f, delta = dec*kPi/180.0f;
        item.entry.direction[0] = std::cos(delta)*std::cos(alpha);
        item.entry.direction[1] = std::cos(delta)*std::sin(alpha);
        item.entry.direction[2] = std::sin(delta);
        item.entry.illuminance = StarIlluminance(magnitude);
        StarColor(colorIndex, item.entry.color);
        float column = 0, row = 0;
        StarCell(item.entry.direction[0], item.entry.direction[1], item.entry.direction[2], column, row);
        item.cell = static_cast<uint32_t>(row)*TG_STAR_COLUMNS+static_cast<uint32_t>(column);
        parsed.push_back(item);
    }
    std::stable_sort(parsed.begin(), parsed.end(), [](const Parsed& a, const Parsed& b) { return a.cell < b.cell; });
    stars.clear(); stars.reserve(parsed.size());
    cellOffsets.assign(size_t(TG_STAR_COLUMNS)*TG_STAR_ROWS+1, 0);
    for (const auto& item : parsed) {
        stars.push_back(item.entry);
        ++cellOffsets[item.cell+1];
    }
    for (size_t i = 1; i < cellOffsets.size(); ++i) cellOffsets[i] += cellOffsets[i-1];
    return !stars.empty();
}

bool StarCatalog::LoadFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        TG_LOG_WARN("星表を開けません: %s", path.string().c_str());
        return false;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    const std::string text = buffer.str();
    if (!Parse(text)) {
        TG_LOG_WARN("星表の形式が不正です: %s", path.string().c_str());
        return false;
    }
    return true;
}
}
