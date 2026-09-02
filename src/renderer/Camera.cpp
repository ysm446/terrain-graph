#include "renderer/Camera.h"

#include <algorithm>
#include <cmath>

using namespace DirectX;

namespace tg::renderer {
namespace {

// 真上・真下でビュー行列が縮退しないよう、わずかに手前で止める。
constexpr float kPitchLimit = 1.55334306f;  // 89 度

// 距離の可動域。Zoom / SetState / Frame で同じ範囲に収める。
// 軌道の距離とクリップ面は、被写体を包む球の半径に合わせて**広げる**。
//
// 基準は素材のプレビュー（2m 平面 = 半径 1.41）で、そのときの値が従来の固定値
//   下限 0.1m / 上限 100m / 近 0.02m / 遠 200m
// になる。**基準より小さい被写体では倍率を 1 で止める**ので、
// 球や 2m 平面の見え方は従来と 1 ピクセルも変わらない。
// 地形（2km 角 = 半径 1448）では 1024 倍に広がり、全体を引きで見られる。
constexpr float kReferenceRadius = 1.41421356f;
constexpr float kMinDistance = 0.1f;
constexpr float kMaxDistance = 100.0f;
constexpr float kNearZ = 0.02f;
constexpr float kFarZ = 200.0f;

// 壊れた値を読み込んだときの歯止め。scene radius に依らない絶対の範囲。
constexpr float kAbsoluteMinDistance = 0.001f;
constexpr float kAbsoluteMaxDistance = 1.0e7f;

// 画角の可動域。SetState / SetFovY で同じ範囲に収める。
constexpr float kMinFovY = 0.2f;
constexpr float kMaxFovY = 1.5f;

// Frame() で被写体の周りに残す余白。1.0 だと画面の端に接する。
constexpr float kFrameMargin = 1.08f;

// ドラッグ 1 ピクセルをホイール何刻みぶんとして扱うか。
// 画面の端から端まで引いて 10 刻みほどになる勘定。
constexpr float kDollyNotchPerPixel = 0.012f;

}  // namespace

void Camera::Orbit(float deltaX, float deltaY) {
    // 右手系では画面の右が +X なので、ヨーは符号を反転する。
    // そうしないと、ドラッグした向きと逆に内容が回る。
    m_yaw -= deltaX;
    m_pitch = std::clamp(m_pitch + deltaY, -kPitchLimit, kPitchLimit);
}

void Camera::Pan(float deltaX, float deltaY) {
    const XMVECTOR forward = XMVectorSet(std::cos(m_pitch) * std::sin(m_yaw), std::sin(m_pitch),
                                         std::cos(m_pitch) * std::cos(m_yaw), 0.0f);
    const XMVECTOR worldUp = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    const XMVECTOR right = XMVector3Normalize(XMVector3Cross(worldUp, forward));
    const XMVECTOR up = XMVector3Cross(forward, right);

    // 距離に比例させ、遠くにいるほど大きく動くようにする。
    const float scale = m_distance * 0.0015f;
    XMVECTOR target = XMLoadFloat3(&m_target);
    target = XMVectorAdd(target, XMVectorScale(right, -deltaX * scale));
    target = XMVectorAdd(target, XMVectorScale(up, deltaY * scale));
    XMStoreFloat3(&m_target, target);
}

void Camera::Zoom(float delta) {
    m_distance = std::clamp(m_distance * std::pow(1.1f, -delta), MinDistance(), MaxDistance());
}

// ズームは距離に対する比で効くので、ホイールと同じ経路へ流す。
// そうしないと、遠くにいるときだけ極端に速くなる。
void Camera::Dolly(float deltaPixels) {
    Zoom(deltaPixels * kDollyNotchPerPixel);
}

void Camera::Focus(const XMFLOAT3& center) {
    m_target = center;
}

// 半径 radius の球がちょうど収まる距離は radius / sin(画角 / 2)。
//
// **縦横のうち画角が狭い方**で決めること。広い方に合わせると、狭い側からはみ出す。
// 横長のビューポートなら縦画角、縦長なら横画角が効く。
void Camera::Frame(const XMFLOAT3& center, float radius) {
    m_target = center;
    if (radius <= 0.0f) {
        return;
    }

    const float aspect = static_cast<float>(m_width) / static_cast<float>(m_height);
    const float fovX = 2.0f * std::atan(std::tan(m_fovY * 0.5f) * aspect);
    const float fov = std::min(m_fovY, fovX);

    const float distance = radius / std::sin(fov * 0.5f);
    m_distance = std::clamp(distance * kFrameMargin, MinDistance(), MaxDistance());
}

void Camera::SetSceneRadius(float radius) {
    // 0 や負の半径は距離もクリップ面も潰すので、下限で受け止める。
    m_sceneRadius = std::max(radius, 0.001f);
}

// 基準（素材のプレビュー）に対する倍率。**1 を下回らせない。**
// 小さい被写体で範囲まで狭めると、従来のプロジェクトの見え方が変わってしまう。
float Camera::SceneScale() const {
    return std::max(1.0f, m_sceneRadius / kReferenceRadius);
}

float Camera::MinDistance() const {
    return kMinDistance * SceneScale();
}

float Camera::MaxDistance() const {
    return kMaxDistance * SceneScale();
}

float Camera::NearZ() const {
    return kNearZ * SceneScale();
}

float Camera::FarZ() const {
    return kFarZ * SceneScale();
}

void Camera::Reset() {
    m_target = {0.0f, 0.0f, 0.0f};
    m_distance = 3.2f;
    m_yaw = 0.6f;
    m_pitch = 0.35f;
}

CameraState Camera::State() const {
    return CameraState{m_target, m_distance, m_yaw, m_pitch, m_fovY};
}

void Camera::SetState(const CameraState& state) {
    m_target = state.target;
    // **ここではシーンの大きさで丸めない。** 被写体の大きさ（平面のサイズなど）を
    // 読み込む前にカメラを復元することがあり、丸めると地形スケールの距離が
    // 素材スケールの上限へ潰れてしまう。歯止めは絶対値の範囲だけにして、
    // シーンに対する丸めは次の Dolly / Frame に任せる。
    m_distance = std::clamp(state.distance, kAbsoluteMinDistance, kAbsoluteMaxDistance);
    m_yaw = state.yaw;
    m_pitch = std::clamp(state.pitch, -kPitchLimit, kPitchLimit);
    m_fovY = std::clamp(state.fovY, kMinFovY, kMaxFovY);
}

void Camera::SetViewportSize(uint32_t width, uint32_t height) {
    m_width = (width > 0) ? width : 1;
    m_height = (height > 0) ? height : 1;
}

void Camera::SetFovY(float fovY) {
    m_fovY = std::clamp(fovY, kMinFovY, kMaxFovY);
}

XMFLOAT3 Camera::Position() const {
    const float x = m_target.x + m_distance * std::cos(m_pitch) * std::sin(m_yaw);
    const float y = m_target.y + m_distance * std::sin(m_pitch);
    const float z = m_target.z + m_distance * std::cos(m_pitch) * std::cos(m_yaw);
    return XMFLOAT3{x, y, z};
}

CameraBasis Camera::Basis() const {
    const XMFLOAT3 position = Position();
    const XMVECTOR eye = XMLoadFloat3(&position);
    const XMVECTOR target = XMLoadFloat3(&m_target);
    const XMVECTOR worldUp = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

    // 右手系なので 右 = 前 × 上。ビュー行列が作る基底と一致する。
    const XMVECTOR forward = XMVector3Normalize(XMVectorSubtract(target, eye));
    const XMVECTOR right = XMVector3Normalize(XMVector3Cross(forward, worldUp));
    const XMVECTOR up = XMVector3Cross(right, forward);

    CameraBasis basis;
    XMStoreFloat3(&basis.right, right);
    XMStoreFloat3(&basis.up, up);
    XMStoreFloat3(&basis.forward, forward);
    return basis;
}

XMMATRIX Camera::ViewMatrix() const {
    const XMFLOAT3 position = Position();
    // 右手系。LH を使うと画面が左右反転し、+X が画面左に出てしまう。
    return XMMatrixLookAtRH(XMLoadFloat3(&position), XMLoadFloat3(&m_target),
                            XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
}

XMMATRIX Camera::ProjectionMatrix() const {
    const float aspect = static_cast<float>(m_width) / static_cast<float>(m_height);
    // ビュー行列と手系を揃える。深度の範囲は LH 版と同じ [0, 1]。
    return XMMatrixPerspectiveFovRH(m_fovY, aspect, NearZ(), FarZ());
}

}  // namespace tg::renderer
