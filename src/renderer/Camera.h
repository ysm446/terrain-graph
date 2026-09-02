#pragma once

#include <DirectXMath.h>

#include <cmath>
#include <cstdint>

namespace tg::renderer {

// カメラのビュー空間の基底をワールド座標で表したもの。
// 座標軸ギズモのように「向きだけ」が要る用途に使う。
struct CameraBasis {
    DirectX::XMFLOAT3 right;    // 画面の右
    DirectX::XMFLOAT3 up;       // 画面の上
    DirectX::XMFLOAT3 forward;  // 画面の奥
};

// 画角は内部ではラジアンで持つが、UI では焦点距離 (mm) で扱う。
// 露出を絞り / シャッター / ISO で決めているので、レンズも同じ言葉に揃える。
//
// 換算の基準は 35mm フルサイズ（36 x 24 mm）。縦画角なのでセンサーの高さを使う。
inline constexpr float kSensorHeightMm = 24.0f;

inline float FocalLengthFromFovY(float fovY) {
    return (kSensorHeightMm * 0.5f) / std::tan(fovY * 0.5f);
}

inline float FovYFromFocalLength(float focalLengthMm) {
    return 2.0f * std::atan((kSensorHeightMm * 0.5f) / focalLengthMm);
}

// カメラの状態。プロジェクトの保存と読み込みで丸ごと出し入れする。
struct CameraState {
    DirectX::XMFLOAT3 target = {0.0f, 0.0f, 0.0f};
    float distance = 3.2f;
    float yaw = 0.6f;
    float pitch = 0.35f;
    float fovY = 0.7853981634f;
};

// 注視点を中心に回る軌道カメラ。マテリアルプレビューではこれで十分。
//
// 座標系は**右手系 Y-up**。X が右、Y が上、Z が手前（画面から見て奥が -Z）。
// 詳細は docs/design/rendering.md の「座標系」を参照。
class Camera {
public:
    // 画面上のドラッグ量で視点を回す。ドラッグした向きに内容が付いてくる。
    void Orbit(float deltaX, float deltaY);
    void Pan(float deltaX, float deltaY);
    // ホイールの刻み単位でズームする。正で寄る。
    void Zoom(float delta);
    // ドラッグ量（ピクセル）でズームする。右へ引くと寄る（Alt + 右ドラッグ）。
    void Dolly(float deltaPixels);
    void Reset();

    // 注視点を center へ戻す。距離と角度は変えない（F キー）。
    // パンで被写体を画面外へ追い出したときの復帰に使う。
    void Focus(const DirectX::XMFLOAT3& center);
    // 注視点を center へ戻したうえで、半径 radius の球が画面に収まる距離へ寄せる（A キー）。
    void Frame(const DirectX::XMFLOAT3& center, float radius);

    void SetViewportSize(uint32_t width, uint32_t height);

    // 被写体を包む球の半径（m）を渡す。**毎フレーム呼んでよい。**
    //
    // 軌道の距離の下限 / 上限とクリップ面をここから決める。素材の 2m 角と
    // 地形の 2km 角では 1000 倍違うので、固定値だと片方でしか使えない
    // （2km の地形は「100m まで」の上限に阻まれて全体が見えない）。
    // 既定（半径 1.41 = 2m 平面の対角の半分）で従来と同じ値になるようにしてある。
    void SetSceneRadius(float radius);

    DirectX::XMMATRIX ViewMatrix() const;
    DirectX::XMMATRIX ProjectionMatrix() const;
    DirectX::XMFLOAT3 Position() const;
    CameraBasis Basis() const;

    CameraState State() const;
    void SetState(const CameraState& state);

    float FovY() const { return m_fovY; }
    // 被写界深度が深度からカメラ前方距離を戻すのに使う。
    // **被写体の大きさに比例する。** 近 / 遠の比は一定なので、
    // 地形スケールでも深度の精度は素材スケールと変わらない。
    float NearZ() const;
    float FarZ() const;
    // SetState と同じ範囲に丸める。UI からの直接代入でクランプを迂回させない
    // （0 や負の画角は投影行列と焦点距離換算のゼロ除算を壊す）。
    void SetFovY(float fovY);
    const DirectX::XMFLOAT3& Target() const { return m_target; }

private:
    // 被写体の大きさから決まる軌道の距離の範囲。
    float SceneScale() const;
    float MinDistance() const;
    float MaxDistance() const;

    DirectX::XMFLOAT3 m_target = {0.0f, 0.0f, 0.0f};
    float m_distance = 3.2f;
    float m_yaw = 0.6f;
    float m_pitch = 0.35f;
    float m_fovY = 0.7853981634f;  // 45 度
    // 被写体の大きさ。SetSceneRadius が入れる。既定は 2m 平面の対角の半分。
    float m_sceneRadius = 1.41421356f;
    uint32_t m_width = 1;
    uint32_t m_height = 1;
};

}  // namespace tg::renderer
