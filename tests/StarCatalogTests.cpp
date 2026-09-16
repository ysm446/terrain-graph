#include "TestSupport.h"
#include "renderer/StarCatalog.h"
#include "../shaders/NightSkyCoordinates.hlsli"
#include <cmath>
#include <initializer_list>
#include <string_view>

void RunStarCatalogTests() {
    using tg::tests::Check;
    using namespace tg::renderer;
    tg::tests::Section("Star catalog");
    constexpr float kPi = 3.14159265358979f;
    const auto near = [](float a, float b, float tolerance) { return std::abs(a-b) <= tolerance; };
    // 等級と照度。0 等が 2.54e-6 lux で 5 等級差が 100 倍。
    Check(near(StarIlluminance(0), 2.54e-6f, 1e-9f) && near(StarIlluminance(5)*100, StarIlluminance(0), 1e-9f),
          "Magnitude scale follows the 100x per 5 magnitudes rule");
    // 色は輝度 1 に正規化し、B-V が小さい（高温）ほど青い。
    float hot[3], cool[3], solar[3];
    StarColor(-0.3f, hot); StarColor(1.5f, cool); StarColor(0.65f, solar);
    Check(hot[2] > hot[0] && cool[0] > cool[2], "Hot stars are blue and cool stars are red");
    Check(near(0.2126f*solar[0]+0.7152f*solar[1]+0.0722f*solar[2], 1.0f, 1e-3f), "Star color keeps unit luminance");
    // 星表の格子。Polaris、Sirius、Vega の短い表を並べ替える。
    StarCatalog catalog;
    const std::string_view text =
        "# comment\n"
        "37.9529,89.2642,2.02,0.60\n"
        "101.2871,-16.7161,-1.46,0.00\n"
        "\n"
        "279.2347,38.7837,0.03,0.00\n";
    Check(catalog.Parse(text) && catalog.stars.size() == 3, "Catalog text parses and skips comments");
    Check(catalog.cellOffsets.size() == size_t(TG_STAR_COLUMNS)*TG_STAR_ROWS+1 && catalog.cellOffsets.back() == 3,
          "Cell offsets cover the whole grid");
    // 各セルの範囲にある星が実際にそのセルへ属する。
    bool consistent = true;
    for (uint32_t cell = 0; cell < uint32_t(TG_STAR_COLUMNS)*TG_STAR_ROWS; ++cell) {
        for (uint32_t i = catalog.cellOffsets[cell]; i < catalog.cellOffsets[cell+1]; ++i) {
            float column = 0, row = 0;
            const auto& d = catalog.stars[i].direction;
            StarCell(d[0], d[1], d[2], column, row);
            if (uint32_t(row)*TG_STAR_COLUMNS+uint32_t(column) != cell) consistent = false;
        }
    }
    Check(consistent, "Every star lies in the cell that references it");
    float column = 0, row = 0;
    StarCell(0, 0, 1, column, row);
    Check(uint32_t(row) == TG_STAR_ROWS-1 && column < TG_STAR_COLUMNS, "Celestial pole maps to the last row");
    const StarEntry* sirius = nullptr;
    for (const auto& star : catalog.stars) if (star.illuminance > 5e-6f) sirius = &star;
    Check(sirius && near(sirius->direction[2], std::sin(-16.7161f*kPi/180), 1e-4f), "Sirius direction encodes its declination");
    // 地平→赤道変換。北極点では天頂が天の北極、赤道では東の地平線が赤経 = 恒星時 + 90 度。
    float cx = 0, cy = 0, cz = 0;
    StarCelestialDirection(0, 1, 0, kPi/2, 0, cx, cy, cz);
    Check(near(cz, 1, 1e-6f), "Zenith at the north pole is the celestial pole");
    StarCelestialDirection(0, 1, 0, 0, 0, cx, cy, cz);
    Check(near(cx, 1, 1e-6f) && near(cz, 0, 1e-6f), "Zenith on the equator at sidereal 0 has zero right ascension");
    StarCelestialDirection(1, 0, 0, 0, 0, cx, cy, cz);
    Check(near(cy, 1, 1e-6f), "East horizon on the equator is 6 hours east of the meridian");
    StarCelestialDirection(0, 0, 1, 0.5f, 0.3f, cx, cy, cz);
    Check(near(cz, std::cos(0.5f), 1e-6f), "North horizon lies at declination 90 minus latitude");
    // Polaris は北緯 35 度で北の地平線から約 35 度の高さに見える（回転によらない）。
    const float polarisRa = 37.9529f*kPi/180, polarisDec = 89.2642f*kPi/180;
    const float px = std::cos(polarisDec)*std::cos(polarisRa), py = std::cos(polarisDec)*std::sin(polarisRa), pz = std::sin(polarisDec);
    bool polarisStable = true;
    for (float rotation : {0.0f, 1.0f, 2.5f}) {
        float bestElevation = 0, bestDistance = 10;
        for (int i = 0; i <= 900; ++i) {
            const float elevation = i*(kPi/2)/900;
            for (int j = -50; j <= 50; ++j) {
                const float azimuth = j*0.002f;
                const float rx = std::cos(elevation)*std::sin(azimuth), ry = std::sin(elevation), rz = std::cos(elevation)*std::cos(azimuth);
                StarCelestialDirection(rx, ry, rz, 35*kPi/180, rotation, cx, cy, cz);
                const float dx = cx-px, dy = cy-py, dz = cz-pz;
                const float distance = std::sqrt(dx*dx+dy*dy+dz*dz);
                if (distance < bestDistance) { bestDistance = distance; bestElevation = elevation; }
            }
        }
        if (bestDistance > 0.02f || std::abs(bestElevation-35*kPi/180) > 1.0f*kPi/180) polarisStable = false;
    }
    Check(polarisStable, "Polaris stays near elevation 35 degrees at latitude 35 for any rotation");
}
