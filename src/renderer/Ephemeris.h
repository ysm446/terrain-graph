#pragma once
#include <cstdint>

namespace tg::renderer {
// 観測地と日時から太陽・月・恒星時を求める設定。mode が 0 のときは使わず、
// 太陽・月・星空の回転を利用者が手動で決める。緯度は AtmosphereSettings::starLatitude と共用する。
struct CelestialSettings {
    uint32_t mode = 0;            // 0: 手動、1: 緯度経度と日時から計算。
    float longitude = 2.43952f;   // 観測地の経度（rad、東経が正。既定 139.77 度）。
    int year = 2026, month = 6, day = 21;
    float hour = 21.0f;           // 地方時（時、小数可）。
    float utcOffset = 9.0f;       // 地方時の UTC からのずれ（時）。
};

struct EquatorialPosition { float rightAscension = 0, declination = 0; }; // rad
struct HorizontalPosition { float azimuth = 0, elevation = 0; };          // rad。方位角は北 0、東 +90 度。

// グレゴリオ暦の日付と UT（時）からユリウス日。
double JulianDay(int year, int month, int day, double utcHours);
// グリニッジ／地方恒星時（rad）。
float GreenwichSiderealTime(double julianDay);
float LocalSiderealTime(double julianDay, float longitude);
// 低精度の太陽・月の地心赤道座標（Montenbruck & Pfleger の簡略式、太陽 約 1 分角、月 約 数分角）。
EquatorialPosition SunEquatorial(double julianDay);
EquatorialPosition MoonEquatorial(double julianDay);
// 月の見かけの照らされた面積の割合（0: 新月、1: 満月）。
float MoonIlluminatedFraction(double julianDay);
// 赤道座標を観測地の地平座標へ。屈折は含めない。
HorizontalPosition ToHorizontal(EquatorialPosition position, float latitude, float localSiderealTime);

struct CelestialState {
    HorizontalPosition sun, moon;
    float moonPhase = 1;       // 見かけの照らされた面積。
    float siderealTime = 0;    // 地方恒星時（rad）。星空の回転にそのまま入る。
};
// 設定と観測緯度から一括で求める。月の高度には平均の地心視差（約 0.95 度）を含める。
CelestialState ComputeCelestialState(const CelestialSettings& settings, float latitude);
}
