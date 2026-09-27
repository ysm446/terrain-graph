#pragma once

#include "compositor/LayerMaterialGpu.h"
#include "compositor/MaterialLayer.h"
#include "renderer/Atmosphere.h"
#include "renderer/Mesh.h"
#include "renderer/ShadowCascades.h"
#include "rhi/Device.h"
#include "rhi/PipelineCache.h"

#include <DirectXMath.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace tg::renderer {

class Environment;

// Mesh Output が描くユニークなメッシュ（Road Mesh の路面など）の 1 つ。
struct GeneratedMeshItem {
    // 同じメッシュを見分ける ID（元のノード）。
    uint64_t id = 0;
    // 形を表す値。前回と違えば GPU へ上げ直す（同じなら geometry は読まない）。
    uint64_t geometryKey = 0;
    const MeshData* geometry = nullptr;
    // 材質（毎フレーム差し替えてよい。形は上げ直さない）。
    bool hasMaterial = false;
    compositor::LayerMaterialGpu material;
    float roadWidthMeters = 0;
    // 材質が無いときの色（リニア）。
    float fallbackColor[3] = {0.18f, 0.18f, 0.18f};
};

// 1 回の描画（本描画か影）で共通の値。
struct GeneratedMeshFrame {
    DirectX::XMFLOAT4X4 viewProjection;
    DirectX::XMFLOAT3 cameraPosition{};
    bool shadow = false;
    const Environment* environment = nullptr;
    float iblIntensity = 1.0f;
    DirectX::XMFLOAT3 lightDirection{0, 1, 0};
    DirectX::XMFLOAT3 lightColor{1, 1, 1};
    float lightIlluminance = 0.0f;
    SceneShadowData shadows;
    AtmosphereSettings atmosphere;
    uint32_t cloudNoiseIndex = 0, atmosphericMode = 0;
    Atmosphere::AmbientBlend ambient;
};

// ユニークなメッシュの GPU 側。形はフレームの外で上げ（Mesh::Create は ExecuteImmediate）、
// 描くのはモデルの配置と同じ所（影の段と本描画）。
class GeneratedMeshes {
public:
    // フレームの外で呼ぶ。形が変わったものだけ上げ直し、無くなったものは遅延解放する。
    void Update(rhi::Device& device, const std::vector<GeneratedMeshItem>& items);
    // 発行した描画の回数を返す。
    uint32_t Draw(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                  ID3D12GraphicsCommandList* commandList, const GeneratedMeshFrame& frame) const;
    void Destroy(rhi::Device& device);
    // 描く量（統計用）。
    uint64_t Vertices() const;
    uint64_t Triangles() const;

private:
    struct Entry {
        uint64_t id = 0;
        uint64_t geometryKey = 0;
        Mesh mesh;
        uint64_t vertices = 0, triangles = 0;
        bool hasMaterial = false;
        compositor::LayerMaterialGpu material;
        float roadWidthMeters = 0;
        float fallbackColor[3] = {0.18f, 0.18f, 0.18f};
    };
    std::vector<std::unique_ptr<Entry>> m_entries;
};

}  // namespace tg::renderer
