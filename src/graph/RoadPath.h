#pragma once

#include "graph/Path.h"

#include <DirectXMath.h>
#include <functional>
#include <string>
#include <vector>

// Road Path（道路の線形）。
//
// **平面の線形は Path と同じ PathSettings で持つ**（地形の正規化 UV + 地形からの高さのずれ）。
// 点の追加・移動・曲線（2 次 / 3 次 / クロソイド）・経路探索・ビューポートでの編集は
// Path と共通。その上に道路設計の縦断曲線とバンク角を足す。
//
// - 高さは既定で地形に沿う（地形の標高 + 点ごとのずれ）。縦断ポイントを置くと、
//   その間の勾配を放物線の縦断曲線でつなぐ（地形のでこぼこを均した設計の高さになる）。
// - バンク角は自動（XZ の曲率半径・設計速度・摩擦係数）か手動。正で Left 側が上がる。
//
// 計算は road-material-editor の graph/RoadProfile を移植したもの（元は road-editor の
// docs/calculation/02_vertical_curve_calculation.md / 03_bank_angle_calculation.md）。
// 座標を実寸（m）の中心線へ直してから計算するので、式は元のまま。
//
// UI / D3D12 には依存しない。地形の高さは呼び出し側が関数で渡す。
namespace tg::graph {

// 縦断ポイント。線形の正規化位置 u（0〜1）に置き、その位置の高さを offset だけずらして、
// 前後を縦断曲線長 vcl の放物線でつなぐ。
struct RoadVerticalPoint {
    PathElementId id = 0;
    float u = 0.0f;
    float vclMeters = 50.0f;
    float offsetMeters = 0.0f;
};

// バンク角ポイント。自動（設計速度から曲率に応じて決める）か手動（角度を直接指定）。
// 手動でないポイントの設計速度も、自動角の速度補間に使う。
struct RoadBankPoint {
    PathElementId id = 0;
    float u = 0.0f;
    float designSpeedKmh = 40.0f;
    bool manual = false;
    float angleDegrees = 0.0f;  // 正で Left 側（進行方向に向かって左）が上がる
};

// 新しく置く点の幅とフェザーの既定（m）。道路なので車線 2 本ぶん、フェザーは無し
// （Path の既定 24 / 12 m は地形のガイド用で広い）。
inline constexpr float kRoadDefaultWidthMeters = 7.0f;
inline constexpr float kRoadDefaultFeatherMeters = 0.0f;

struct RoadPathSettings {
    // 平面の線形。ID は path.nextId から振る（縦断・バンクのポイントも同じ番号の空間を使う）。
    PathSettings path;
    std::vector<RoadVerticalPoint> verticalPoints;
    std::vector<RoadBankPoint> bankPoints;
    // バンク角を道路へ反映するか。偽なら手動ポイントがあっても水平のまま。
    bool bankEnabled = false;
    float designSpeedKmh = 40.0f;       // ポイントの無い所の設計速度（km/h）
    float frictionCoefficient = 0.15f;  // 自動バンクの横方向摩擦係数
    bool smoothBank = false;            // 距離方向のガウス平滑化
    float bankSmoothMeters = 20.0f;
};

// 距離パラメータ付きの折れ線（実寸 m。X / Z は地形の中心が原点、Y はワールドの高さ）。
struct RoadProfileCurve {
    std::vector<DirectX::XMFLOAT3> points;
    std::vector<float> arcLengths;  // points と同じ長さ。先頭は 0
    float TotalLength() const { return arcLengths.empty() ? 0.0f : arcLengths.back(); }
    // 距離（両端へクランプ）の位置。
    DirectX::XMFLOAT3 At(float distance) const;
};
RoadProfileCurve BuildRoadProfileCurve(const std::vector<DirectX::XMFLOAT3>& points);

// 地形の高さ。UV（0〜1）を受け、ワールドの Y（m）を返す。
using RoadHeightSampler = std::function<float(float u, float v)>;

// 中心線を割る間隔（m）。地形の高さをこの細かさで拾う。
inline constexpr float kRoadCenterlineStepMeters = 1.0f;

// 縦断を掛ける前の中心線（地形の高さ + 点ごとのずれ）。Path の唯一の開いた鎖から作る。
// sizeMeters は地形の一辺。失敗（鎖が 1 本でない等）なら偽で、error に理由を入れる。
bool BuildRoadBaseline(const RoadPathSettings& road, float sizeMeters, const RoadHeightSampler& height,
                       RoadProfileCurve& outBaseline, std::string* error);
// 縦断曲線を適用した高さ。base の各点に対応する高さを返す（ポイントが無ければ base の y のまま）。
std::vector<float> EvaluateVerticalProfile(const RoadPathSettings& road, const RoadProfileCurve& base);
// 縦断を反映した中心線（道路の設計の高さ）。
bool BuildRoadCenterline(const RoadPathSettings& road, float sizeMeters, const RoadHeightSampler& height,
                         RoadProfileCurve& outCenterline, std::string* error);

// バンク角（ラジアン）。curve は縦断反映後の中心線。Raw は平滑化なし。
float EvaluateBankAngleRadiansRaw(const RoadPathSettings& road, const RoadProfileCurve& curve, float distance);
float EvaluateBankAngleRadians(const RoadPathSettings& road, const RoadProfileCurve& curve, float distance);
// 曲率半径と設計速度・摩擦係数から求める自動バンクの大きさ（符号なし）。
float ComputeAutoBankRadians(float radius, float designSpeedKmh, float friction);

// 線形上の位置とフレーム。UI のマーカーに使う。
struct RoadProfileFrame {
    DirectX::XMFLOAT3 position{};
    DirectX::XMFLOAT3 tangent{0.0f, 0.0f, 1.0f};
    DirectX::XMFLOAT3 right{1.0f, 0.0f, 0.0f};  // 進行方向に向かって右（水平、バンク前）
    DirectX::XMFLOAT3 up{0.0f, 1.0f, 0.0f};
    float distance = 0.0f;
    float bankRadians = 0.0f;
};
// 正規化位置 u（0〜1）のフレーム。
RoadProfileFrame EvaluateRoadProfileFrame(const RoadPathSettings& road, const RoadProfileCurve& centerline,
                                          float u);

// --- ポイントの編集 ---------------------------------------------------------
PathElementId AddVerticalPoint(RoadPathSettings& road, float u);
PathElementId AddBankPoint(RoadPathSettings& road, float u);
bool DeleteRoadProfilePoint(RoadPathSettings& road, PathElementId id);
RoadVerticalPoint* FindVerticalPoint(RoadPathSettings& road, PathElementId id);
RoadBankPoint* FindBankPoint(RoadPathSettings& road, PathElementId id);

}  // namespace tg::graph
