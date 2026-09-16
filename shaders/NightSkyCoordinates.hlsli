#ifndef TG_NIGHT_SKY_COORDINATES
#define TG_NIGHT_SKY_COORDINATES
// C++ と HLSL で共有する地平座標→赤道座標の変換。星表の格子分割もここで定義する。
#ifdef __cplusplus
#include <cmath>
namespace tg::renderer {
#define TG_NSK_INLINE inline
#define TG_NSK_OUT float&
#else
#define TG_NSK_INLINE
#define TG_NSK_OUT out float
#endif
// 赤経 360 分割・赤緯 180 分割の 1 度格子。セル c の星は offsets[c]..offsets[c+1)。
#define TG_STAR_COLUMNS 360
#define TG_STAR_ROWS 180
// ワールドは +Z が北、+X が東、+Y が天頂。天の北極は北の地平線から緯度ぶん持ち上がる。
// rotation は地方恒星時に相当し、赤経 = rotation の星が南中する。
// 出力は赤道座標の単位ベクトル (cos dec cos ra, cos dec sin ra, sin dec)。
TG_NSK_INLINE void StarCelestialDirection(float rx, float ry, float rz, float latitude, float rotation,
                                          TG_NSK_OUT cx, TG_NSK_OUT cy, TG_NSK_OUT cz) {
#ifdef __cplusplus
    const float sinLat = std::sin(latitude), cosLat = std::cos(latitude);
    const float sinRot = std::sin(rotation), cosRot = std::cos(rotation);
#else
    float sinLat, cosLat, sinRot, cosRot;
    sincos(latitude, sinLat, cosLat);
    sincos(rotation, sinRot, cosRot);
#endif
    // 南中点（赤緯 0・時角 0）方向と東方向への射影は (cos dec cos H, -cos dec sin H)。
    const float meridian = ry*cosLat - rz*sinLat;
    const float east = rx;
    cx = meridian*cosRot - east*sinRot;
    cy = meridian*sinRot + east*cosRot;
    cz = ry*sinLat + rz*cosLat;
}
// 赤道座標の方向が属する格子セルの列と行。
TG_NSK_INLINE void StarCell(float cx, float cy, float cz, TG_NSK_OUT column, TG_NSK_OUT row) {
    const float kPi = 3.14159265358979f;
#ifdef __cplusplus
    float ra = std::atan2(cy, cx);
    const float dec = std::asin(std::fmin(1.0f, std::fmax(-1.0f, cz)));
#else
    float ra = atan2(cy, cx);
    const float dec = asin(clamp(cz, -1.0f, 1.0f));
#endif
    if (ra < 0) ra += 2*kPi;
    column = ra/(2*kPi)*TG_STAR_COLUMNS;
    row = (dec/kPi+0.5f)*TG_STAR_ROWS;
    if (column >= TG_STAR_COLUMNS) column = 0;
    if (row >= TG_STAR_ROWS) row = TG_STAR_ROWS-1;
}
#ifdef __cplusplus
}
#endif
#endif
