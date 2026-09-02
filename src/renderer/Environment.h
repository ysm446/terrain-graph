#pragma once

#include "rhi/Device.h"
#include "rhi/PipelineCache.h"

#include <DirectXMath.h>

#include <filesystem>
#include <string>

namespace tg::renderer {

// 手続き的な空の設定。単位は cd/m^2 相当。
// 太陽はディレクショナルライトで別に扱うため、ここには入れない
// （環境マップに入れると二重計上になり、解像度の都合でエイリアスも出る）。
struct SkySettings {
    DirectX::XMFLOAT3 zenithColor = {0.20f, 0.36f, 0.78f};
    DirectX::XMFLOAT3 horizonColor = {0.70f, 0.80f, 0.95f};
    // 地面は日射を受けて反射している想定。暗くしすぎると日中の露出で黒く潰れる。
    DirectX::XMFLOAT3 groundColor = {0.45f, 0.42f, 0.38f};
    // 晴天の空はおよそ 4000-15000 cd/m^2。日中の露出で見て自然になる値にしている。
    float intensity = 12000.0f;
};

// HDRI の生の値を cd/m^2 へ直す倍率。`skyLuminance` は「この HDRI の空を
// 何 cd/m^2 とみなすか」、`measuredSky` は画像から測った空の代表輝度。
// **環境本体とサムネイルで同じ倍率を使うため、ここに置いてある。**
float SkyLuminanceScale(float skyLuminance, float measuredSky);

// IBL 用の環境一式。
//   equirect → キューブマップ（ミップ付き）
//            → irradiance キューブ（拡散）
//            → プリフィルタ済みキューブ（鏡面、ラフネス別ミップ）
//   + 環境 BRDF の LUT
//
// 生成はすべてコンピュートで行い、Device::ExecuteImmediate でその場で完了させる。
class Environment {
public:
    bool Initialize(rhi::Device& device, rhi::PipelineCache& pipelineCache);
    void Shutdown(rhi::Device& device);

    // 手続き的な空から作り直す。
    bool BuildFromSky(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                      const SkySettings& sky);

    // Radiance HDR (.hdr) から作り直す。
    //
    // skyLuminance は**この HDRI の空を何 cd/m^2 とみなすか**（晴天でおよそ 1 万）。
    //
    // HDRI は絶対輝度で較正されていない（`EXPOSURE=` ヘッダを持つものは稀で、
    // 合成ツールが出すものはまず持たない）。画像内の比は本物なので、定数倍しても
    // 物理的な関係は壊れない。**基準だけが無い**ので、ここで与える。
    //
    // 倍率を直接指定させないのは、ファイルによって空の値が違うため
    // （実測で 0.15〜0.2 のものもあれば 1.0 のものもある）。
    // 「空が何 cd/m^2 か」なら、どのファイルでも同じ意味になる。
    bool BuildFromHdrFile(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                          const std::filesystem::path& path, float skyLuminance);

    // 目標輝度だけを変えて作り直す。読み込んだ equirect をそのまま使うので、
    // ファイルを読み直さない（スライダーを動かしても速い）。
    bool RebuildWithSkyLuminance(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                                 float skyLuminance);

    // 読み込んだ HDRI の空の代表輝度（ファイルの生の値）。較正の分母。
    float MeasuredSkyLuminance() const { return m_measuredSkyLuminance; }

    bool IsReady() const { return m_ready; }

    uint32_t EnvironmentSrvIndex() const { return m_cube.SrvIndex(); }
    uint32_t IrradianceSrvIndex() const { return m_irradiance.SrvIndex(); }
    uint32_t PrefilteredSrvIndex() const { return m_prefiltered.SrvIndex(); }
    uint32_t BrdfLutSrvIndex() const { return m_brdfLut.SrvIndex(); }
    uint32_t PrefilteredMipCount() const { return m_prefiltered.mipLevels; }

    const std::string& SourceName() const { return m_sourceName; }
    uint32_t EquirectWidth() const { return m_equirect.width; }
    uint32_t EquirectHeight() const { return m_equirect.height; }

private:
    // equirect からキューブ以降を作り直す。luminanceScale はそのとき掛ける倍率。
    bool BuildFromEquirect(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                           float luminanceScale);
    bool CreateTargets(rhi::Device& device, uint32_t equirectWidth, uint32_t equirectHeight);
    void ReleaseTargets(rhi::Device& device);
    bool BuildBrdfLut(rhi::Device& device, rhi::PipelineCache& pipelineCache);

    // equirect が書き込まれた状態から、キューブ・irradiance・プリフィルタを作る。

    rhi::GpuTexture m_equirect;
    rhi::GpuTexture m_cube;
    rhi::GpuTexture m_irradiance;
    rhi::GpuTexture m_prefiltered;
    rhi::GpuTexture m_brdfLut;

    // 読み込んだ HDRI の空の代表輝度。較正の分母。手続き的な空では使わない。
    float m_measuredSkyLuminance = 0.0f;
    std::string m_sourceName = "手続き的な空";
    bool m_ready = false;
};

}  // namespace tg::renderer
