#pragma once

#include "renderer/Environment.h"
#include "renderer/PreviewRenderer.h"
#include "rhi/Device.h"
#include "rhi/PipelineCache.h"

namespace tg::renderer {

// 適用中の天球を、回せる球で描く（天球プレビューの窓が使う）。
//
// **裏返した球**（内側に環境が貼られた玉）で、一覧のサムネイルと同じ見え方。
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

    // 向きを回す。ドラッグ量（度）で受ける。**絵はドラッグに付いてくる**
    // （右へ引けば中身が右へ動く）。マテリアルの球と同じ手触り。
    void Orbit(float deltaXDegrees, float deltaYDegrees);
    // 寄る / 引く。ホイールの段で受ける。寄ると球が枠からはみ出す。
    void Zoom(float steps);
    void ResetView();

private:
    rhi::GpuTexture m_output;

    float m_yawDegrees = 0.0f;
    float m_pitchDegrees = 0.0f;
    float m_zoom = 1.0f;
};

}  // namespace tg::renderer
