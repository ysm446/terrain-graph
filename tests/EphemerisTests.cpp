#include "TestSupport.h"
#include "renderer/Ephemeris.h"
#include <cmath>

void RunEphemerisTests() {
    using tg::tests::Check;
    using namespace tg::renderer;
    tg::tests::Section("Ephemeris");
    constexpr double kPi = 3.14159265358979;
    const auto degrees = [](double radians) { return radians*180/kPi; };
    const auto near = [](double a, double b, double tolerance) { return std::abs(a-b) <= tolerance; };
    // J2000.0 の基準ユリウス日と、その時刻のグリニッジ恒星時（18h41m50.5s）。
    Check(near(JulianDay(2000, 1, 1, 12), 2451545.0, 1e-9), "Julian day of J2000.0");
    Check(near(JulianDay(1999, 12, 31, 0), 2451543.5, 1e-9), "Julian day handles January and February shift");
    Check(near(degrees(GreenwichSiderealTime(2451545.0)), 280.46, 0.01), "Greenwich sidereal time at J2000.0");
    // 2000-01-01 12:00 UT の太陽（天文年鑑値 赤経 18h44.8m、赤緯 -23.03 度）。
    const auto sun = SunEquatorial(2451545.0);
    Check(near(degrees(sun.rightAscension), 281.2, 0.3) && near(degrees(sun.declination), -23.03, 0.1),
          "Sun position at J2000.0 within a few arc minutes");
    // 新月 2000-01-06 18:14 UT、満月 2000-01-21 04:40 UT。
    Check(MoonIlluminatedFraction(JulianDay(2000, 1, 6, 18.23)) < 0.005, "New moon has no illuminated area");
    Check(MoonIlluminatedFraction(JulianDay(2000, 1, 21, 4.67)) > 0.99, "Full moon is fully illuminated");
    {
        const auto moon = MoonEquatorial(JulianDay(2000, 1, 6, 18.23));
        const auto sunAtNew = SunEquatorial(JulianDay(2000, 1, 6, 18.23));
        double delta = degrees(moon.rightAscension-sunAtNew.rightAscension);
        delta = std::abs(std::remainder(delta, 360.0));
        Check(delta < 3.0, "New moon lies near the sun in right ascension");
    }
    // 地平座標。赤道上で赤緯 0 の天体が南中すると方位 180 度・高度 90 度、6 時間前は東の地平線。
    {
        const auto zenith = ToHorizontal({0, 0}, 0, 0);
        Check(near(degrees(zenith.elevation), 90, 1e-3), "Meridian transit on the equator reaches the zenith");
        const auto east = ToHorizontal({static_cast<float>(kPi/2), 0}, 0, 0);
        Check(near(degrees(east.azimuth), 90, 1e-3) && near(degrees(east.elevation), 0, 1e-3),
              "Rising body six hours before transit is due east on the horizon");
        const auto north = ToHorizontal({0, static_cast<float>(kPi/2)}, static_cast<float>(35*kPi/180), 1.0f);
        Check(near(degrees(north.elevation), 35, 1e-3) && near(std::abs(degrees(north.azimuth)), 0, 1e-3),
              "Celestial pole stands due north at the latitude's elevation");
    }
    // 東京（北緯 35.68、東経 139.77）の夏至 2026-06-21 12:00 JST。南中は 11:43 頃で高度は約 77.8 度。
    CelestialSettings tokyo;
    tokyo.mode = 1; tokyo.longitude = static_cast<float>(139.77*kPi/180);
    tokyo.year = 2026; tokyo.month = 6; tokyo.day = 21; tokyo.hour = 12; tokyo.utcOffset = 9;
    const float latitude = static_cast<float>(35.68*kPi/180);
    const auto noon = ComputeCelestialState(tokyo, latitude);
    // 南中の 17 分後なので高度は 77.2 度、方位は天頂に近いぶん速く動き南から約 18 度西。
    Check(near(degrees(noon.sun.elevation), 77.2, 0.3) && std::abs(degrees(noon.sun.azimuth)) > 160,
          "Summer solstice noon sun in Tokyo is high in the south");
    tokyo.hour = 0;
    const auto midnight = ComputeCelestialState(tokyo, latitude);
    Check(near(degrees(midnight.sun.elevation), -30.9, 0.7) && std::abs(degrees(midnight.sun.azimuth)) < 15,
          "Summer solstice midnight sun in Tokyo is below the northern horizon");
    // 満月 2026-06-29 23:57 UT は東京では 6/30 08:57 JST。前夜 6/30 00:00 JST の月は南寄りに見える。
    tokyo.month = 6; tokyo.day = 30; tokyo.hour = 0;
    const auto fullMoon = ComputeCelestialState(tokyo, latitude);
    Check(fullMoon.moonPhase > 0.98 && degrees(fullMoon.moon.elevation) > 20 && std::abs(degrees(fullMoon.moon.azimuth)) > 120,
          "Full moon near midnight stands high in the southern sky");
    Check(near(degrees(fullMoon.siderealTime), degrees(LocalSiderealTime(JulianDay(2026, 6, 29, 15), tokyo.longitude)), 1e-3),
          "Sidereal time uses the UTC-converted local time");
}
