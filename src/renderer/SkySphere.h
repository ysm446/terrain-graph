#pragma once

#include "renderer/Environment.h"
#include "renderer/PreviewRenderer.h"
#include "rhi/Device.h"
#include "rhi/PipelineCache.h"

namespace tg::renderer {

// 適用中の天球を、回せる球で描く（天球プレビューの窓が使う）。
//
// **面を反転した球（天球メッシュ）を外から見た絵。** 手前の面は抜けていて、
// 向こう側の内壁に環境が貼られている。メッシュは持たず、レイと単位球の交点を
// 解析的に解く（見え方は無限に細かい球メッシュと同じ）。遠近も付く。
// 中身は**適用中の環境キューブ**をそのまま引くので、HDRI を読み直さずに毎フレーム描ける。
// 露出とトーンマップはビューポートに合わせる。
class SkySphere {
public:
    void Destroy(rhi::Device& device);

    // 窓が開いている間、**フレームの中で**毎フレーム呼ぶ。
    // 環境がまだ出来ていなければ、何も描かずに透明のままにする。
    void Render(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                ID3D12GraphicsCommandList* commandList, const Environment& environment,
                float iblIntensity, float exposure, TonemapMode tonemap);

    bool HasOutput() const { return m_output.IsValid(); }
    D3D12_GPU_DESCRIPTOR_HANDLE OutputHandle() const { return m_output.srv.gpu; }

    // 向きを回す。ドラッグ量（度）で受ける。**掴んだ面がドラッグに付いてくる**
    // （見えているのは内壁なので、符号は上下左右とも反転させてある）。
    void Orbit(float deltaXDegrees, float deltaYDegrees);
    // 寄る / 引く。ホイールの段で受ける。カメラが球へ近づき、遠近が強くなる。
    void Zoom(float steps);
    void ResetView();

private:
    rhi::GpuTexture m_output;

    float m_yawDegrees = 0.0f;
    float m_pitchDegrees = 0.0f;
    // 既定値は .cpp の kDefaultDistance と揃える（ResetView が入れ直す値）。
    float m_distance = 3.2f;
};

}  // namespace tg::renderer
