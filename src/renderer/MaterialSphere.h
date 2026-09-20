#pragma once

#include "compositor/MaterialLibrary.h"
#include "compositor/TextureLibrary.h"
#include "renderer/Environment.h"
#include "renderer/PreviewRenderer.h"
#include "rhi/Device.h"
#include "rhi/PipelineCache.h"

namespace tg::renderer {

// マテリアル 1 つを、回せる球で描く（マテリアルプレビューの窓が使う）。
//
// **照らし方はビューポートに合わせる**（適用中の天球の IBL + 太陽 + 露出 + トーンマップ）。
// 素材が本番の環境でどう見えるかを確かめるためのもので、一覧のサムネイルとは目的が違う
// （あちらは見比べるための固定 2 灯で、正面から見た円板）。
class MaterialSphere {
public:
    // 画角。**素材を見るための望遠寄り**にしてある（.cpp のレイ生成と共有する）。
    static constexpr float kFovYDegrees = 30.0f;

    void Destroy(rhi::Device& device);

    // 窓が開いている間、**フレームの中で**毎フレーム呼ぶ。
    // 出力は ImGui が読むので、ピクセルシェーダ可視の状態で置いて返す。
    void Render(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                ID3D12GraphicsCommandList* commandList, const compositor::MaterialAsset& asset,
                const compositor::TextureLibrary& textures, const Environment& environment,
                float iblIntensity, const LightSettings& light, float exposure,
                TonemapMode tonemap);

    D3D12_GPU_DESCRIPTOR_HANDLE MaskHandle() const { return m_masks.srv.gpu; }
    bool HasMasks() const { return m_masks.IsValid(); }
    int Shape() const { return m_shape; }
    void SetShape(int shape) { m_shape = shape; ResetView(); }
    void RotateLight(float x, float y);
    void ResetLight() { m_lightAzimuthOffset = 0; m_lightElevationOffset = 0; }
    bool HasOutput() const { return m_output.IsValid(); }
    D3D12_GPU_DESCRIPTOR_HANDLE OutputHandle() const { return m_output.srv.gpu; }

    // 軌道の操作。ドラッグ量（度）とホイールの段で動かす。
    // **符号はビューポートのカメラと同じ**（右へ引けば右へ回る）。
    void Orbit(float deltaXDegrees, float deltaYDegrees);
    void Zoom(float steps);
    // 正面・既定の距離へ戻す。窓を開いた直後と「戻す」ボタンで使う。
    void ResetView();

    // 重ねて描くギズモのための視点と光源。シェーダのレイ生成と同じ値を返す。
    // 球・平面はどちらも原点まわりの半径 1 なので、ギズモの半径も 1 でよい。
    DirectX::XMFLOAT3 CameraPosition() const;
    // シーンの光に、この窓だけの回し量（L＋ドラッグ）を足したもの。
    LightSettings PreviewLight(const LightSettings& scene) const;

    // プレビューに映す長さ（m）。**平面なら一辺、球なら直径。**
    // 素材を実寸で見るための表示設定で、マテリアル自体には保存しない。
    bool& ShowDisplacement() { return m_showDisplacement; }
    // 変位した面が自分に落とす影。太陽光にだけ効く（変位を出していないときは無視）。
    bool& CastShadow() { return m_castShadow; }
    float& DisplacementMeters() { return m_displacementMeters; }
    float& LengthMeters() { return m_lengthMeters; }

private:
    rhi::GpuTexture m_output;
    rhi::GpuTexture m_masks;
    rhi::GpuTexture m_heightField;
    float m_displacementMeters = 0.1f;
    bool m_showDisplacement = true;
    bool m_castShadow = true;
    // 既定は平面。**模様の実寸とタイリングを読むのが主目的**なので、
    // 丸みの見え方を確かめる球よりこちらを先に出す。
    int m_shape = 1;
    float m_lightAzimuthOffset = 0;
    float m_lightElevationOffset = 0;

    float m_yawDegrees = 0.0f;
    float m_pitchDegrees = 40.0f;  // 既定の平面を見下ろす角
    // 既定値は .cpp の kDefault* と揃える（ResetView が入れ直す値）。
    float m_distance = 4.5f;
    float m_lengthMeters = 2.0f;
};

}  // namespace tg::renderer
