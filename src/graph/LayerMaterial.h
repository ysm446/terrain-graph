#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>
namespace tg::graph {
enum class RoadMaskShape : uint32_t {
    WheelTracks = 0,  // 轍。車線中央 ± タイヤ間隔/2 の帯
    EdgeFalloff = 1,  // 道路端からの距離で減衰
    LengthNoise = 2,  // 長さ方向のノイズをしきい値で切る
    Constant = 3,     // 一様
    WorldNoise = 4,   // ワールド XZ の等方ノイズ（FBM）をしきい値で切る。路肩や地面と地続きの模様
};

enum class RoadMaskSide : uint32_t {
    Both = 0,
    Left = 1,
    Right = 2,
};

struct RoadMaskNodeSettings {
    RoadMaskShape shape = RoadMaskShape::WheelTracks;
    // 轍。車線中央の中心線からの距離、タイヤ間隔、帯の幅、縁のぼかし。
    float laneOffsetMeters = 1.5f;
    float trackSpacingMeters = 1.5f;
    float trackWidthMeters = 0.35f;
    float featherMeters = 0.25f;
    // 真なら Road の車線数から各車線の中央に置く（laneOffsetMeters は使わない）。
    // 旧ファイルにキーが無ければ偽（手入力のまま）。路肩など車線の無い面では手入力に落ちる。
    bool tracksFromLanes = true;
    // 対向車線にも置くか（車線に合わせるとき）。手入力のときは中心線の左右両方に置くか。
    bool bothLanes = true;
    // 端の減衰。端で 1 になる幅と、その内側のぼかし幅。側を選ぶと片側の端だけになる。
    float edgeWidthMeters = 0.3f;
    RoadMaskSide edgeSide = RoadMaskSide::Both;
    // 長さ方向ノイズ。
    float noiseScaleMeters = 4.0f;
    float threshold = 0.5f;
    float softness = 0.2f;
    uint32_t seed = 1u;
    // 長さ方向のムラ（どの形にも掛かる）。0 で一様。
    float breakupAmount = 0.3f;
    float breakupScaleMeters = 3.0f;
    float strength = 1.0f;
    bool invert = false;
};

// レイヤーマテリアル内部の PBR 素材と合成設定。
struct PresetMaterial {
    uint32_t material = 0; // プロジェクトに埋め込まれるPBR素材への参照。0は定数材質。
    float uvRepeatMeters = 2;
    bool worldUv = false;
    std::array<float, 3> baseColor{0.42f, 0.4f, 0.36f};
    float roughness = 0.8f;
    float metallic = 0, ambientOcclusion = 1;
    // 下地には不要。上層で未指定なら被覆0（旧版の未結線スロットも同じ）。
    std::optional<RoadMaskNodeSettings> mask;
    uint32_t blendMode = 0, heightGate = 0;
    float heightGateThreshold = 0.5f, heightGateSoftness = 0.2f;
    bool enabled = true;
};
enum class PresetNodeKind : uint32_t { Material, Mask, Blend, Output };
struct PresetNode {
    uint32_t id = 0;
    PresetNodeKind kind = PresetNodeKind::Material;
    // 合成: 下地・上層素材・マスク。出力: 入力0。0は未接続。
    std::array<uint32_t, 3> inputs{};
    std::array<float, 2> position{};
    PresetMaterial settings;
};
struct PresetGraph {
    uint32_t nextId = 1;
    std::vector<PresetNode> nodes;
};
struct LayerMaterial {
    uint32_t id = 0;
    std::string name;
    float displacementMeters = 0;
    float layerBlendRange = 0.2f;
    std::vector<PresetMaterial> materials;
    std::optional<PresetGraph> materialGraph;
    // 共有アセット（.tglayer）のファイルと固定 ID。未保存・複製直後は空。アンドゥの写しにも入る。
    std::filesystem::path assetPath;
    std::string assetUid;
};

}
