#include "io/ProjectIo.h"

#include "core/PathUtf8.h"

#include "core/ImageIo.h"
#include "core/Log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace tg::io {
namespace {

namespace fs = std::filesystem;
using nlohmann::json;

constexpr const char* kProjectFormat = "terrain-graph.project";
constexpr const char* kMaterialFormat = "terrain-graph.material";
// 形式を変えたら上げる。読み込み側は「これ以下なら読める」として扱う。
//
// 2: ハイトに gain を追加し、base の意味を変えた（h = base + (src - 0.5) * gain）。
//    キーが増えただけに見えるが base の解釈が変わっているので、古いビルドに
//    読ませると黙って違う絵が出る。それを断れるように版を上げている。
// 3: レイヤーに kind（surface / shape / liquid）を追加した。古いビルドはキーを
//    無視してシェイプや水面をサーフェスとして合成し、黙って違う絵を出すため版を上げる。
//    kind の無い旧ファイルは全レイヤーをサーフェスとして読む。
// プロジェクトの版。4 で `layers` 節を廃止し、グラフ (`graph`) を唯一の合成にした
// （旧ファイルの layers はグラフへ移行して読む）。
constexpr int kProjectFormatVersion = 4;
// マテリアル単体 (.tgmat) の版。中身は変わっていないので 3 のまま。
constexpr int kMaterialFormatVersion = 3;

// --- 文字列とパス ---------------------------------------------------------
//
// JSON は UTF-8。変換は core/PathUtf8.h に一本化してある。
// 保存する文字列は区切りを '/' に揃える（ToUtf8Portable）。

// baseDir から見た相対パスにする。ドライブが違うなど relative が使えないときは
// 絶対パスのまま書く。プロジェクトごと移動しても壊れないようにするため。
std::string RelativePathString(const fs::path& target, const fs::path& baseDir) {
    if (target.empty()) {
        return {};
    }
    std::error_code error;
    const fs::path absolute = fs::absolute(target, error);
    const fs::path& source = error ? target : absolute;

    std::error_code relativeError;
    const fs::path relative = fs::relative(source, baseDir, relativeError);
    if (relativeError || relative.empty()) {
        return ToUtf8Portable(source);
    }
    return ToUtf8Portable(relative);
}

// 相対パスなら baseDir から解決する。絶対パスならそのまま。
fs::path ResolvePath(const std::string& text, const fs::path& baseDir) {
    if (text.empty()) {
        return {};
    }
    const fs::path path = FromUtf8(text);
    if (path.is_absolute()) {
        return path;
    }
    return (baseDir / path).lexically_normal();
}

// --- JSON の読み書き（例外を投げない） ------------------------------------
//
// 型が食い違っていたら既定値に落とす。手で編集されたファイルでも落ちないようにする。

const json* FindMember(const json& node, const char* key) {
    const auto it = node.find(key);
    return (it != node.end()) ? &(*it) : nullptr;
}

float ReadFloat(const json& node, const char* key, float fallback) {
    const json* member = FindMember(node, key);
    return (member != nullptr && member->is_number()) ? member->get<float>() : fallback;
}

int ReadInt(const json& node, const char* key, int fallback) {
    const json* member = FindMember(node, key);
    return (member != nullptr && member->is_number_integer()) ? member->get<int>() : fallback;
}

uint32_t ReadUInt(const json& node, const char* key, uint32_t fallback) {
    const json* member = FindMember(node, key);
    if (member == nullptr || !member->is_number_integer()) {
        return fallback;
    }
    const int64_t value = member->get<int64_t>();
    return (value < 0) ? fallback : static_cast<uint32_t>(value);
}

bool ReadBool(const json& node, const char* key, bool fallback) {
    const json* member = FindMember(node, key);
    return (member != nullptr && member->is_boolean()) ? member->get<bool>() : fallback;
}

std::string ReadString(const json& node, const char* key, const std::string& fallback = {}) {
    const json* member = FindMember(node, key);
    return (member != nullptr && member->is_string()) ? member->get<std::string>() : fallback;
}

json WriteFloat3(const DirectX::XMFLOAT3& value) {
    return json::array({value.x, value.y, value.z});
}

DirectX::XMFLOAT3 ReadFloat3(const json& node, const char* key,
                             const DirectX::XMFLOAT3& fallback) {
    const json* member = FindMember(node, key);
    if (member == nullptr || !member->is_array() || member->size() != 3) {
        return fallback;
    }
    DirectX::XMFLOAT3 value = fallback;
    float* components[3] = {&value.x, &value.y, &value.z};
    for (size_t i = 0; i < 3; ++i) {
        const json& element = (*member)[i];
        if (element.is_number()) {
            *components[i] = element.get<float>();
        }
    }
    return value;
}

// --- 列挙 -----------------------------------------------------------------
//
// 数値ではなく名前で書く。ファイルを直接読んだときに意味が分かるようにするため。
// 名前の並びは enum の値と一致させること。

const char* const kTextureChannelNames[] = {"r", "g", "b", "a"};
const char* const kValueSourceNames[] = {"constant", "noise", "texture"};
const char* const kNoiseTypeNames[] = {"fbm",    "ridged", "worley",
                                      "perlin", "billow", "cracks"};
// マスクのソース。`node` はグラフのマスクノードの結果を指す
// （どのノードかはグラフの繋ぎ方から決まるので、ここには書かない）。
const char* const kMaskSourceNames[] = {"constant", "noise",     "texture", "height",
                                        "slope",    "curvature", "cavity",  "paint",
                                        "node"};
const char* const kFluvialCurveNames[] = {"log", "threshold", "linear"};
// 曲率マスクの向き。compositor::CurvatureMode の並びと一致させること。
const char* const kCurvatureModeNames[] = {"ridges", "valleys", "absolute"};
const char* const kMaskBlendModeNames[] = {"add", "multiply", "min", "max", "subtract"};
const char* const kChannelNames[] = {"baseColor", "normal", "surface", "height"};
const char* const kLayerKindNames[] = {"surface",   "shape", "liquid", "blur",    "sediment",
                                       "crumbling", "snow",  "river",  "droplet", "scatter", "multiScaleErosion", "fluvialErosion", "flattenBorders", "snowCover", "lake", "meanderingRivers"};
// 散布の形 / 向き。compositor::ScatterShape / ScatterOrientation の並びと一致させること。
const char* const kScatterShapeNames[] = {"hemisphere", "cone"};
const char* const kScatterOrientationNames[] = {"flat", "followGround", "slopeOriented"};
// 岩片の形。compositor::RockStyle の並びと一致させること。
const char* const kRockStyleNames[] = {"classic", "polygonal", "shard"};
const char* const kTonemapNames[] = {"none", "reinhard", "aces"};
const char* const kSkySourceNames[] = {"procedural", "hdri"};
const char* const kCloudNoiseNames[] = {"perlinFbm", "perlinWorley"};
const char* const kCloudReplicationNames[] = {"count", "surfaceDensity", "volumeDensity"};
const char* const kProceduralCloudNoiseNames[] = {"perlin", "perlinFbm", "perlinWorley"};
const char* const kApertureShapeNames[] = {"circle", "triangle", "hexagon", "octagon"};

template <size_t N>
const char* EnumName(const char* const (&names)[N], uint32_t value) {
    return (value < N) ? names[value] : names[0];
}

template <size_t N>
uint32_t EnumValue(const char* const (&names)[N], const json& node, const char* key,
                   uint32_t fallback) {
    const json* member = FindMember(node, key);
    if (member == nullptr || !member->is_string()) {
        return fallback;
    }
    const std::string text = member->get<std::string>();
    for (uint32_t i = 0; i < N; ++i) {
        if (text == names[i]) {
            return i;
        }
    }
    return fallback;
}

// --- テクスチャ参照 -------------------------------------------------------
//
// 参照の書き方は用途で変わる。
//   プロジェクト:   textures 配列の通し番号
//   マテリアル単体: 画像ファイルへの相対パス
// どちらも「参照が無い」は null で表す。

using TextureWriter = std::function<json(compositor::TextureId)>;
using TextureReader = std::function<compositor::TextureId(const json&)>;

json WriteMapSlot(const compositor::MapSlot& slot, const TextureWriter& writeTexture) {
    json node;
    node["texture"] = writeTexture(slot.texture);
    node["channel"] = EnumName(kTextureChannelNames, static_cast<uint32_t>(slot.channel));
    return node;
}

compositor::MapSlot ReadMapSlot(const json& node, const char* key,
                                const TextureReader& readTexture) {
    compositor::MapSlot slot;
    const json* member = FindMember(node, key);
    if (member == nullptr || !member->is_object()) {
        return slot;
    }
    const json* texture = FindMember(*member, "texture");
    slot.texture = (texture != nullptr) ? readTexture(*texture) : compositor::kNoTexture;
    slot.channel = static_cast<compositor::TextureChannel>(
        EnumValue(kTextureChannelNames, *member, "channel", 0));
    return slot;
}

// --- ノイズ ---------------------------------------------------------------

json WriteNoise(const compositor::NoiseParams& noise) {
    json node;
    node["type"] = EnumName(kNoiseTypeNames, static_cast<uint32_t>(noise.type));
    node["scale"] = noise.scale;
    node["amount"] = noise.amount;
    node["octaves"] = noise.octaves;
    node["offset"] = noise.offset;
    return node;
}

compositor::NoiseParams ReadNoise(const json& node, const char* key,
                                  const compositor::NoiseParams& fallback) {
    const json* member = FindMember(node, key);
    if (member == nullptr || !member->is_object()) {
        return fallback;
    }
    compositor::NoiseParams noise;
    noise.type = static_cast<compositor::NoiseType>(
        EnumValue(kNoiseTypeNames, *member, "type", static_cast<uint32_t>(fallback.type)));
    noise.scale = ReadFloat(*member, "scale", fallback.scale);
    noise.amount = ReadFloat(*member, "amount", fallback.amount);
    noise.octaves = ReadInt(*member, "octaves", fallback.octaves);
    noise.offset = ReadFloat(*member, "offset", fallback.offset);
    return noise;
}

// --- マテリアル -----------------------------------------------------------
//
// プロジェクトへの埋め込みと .tgmat で同じ形を使う。違うのはテクスチャ参照の書き方だけ。

json WriteMaterialBody(const compositor::MaterialAsset& asset, const TextureWriter& writeTexture) {
    json node;
    node["name"] = asset.name;
    node["baseColorTint"] = WriteFloat3(asset.baseColorTint);
    node["hueShift"] = asset.hueShiftDegrees;
    node["saturation"] = asset.saturation;
    node["roughness"] = asset.roughnessValue;
    node["metallic"] = asset.metallicValue;
    node["ambientOcclusion"] = asset.ambientOcclusionValue;

    json maps;
    maps["baseColor"] = writeTexture(asset.baseColor);
    maps["normal"] = writeTexture(asset.normal);
    // 法線マップの規約（緑の向き）。既定は OpenGL（反転して読む）。
    node["flipNormalGreen"] = asset.flipNormalGreen;
    maps["roughness"] = WriteMapSlot(asset.roughness, writeTexture);
    maps["metallic"] = WriteMapSlot(asset.metallic, writeTexture);
    maps["ambientOcclusion"] = WriteMapSlot(asset.ambientOcclusion, writeTexture);
    maps["height"] = WriteMapSlot(asset.height, writeTexture);
    node["maps"] = std::move(maps);
    return node;
}

void ReadMaterialBody(const json& node, compositor::MaterialAsset& asset,
                      const TextureReader& readTexture) {
    const compositor::MaterialAsset defaults;
    asset.name = ReadString(node, "name", defaults.name);
    asset.baseColorTint = ReadFloat3(node, "baseColorTint", defaults.baseColorTint);
    asset.hueShiftDegrees = ReadFloat(node, "hueShift", defaults.hueShiftDegrees);
    asset.saturation = ReadFloat(node, "saturation", defaults.saturation);
    asset.roughnessValue = ReadFloat(node, "roughness", defaults.roughnessValue);
    asset.metallicValue = ReadFloat(node, "metallic", defaults.metallicValue);
    asset.ambientOcclusionValue =
        ReadFloat(node, "ambientOcclusion", defaults.ambientOcclusionValue);

    const json* maps = FindMember(node, "maps");
    if (maps == nullptr || !maps->is_object()) {
        return;
    }
    const json* baseColor = FindMember(*maps, "baseColor");
    asset.baseColor = (baseColor != nullptr) ? readTexture(*baseColor) : compositor::kNoTexture;
    asset.flipNormalGreen = ReadBool(node, "flipNormalGreen", defaults.flipNormalGreen);
    const json* normal = FindMember(*maps, "normal");
    asset.normal = (normal != nullptr) ? readTexture(*normal) : compositor::kNoTexture;
    asset.roughness = ReadMapSlot(*maps, "roughness", readTexture);
    asset.metallic = ReadMapSlot(*maps, "metallic", readTexture);
    asset.ambientOcclusion = ReadMapSlot(*maps, "ambientOcclusion", readTexture);
    asset.height = ReadMapSlot(*maps, "height", readTexture);
}

// --- レイヤー -------------------------------------------------------------

json WriteChannelMask(uint32_t channelMask) {
    json channels = json::array();
    for (uint32_t i = 0; i < static_cast<uint32_t>(compositor::Channel::Count); ++i) {
        if ((channelMask & (1u << i)) != 0) {
            channels.push_back(kChannelNames[i]);
        }
    }
    return channels;
}

uint32_t ReadChannelMask(const json& node, const char* key, uint32_t fallback) {
    const json* member = FindMember(node, key);
    if (member == nullptr || !member->is_array()) {
        return fallback;
    }
    uint32_t mask = 0;
    for (const json& element : *member) {
        if (!element.is_string()) {
            continue;
        }
        const std::string text = element.get<std::string>();
        for (uint32_t i = 0; i < static_cast<uint32_t>(compositor::Channel::Count); ++i) {
            if (text == kChannelNames[i]) {
                mask |= (1u << i);
            }
        }
    }
    return mask;
}

json WriteFlowline(const compositor::FlowlineParams& params) {
    json value;
    value["scatteringMode"] = params.scatteringMode;
    value["numberOfFlows"] = params.numberOfFlows;
    value["density"] = params.density;
    value["lengthMeters"] = params.lengthMeters;
    value["friction"] = params.friction;
    value["reflectVelocity"] = params.reflectVelocity;
    value["reflectionAmount"] = params.reflectionAmount;
    value["allowPooling"] = params.allowPooling;
    value["strength"] = params.strength;
    value["volume"] = params.volume;
    value["talusAngle"] = params.talusAngle;
    value["talusFalloff"] = params.talusFalloff;
    value["clampSource"] = params.clampSource;
    value["showOutflow"] = params.showOutflow;
    value["normalize"] = params.normalize;
    return value;
}

compositor::FlowlineParams ReadFlowline(const json& parent) {
    compositor::FlowlineParams params;
    const auto* value = FindMember(parent, "flowline");
    if (!value || !value->is_object()) return params;
    params.scatteringMode = std::clamp(ReadInt(*value, "scatteringMode", params.scatteringMode), 0, 1);
    params.numberOfFlows = std::clamp(ReadInt(*value, "numberOfFlows", params.numberOfFlows), 1, 1000000);
    params.density = std::clamp(ReadFloat(*value, "density", params.density), 0.0f, 1.0f);
    params.lengthMeters = std::clamp(ReadFloat(*value, "lengthMeters", params.lengthMeters), 0.0f, 1024.0f);
    params.friction = std::clamp(ReadFloat(*value, "friction", params.friction), 0.01f, 1.0f);
    params.reflectVelocity = ReadBool(*value, "reflectVelocity", params.reflectVelocity);
    params.reflectionAmount = std::clamp(ReadFloat(*value, "reflectionAmount", params.reflectionAmount), 0.0f, 1.0f);
    params.allowPooling = ReadBool(*value, "allowPooling", params.allowPooling);
    params.strength = std::clamp(ReadFloat(*value, "strength", params.strength), 0.0f, 1.0f);
    params.volume = std::clamp(ReadFloat(*value, "volume", params.volume), 0.0f, 1.0f);
    params.talusAngle = std::clamp(ReadFloat(*value, "talusAngle", params.talusAngle), 0.0f, 90.0f);
    params.talusFalloff = std::clamp(ReadFloat(*value, "talusFalloff", params.talusFalloff), 0.0f, 90.0f);
    params.clampSource = ReadBool(*value, "clampSource", params.clampSource);
    params.showOutflow = ReadBool(*value, "showOutflow", params.showOutflow);
    params.normalize = ReadBool(*value, "normalize", params.normalize);
    return params;
}

json WriteFluvial(const compositor::FluvialParams& fluvial) {
    json node;
    node["curve"] = EnumName(kFluvialCurveNames, static_cast<uint32_t>(fluvial.curve));
    node["threshold"] = fluvial.threshold;
    node["gamma"] = fluvial.gamma;
    node["softness"] = fluvial.softness;
    node["edgePower"] = fluvial.edgePower;
    node["detail"] = fluvial.detailMeters;
    node["concentration"] = fluvial.concentration;
    node["resolution"] = fluvial.resolution;
    return node;
}

compositor::FluvialParams ReadFluvial(const json& parent, const char* key) {
    const compositor::FluvialParams defaults;
    const json* node = FindMember(parent, key);
    if (node == nullptr || !node->is_object()) {
        return defaults;
    }
    compositor::FluvialParams fluvial;
    fluvial.curve = static_cast<compositor::FluvialCurve>(
        EnumValue(kFluvialCurveNames, *node, "curve", static_cast<uint32_t>(defaults.curve)));
    fluvial.threshold = ReadFloat(*node, "threshold", defaults.threshold);
    fluvial.gamma = ReadFloat(*node, "gamma", defaults.gamma);
    fluvial.softness = ReadFloat(*node, "softness", defaults.softness);
    fluvial.edgePower = ReadFloat(*node, "edgePower", defaults.edgePower);
    fluvial.detailMeters = ReadFloat(*node, "detail", defaults.detailMeters);
    fluvial.concentration = ReadFloat(*node, "concentration", defaults.concentration);
    fluvial.resolution =
        static_cast<uint32_t>(ReadInt(*node, "resolution", static_cast<int>(defaults.resolution)));
    return fluvial;
}

json WriteHeightMask(const compositor::HeightParams& height) {
    json node;
    node["fullRange"] = height.useFullRange;
    node["min"] = height.minMeters;
    node["max"] = height.maxMeters;
    node["feather"] = height.featherMeters;
    node["gamma"] = height.gamma;
    node["invert"] = height.invert;
    return node;
}

compositor::HeightParams ReadHeightMask(const json& parent, const char* key) {
    const compositor::HeightParams defaults;
    const json* node = FindMember(parent, key);
    if (node == nullptr || !node->is_object()) {
        return defaults;
    }
    compositor::HeightParams height;
    height.useFullRange = ReadBool(*node, "fullRange", defaults.useFullRange);
    height.minMeters = ReadFloat(*node, "min", defaults.minMeters);
    height.maxMeters = ReadFloat(*node, "max", defaults.maxMeters);
    height.featherMeters = ReadFloat(*node, "feather", defaults.featherMeters);
    height.gamma = ReadFloat(*node, "gamma", defaults.gamma);
    height.invert = ReadBool(*node, "invert", defaults.invert);
    return height;
}

json WriteSlope(const compositor::SlopeParams& slope) {
    json node;
    node["detail"] = slope.detailMeters;
    node["min"] = slope.minDegrees;
    node["max"] = slope.maxDegrees;
    node["gamma"] = slope.gamma;
    node["invert"] = slope.invert;
    return node;
}

json WriteCurvature(const compositor::CurvatureParams& curvature) {
    json node;
    node["mode"] = EnumName(kCurvatureModeNames, static_cast<uint32_t>(curvature.mode));
    node["detail"] = curvature.detailMeters;
    node["sensitivity"] = curvature.sensitivityMeters;
    node["threshold"] = curvature.threshold;
    node["gamma"] = curvature.gamma;
    return node;
}

compositor::CurvatureParams ReadCurvature(const json& parent, const char* key) {
    const compositor::CurvatureParams defaults;
    const json* node = FindMember(parent, key);
    if (node == nullptr || !node->is_object()) {
        return defaults;
    }
    compositor::CurvatureParams curvature;
    curvature.mode = static_cast<compositor::CurvatureMode>(
        EnumValue(kCurvatureModeNames, *node, "mode", static_cast<uint32_t>(defaults.mode)));
    curvature.detailMeters = ReadFloat(*node, "detail", defaults.detailMeters);
    curvature.sensitivityMeters = ReadFloat(*node, "sensitivity", defaults.sensitivityMeters);
    curvature.threshold = ReadFloat(*node, "threshold", defaults.threshold);
    curvature.gamma = ReadFloat(*node, "gamma", defaults.gamma);
    return curvature;
}

compositor::SlopeParams ReadSlope(const json& parent, const char* key) {
    const compositor::SlopeParams defaults;
    const json* node = FindMember(parent, key);
    if (node == nullptr || !node->is_object()) {
        return defaults;
    }
    compositor::SlopeParams slope;
    slope.detailMeters = ReadFloat(*node, "detail", defaults.detailMeters);
    slope.minDegrees = ReadFloat(*node, "min", defaults.minDegrees);
    slope.maxDegrees = ReadFloat(*node, "max", defaults.maxDegrees);
    slope.gamma = ReadFloat(*node, "gamma", defaults.gamma);
    slope.invert = ReadBool(*node, "invert", defaults.invert);
    return slope;
}

json WriteLevels(const compositor::LevelsParams& levels) {
    json node;
    node["black"] = levels.blackPoint;
    node["white"] = levels.whitePoint;
    node["gamma"] = levels.gamma;
    node["invert"] = levels.invert;
    return node;
}

compositor::LevelsParams ReadLevels(const json& parent, const char* key) {
    const compositor::LevelsParams defaults;
    const json* node = FindMember(parent, key);
    if (node == nullptr || !node->is_object()) {
        return defaults;
    }
    compositor::LevelsParams levels;
    levels.blackPoint = ReadFloat(*node, "black", defaults.blackPoint);
    levels.whitePoint = ReadFloat(*node, "white", defaults.whitePoint);
    levels.gamma = ReadFloat(*node, "gamma", defaults.gamma);
    levels.invert = ReadBool(*node, "invert", defaults.invert);
    return levels;
}

json WriteMaskBlur(const compositor::MaskBlurParams& blur) {
    json node = json::object();
    node["radius"] = blur.radiusMeters;
    node["strength"] = blur.strength;
    node["iterations"] = blur.iterations;
    return node;
}

compositor::MaskBlurParams ReadMaskBlur(const json& parent, const char* key) {
    const compositor::MaskBlurParams defaults;
    const json* node = FindMember(parent, key);
    if (node == nullptr || !node->is_object()) {
        return defaults;
    }
    compositor::MaskBlurParams blur;
    blur.radiusMeters = ReadFloat(*node, "radius", defaults.radiusMeters);
    blur.strength = ReadFloat(*node, "strength", defaults.strength);
    blur.iterations = ReadInt(*node, "iterations", defaults.iterations);
    return blur;
}

json WriteBlend(const compositor::BlendParams& blend) {
    json node;
    node["mode"] = EnumName(kMaskBlendModeNames, static_cast<uint32_t>(blend.mode));
    node["intensity"] = blend.intensity;
    return node;
}

compositor::BlendParams ReadBlend(const json& parent, const char* key) {
    const compositor::BlendParams defaults;
    const json* node = FindMember(parent, key);
    if (node == nullptr || !node->is_object()) {
        return defaults;
    }
    compositor::BlendParams blend;
    blend.mode = static_cast<compositor::MaskBlendMode>(
        EnumValue(kMaskBlendModeNames, *node, "mode", static_cast<uint32_t>(defaults.mode)));
    blend.intensity = ReadFloat(*node, "intensity", defaults.intensity);
    return blend;
}

json WritePathMask(const compositor::PathMaskParams& params) {
    json node;
    node["gamma"] = params.gamma;
    node["invert"] = params.invert;
    return node;
}

compositor::PathMaskParams ReadPathMask(const json& parent, const char* key) {
    const compositor::PathMaskParams defaults;
    const json* node = FindMember(parent, key);
    if (node == nullptr || !node->is_object()) {
        return defaults;
    }
    compositor::PathMaskParams params;
    params.gamma = ReadFloat(*node, "gamma", defaults.gamma);
    params.invert = ReadBool(*node, "invert", defaults.invert);
    return params;
}

json WriteAreaMask(const compositor::AreaMaskParams& params) {
    json node;
    node["feather"] = params.featherMeters;
    node["offset"] = params.offsetMeters;
    node["gamma"] = params.gamma;
    node["invert"] = params.invert;
    return node;
}

compositor::AreaMaskParams ReadAreaMask(const json& parent, const char* key) {
    const compositor::AreaMaskParams defaults;
    const json* node = FindMember(parent, key);
    if (node == nullptr || !node->is_object()) {
        return defaults;
    }
    compositor::AreaMaskParams params;
    params.featherMeters = ReadFloat(*node, "feather", defaults.featherMeters);
    params.offsetMeters = ReadFloat(*node, "offset", defaults.offsetMeters);
    params.gamma = ReadFloat(*node, "gamma", defaults.gamma);
    params.invert = ReadBool(*node, "invert", defaults.invert);
    return params;
}

// パス（Path ノード）。点とエッジをそのまま書く。座標は正規化 UV、寸法は m。
json WritePath(const graph::PathSettings& path) {
    json node;
    json points = json::array();
    for (const graph::PathPoint& point : path.points) {
        json item;
        item["id"] = point.id;
        item["u"] = point.u;
        item["v"] = point.v;
        item["width"] = point.widthMeters;
        item["feather"] = point.featherMeters;
        item["intensity"] = point.intensity;
        item["heightOffset"] = point.heightOffsetMeters;
        points.push_back(std::move(item));
    }
    node["points"] = std::move(points);
    json edges = json::array();
    static const char* const kPathCurveNames[] = {"line", "quadratic", "cubic", "clothoid"};
    for (const graph::PathEdge& edge : path.edges) {
        json item;
        item["id"] = edge.id;
        item["from"] = edge.from;
        item["to"] = edge.to;
        item["curve"] = EnumName(kPathCurveNames, static_cast<uint32_t>(edge.curve));
        item["rounding"] = edge.rounding;
        item["clothoidRatio"] = edge.clothoidRatio;
        // 幅の上書き。切っているときは書かない（点の値を使う）。
        if (edge.overrideValues) {
            item["overrideValues"] = true;
            item["width"] = edge.widthMeters;
            item["feather"] = edge.featherMeters;
            item["intensity"] = edge.intensity;
        }
        // 経路探索。内部点は導出したものだが保存する（地形を評価しないと作れないため）。
        if (edge.route != graph::PathRoute::None) {
            static const char* const kPathRouteNames[] = {"none", "road", "flow"};
            item["route"] = EnumName(kPathRouteNames, static_cast<uint32_t>(edge.route));
            item["maxGrade"] = edge.maxGradePercent;
            if (edge.routed) {
                item["routedFrom"] = json::array({edge.routedFromU, edge.routedFromV});
                item["routedTo"] = json::array({edge.routedToU, edge.routedToV});
                json waypoints = json::array();
                for (const graph::PathRouteWaypoint& waypoint : edge.waypoints) {
                    waypoints.push_back(waypoint.u);
                    waypoints.push_back(waypoint.v);
                }
                item["waypoints"] = std::move(waypoints);
            }
        }
        edges.push_back(std::move(item));
    }
    node["edges"] = std::move(edges);
    node["defaultWidth"] = path.defaultWidthMeters;
    node["defaultFeather"] = path.defaultFeatherMeters;
    node["defaultIntensity"] = path.defaultIntensity;
    node["nextId"] = path.nextId;
    return node;
}

graph::PathSettings ReadPath(const json& parent, const char* key) {
    graph::PathSettings path;
    const json* node = FindMember(parent, key);
    if (node == nullptr || !node->is_object()) {
        return path;
    }
    const graph::PathSettings defaults;
    path.defaultWidthMeters = ReadFloat(*node, "defaultWidth", defaults.defaultWidthMeters);
    path.defaultFeatherMeters = ReadFloat(*node, "defaultFeather", defaults.defaultFeatherMeters);
    path.defaultIntensity = ReadFloat(*node, "defaultIntensity", defaults.defaultIntensity);
    graph::PathElementId maxId = 0;
    if (const json* points = FindMember(*node, "points"); points != nullptr && points->is_array()) {
        for (const json& item : *points) {
            if (!item.is_object()) {
                continue;
            }
            graph::PathPoint point;
            point.id = ReadInt(item, "id", 0);
            if (point.id <= 0) {
                continue;
            }
            point.u = std::clamp(ReadFloat(item, "u", 0.5f), 0.0f, 1.0f);
            point.v = std::clamp(ReadFloat(item, "v", 0.5f), 0.0f, 1.0f);
            point.widthMeters = ReadFloat(item, "width", path.defaultWidthMeters);
            point.featherMeters = ReadFloat(item, "feather", path.defaultFeatherMeters);
            point.intensity = ReadFloat(item, "intensity", path.defaultIntensity);
            point.heightOffsetMeters = ReadFloat(item, "heightOffset", 0.0f);
            maxId = std::max(maxId, point.id);
            path.points.push_back(point);
        }
    }
    if (const json* edges = FindMember(*node, "edges"); edges != nullptr && edges->is_array()) {
        for (const json& item : *edges) {
            if (!item.is_object()) {
                continue;
            }
            static const char* const kPathCurveNames[] = {"line", "quadratic", "cubic",
                                                          "clothoid"};
            graph::PathEdge edge;
            edge.id = ReadInt(item, "id", 0);
            edge.from = ReadInt(item, "from", 0);
            edge.to = ReadInt(item, "to", 0);
            edge.curve = static_cast<graph::PathCurve>(EnumValue(
                kPathCurveNames, item, "curve", static_cast<uint32_t>(graph::PathCurve::Line)));
            edge.rounding = std::clamp(ReadFloat(item, "rounding", 1.0f), 0.0f, 1.0f);
            edge.clothoidRatio = std::clamp(ReadFloat(item, "clothoidRatio", 0.5f), 0.0f, 1.0f);
            if (const json* override = FindMember(item, "overrideValues");
                override != nullptr && override->is_boolean() && override->get<bool>()) {
                edge.overrideValues = true;
                edge.widthMeters = ReadFloat(item, "width", path.defaultWidthMeters);
                edge.featherMeters = ReadFloat(item, "feather", path.defaultFeatherMeters);
                edge.intensity = ReadFloat(item, "intensity", path.defaultIntensity);
            }
            static const char* const kPathRouteNames[] = {"none", "road", "flow"};
            edge.route = static_cast<graph::PathRoute>(EnumValue(
                kPathRouteNames, item, "route", static_cast<uint32_t>(graph::PathRoute::None)));
            edge.maxGradePercent = std::clamp(ReadFloat(item, "maxGrade", 10.0f), 0.1f, 100.0f);
            // 内部点は「計算時の両端」が揃っているときだけ生かす。
            const json* routedFrom = FindMember(item, "routedFrom");
            const json* routedTo = FindMember(item, "routedTo");
            const json* waypoints = FindMember(item, "waypoints");
            const auto isPair = [](const json* value) {
                return value != nullptr && value->is_array() && value->size() == 2 &&
                       (*value)[0].is_number() && (*value)[1].is_number();
            };
            if (edge.route != graph::PathRoute::None && isPair(routedFrom) && isPair(routedTo)) {
                edge.routed = true;
                edge.routedFromU = (*routedFrom)[0].get<float>();
                edge.routedFromV = (*routedFrom)[1].get<float>();
                edge.routedToU = (*routedTo)[0].get<float>();
                edge.routedToV = (*routedTo)[1].get<float>();
                if (waypoints != nullptr && waypoints->is_array()) {
                    for (size_t i = 0; i + 1 < waypoints->size(); i += 2) {
                        const json& u = (*waypoints)[i];
                        const json& v = (*waypoints)[i + 1];
                        if (!u.is_number() || !v.is_number()) {
                            continue;
                        }
                        edge.waypoints.push_back({std::clamp(u.get<float>(), 0.0f, 1.0f),
                                                  std::clamp(v.get<float>(), 0.0f, 1.0f)});
                    }
                }
            }
            // 端点が無い / 自分へ戻るエッジは捨てる（壊れたファイルの安全網）。
            if (edge.id <= 0 || edge.from == edge.to || path.FindPoint(edge.from) == nullptr ||
                path.FindPoint(edge.to) == nullptr) {
                continue;
            }
            maxId = std::max(maxId, edge.id);
            path.edges.push_back(edge);
        }
    }
    path.nextId = std::max(ReadInt(*node, "nextId", 1), maxId + 1);
    return path;
}

json WriteMask(const compositor::LayerMask& mask, const TextureWriter& writeTexture,
               const std::function<json(compositor::PaintMaskId)>& writePaint) {
    json node;
    node["source"] = EnumName(kMaskSourceNames, static_cast<uint32_t>(mask.source));
    node["constant"] = mask.constant;
    node["noise"] = WriteNoise(mask.noise);
    node["derivedScale"] = mask.derivedScale;
    node["contrast"] = mask.contrast;
    node["levelsLow"] = mask.levelsLow;
    node["levelsHigh"] = mask.levelsHigh;
    node["invert"] = mask.invert;
    node["paint"] = writePaint(mask.paint);
    node["texture"] = WriteMapSlot(mask.texture, writeTexture);
    return node;
}

void ReadMask(const json& node, compositor::LayerMask& mask, const TextureReader& readTexture,
              const std::function<compositor::PaintMaskId(const json&)>& readPaint) {
    const compositor::LayerMask defaults;
    mask.source = static_cast<compositor::MaskSource>(
        EnumValue(kMaskSourceNames, node, "source", static_cast<uint32_t>(defaults.source)));
    mask.constant = ReadFloat(node, "constant", defaults.constant);
    mask.noise = ReadNoise(node, "noise", defaults.noise);
    mask.derivedScale = ReadFloat(node, "derivedScale", defaults.derivedScale);
    mask.contrast = ReadFloat(node, "contrast", defaults.contrast);
    mask.levelsLow = ReadFloat(node, "levelsLow", defaults.levelsLow);
    mask.levelsHigh = ReadFloat(node, "levelsHigh", defaults.levelsHigh);
    mask.invert = ReadBool(node, "invert", defaults.invert);
    const json* paint = FindMember(node, "paint");
    mask.paint = (paint != nullptr) ? readPaint(*paint) : compositor::kNoPaintMask;
    mask.texture = ReadMapSlot(node, "texture", readTexture);
}

json WriteLayer(const compositor::MaterialLayer& layer, const TextureWriter& writeTexture,
                const std::function<json(compositor::MaterialAssetId)>& writeMaterial,
                const std::function<json(compositor::PaintMaskId)>& writePaint) {
    json node;
    node["name"] = layer.name;
    node["enabled"] = layer.enabled;
    node["kind"] = EnumName(kLayerKindNames, static_cast<uint32_t>(layer.kind));
    node["channels"] = WriteChannelMask(layer.channelMask);
    node["material"] = writeMaterial(layer.material);

    // マテリアルを割り当てているレイヤーでは使われない値だが、
    // 「なし」へ戻したときに元の値が消えていると驚くので、そのまま持ち回る。
    node["baseColor"] = WriteFloat3(layer.baseColor);
    node["roughness"] = layer.roughness;
    node["metallic"] = layer.metallic;
    node["ambientOcclusion"] = layer.ambientOcclusion;

    json height;
    height["source"] = EnumName(kValueSourceNames, static_cast<uint32_t>(layer.heightSource));
    height["base"] = layer.heightBase;
    height["gain"] = layer.heightGain;
    height["noise"] = WriteNoise(layer.heightNoise);
    // レイヤー直結のハイトマップ（シェイプ用）。マテリアルがあれば使われない。
    height["texture"] = WriteMapSlot(layer.heightTexture, writeTexture);
    node["height"] = std::move(height);

    // 堆積（堆積レイヤーだけが使う）。
    json sediment;
    sediment["emission"] = layer.sediment.emissionMeters;
    sediment["emissionTime"] = layer.sediment.emissionTime;
    sediment["detail"] = layer.sediment.detailMeters;
    sediment["iterations"] = layer.sediment.iterations;
    sediment["stabilization"] = layer.sediment.stabilization;
    sediment["viscosity"] = layer.sediment.viscosity;
    sediment["convertTerrain"] = layer.sediment.convertTerrain;
    sediment["resolution"] = layer.sediment.resolution;
    sediment["maskContrast"] = layer.sediment.maskContrast;
    sediment["maskThicknessMeters"] = layer.sediment.maskThicknessMeters;
    node["sediment"] = std::move(sediment);

    // 崩落（崩落レイヤーだけが使う）。
    json crumbling;
    crumbling["physicsCount"] = layer.crumbling.physicsCount;
    crumbling["amount"] = layer.crumbling.amount;
    crumbling["sizeMin"] = layer.crumbling.sizeMinMeters;
    crumbling["sizeMax"] = layer.crumbling.sizeMaxMeters;
    crumbling["style"] = EnumName(kRockStyleNames, static_cast<uint32_t>(layer.crumbling.style));
    crumbling["gravity"] = layer.crumbling.gravity;
    crumbling["spread"] = layer.crumbling.spread;
    crumbling["seed"] = layer.crumbling.seed;
    node["crumbling"] = std::move(crumbling);

    // 積雪（積雪レイヤーだけが使う）。
    node["meanderingRivers"] = {
        {"iterations", layer.meanderingRivers.iterations},
        {"riverWidth", layer.meanderingRivers.riverWidth},
        {"meanderScale", layer.meanderingRivers.meanderScale},
        {"intensity", layer.meanderingRivers.intensity},
        {"heightInfluence", layer.meanderingRivers.heightInfluence},
        {"smoothing", layer.meanderingRivers.smoothing},
        {"riverDepth", layer.meanderingRivers.riverDepth},
        {"basinWidth", layer.meanderingRivers.basinWidth},
        {"basinDepth", layer.meanderingRivers.basinDepth},
        {"bankNoise", layer.meanderingRivers.bankNoise},
        {"seed", layer.meanderingRivers.seed},
        {"basinEnabled", layer.meanderingRivers.basinEnabled},
        {"flattenUphill", layer.meanderingRivers.flattenUphill}
    };
    node["lake"] = {{"optimizationSteps", layer.lake.optimizationSteps},
                    {"waterAmount", layer.lake.waterAmount},
                    {"allowOutflow", layer.lake.allowOutflow},
                    {"referenceDetailScale", layer.lake.referenceDetailScale}};
    json snowCover;
    snowCover["erodeDusting"] = layer.snowCover.erodeDusting;
    snowCover["advectionLength"] = layer.snowCover.advectionLength;
    snowCover["advectionVolume"] = layer.snowCover.advectionVolume;
    snowCover["advectionStrength"] = layer.snowCover.advectionStrength;
    snowCover["valuePreservation"] = layer.snowCover.valuePreservation;

    snowCover["deepSnow"] = layer.snowCover.deepSnow;
    snowCover["featureSize"] = layer.snowCover.featureSize;
    snowCover["settleIterations"] = layer.snowCover.settleIterations;
    snowCover["snowfallDepth"] = layer.snowCover.snowfallDepth;
    snowCover["flowVolume"] = layer.snowCover.flowVolume;
    snowCover["maxSlope"] = layer.snowCover.maxSlope;
    snowCover["melt"] = layer.snowCover.melt;
    snowCover["multigrid"] = layer.snowCover.multigrid;
    snowCover["referenceDetailScale"] = layer.snowCover.referenceDetailScale;
    snowCover["snowLine"] = layer.snowCover.snowLine;
    snowCover["snowLineStrength"] = layer.snowCover.snowLineStrength;
    snowCover["snowLineHeight"] = layer.snowCover.snowLineHeight;
    snowCover["snowLineFalloff"] = layer.snowCover.snowLineFalloff;
    snowCover["wind"] = layer.snowCover.wind;
    snowCover["windX"] = layer.snowCover.windX;
    snowCover["windY"] = layer.snowCover.windY;
    snowCover["windZ"] = layer.snowCover.windZ;
    snowCover["windStrength"] = layer.snowCover.windStrength;
    snowCover["blurWind"] = layer.snowCover.blurWind;
    snowCover["windBlurRadius"] = layer.snowCover.windBlurRadius;
    snowCover["dusting"] = layer.snowCover.dusting;
    snowCover["dustingIntensity"] = layer.snowCover.dustingIntensity;
    snowCover["slipoffAngle"] = layer.snowCover.slipoffAngle;
    snowCover["slipoffFalloff"] = layer.snowCover.slipoffFalloff;
    snowCover["curvatureInfluence"] = layer.snowCover.curvatureInfluence;
    snowCover["noise"] = layer.snowCover.noise;
    snowCover["noiseStrength"] = layer.snowCover.noiseStrength;
    snowCover["noiseScale"] = layer.snowCover.noiseScale;
    snowCover["noiseRoughness"] = layer.snowCover.noiseRoughness;
    snowCover["noiseOctaves"] = layer.snowCover.noiseOctaves;
    snowCover["ramp"] = json::array();
    for (int i = 0; i < std::clamp(layer.snowCover.rampCount, 2, 8); ++i) {
        const auto& point = layer.snowCover.ramp[i];
        snowCover["ramp"].push_back({{"position", point.position}, {"value", point.value},
                                      {"interpolation", point.interpolation}});
    }
    node["snowCover"] = std::move(snowCover);
    json snow;
    snow["emission"] = layer.snow.emissionMeters;
    snow["emissionTime"] = layer.snow.emissionTime;
    snow["iterations"] = layer.snow.iterations;
    snow["settlingPasses"] = layer.snow.settlingPasses;
    snow["motionSlopeDegrees"] = layer.snow.motionSlopeDegrees;
    snow["transportRate"] = layer.snow.transportRate;
    snow["surfaceSmoothing"] = layer.snow.surfaceSmoothing;
    snow["detail"] = layer.snow.detailMeters;
    snow["resolution"] = layer.snow.resolution;
    snow["maskThresholdMeters"] = layer.snow.maskThresholdMeters;
    snow["maskFeatherMeters"] = layer.snow.maskFeatherMeters;
    node["snow"] = std::move(snow);

    // 河川（河川レイヤーだけが使う）。
    json river;
    river["threshold"] = layer.river.threshold;
    river["detail"] = layer.river.detailMeters;
    river["concentration"] = layer.river.concentration;
    river["resolution"] = layer.river.resolution;
    river["mainWidth"] = layer.river.mainWidthMeters;
    river["minWidth"] = layer.river.minWidthMeters;
    river["widthExponent"] = layer.river.widthExponent;
    river["bedDepth"] = layer.river.bedDepthMeters;
    river["bankWidth"] = layer.river.bankWidthMeters;
    river["bankHardness"] = layer.river.bankHardness;
    river["fillWater"] = layer.river.fillWater;
    river["minSlope"] = layer.river.minSlope;
    river["shoreWidth"] = layer.river.shoreWidthMeters;
    river["shoreHeight"] = layer.river.shoreHeightMeters;
    river["shoreFeather"] = layer.river.shoreFeather;
    node["river"] = std::move(river);

    // 水滴侵食（水滴侵食レイヤーだけが使う）。
    json fluvialErosion;
    fluvialErosion["resolution"] = layer.fluvialErosion.resolution;
    fluvialErosion["iterations"] = layer.fluvialErosion.iterations;
    fluvialErosion["featureSize"] = layer.fluvialErosion.featureSize;
    fluvialErosion["geologicalAge"] = layer.fluvialErosion.geologicalAge;
    fluvialErosion["channelLength"] = layer.fluvialErosion.channelLength;
    fluvialErosion["strength"] = layer.fluvialErosion.strength;
    fluvialErosion["channeling"] = layer.fluvialErosion.channeling;
    fluvialErosion["friction"] = layer.fluvialErosion.friction;
    fluvialErosion["wearAngle"] = layer.fluvialErosion.wearAngle;
    fluvialErosion["depositAngle"] = layer.fluvialErosion.depositAngle;
    fluvialErosion["maxAngle"] = layer.fluvialErosion.maxAngle;
    fluvialErosion["granularity"] = layer.fluvialErosion.granularity;
    fluvialErosion["flowVolume"] = layer.fluvialErosion.flowVolume;
    fluvialErosion["smallChannels"] = layer.fluvialErosion.smallChannels;
    fluvialErosion["velocity"] = layer.fluvialErosion.velocity;
    fluvialErosion["detailMeters"] = layer.fluvialErosion.detailMeters;
    fluvialErosion["detailSmoothing"] = layer.fluvialErosion.detailSmoothing;
    fluvialErosion["forceX"] = layer.fluvialErosion.forceX;
    fluvialErosion["forceZ"] = layer.fluvialErosion.forceZ;
    fluvialErosion["shearX"] = layer.fluvialErosion.shearX;
    fluvialErosion["shearZ"] = layer.fluvialErosion.shearZ;
    fluvialErosion["hardness"] = layer.fluvialErosion.hardness;
    node["fluvialErosion"] = std::move(fluvialErosion);

    json flattenBorders;
    flattenBorders["falloffMeters"] = layer.flattenBorders.falloffMeters;
    flattenBorders["elevationMeters"] = layer.flattenBorders.elevationMeters;
    flattenBorders["liftMeters"] = layer.flattenBorders.liftMeters;
    flattenBorders["strength"] = layer.flattenBorders.strength;
    flattenBorders["lowerX"] = layer.flattenBorders.lowerX;
    flattenBorders["upperX"] = layer.flattenBorders.upperX;
    flattenBorders["lowerZ"] = layer.flattenBorders.lowerZ;
    flattenBorders["upperZ"] = layer.flattenBorders.upperZ;
    node["flattenBorders"] = std::move(flattenBorders);

    json multiScaleErosion;
    multiScaleErosion["resolution"] = layer.multiScaleErosion.resolution;
    multiScaleErosion["baseResolution"] = layer.multiScaleErosion.baseResolution;
    multiScaleErosion["erosionIterations"] = layer.multiScaleErosion.erosionIterations;
    multiScaleErosion["thermalIterations"] = layer.multiScaleErosion.thermalIterations;
    multiScaleErosion["depositionIterations"] = layer.multiScaleErosion.depositionIterations;
    multiScaleErosion["coarseDepthMeters"] = layer.multiScaleErosion.coarseDepthMeters;
    multiScaleErosion["detailDecay"] = layer.multiScaleErosion.detailDecay;
    multiScaleErosion["flowExponent"] = layer.multiScaleErosion.flowExponent;
    multiScaleErosion["slopeExponent"] = layer.multiScaleErosion.slopeExponent;
    multiScaleErosion["drainageExponent"] = layer.multiScaleErosion.drainageExponent;
    multiScaleErosion["maximumSlope"] = layer.multiScaleErosion.maximumSlope;
    multiScaleErosion["maximumDrainageArea"] = layer.multiScaleErosion.maximumDrainageArea;
    multiScaleErosion["talusDegrees"] = layer.multiScaleErosion.talusDegrees;
    multiScaleErosion["thermalStepMeters"] = layer.multiScaleErosion.thermalStepMeters;
    multiScaleErosion["sedimentCreation"] = layer.multiScaleErosion.sedimentCreation;
    multiScaleErosion["depositionRate"] = layer.multiScaleErosion.depositionRate;
    multiScaleErosion["sedimentHeightScale"] = layer.multiScaleErosion.sedimentHeightScale;
    multiScaleErosion["ridgeRestoration"] = layer.multiScaleErosion.ridgeRestoration;
    multiScaleErosion["ridgeAreaThreshold"] = layer.multiScaleErosion.ridgeAreaThreshold;
    multiScaleErosion["restorationIterations"] = layer.multiScaleErosion.restorationIterations;
    multiScaleErosion["drainageCorrection"] = layer.multiScaleErosion.drainageCorrection;
    multiScaleErosion["breachingRadius"] = layer.multiScaleErosion.breachingRadius;
    node["multiScaleErosion"] = std::move(multiScaleErosion);

    json droplet;
    droplet["density"] = layer.droplet.dropletDensity;
    droplet["travel"] = layer.droplet.travelMeters;
    droplet["erosion"] = layer.droplet.erosionStrength;
    droplet["deposition"] = layer.droplet.depositionStrength;
    droplet["inertia"] = layer.droplet.inertia;
    droplet["minSlope"] = layer.droplet.minSlope;
    droplet["capacity"] = layer.droplet.sedimentCapacity;
    droplet["evaporation"] = layer.droplet.evaporationPerMeter;
    droplet["gravity"] = layer.droplet.gravity;
    droplet["multigrid"] = layer.droplet.multigrid;
    droplet["iterations"] = layer.droplet.iterations;
    droplet["seed"] = layer.droplet.seed;
    droplet["resolution"] = layer.droplet.resolution;
    node["droplet"] = std::move(droplet);

    // 散布（散布レイヤーだけが使う）。
    json scatter;
    scatter["shape"] = EnumName(kScatterShapeNames, static_cast<uint32_t>(layer.scatter.shape));
    scatter["orientation"] =
        EnumName(kScatterOrientationNames, static_cast<uint32_t>(layer.scatter.orientation));
    scatter["seed"] = layer.scatter.seed;
    scatter["density"] = layer.scatter.densityMeters;
    scatter["coverage"] = layer.scatter.coverage;
    scatter["sizeMin"] = layer.scatter.sizeMinMeters;
    scatter["sizeMax"] = layer.scatter.sizeMaxMeters;
    scatter["height"] = layer.scatter.heightMeters;
    scatter["heightJitter"] = layer.scatter.heightJitter;
    scatter["rotationVariation"] = layer.scatter.rotationVariation;
    scatter["aspectVariation"] = layer.scatter.aspectVariation;
    scatter["smoothness"] = layer.scatter.smoothness;
    node["scatter"] = std::move(scatter);

    // ぼかし（ブラーレイヤーだけが使う）。
    json blur;
    blur["radius"] = layer.blur.radiusMeters;
    blur["strength"] = layer.blur.strength;
    blur["iterations"] = layer.blur.iterations;
    node["blur"] = std::move(blur);

    node["mask"] = WriteMask(layer.mask, writeTexture, writePaint);
    node["blendRange"] = layer.blendRange;
    node["wrapToUnderlying"] = layer.wrapToUnderlying;
    node["uvScale"] = layer.uvScale;
    return node;
}

compositor::MaterialLayer ReadLayer(
    const json& node, const TextureReader& readTexture,
    const std::function<compositor::MaterialAssetId(const json&)>& readMaterial,
    const std::function<compositor::PaintMaskId(const json&)>& readPaint) {
    const compositor::MaterialLayer defaults;
    compositor::MaterialLayer layer;
    layer.name = ReadString(node, "name", defaults.name);
    layer.enabled = ReadBool(node, "enabled", defaults.enabled);
    // kind の無い旧形式（版 2 以前）はサーフェスとして読む。
    layer.kind = static_cast<compositor::LayerKind>(
        EnumValue(kLayerKindNames, node, "kind", static_cast<uint32_t>(defaults.kind)));
    layer.channelMask = ReadChannelMask(node, "channels", defaults.channelMask);
    const json* material = FindMember(node, "material");
    layer.material =
        (material != nullptr) ? readMaterial(*material) : compositor::kNoMaterialAsset;

    layer.baseColor = ReadFloat3(node, "baseColor", defaults.baseColor);
    layer.roughness = ReadFloat(node, "roughness", defaults.roughness);
    layer.metallic = ReadFloat(node, "metallic", defaults.metallic);
    layer.ambientOcclusion = ReadFloat(node, "ambientOcclusion", defaults.ambientOcclusion);

    if (const json* height = FindMember(node, "height");
        height != nullptr && height->is_object()) {
        layer.heightSource = static_cast<compositor::ValueSource>(EnumValue(
            kValueSourceNames, *height, "source", static_cast<uint32_t>(defaults.heightSource)));
        layer.heightBase = ReadFloat(*height, "base", defaults.heightBase);
        layer.heightNoise = ReadNoise(*height, "noise", defaults.heightNoise);
        layer.heightTexture = ReadMapSlot(*height, "texture", readTexture);

        if (FindMember(*height, "gain") != nullptr) {
            layer.heightGain = ReadFloat(*height, "gain", defaults.heightGain);
        } else {
            // gain を分離する前の形式。起伏の強さはノイズの amount が兼ねていて、
            // 式は h = base + src * amount だった。基準面を挟む式へ寄せる。
            //
            //   base + src * gain == base' + (src - kHeightPivot) * gain
            //   ただし base' = base + kHeightPivot * gain
            //
            // これは近似ではなく厳密に同じ値になる。定数は src の項がないので触らない。
            layer.heightGain = layer.heightNoise.amount;
            if (layer.heightSource != compositor::ValueSource::Constant) {
                layer.heightBase += compositor::kHeightPivot * layer.heightGain;
            }
        }
    }

    if (const json* sediment = FindMember(node, "sediment");
        sediment != nullptr && sediment->is_object()) {
        layer.sediment.emissionMeters =
            ReadFloat(*sediment, "emission", defaults.sediment.emissionMeters);
        layer.sediment.emissionTime =
            ReadFloat(*sediment, "emissionTime", defaults.sediment.emissionTime);
        layer.sediment.detailMeters =
            ReadFloat(*sediment, "detail", defaults.sediment.detailMeters);
        layer.sediment.iterations =
            ReadInt(*sediment, "iterations", defaults.sediment.iterations);
        layer.sediment.stabilization =
            ReadInt(*sediment, "stabilization", defaults.sediment.stabilization);
        layer.sediment.viscosity =
            ReadFloat(*sediment, "viscosity", defaults.sediment.viscosity);
        layer.sediment.convertTerrain =
            ReadBool(*sediment, "convertTerrain", defaults.sediment.convertTerrain);
        layer.sediment.resolution = static_cast<uint32_t>(ReadInt(
            *sediment, "resolution", static_cast<int>(defaults.sediment.resolution)));
        layer.sediment.maskContrast =
            ReadFloat(*sediment, "maskContrast", defaults.sediment.maskContrast);
        layer.sediment.maskThicknessMeters = ReadFloat(*sediment, "maskThicknessMeters",
                                                       defaults.sediment.maskThicknessMeters);
    }

    if (const json* value = FindMember(node, "meanderingRivers"); value != nullptr && value->is_object()) {
        layer.meanderingRivers.iterations = std::clamp(ReadInt(*value, "iterations", defaults.meanderingRivers.iterations), 0, 512);
        layer.meanderingRivers.riverWidth = std::clamp(ReadFloat(*value, "riverWidth", defaults.meanderingRivers.riverWidth), 0.1f, 1000.0f);
        layer.meanderingRivers.meanderScale = std::clamp(ReadFloat(*value, "meanderScale", defaults.meanderingRivers.meanderScale), 0.1f, 10.0f);
        layer.meanderingRivers.intensity = std::clamp(ReadFloat(*value, "intensity", defaults.meanderingRivers.intensity), 0.0f, 2.0f);
        layer.meanderingRivers.heightInfluence = std::clamp(ReadFloat(*value, "heightInfluence", defaults.meanderingRivers.heightInfluence), 0.0f, 10.0f);
        layer.meanderingRivers.smoothing = std::clamp(ReadFloat(*value, "smoothing", defaults.meanderingRivers.smoothing), 0.0f, 1.0f);
        layer.meanderingRivers.riverDepth = std::clamp(ReadFloat(*value, "riverDepth", defaults.meanderingRivers.riverDepth), 0.0f, 2.0f);
        layer.meanderingRivers.basinWidth = std::clamp(ReadFloat(*value, "basinWidth", defaults.meanderingRivers.basinWidth), 0.0f, 5000.0f);
        layer.meanderingRivers.basinDepth = std::clamp(ReadFloat(*value, "basinDepth", defaults.meanderingRivers.basinDepth), 0.0f, 1000.0f);
        layer.meanderingRivers.bankNoise = std::clamp(ReadFloat(*value, "bankNoise", defaults.meanderingRivers.bankNoise), 0.0f, 0.5f);
        layer.meanderingRivers.seed = std::clamp(ReadInt(*value, "seed", defaults.meanderingRivers.seed), 0, 1000000);
        layer.meanderingRivers.basinEnabled = ReadBool(*value, "basinEnabled", defaults.meanderingRivers.basinEnabled);
        layer.meanderingRivers.flattenUphill = ReadBool(*value, "flattenUphill", defaults.meanderingRivers.flattenUphill);
    }
    if (const json* value = FindMember(node, "lake"); value != nullptr && value->is_object()) {
        layer.lake.optimizationSteps = std::clamp(ReadInt(*value, "optimizationSteps", defaults.lake.optimizationSteps), 0, 10);
        layer.lake.waterAmount = std::clamp(ReadFloat(*value, "waterAmount", defaults.lake.waterAmount), 0.0f, 100.0f);
        layer.lake.allowOutflow = ReadBool(*value, "allowOutflow", defaults.lake.allowOutflow);
        layer.lake.referenceDetailScale = std::clamp(ReadFloat(*value, "referenceDetailScale", defaults.lake.referenceDetailScale), 0.01f, 100.0f);
    }
    if (const json* value = FindMember(node, "snowCover"); value != nullptr && value->is_object()) {
        layer.snowCover.erodeDusting = ReadBool(*value, "erodeDusting", defaults.snowCover.erodeDusting);
        layer.snowCover.advectionLength = ReadFloat(*value, "advectionLength", defaults.snowCover.advectionLength);
        layer.snowCover.advectionVolume = ReadFloat(*value, "advectionVolume", defaults.snowCover.advectionVolume);
        layer.snowCover.advectionStrength = ReadFloat(*value, "advectionStrength", defaults.snowCover.advectionStrength);
        layer.snowCover.valuePreservation = ReadFloat(*value, "valuePreservation", defaults.snowCover.valuePreservation);
        layer.snowCover.deepSnow = ReadBool(*value, "deepSnow", defaults.snowCover.deepSnow);
        layer.snowCover.featureSize = ReadFloat(*value, "featureSize", defaults.snowCover.featureSize);
        layer.snowCover.settleIterations = ReadInt(*value, "settleIterations", defaults.snowCover.settleIterations);
        layer.snowCover.snowfallDepth = ReadFloat(*value, "snowfallDepth", defaults.snowCover.snowfallDepth);
        layer.snowCover.flowVolume = ReadFloat(*value, "flowVolume", defaults.snowCover.flowVolume);
        layer.snowCover.maxSlope = ReadFloat(*value, "maxSlope", defaults.snowCover.maxSlope);
        layer.snowCover.melt = ReadFloat(*value, "melt", defaults.snowCover.melt);
        layer.snowCover.multigrid = ReadBool(*value, "multigrid", defaults.snowCover.multigrid);
        layer.snowCover.referenceDetailScale = ReadFloat(*value, "referenceDetailScale", defaults.snowCover.referenceDetailScale);
        layer.snowCover.snowLine = ReadBool(*value, "snowLine", defaults.snowCover.snowLine);
        layer.snowCover.snowLineStrength = ReadFloat(*value, "snowLineStrength", defaults.snowCover.snowLineStrength);
        layer.snowCover.snowLineHeight = ReadFloat(*value, "snowLineHeight", defaults.snowCover.snowLineHeight);
        layer.snowCover.snowLineFalloff = ReadFloat(*value, "snowLineFalloff", defaults.snowCover.snowLineFalloff);
        layer.snowCover.wind = ReadBool(*value, "wind", defaults.snowCover.wind);
        layer.snowCover.windX = ReadFloat(*value, "windX", defaults.snowCover.windX);
        layer.snowCover.windY = ReadFloat(*value, "windY", defaults.snowCover.windY);
        layer.snowCover.windZ = ReadFloat(*value, "windZ", defaults.snowCover.windZ);
        layer.snowCover.windStrength = ReadFloat(*value, "windStrength", defaults.snowCover.windStrength);
        layer.snowCover.blurWind = ReadBool(*value, "blurWind", defaults.snowCover.blurWind);
        layer.snowCover.windBlurRadius = ReadFloat(*value, "windBlurRadius", defaults.snowCover.windBlurRadius);
        layer.snowCover.dusting = ReadBool(*value, "dusting", defaults.snowCover.dusting);
        layer.snowCover.dustingIntensity = ReadFloat(*value, "dustingIntensity", defaults.snowCover.dustingIntensity);
        layer.snowCover.slipoffAngle = ReadFloat(*value, "slipoffAngle", defaults.snowCover.slipoffAngle);
        layer.snowCover.slipoffFalloff = ReadFloat(*value, "slipoffFalloff", defaults.snowCover.slipoffFalloff);
        layer.snowCover.curvatureInfluence = ReadFloat(*value, "curvatureInfluence", defaults.snowCover.curvatureInfluence);
        layer.snowCover.noise = ReadBool(*value, "noise", defaults.snowCover.noise);
        layer.snowCover.noiseStrength = ReadFloat(*value, "noiseStrength", defaults.snowCover.noiseStrength);
        layer.snowCover.noiseScale = ReadFloat(*value, "noiseScale", defaults.snowCover.noiseScale);
        layer.snowCover.noiseRoughness = ReadFloat(*value, "noiseRoughness", defaults.snowCover.noiseRoughness);
        layer.snowCover.noiseOctaves = ReadInt(*value, "noiseOctaves", defaults.snowCover.noiseOctaves);
        if (const json* ramp = FindMember(*value, "ramp"); ramp && ramp->is_array() && ramp->size() >= 2) {
            layer.snowCover.rampCount = static_cast<int>(std::min<size_t>(ramp->size(), 8));
            for (int i = 0; i < layer.snowCover.rampCount; ++i) {
                auto& point = layer.snowCover.ramp[i];
                point.position = std::clamp(ReadFloat((*ramp)[i], "position", point.position), 0.0f, 1.0f);
                point.value = std::clamp(ReadFloat((*ramp)[i], "value", point.value), 0.0f, 1.0f);
                point.interpolation = std::clamp(ReadInt((*ramp)[i], "interpolation", 1), 0, 2);
            }
        }
    }
    if (const json* snow = FindMember(node, "snow"); snow != nullptr && snow->is_object()) {
        layer.snow.emissionMeters = ReadFloat(*snow, "emission", defaults.snow.emissionMeters);
        layer.snow.emissionTime =
            ReadFloat(*snow, "emissionTime", defaults.snow.emissionTime);
        layer.snow.iterations = ReadInt(*snow, "iterations", defaults.snow.iterations);
        layer.snow.settlingPasses =
            ReadInt(*snow, "settlingPasses", defaults.snow.settlingPasses);
        layer.snow.motionSlopeDegrees =
            ReadFloat(*snow, "motionSlopeDegrees", defaults.snow.motionSlopeDegrees);
        layer.snow.transportRate =
            ReadFloat(*snow, "transportRate", defaults.snow.transportRate);
        layer.snow.surfaceSmoothing =
            ReadFloat(*snow, "surfaceSmoothing", defaults.snow.surfaceSmoothing);
        layer.snow.detailMeters = ReadFloat(*snow, "detail", defaults.snow.detailMeters);
        layer.snow.resolution = static_cast<uint32_t>(
            ReadInt(*snow, "resolution", static_cast<int>(defaults.snow.resolution)));
        layer.snow.maskThresholdMeters =
            ReadFloat(*snow, "maskThresholdMeters", defaults.snow.maskThresholdMeters);
        layer.snow.maskFeatherMeters =
            ReadFloat(*snow, "maskFeatherMeters", defaults.snow.maskFeatherMeters);
    }

    if (const json* river = FindMember(node, "river"); river != nullptr && river->is_object()) {
        layer.river.threshold = ReadFloat(*river, "threshold", defaults.river.threshold);
        layer.river.detailMeters = ReadFloat(*river, "detail", defaults.river.detailMeters);
        layer.river.concentration =
            ReadFloat(*river, "concentration", defaults.river.concentration);
        layer.river.resolution = static_cast<uint32_t>(
            ReadInt(*river, "resolution", static_cast<int>(defaults.river.resolution)));
        layer.river.mainWidthMeters =
            ReadFloat(*river, "mainWidth", defaults.river.mainWidthMeters);
        layer.river.minWidthMeters = ReadFloat(*river, "minWidth", defaults.river.minWidthMeters);
        layer.river.widthExponent =
            ReadFloat(*river, "widthExponent", defaults.river.widthExponent);
        layer.river.bedDepthMeters = ReadFloat(*river, "bedDepth", defaults.river.bedDepthMeters);
        layer.river.bankWidthMeters =
            ReadFloat(*river, "bankWidth", defaults.river.bankWidthMeters);
        layer.river.bankHardness = ReadFloat(*river, "bankHardness", defaults.river.bankHardness);
        layer.river.fillWater = ReadBool(*river, "fillWater", defaults.river.fillWater);
        layer.river.minSlope = ReadFloat(*river, "minSlope", defaults.river.minSlope);
        layer.river.shoreWidthMeters =
            ReadFloat(*river, "shoreWidth", defaults.river.shoreWidthMeters);
        layer.river.shoreHeightMeters =
            ReadFloat(*river, "shoreHeight", defaults.river.shoreHeightMeters);
        layer.river.shoreFeather = ReadFloat(*river, "shoreFeather", defaults.river.shoreFeather);
    }

    if (const json* crumbling = FindMember(node, "crumbling");
        crumbling != nullptr && crumbling->is_object()) {
        layer.crumbling.physicsCount =
            ReadInt(*crumbling, "physicsCount", defaults.crumbling.physicsCount);
        layer.crumbling.amount = ReadFloat(*crumbling, "amount", defaults.crumbling.amount);
        layer.crumbling.sizeMinMeters =
            ReadFloat(*crumbling, "sizeMin", defaults.crumbling.sizeMinMeters);
        layer.crumbling.sizeMaxMeters =
            ReadFloat(*crumbling, "sizeMax", defaults.crumbling.sizeMaxMeters);
        layer.crumbling.style = static_cast<compositor::RockStyle>(EnumValue(
            kRockStyleNames, *crumbling, "style",
            static_cast<uint32_t>(defaults.crumbling.style)));
        layer.crumbling.gravity = ReadFloat(*crumbling, "gravity", defaults.crumbling.gravity);
        layer.crumbling.spread = ReadFloat(*crumbling, "spread", defaults.crumbling.spread);
        layer.crumbling.seed = ReadInt(*crumbling, "seed", defaults.crumbling.seed);
    }

    if (const json* value = FindMember(node, "fluvialErosion"); value != nullptr && value->is_object()) {
        layer.fluvialErosion.resolution = static_cast<uint32_t>(std::clamp(ReadInt(*value, "resolution", defaults.fluvialErosion.resolution), 64, 2048));
        layer.fluvialErosion.iterations = ReadInt(*value, "iterations", defaults.fluvialErosion.iterations);
        layer.fluvialErosion.featureSize = ReadFloat(*value, "featureSize", defaults.fluvialErosion.featureSize);
        layer.fluvialErosion.geologicalAge = ReadFloat(*value, "geologicalAge", defaults.fluvialErosion.geologicalAge);
        layer.fluvialErosion.channelLength = ReadFloat(*value, "channelLength", defaults.fluvialErosion.channelLength);
        layer.fluvialErosion.strength = ReadFloat(*value, "strength", defaults.fluvialErosion.strength);
        layer.fluvialErosion.channeling = ReadFloat(*value, "channeling", defaults.fluvialErosion.channeling);
        layer.fluvialErosion.friction = ReadFloat(*value, "friction", defaults.fluvialErosion.friction);
        layer.fluvialErosion.wearAngle = ReadFloat(*value, "wearAngle", defaults.fluvialErosion.wearAngle);
        layer.fluvialErosion.depositAngle = ReadFloat(*value, "depositAngle", defaults.fluvialErosion.depositAngle);
        layer.fluvialErosion.maxAngle = ReadFloat(*value, "maxAngle", defaults.fluvialErosion.maxAngle);
        layer.fluvialErosion.granularity = ReadFloat(*value, "granularity", defaults.fluvialErosion.granularity);
        layer.fluvialErosion.flowVolume = ReadFloat(*value, "flowVolume", defaults.fluvialErosion.flowVolume);
        layer.fluvialErosion.smallChannels = ReadFloat(*value, "smallChannels", defaults.fluvialErosion.smallChannels);
        layer.fluvialErosion.velocity = ReadFloat(*value, "velocity", defaults.fluvialErosion.velocity);
        layer.fluvialErosion.detailMeters = ReadFloat(*value, "detailMeters", defaults.fluvialErosion.detailMeters);
        layer.fluvialErosion.detailSmoothing = ReadFloat(*value, "detailSmoothing", defaults.fluvialErosion.detailSmoothing);
        layer.fluvialErosion.forceX = ReadFloat(*value, "forceX", defaults.fluvialErosion.forceX);
        layer.fluvialErosion.forceZ = ReadFloat(*value, "forceZ", defaults.fluvialErosion.forceZ);
        layer.fluvialErosion.shearX = ReadFloat(*value, "shearX", defaults.fluvialErosion.shearX);
        layer.fluvialErosion.shearZ = ReadFloat(*value, "shearZ", defaults.fluvialErosion.shearZ);
        layer.fluvialErosion.hardness = ReadFloat(*value, "hardness", defaults.fluvialErosion.hardness);
    }
    if (const json* value = FindMember(node, "flattenBorders"); value != nullptr && value->is_object()) {
        layer.flattenBorders.falloffMeters = ReadFloat(*value, "falloffMeters", defaults.flattenBorders.falloffMeters);
        layer.flattenBorders.elevationMeters = ReadFloat(*value, "elevationMeters", defaults.flattenBorders.elevationMeters);
        layer.flattenBorders.liftMeters = ReadFloat(*value, "liftMeters", defaults.flattenBorders.liftMeters);
        layer.flattenBorders.strength = ReadFloat(*value, "strength", defaults.flattenBorders.strength);
        layer.flattenBorders.lowerX = ReadBool(*value, "lowerX", defaults.flattenBorders.lowerX);
        layer.flattenBorders.upperX = ReadBool(*value, "upperX", defaults.flattenBorders.upperX);
        layer.flattenBorders.lowerZ = ReadBool(*value, "lowerZ", defaults.flattenBorders.lowerZ);
        layer.flattenBorders.upperZ = ReadBool(*value, "upperZ", defaults.flattenBorders.upperZ);
    }
    if (const json* mse = FindMember(node, "multiScaleErosion"); mse != nullptr && mse->is_object()) {
        layer.multiScaleErosion.resolution = static_cast<uint32_t>(std::clamp(ReadInt(*mse, "resolution", defaults.multiScaleErosion.resolution), 16, 2048));
        layer.multiScaleErosion.baseResolution = static_cast<uint32_t>(std::clamp(ReadInt(*mse, "baseResolution", defaults.multiScaleErosion.baseResolution), 16, 2048));
        layer.multiScaleErosion.erosionIterations = ReadInt(*mse, "erosionIterations", defaults.multiScaleErosion.erosionIterations);
        layer.multiScaleErosion.thermalIterations = ReadInt(*mse, "thermalIterations", defaults.multiScaleErosion.thermalIterations);
        layer.multiScaleErosion.depositionIterations = ReadInt(*mse, "depositionIterations", defaults.multiScaleErosion.depositionIterations);
        layer.multiScaleErosion.coarseDepthMeters = ReadFloat(*mse, "coarseDepthMeters", defaults.multiScaleErosion.coarseDepthMeters);
        layer.multiScaleErosion.detailDecay = ReadFloat(*mse, "detailDecay", defaults.multiScaleErosion.detailDecay);
        layer.multiScaleErosion.flowExponent = ReadFloat(*mse, "flowExponent", defaults.multiScaleErosion.flowExponent);
        layer.multiScaleErosion.slopeExponent = ReadFloat(*mse, "slopeExponent", defaults.multiScaleErosion.slopeExponent);
        layer.multiScaleErosion.drainageExponent = ReadFloat(*mse, "drainageExponent", defaults.multiScaleErosion.drainageExponent);
        layer.multiScaleErosion.maximumSlope = ReadFloat(*mse, "maximumSlope", defaults.multiScaleErosion.maximumSlope);
        layer.multiScaleErosion.maximumDrainageArea = ReadFloat(*mse, "maximumDrainageArea", defaults.multiScaleErosion.maximumDrainageArea);
        layer.multiScaleErosion.talusDegrees = ReadFloat(*mse, "talusDegrees", defaults.multiScaleErosion.talusDegrees);
        layer.multiScaleErosion.thermalStepMeters = ReadFloat(*mse, "thermalStepMeters", defaults.multiScaleErosion.thermalStepMeters);
        layer.multiScaleErosion.sedimentCreation = ReadFloat(*mse, "sedimentCreation", defaults.multiScaleErosion.sedimentCreation);
        layer.multiScaleErosion.depositionRate = ReadFloat(*mse, "depositionRate", defaults.multiScaleErosion.depositionRate);
        layer.multiScaleErosion.sedimentHeightScale = ReadFloat(*mse, "sedimentHeightScale", defaults.multiScaleErosion.sedimentHeightScale);
        layer.multiScaleErosion.ridgeRestoration = ReadFloat(*mse, "ridgeRestoration", defaults.multiScaleErosion.ridgeRestoration);
        layer.multiScaleErosion.ridgeAreaThreshold = ReadFloat(*mse, "ridgeAreaThreshold", defaults.multiScaleErosion.ridgeAreaThreshold);
        layer.multiScaleErosion.restorationIterations = ReadInt(*mse, "restorationIterations", defaults.multiScaleErosion.restorationIterations);
        layer.multiScaleErosion.drainageCorrection = ReadBool(*mse, "drainageCorrection", defaults.multiScaleErosion.drainageCorrection);
        layer.multiScaleErosion.breachingRadius = ReadInt(*mse, "breachingRadius", defaults.multiScaleErosion.breachingRadius);
    }

    if (const json* droplet = FindMember(node, "droplet");
        droplet != nullptr && droplet->is_object()) {
        layer.droplet.dropletDensity =
            ReadFloat(*droplet, "density", defaults.droplet.dropletDensity);
        layer.droplet.travelMeters = ReadFloat(*droplet, "travel", defaults.droplet.travelMeters);
        layer.droplet.erosionStrength =
            ReadFloat(*droplet, "erosion", defaults.droplet.erosionStrength);
        layer.droplet.depositionStrength =
            ReadFloat(*droplet, "deposition", defaults.droplet.depositionStrength);
        layer.droplet.inertia = ReadFloat(*droplet, "inertia", defaults.droplet.inertia);
        layer.droplet.minSlope = ReadFloat(*droplet, "minSlope", defaults.droplet.minSlope);
        layer.droplet.sedimentCapacity =
            ReadFloat(*droplet, "capacity", defaults.droplet.sedimentCapacity);
        layer.droplet.evaporationPerMeter =
            ReadFloat(*droplet, "evaporation", defaults.droplet.evaporationPerMeter);
        layer.droplet.gravity = ReadFloat(*droplet, "gravity", defaults.droplet.gravity);
        layer.droplet.multigrid = ReadBool(*droplet, "multigrid", defaults.droplet.multigrid);
        layer.droplet.iterations = ReadInt(*droplet, "iterations", defaults.droplet.iterations);
        layer.droplet.seed = ReadInt(*droplet, "seed", defaults.droplet.seed);
        layer.droplet.resolution = static_cast<uint32_t>(
            ReadInt(*droplet, "resolution", static_cast<int>(defaults.droplet.resolution)));
    }

    if (const json* scatter = FindMember(node, "scatter");
        scatter != nullptr && scatter->is_object()) {
        layer.scatter.shape = static_cast<compositor::ScatterShape>(
            EnumValue(kScatterShapeNames, *scatter, "shape",
                      static_cast<uint32_t>(defaults.scatter.shape)));
        layer.scatter.orientation = static_cast<compositor::ScatterOrientation>(
            EnumValue(kScatterOrientationNames, *scatter, "orientation",
                      static_cast<uint32_t>(defaults.scatter.orientation)));
        layer.scatter.seed = ReadInt(*scatter, "seed", defaults.scatter.seed);
        layer.scatter.densityMeters =
            ReadFloat(*scatter, "density", defaults.scatter.densityMeters);
        layer.scatter.coverage = ReadFloat(*scatter, "coverage", defaults.scatter.coverage);
        layer.scatter.sizeMinMeters =
            ReadFloat(*scatter, "sizeMin", defaults.scatter.sizeMinMeters);
        layer.scatter.sizeMaxMeters =
            ReadFloat(*scatter, "sizeMax", defaults.scatter.sizeMaxMeters);
        layer.scatter.heightMeters = ReadFloat(*scatter, "height", defaults.scatter.heightMeters);
        layer.scatter.heightJitter =
            ReadFloat(*scatter, "heightJitter", defaults.scatter.heightJitter);
        layer.scatter.rotationVariation =
            ReadFloat(*scatter, "rotationVariation", defaults.scatter.rotationVariation);
        layer.scatter.aspectVariation =
            ReadFloat(*scatter, "aspectVariation", defaults.scatter.aspectVariation);
        layer.scatter.smoothness =
            ReadFloat(*scatter, "smoothness", defaults.scatter.smoothness);
    }

    if (const json* blur = FindMember(node, "blur"); blur != nullptr && blur->is_object()) {
        layer.blur.radiusMeters = ReadFloat(*blur, "radius", defaults.blur.radiusMeters);
        layer.blur.strength = ReadFloat(*blur, "strength", defaults.blur.strength);
        layer.blur.iterations = ReadInt(*blur, "iterations", defaults.blur.iterations);
    }

    if (const json* mask = FindMember(node, "mask"); mask != nullptr && mask->is_object()) {
        ReadMask(*mask, layer.mask, readTexture, readPaint);
    }
    layer.blendRange = ReadFloat(node, "blendRange", defaults.blendRange);
    layer.wrapToUnderlying = ReadBool(node, "wrapToUnderlying", defaults.wrapToUnderlying);
    layer.uvScale = ReadFloat(node, "uvScale", defaults.uvScale);
    return layer;
}

// --- ノードグラフ ---------------------------------------------------------
//
// ノードの kind は enum の数値ではなく定義テーブルの名前で書く
// （「列挙は名前で書く」）。ピンはノードの定義から再生成するので、
// ファイルには ID の並びだけを持つ（リンクがピン ID を参照するため）。

json WriteGraph(const graph::NodeGraph& graphData, const TextureWriter& writeTexture,
                const std::function<json(compositor::MaterialAssetId)>& writeMaterial,
                const std::function<json(compositor::PaintMaskId)>& writePaint) {
    json out;
    json nodes = json::array();
    for (const graph::Node& node : graphData.Nodes()) {
        const graph::NodeDefinition* definition = graph::FindNodeDefinition(node.kind);
        if (definition == nullptr) {
            continue;
        }
        json item;
        item["id"] = node.id;
        item["kind"] = definition->name;
        item["position"] = json::array({node.posX, node.posY});
        json inputs = json::array();
        for (const graph::Pin& pin : node.inputs) {
            inputs.push_back(pin.id);
        }
        item["inputs"] = std::move(inputs);
        json outputs = json::array();
        for (const graph::Pin& pin : node.outputs) {
            outputs.push_back(pin.id);
        }
        item["outputs"] = std::move(outputs);
        if (const auto* settings = std::get_if<graph::LayerNodeSettings>(&node.settings)) {
            item["layer"] = WriteLayer(settings->layer, writeTexture, writeMaterial, writePaint);
            // 地形の実寸（m）。ソース（Heightmap）だけが持つ。
            if (graph::IsSourceNodeKind(node.kind)) {
                json scale;
                scale["size"] = settings->scale.sizeMeters;
                scale["height"] = settings->scale.heightMeters;
                item["scale"] = std::move(scale);
            }
        } else if (const auto* mask = std::get_if<graph::MaskNodeSettings>(&node.settings)) {
            // マスクのノードは種類ごとに使う設定が違うが、**全部書く**。
            // 種類を変えて戻したときに値が消えていると驚くため。
            item["map"] = WriteMapSlot(mask->map, writeTexture);
            item["noise"] = WriteNoise(mask->noise);
            item["fluvial"] = WriteFluvial(mask->fluvial);
            item["flowline"] = WriteFlowline(mask->flowline);
            item["height"] = WriteHeightMask(mask->height);
            item["slope"] = WriteSlope(mask->slope);
            item["curvature"] = WriteCurvature(mask->curvature);
            item["levels"] = WriteLevels(mask->levels);
            item["blur"] = WriteMaskBlur(mask->blur);
            item["blend"] = WriteBlend(mask->blend);
            item["maskPath"] = WritePathMask(mask->pathMask);
            item["maskArea"] = WriteAreaMask(mask->areaMask);
        } else if (const auto* cloudLine = std::get_if<graph::CloudLineSettings>(&node.settings)) {
            item["proceduralCloud"]["startX"] = cloudLine->startX;
            item["proceduralCloud"]["startY"] = cloudLine->startY;
            item["proceduralCloud"]["startZ"] = cloudLine->startZ;
            item["proceduralCloud"]["endX"] = cloudLine->endX;
            item["proceduralCloud"]["endY"] = cloudLine->endY;
            item["proceduralCloud"]["endZ"] = cloudLine->endZ;
        } else if (const auto* cloudSpheres = std::get_if<graph::CloudSpheresSettings>(&node.settings)) {
            item["proceduralCloud"]["count"] = cloudSpheres->count;
            item["proceduralCloud"]["startRadius"] = cloudSpheres->startRadius;
            item["proceduralCloud"]["endRadius"] = cloudSpheres->endRadius;
            item["proceduralCloud"]["jitter"] = cloudSpheres->jitter;
            item["proceduralCloud"]["radiusVariation"] = cloudSpheres->radiusVariation;
            item["proceduralCloud"]["seed"] = cloudSpheres->seed;
        } else if (const auto* cloudEllipsoid = std::get_if<graph::CloudEllipsoidSettings>(&node.settings)) {
            item["proceduralCloud"]["centerX"] = cloudEllipsoid->centerX;
            item["proceduralCloud"]["centerY"] = cloudEllipsoid->centerY;
            item["proceduralCloud"]["centerZ"] = cloudEllipsoid->centerZ;
            item["proceduralCloud"]["radiusX"] = cloudEllipsoid->radiusX;
            item["proceduralCloud"]["radiusY"] = cloudEllipsoid->radiusY;
            item["proceduralCloud"]["radiusZ"] = cloudEllipsoid->radiusZ;
        } else if (const auto* cloudMerge = std::get_if<graph::CloudMergeSettings>(&node.settings)) {
            item["proceduralCloud"]["smoothness"] = cloudMerge->smoothness;
        } else if (const auto* transform = std::get_if<graph::CloudTransformSettings>(&node.settings)) {
            item["proceduralCloud"]={{"translateX",transform->translateX},{"translateY",transform->translateY},{"translateZ",transform->translateZ}};
        } else if (const auto* replicate = std::get_if<graph::CloudReplicateSettings>(&node.settings)) {
            item["proceduralCloud"]={{"distribution",EnumName(kCloudReplicationNames,static_cast<uint32_t>(replicate->distribution))},
                {"packingDensity",replicate->packingDensity},{"count",replicate->count},{"radiusScale",replicate->radiusScale},
                {"radiusVariation",replicate->radiusVariation},{"jitter",replicate->jitter},
                {"smoothness",replicate->smoothness},{"seed",replicate->seed},{"keepSource",replicate->keepSource}};
        } else if (const auto* cloudNoise = std::get_if<graph::CloudNoiseSettings>(&node.settings)) {
            item["proceduralCloud"]["noiseType"] = EnumName(kProceduralCloudNoiseNames, static_cast<uint32_t>(cloudNoise->noiseType));
            item["proceduralCloud"]["scale"] = cloudNoise->scale;
            item["proceduralCloud"]["displacement"] = cloudNoise->displacement;
            item["proceduralCloud"]["detail"] = cloudNoise->detail;
            item["proceduralCloud"]["feather"] = cloudNoise->feather;
            item["proceduralCloud"]["flattenBottom"] = cloudNoise->flattenBottom;
            item["proceduralCloud"]["bottomHeight"] = cloudNoise->bottomHeight;
            item["proceduralCloud"]["bottomFeather"] = cloudNoise->bottomFeather;
            item["proceduralCloud"]["extinction"] = cloudNoise->extinction;
            item["proceduralCloud"]["indirectLight"] = cloudNoise->indirectLight;
            item["proceduralCloud"]["ambientLight"] = cloudNoise->ambientLight;
            item["proceduralCloud"]["seed"] = cloudNoise->seed;
        } else if (const auto* cloud = std::get_if<graph::CloudNodeSettings>(&node.settings)) {
            item["cloud"]["enabled"] = cloud->enabled;
            item["cloud"]["coverage"] = cloud->coverage;
            item["cloud"]["centerX"] = cloud->centerX;
            item["cloud"]["centerY"] = cloud->centerY;
            item["cloud"]["centerZ"] = cloud->centerZ;
            item["cloud"]["width"] = cloud->width;
            item["cloud"]["thickness"] = cloud->thickness;
            item["cloud"]["depth"] = cloud->depth;
            item["cloud"]["extinction"] = cloud->extinction;
            item["cloud"]["noiseScale"] = cloud->noiseScale;
            item["cloud"]["shapeStrength"] = cloud->shapeStrength;
            item["cloud"]["detailStrength"] = cloud->detailStrength;
            item["cloud"]["cellCount"] = cloud->cellCount;
            item["cloud"]["noiseType"] = EnumName(kCloudNoiseNames, static_cast<uint32_t>(cloud->noiseType));
            item["cloud"]["edgeSoftness"] = cloud->edgeSoftness;
            item["cloud"]["flatBottom"] = cloud->flatBottom;
            item["cloud"]["bottomFlatness"] = cloud->bottomFlatness;
            item["cloud"]["indirectLight"] = cloud->indirectLight;
            item["cloud"]["ambientLight"] = cloud->ambientLight;
            item["cloud"]["seed"] = cloud->seed;
            item["cloud"]["animate"] = cloud->animate;
            item["cloud"]["motionMode"] = cloud->motionMode == 2 ? "drift" : cloud->motionMode == 1 ? "flow" : "translate";
            item["cloud"]["noiseSpeedRatio"] = cloud->noiseSpeedRatio;
            item["cloud"]["windSpeed"] = cloud->windSpeed;
            item["cloud"]["windDirection"] = cloud->windDirection;
        } else if (const auto* path = std::get_if<graph::PathNodeSettings>(&node.settings)) {
            item["path"] = WritePath(path->path);
        }
        nodes.push_back(std::move(item));
    }
    out["nodes"] = std::move(nodes);

    json links = json::array();
    for (const graph::Link& link : graphData.Links()) {
        json item;
        item["id"] = link.id;
        item["start"] = link.startPin;
        item["end"] = link.endPin;
        links.push_back(std::move(item));
    }
    out["links"] = std::move(links);
    return out;
}

// 戻り値はノードを 1 つ以上読めたか。空のグラフ節は「グラフ未使用」とみなし、
// 呼び出し側が旧 layers からの移行に切り替える。
bool ReadGraph(const json& node, graph::NodeGraph& graphData, const TextureReader& readTexture,
               const std::function<compositor::MaterialAssetId(const json&)>& readMaterial,
               const std::function<compositor::PaintMaskId(const json&)>& readPaint,
               const graph::TerrainScale& scaleFallback) {
    std::vector<graph::Node> nodes;
    std::vector<graph::Link> links;
    graph::GraphId maxId = 0;

    // **リンクの ID を先に見ておく。** ノードの種類にピンを足した後で古いファイルを
    // 開くと、足りないピンへ「いまの最大 + 1」を振ることになる。リンクを読む前に
    // 振ると、既にあるリンクの ID とぶつかる（ノード / ピン / リンクは同じ ID 空間）。
    if (const json* items = FindMember(node, "links"); items != nullptr && items->is_array()) {
        for (const json& item : *items) {
            if (!item.is_object()) {
                continue;
            }
            maxId = std::max(maxId, static_cast<graph::GraphId>(ReadInt(item, "id", 0)));
            maxId = std::max(maxId, static_cast<graph::GraphId>(ReadInt(item, "start", 0)));
            maxId = std::max(maxId, static_cast<graph::GraphId>(ReadInt(item, "end", 0)));
        }
    }

    if (const json* items = FindMember(node, "nodes"); items != nullptr && items->is_array()) {
        for (const json& item : *items) {
            if (!item.is_object()) {
                continue;
            }
            const graph::NodeDefinition* definition =
                graph::FindNodeDefinitionByName(ReadString(item, "kind"));
            const int id = ReadInt(item, "id", 0);
            if (definition == nullptr || id <= 0) {
                // 知らない種類は捨てる（将来のビルドで増えた種類を古いビルドで開いた場合）。
                continue;
            }
            graph::Node created;
            created.id = id;
            created.kind = definition->kind;
            maxId = std::max(maxId, created.id);
            if (const json* position = FindMember(item, "position");
                position != nullptr && position->is_array() && position->size() >= 2 &&
                (*position)[0].is_number() && (*position)[1].is_number()) {
                created.posX = (*position)[0].get<float>();
                created.posY = (*position)[1].get<float>();
                // 壊れた座標（非有限・極端に大きい値）は「位置なし」へ落とす。
                // エディタへ流し込むとキャンバスの座標計算が壊れるため、
                // パネル側がビュー中央へ置き直す。
                created.positionValid = std::isfinite(created.posX) &&
                                        std::isfinite(created.posY) &&
                                        std::abs(created.posX) <= 1.0e6f &&
                                        std::abs(created.posY) <= 1.0e6f;
            }

            // ピンは定義から再生成し、ID だけファイルの値を使う。
            // 欠けているぶんは後で maxId から振り直す（リンクは繋がらないまま消える）。
            const json* inputIds = FindMember(item, "inputs");
            const json* outputIds = FindMember(item, "outputs");
            size_t inputIndex = 0;
            size_t outputIndex = 0;
            for (const graph::PinDefinition& pin : definition->pins) {
                graph::Pin createdPin;
                createdPin.nodeId = created.id;
                createdPin.kind = pin.kind;
                createdPin.valueType = pin.valueType;
                createdPin.label = pin.label;
                const json* ids = (pin.kind == graph::PinKind::Input) ? inputIds : outputIds;
                size_t& index = (pin.kind == graph::PinKind::Input) ? inputIndex : outputIndex;
                if (ids != nullptr && ids->is_array() && index < ids->size() &&
                    (*ids)[index].is_number_integer()) {
                    createdPin.id = (*ids)[index].get<int>();
                }
                ++index;
                maxId = std::max(maxId, createdPin.id);
                if (pin.kind == graph::PinKind::Input) {
                    created.inputs.push_back(std::move(createdPin));
                } else {
                    created.outputs.push_back(std::move(createdPin));
                }
            }

            // Cloud Merge の可変入力をすべて復元してからリンクを解決する。
            // 旧形式の固定A/Bピンも同じIDで引き継ぐ。
            if (created.kind==graph::NodeKind::CloudMerge && inputIds && inputIds->is_array()) {
                for (size_t i=created.inputs.size();i<inputIds->size();++i) {
                    graph::Pin pin;
                    pin.nodeId=created.id; pin.kind=graph::PinKind::Input; pin.valueType=graph::ValueType::CloudShape;
                    if ((*inputIds)[i].is_number_integer()) pin.id=(*inputIds)[i].get<int>();
                    maxId=std::max(maxId,pin.id);
                    created.inputs.push_back(std::move(pin));
                }
            }

            if (graph::IsLayerNodeKind(created.kind)) {
                graph::LayerNodeSettings settings;
                if (const json* layer = FindMember(item, "layer");
                    layer != nullptr && layer->is_object()) {
                    settings.layer = ReadLayer(*layer, readTexture, readMaterial, readPaint);
                }
                // scale を持たないのは、実寸をノードへ移す前に保存されたファイル。
                // **プレビュー設定の値を引き継ぐ**（既定値を入れると地形の
                // 大きさが勝手に変わってしまう）。
                settings.scale = scaleFallback;
                if (const json* scale = FindMember(item, "scale");
                    scale != nullptr && scale->is_object()) {
                    settings.scale.sizeMeters =
                        ReadFloat(*scale, "size", scaleFallback.sizeMeters);
                    settings.scale.heightMeters =
                        ReadFloat(*scale, "height", scaleFallback.heightMeters);
                }
                // 種類とレイヤー種別は常に一致させる（ファイルの食い違いは種類を信じる）。
                settings.layer.kind = graph::LayerKindFor(created.kind);
                created.settings = std::move(settings);
            } else if (graph::IsMaskNodeKind(created.kind)) {
                graph::MaskNodeSettings settings;
                settings.map = ReadMapSlot(item, "map", readTexture);
                settings.noise = ReadNoise(item, "noise", graph::MaskNodeSettings().noise);
                settings.fluvial = ReadFluvial(item, "fluvial");
                settings.flowline = ReadFlowline(item);
                settings.height = ReadHeightMask(item, "height");
                settings.slope = ReadSlope(item, "slope");
                settings.curvature = ReadCurvature(item, "curvature");
                settings.levels = ReadLevels(item, "levels");
                settings.blur = ReadMaskBlur(item, "blur");
                settings.blend = ReadBlend(item, "blend");
                settings.pathMask = ReadPathMask(item, "maskPath");
                settings.areaMask = ReadAreaMask(item, "maskArea");
                created.settings = std::move(settings);
            } else if (created.kind == graph::NodeKind::CloudLine) {
                graph::CloudLineSettings settings;
                if (const auto* shape = FindMember(item, "proceduralCloud"); shape && shape->is_object()) {
                    settings.startX = std::clamp(ReadFloat(*shape, "startX", settings.startX), -10000.0f, 10000.0f);
                    settings.startY = std::clamp(ReadFloat(*shape, "startY", settings.startY), -10000.0f, 10000.0f);
                    settings.startZ = std::clamp(ReadFloat(*shape, "startZ", settings.startZ), -10000.0f, 10000.0f);
                    settings.endX = std::clamp(ReadFloat(*shape, "endX", settings.endX), -10000.0f, 10000.0f);
                    settings.endY = std::clamp(ReadFloat(*shape, "endY", settings.endY), -10000.0f, 10000.0f);
                    settings.endZ = std::clamp(ReadFloat(*shape, "endZ", settings.endZ), -10000.0f, 10000.0f);
                }
                created.settings = settings;
            } else if (created.kind == graph::NodeKind::CloudSpheres) {
                graph::CloudSpheresSettings settings;
                if (const auto* shape = FindMember(item, "proceduralCloud"); shape && shape->is_object()) {
                    settings.count = std::max(ReadInt(*shape, "count", settings.count), 1);
                    settings.startRadius = std::clamp(ReadFloat(*shape, "startRadius", settings.startRadius), 10.0f, 3000.0f);
                    settings.endRadius = std::clamp(ReadFloat(*shape, "endRadius", settings.endRadius), 10.0f, 3000.0f);
                    settings.jitter = std::clamp(ReadFloat(*shape, "jitter", settings.jitter), 0.0f, 1.0f);
                    settings.radiusVariation = std::clamp(ReadFloat(*shape, "radiusVariation", settings.radiusVariation), 0.0f, 0.9f);
                    settings.seed = std::clamp(ReadInt(*shape, "seed", settings.seed), 0, 10000);
                }
                created.settings = settings;
            } else if (created.kind == graph::NodeKind::CloudEllipsoid) {
                graph::CloudEllipsoidSettings settings;
                if (const auto* shape = FindMember(item, "proceduralCloud"); shape && shape->is_object()) {
                    settings.centerX = std::clamp(ReadFloat(*shape, "centerX", settings.centerX), -10000.0f, 10000.0f);
                    settings.centerY = std::clamp(ReadFloat(*shape, "centerY", settings.centerY), -10000.0f, 10000.0f);
                    settings.centerZ = std::clamp(ReadFloat(*shape, "centerZ", settings.centerZ), -10000.0f, 10000.0f);
                    settings.radiusX = std::clamp(ReadFloat(*shape, "radiusX", settings.radiusX), 10.0f, 3000.0f);
                    settings.radiusY = std::clamp(ReadFloat(*shape, "radiusY", settings.radiusY), 10.0f, 3000.0f);
                    settings.radiusZ = std::clamp(ReadFloat(*shape, "radiusZ", settings.radiusZ), 10.0f, 3000.0f);
                }
                created.settings = settings;
            } else if (created.kind == graph::NodeKind::CloudMerge) {
                graph::CloudMergeSettings settings;
                if (const auto* shape = FindMember(item, "proceduralCloud"); shape && shape->is_object()) {
                    settings.smoothness = std::clamp(ReadFloat(*shape, "smoothness", settings.smoothness), 0.0f, 500.0f);
                }
                created.settings = settings;
            } else if (created.kind == graph::NodeKind::CloudTransform) {
                graph::CloudTransformSettings settings;
                if (const auto* shape=FindMember(item,"proceduralCloud"); shape && shape->is_object()) {
                    settings.translateX=std::clamp(ReadFloat(*shape,"translateX",0),-10000.0f,10000.0f);
                    settings.translateY=std::clamp(ReadFloat(*shape,"translateY",0),-10000.0f,10000.0f);
                    settings.translateZ=std::clamp(ReadFloat(*shape,"translateZ",0),-10000.0f,10000.0f);
                }
                created.settings=settings;
            } else if (created.kind == graph::NodeKind::CloudReplicate) {
                graph::CloudReplicateSettings settings;
                settings.distribution=0; // 未指定の既存ノードは元の個数と配置を維持。
                if (const auto* shape=FindMember(item,"proceduralCloud");shape && shape->is_object()) {
                    settings.distribution=static_cast<int>(EnumValue(kCloudReplicationNames,*shape,"distribution",0));
                    settings.packingDensity=std::clamp(ReadFloat(*shape,"packingDensity",settings.packingDensity),0.0f,10000.0f);
                    settings.count=std::max(ReadInt(*shape,"count",settings.count),1);
                    settings.radiusScale=std::clamp(ReadFloat(*shape,"radiusScale",settings.radiusScale),0.05f,1.0f);
                    settings.radiusVariation=std::clamp(ReadFloat(*shape,"radiusVariation",settings.radiusVariation),0.0f,0.9f);
                    settings.jitter=std::clamp(ReadFloat(*shape,"jitter",settings.jitter),0.0f,1.0f);
                    settings.smoothness=std::clamp(ReadFloat(*shape,"smoothness",settings.smoothness),0.0f,500.0f);
                    settings.seed=std::clamp(ReadInt(*shape,"seed",settings.seed),0,10000);
                    settings.keepSource=ReadBool(*shape,"keepSource",settings.keepSource);
                }
                created.settings=settings;
            } else if (created.kind == graph::NodeKind::CloudNoise) {
                graph::CloudNoiseSettings settings;
                if (const auto* shape = FindMember(item, "proceduralCloud"); shape && shape->is_object()) {
                    settings.noiseType = static_cast<int>(EnumValue(kProceduralCloudNoiseNames, *shape, "noiseType", 0));
                    settings.scale = std::clamp(ReadFloat(*shape, "scale", settings.scale), 10.0f, 5000.0f);
                    settings.displacement = std::clamp(ReadFloat(*shape, "displacement", settings.displacement), 0.0f, 500.0f);
                    settings.detail = std::clamp(ReadFloat(*shape, "detail", settings.detail), 0.0f, 200.0f);
                    settings.feather = std::clamp(ReadFloat(*shape, "feather", settings.feather), 1.0f, 300.0f);
                    settings.flattenBottom=ReadBool(*shape,"flattenBottom",settings.flattenBottom);
                    settings.bottomHeight=std::clamp(ReadFloat(*shape,"bottomHeight",settings.bottomHeight),-20000.0f,20000.0f);
                    settings.bottomFeather=std::clamp(ReadFloat(*shape,"bottomFeather",settings.bottomFeather),0.0f,300.0f);
                    settings.extinction = std::clamp(ReadFloat(*shape, "extinction", settings.extinction), 0.0001f, 0.03f);
                    settings.indirectLight = std::clamp(ReadFloat(*shape, "indirectLight", settings.indirectLight), 0.0f, 5.0f);
                    settings.ambientLight = std::clamp(ReadFloat(*shape, "ambientLight", settings.ambientLight), 0.0f, 5.0f);
                    settings.seed = std::clamp(ReadInt(*shape, "seed", settings.seed), 0, 10000);
                }
                created.settings = settings;
            } else if (created.kind == graph::NodeKind::Cloud || created.kind == graph::NodeKind::CloudLayer) {
                graph::CloudNodeSettings settings;
                settings.flatBottom = false; // 未指定の既存ノードは従来形状を維持する。
                if (const json* cloud = FindMember(item, "cloud"); cloud && cloud->is_object()) {
                    settings.enabled = ReadBool(*cloud, "enabled", settings.enabled);
                    settings.coverage = std::clamp(ReadFloat(*cloud, "coverage", settings.coverage), 0.0f, 1.0f);
                    settings.animate = ReadBool(*cloud, "animate", settings.animate);
                    const auto motionMode = ReadString(*cloud, "motionMode", "translate");
                    settings.motionMode = motionMode == "drift" ? 2 : motionMode == "flow" ? 1 : 0;
                    settings.noiseSpeedRatio = std::clamp(ReadFloat(*cloud, "noiseSpeedRatio", settings.noiseSpeedRatio), 0.0f, 1.0f);
                    settings.windSpeed = std::clamp(ReadFloat(*cloud, "windSpeed", settings.windSpeed), 0.0f, 1000.0f);
                    settings.windDirection = std::clamp(ReadFloat(*cloud, "windDirection", settings.windDirection), -180.0f, 180.0f);
                    settings.seed = std::clamp(ReadInt(*cloud, "seed", settings.seed), 0, 10000);
                    settings.centerX = std::clamp(ReadFloat(*cloud, "centerX", settings.centerX), -10000.0f, 10000.0f);
                    settings.centerY = std::clamp(ReadFloat(*cloud, "centerY", settings.centerY), -10000.0f, 10000.0f);
                    settings.centerZ = std::clamp(ReadFloat(*cloud, "centerZ", settings.centerZ), -10000.0f, 10000.0f);
                    settings.width = std::clamp(ReadFloat(*cloud, "width", settings.width), 10.0f, 20000.0f);
                    settings.thickness = std::clamp(ReadFloat(*cloud, "thickness", settings.thickness), 10.0f, 20000.0f);
                    settings.depth = std::clamp(ReadFloat(*cloud, "depth", settings.depth), 10.0f, 20000.0f);
                    settings.noiseScale = std::clamp(ReadFloat(*cloud, "noiseScale", settings.noiseScale), 10.0f, 20000.0f);
                    settings.extinction = std::clamp(ReadFloat(*cloud, "extinction", settings.extinction), 0.0001f, 0.03f);
                    settings.shapeStrength = std::clamp(ReadFloat(*cloud, "shapeStrength", settings.shapeStrength), 0.0f, 1.0f);
                    settings.detailStrength = std::clamp(ReadFloat(*cloud, "detailStrength", settings.detailStrength), 0.0f, 1.0f);
                    settings.cellCount = static_cast<int>(std::clamp(ReadUInt(*cloud, "cellCount", 10), 1u, 32u));
                    settings.noiseType = static_cast<int>(EnumValue(kCloudNoiseNames, *cloud, "noiseType", 0));
                    settings.edgeSoftness = std::clamp(ReadFloat(*cloud, "edgeSoftness", settings.edgeSoftness), 0.02f, 1.0f);
                    settings.flatBottom = ReadBool(*cloud, "flatBottom", false);
                    settings.bottomFlatness = std::clamp(ReadFloat(*cloud, "bottomFlatness", settings.bottomFlatness), 0.0f, 1.0f);
                    settings.indirectLight = std::clamp(ReadFloat(*cloud, "indirectLight", settings.indirectLight), 0.0f, 5.0f);
                    settings.ambientLight = std::clamp(ReadFloat(*cloud, "ambientLight", settings.ambientLight), 0.0f, 5.0f);
                }
                created.settings = settings;
            } else if (created.kind == graph::NodeKind::Path) {
                graph::PathNodeSettings settings;
                settings.path = ReadPath(item, "path");
                created.settings = std::move(settings);
            } else {
                created.settings = graph::OutputNodeSettings{};
            }
            nodes.push_back(std::move(created));
        }
    }

    // ID が欠けていたピンへ新しい番号を振る（0 のままだと ID が衝突する）。
    for (graph::Node& created : nodes) {
        for (graph::Pin& pin : created.inputs) {
            if (pin.id <= 0) {
                pin.id = ++maxId;
            }
        }
        for (graph::Pin& pin : created.outputs) {
            if (pin.id <= 0) {
                pin.id = ++maxId;
            }
        }
    }

    if (const json* items = FindMember(node, "links"); items != nullptr && items->is_array()) {
        for (const json& item : *items) {
            if (!item.is_object()) {
                continue;
            }
            graph::Link link;
            link.id = ReadInt(item, "id", 0);
            link.startPin = ReadInt(item, "start", 0);
            link.endPin = ReadInt(item, "end", 0);
            if (link.id <= 0 || link.startPin <= 0 || link.endPin <= 0) {
                continue;
            }
            links.push_back(link);
        }
    }

    if (nodes.empty()) {
        return false;
    }
    // Replace が壊れたリンクの除去と次の採番の再構築を行う。
    graphData.Replace(std::move(nodes), std::move(links));
    return true;
}

// 旧形式（版 3 以前）の layers[] をグラフへ移行する。
// 下から上のレイヤー列を「下地」チェーンとして繋ぎ、末尾を出力ノードへ繋ぐ。
// CompileLayers() が同じ列を返すので、見た目は移行前と変わらない。
graph::NodeGraph MigrateLayersToGraph(std::vector<compositor::MaterialLayer> layers) {
    graph::NodeGraph migrated;
    if (layers.empty()) {
        return graph::NodeGraph::CreateDefault();
    }
    graph::GraphId previousOutput = 0;
    float x = 60.0f;
    for (compositor::MaterialLayer& layer : layers) {
        graph::NodeKind kind = graph::NodeKind::Surface;
        if (layer.kind == compositor::LayerKind::Shape) {
            kind = graph::NodeKind::Shape;
        } else if (layer.kind == compositor::LayerKind::Liquid) {
            kind = graph::NodeKind::Liquid;
        }
        const graph::GraphId nodeId = migrated.CreateNode(kind);
        graph::Node* node = migrated.FindMutableNode(nodeId);
        if (node == nullptr) {
            continue;
        }
        if (auto* settings = std::get_if<graph::LayerNodeSettings>(&node->settings)) {
            settings->layer = std::move(layer);
        }
        node->posX = x;
        node->posY = 120.0f;
        node->positionValid = true;
        x += 240.0f;
        if (previousOutput != 0 && !node->inputs.empty()) {
            migrated.CreateLink(previousOutput, node->inputs.front().id);
        }
        previousOutput = node->outputs.empty() ? 0 : node->outputs.front().id;
    }
    const graph::GraphId outputId = migrated.CreateNode(graph::NodeKind::Output);
    if (graph::Node* output = migrated.FindMutableNode(outputId)) {
        output->posX = x;
        output->posY = 120.0f;
        output->positionValid = true;
        if (previousOutput != 0 && !output->inputs.empty()) {
            migrated.CreateLink(previousOutput, output->inputs.front().id);
        }
    }
    return migrated;
}

// --- プレビューの設定 -----------------------------------------------------

// 天球アセット（M5b-2）で HDRI のパスが preview から抜けたため、パスの解決は不要になった。
json WritePreview(renderer::PreviewRenderer& renderer) {
    json node;
    node["tonemap"] = EnumName(kTonemapNames, static_cast<uint32_t>(renderer.Tonemap()));
    node["useMaterialTextures"] = renderer.UseMaterialTextures();
    node["maskSaturationHatch"] = renderer.MaskSaturationHatch();
    node["displacementScale"] = renderer.DisplacementScale();
    node["planeSize"] = renderer.PlaneSize();
    node["tessellation"] = renderer.TessellationEnabled();
    node["tessellationFactor"] = renderer.TessellationFactor();
    node["materialResolution"] = renderer.MaterialResolution();
    node["meshSubdivisions"] = renderer.MeshSubdivisions();
    node["showSkybox"] = renderer.ShowSkybox();
    node["skyboxBlur"] = renderer.SkyboxBlur();
    node["shadow"] = renderer.ShadowEnabled();
    node["lightingMode"] = renderer.AtmosphericMode() ? "atmospheric" : "ibl";
    const auto& atmosphere = renderer.AtmosphericSettings();
    const auto& sun = renderer.AtmosphericLight();
    json atmosphereNode;
    atmosphereNode["azimuth"] = sun.azimuth;
    atmosphereNode["elevation"] = sun.elevation;
    atmosphereNode["illuminance"] = sun.illuminance;
    atmosphereNode["density"] = atmosphere.density;
    atmosphereNode["mie"] = atmosphere.mie;
    atmosphereNode["eccentricity"] = atmosphere.eccentricity;
    atmosphereNode["altitude"] = atmosphere.altitude;
    atmosphereNode["groundAlbedo"] = atmosphere.groundAlbedo;
    atmosphereNode["lowerHemisphere"] = atmosphere.lowerHemisphere == 0 ? "skyExtension" : "groundReflection";
    atmosphereNode["clouds"] = atmosphere.clouds != 0;
    atmosphereNode["indirectLight"] = atmosphere.indirectLight;
    atmosphereNode["ambientLight"] = atmosphere.ambientLight;
    atmosphereNode["coverage"] = atmosphere.coverage;
    atmosphereNode["extinction"] = atmosphere.extinction;
    atmosphereNode["cloudBottom"] = atmosphere.cloudBottom;
    atmosphereNode["cloudThickness"] = atmosphere.cloudThickness;
    atmosphereNode["cloudScale"] = atmosphere.cloudScale;
    atmosphereNode["cloudNoiseType"] = EnumName(kCloudNoiseNames, atmosphere.cloudNoiseType);
    atmosphereNode["skylightIntensity"] = renderer.AtmosphericEnvironmentIntensity();
    atmosphereNode["animateClouds"] = atmosphere.animateClouds != 0;
    atmosphereNode["windSpeed"] = atmosphere.windSpeed;
    atmosphereNode["windDirection"] = atmosphere.windDirection;
    atmosphereNode["fieldCenterX"] = atmosphere.fieldCenterX;
    atmosphereNode["fieldCenterZ"] = atmosphere.fieldCenterZ;
    atmosphereNode["fieldRadius"] = atmosphere.fieldRadius;
    atmosphereNode["fieldFalloff"] = atmosphere.fieldFalloff;
    atmosphereNode["seed"] = atmosphere.seed;
    atmosphereNode["samples"] = atmosphere.samples;
    atmosphereNode["godRays"] = renderer.GodRays().enabled;
    atmosphereNode["godRayDensity"] = renderer.GodRays().density;
    atmosphereNode["godRayDistance"] = renderer.GodRays().distance;
    atmosphereNode["fullResolutionClouds"] = renderer.FullResolutionClouds();
    atmosphereNode["cloudLightingCache"] = renderer.CloudLightingCache();
    node["atmosphere"] = std::move(atmosphereNode);


    // 被写界深度。見え方だけの設定だが、プロジェクトごとに変えるものなので残す。
    const renderer::DofSettings& dof = renderer.Dof();
    json dofNode;
    dofNode["enabled"] = dof.enabled;
    dofNode["focusOnTarget"] = dof.focusOnTarget;
    dofNode["focusDistance"] = dof.focusDistance;
    dofNode["blurScale"] = dof.blurScale;
    dofNode["miniatureScale"] = dof.miniatureScale;
    dofNode["maxBlurPixels"] = dof.maxBlurPixels;
    dofNode["shape"] = EnumName(kApertureShapeNames, static_cast<uint32_t>(dof.shape));
    dofNode["rotationDegrees"] = dof.rotationDegrees;
    node["depthOfField"] = std::move(dofNode);
    // 環境そのもの（HDRI・較正値・空のパラメータ）は天球アセットが持つ。
    // ここには「見え方」だけを書く。

    const renderer::CameraState camera = renderer.GetCamera().State();
    json cameraNode;
    cameraNode["target"] = WriteFloat3(camera.target);
    cameraNode["distance"] = camera.distance;
    cameraNode["yaw"] = camera.yaw;
    cameraNode["pitch"] = camera.pitch;
    cameraNode["fovY"] = camera.fovY;
    node["camera"] = std::move(cameraNode);

    const renderer::LightSettings& light = renderer.LegacyLight();
    json lightNode;
    lightNode["azimuth"] = light.azimuth;
    lightNode["elevation"] = light.elevation;
    lightNode["illuminance"] = light.illuminance;
    lightNode["color"] = WriteFloat3(light.color);
    node["light"] = std::move(lightNode);

    const renderer::ExposureSettings& exposure = renderer.Exposure();
    json exposureNode;
    exposureNode["useManualEv"] = exposure.useManualEv;
    exposureNode["manualEv100"] = exposure.manualEv100;
    exposureNode["aperture"] = exposure.aperture;
    exposureNode["shutterSpeed"] = exposure.shutterSpeed;
    exposureNode["iso"] = exposure.iso;
    node["exposure"] = std::move(exposureNode);

    const renderer::MaterialSettings& material = renderer.Material();
    json materialNode;
    materialNode["baseColor"] = WriteFloat3(material.baseColor);
    materialNode["roughness"] = material.roughness;
    materialNode["metallic"] = material.metallic;
    node["flatMaterial"] = std::move(materialNode);
    return node;
}

void ReadPreview(const json& node, renderer::PreviewRenderer& renderer) {
    renderer.ResetAtmosphereAnimation();
    // 既定値は renderer::kPreviewDefaults の一択。数値を直接書かない。
    // 名前は各節ローカルの defaults（LightSettings など）と衝突させない。
    const renderer::PreviewDefaults& previewDefaults = renderer::kPreviewDefaults;
    renderer.AtmosphericMode() = ReadString(node, "lightingMode", "ibl") == "atmospheric";
    {
        const json empty = json::object();
        const auto it = node.find("atmosphere");
        const json& source = it != node.end() && it->is_object() ? *it : empty;
        auto& atmosphere = renderer.AtmosphericSettings();
        auto& sun = renderer.AtmosphericLight();
        const renderer::AtmosphereSettings defaults;
        renderer.AtmosphericEnvironmentIntensity() = std::clamp(ReadFloat(source, "skylightIntensity", renderer::PreviewRenderer::DefaultSkylightIntensity), 0.0f, 8.0f);
        atmosphere.animateClouds = ReadBool(source, "animateClouds", false) ? 1u : 0u;
        atmosphere.windSpeed = std::clamp(ReadFloat(source, "windSpeed", defaults.windSpeed), 0.0f, 1000.0f);
        atmosphere.windDirection = std::clamp(ReadFloat(source, "windDirection", defaults.windDirection), -3.14159265f, 3.14159265f);
        sun.azimuth = std::clamp(ReadFloat(source, "azimuth", defaults.azimuth), -3.1415927f, 3.1415927f);
        sun.elevation = std::clamp(ReadFloat(source, "elevation", defaults.elevation), -1.55334f, 1.55334f);
        sun.illuminance = std::clamp(ReadFloat(source, "illuminance", defaults.illuminance), 0.0f, 200000.0f);
        sun.color = {1.0f, 1.0f, 1.0f};
        atmosphere.density = std::clamp(ReadFloat(source, "density", defaults.density), .1f, 3.0f);
        atmosphere.mie = std::clamp(ReadFloat(source, "mie", defaults.mie), 0.0f, 2.0f);
        atmosphere.eccentricity = std::clamp(ReadFloat(source, "eccentricity", defaults.eccentricity), 0.0f, .95f);
        atmosphere.altitude = std::clamp(ReadFloat(source, "altitude", defaults.altitude), 0.0f, 10000.0f);
        atmosphere.lowerHemisphere = ReadString(source, "lowerHemisphere", "groundReflection") == "skyExtension" ? 0u : 1u;
        atmosphere.groundAlbedo = std::clamp(ReadFloat(source, "groundAlbedo", defaults.groundAlbedo), 0.0f, 1.0f);
        atmosphere.coverage = std::clamp(ReadFloat(source, "coverage", defaults.coverage), 0.0f, 1.0f);
        atmosphere.cloudNoiseType = EnumValue(kCloudNoiseNames, source, "cloudNoiseType", defaults.cloudNoiseType);
        atmosphere.extinction = std::clamp(ReadFloat(source, "extinction", defaults.extinction), .0001f, .03f);
        atmosphere.cloudBottom = std::clamp(ReadFloat(source, "cloudBottom", defaults.cloudBottom), -10000.0f, 10000.0f);
        atmosphere.indirectLight = std::clamp(ReadFloat(source, "indirectLight", defaults.indirectLight), 0.0f, 5.0f);
        atmosphere.ambientLight = std::clamp(ReadFloat(source, "ambientLight", defaults.ambientLight), 0.0f, 5.0f);
        atmosphere.cloudThickness = std::clamp(ReadFloat(source, "cloudThickness", defaults.cloudThickness), 10.0f, 6000.0f);
        atmosphere.cloudScale = std::clamp(ReadFloat(source, "cloudScale", defaults.cloudScale), 10.0f, 40000.0f);
        atmosphere.fieldCenterX = std::clamp(ReadFloat(source, "fieldCenterX", defaults.fieldCenterX), -200000.0f, 200000.0f);
        atmosphere.fieldCenterZ = std::clamp(ReadFloat(source, "fieldCenterZ", defaults.fieldCenterZ), -200000.0f, 200000.0f);
        atmosphere.fieldRadius = std::clamp(ReadFloat(source, "fieldRadius", defaults.fieldRadius), 1.0f, 200000.0f);
        atmosphere.fieldFalloff = std::clamp(ReadFloat(source, "fieldFalloff", defaults.fieldFalloff), 1.0f, 50000.0f);
        atmosphere.clouds = ReadBool(source, "clouds", ReadUInt(source, "clouds", defaults.clouds) != 0) ? 1u : 0u;
        atmosphere.seed = std::min(ReadUInt(source, "seed", defaults.seed), 10000u);
        const renderer::GodRaySettings rayDefaults;
        auto& rays = renderer.GodRays();
        rays.enabled = ReadBool(source, "godRays", rayDefaults.enabled);
        rays.density = std::clamp(ReadFloat(source, "godRayDensity", rayDefaults.density), 0.0f, 0.0002f);
        rays.distance = std::clamp(ReadFloat(source, "godRayDistance", rayDefaults.distance), 100.0f, 20000.0f);
        renderer.FullResolutionClouds() = ReadBool(source, "fullResolutionClouds", false);
        renderer.CloudLightingCache() = ReadBool(source, "cloudLightingCache", true);
        const auto samples = ReadUInt(source, "samples", defaults.samples);
        atmosphere.samples = samples <= 32 ? 32u : samples <= 64 ? 64u : 128u;
    }

    renderer.Tonemap() = static_cast<renderer::TonemapMode>(
        EnumValue(kTonemapNames, node, "tonemap", static_cast<uint32_t>(previewDefaults.tonemap)));
    renderer.UseMaterialTextures() =
        ReadBool(node, "useMaterialTextures", previewDefaults.useMaterialTextures);
    renderer.MaskSaturationHatch() =
        ReadBool(node, "maskSaturationHatch", previewDefaults.maskSaturationHatch);
    renderer.DisplacementScale() =
        ReadFloat(node, "displacementScale", previewDefaults.displacementScale);
    // 平面のサイズ（m）。**カメラより先に読む。** 軌道の距離の範囲がこれで決まるので、
    // 後に読むと地形スケールのカメラ位置が素材スケールの範囲へ丸められる。
    renderer.PlaneSize() = ReadFloat(node, "planeSize", previewDefaults.planeSize);
    renderer.TessellationEnabled() =
        ReadBool(node, "tessellation", previewDefaults.tessellationEnabled);
    renderer.TessellationFactor() =
        ReadFloat(node, "tessellationFactor", previewDefaults.tessellationFactor);
    renderer.RequestMaterialResolution(
        ReadUInt(node, "materialResolution", previewDefaults.materialResolution));
    renderer.RequestMeshSubdivisions(
        ReadUInt(node, "meshSubdivisions", previewDefaults.meshSubdivisions));
    renderer.ShowSkybox() = ReadBool(node, "showSkybox", previewDefaults.showSkybox);
    renderer.SkyboxBlur() = ReadBool(node, "skyboxBlur", previewDefaults.skyboxBlur);
    renderer.ShadowEnabled() = ReadBool(node, "shadow", previewDefaults.shadowEnabled);

    // 節が丸ごと欠けていても既定値で埋める。file-format.md の「欠けているキーは
    // 既定値で埋める」に合わせる（節ごと飛ばすと前のプロジェクトの値が残る）。
    const json emptySection = json::object();
    const auto section = [&node, &emptySection](const char* key) -> const json& {
        const json* member = FindMember(node, key);
        return (member != nullptr && member->is_object()) ? *member : emptySection;
    };

    {
        const json& camera = section("camera");
        renderer::CameraState state;
        state.target = ReadFloat3(camera, "target", state.target);
        state.distance = ReadFloat(camera, "distance", state.distance);
        state.yaw = ReadFloat(camera, "yaw", state.yaw);
        state.pitch = ReadFloat(camera, "pitch", state.pitch);
        state.fovY = ReadFloat(camera, "fovY", state.fovY);
        renderer.GetCamera().SetState(state);
    }

    {
        const json& light = section("light");
        renderer::LightSettings& target = renderer.LegacyLight();
        const renderer::LightSettings defaults;
        target.azimuth = ReadFloat(light, "azimuth", defaults.azimuth);
        target.elevation = ReadFloat(light, "elevation", defaults.elevation);
        target.illuminance = ReadFloat(light, "illuminance", defaults.illuminance);
        target.color = ReadFloat3(light, "color", defaults.color);
    }

    {
        const json& exposure = section("exposure");
        renderer::ExposureSettings& target = renderer.Exposure();
        const renderer::ExposureSettings defaults;
        target.useManualEv = ReadBool(exposure, "useManualEv", defaults.useManualEv);
        target.manualEv100 = ReadFloat(exposure, "manualEv100", defaults.manualEv100);
        target.aperture = ReadFloat(exposure, "aperture", defaults.aperture);
        target.shutterSpeed = ReadFloat(exposure, "shutterSpeed", defaults.shutterSpeed);
        target.iso = ReadFloat(exposure, "iso", defaults.iso);
    }

    {
        const json& dofNode = section("depthOfField");
        renderer::DofSettings& target = renderer.Dof();
        const renderer::DofSettings defaults;
        target.enabled = ReadBool(dofNode, "enabled", defaults.enabled);
        target.focusOnTarget = ReadBool(dofNode, "focusOnTarget", defaults.focusOnTarget);
        target.focusDistance = ReadFloat(dofNode, "focusDistance", defaults.focusDistance);
        target.blurScale = ReadFloat(dofNode, "blurScale", defaults.blurScale);
        target.miniatureScale =
            ReadFloat(dofNode, "miniatureScale", defaults.miniatureScale);
        target.maxBlurPixels = ReadFloat(dofNode, "maxBlurPixels", defaults.maxBlurPixels);
        target.shape = static_cast<renderer::ApertureShape>(EnumValue(
            kApertureShapeNames, dofNode, "shape", static_cast<uint32_t>(defaults.shape)));
        target.rotationDegrees =
            ReadFloat(dofNode, "rotationDegrees", defaults.rotationDegrees);
    }

    {
        const json& flat = section("flatMaterial");
        renderer::MaterialSettings& target = renderer.Material();
        const renderer::MaterialSettings defaults;
        target.baseColor = ReadFloat3(flat, "baseColor", defaults.baseColor);
        target.roughness = ReadFloat(flat, "roughness", defaults.roughness);
        target.metallic = ReadFloat(flat, "metallic", defaults.metallic);
    }
}

// --- 天球 -----------------------------------------------------------------

json WriteSky(const renderer::SkyAsset& asset, const fs::path& baseDir) {
    json node;
    node["name"] = asset.name;
    node["source"] = EnumName(kSkySourceNames, static_cast<uint32_t>(asset.sky.source));
    // 画像はテクスチャと同じく相対パスの参照で持つ。使っていなければ null。
    node["hdri"] = asset.sky.hdriPath.empty()
                       ? json()
                       : json(RelativePathString(asset.sky.hdriPath, baseDir));
    node["skyLuminance"] = asset.sky.skyLuminance;
    node["iblIntensity"] = asset.sky.iblIntensity;

    const renderer::SkySettings& procedural = asset.sky.procedural;
    json proceduralNode;
    proceduralNode["zenithColor"] = WriteFloat3(procedural.zenithColor);
    proceduralNode["horizonColor"] = WriteFloat3(procedural.horizonColor);
    proceduralNode["groundColor"] = WriteFloat3(procedural.groundColor);
    proceduralNode["intensity"] = procedural.intensity;
    node["procedural"] = std::move(proceduralNode);
    return node;
}

// 天球 1 つを読み込んでライブラリへ足す。
renderer::SkyAssetId ReadSky(const json& node, renderer::SkyLibrary& skies,
                             const fs::path& baseDir) {
    const renderer::SkyDefinition defaults;
    std::string name = ReadString(node, "name");
    if (name.empty()) {
        name = "天球";
    }
    const renderer::SkyAssetId id = skies.Add(name);
    renderer::SkyAsset* asset = skies.FindMutable(id);
    if (asset == nullptr) {
        return renderer::kNoSkyAsset;
    }

    asset->sky.source = static_cast<renderer::SkySource>(
        EnumValue(kSkySourceNames, node, "source", static_cast<uint32_t>(defaults.source)));
    if (const std::string hdri = ReadString(node, "hdri"); !hdri.empty()) {
        asset->sky.hdriPath = ResolvePath(hdri, baseDir);
    }
    asset->sky.skyLuminance = ReadFloat(node, "skyLuminance", defaults.skyLuminance);
    asset->sky.iblIntensity = ReadFloat(node, "iblIntensity", defaults.iblIntensity);

    if (const json* procedural = FindMember(node, "procedural");
        procedural != nullptr && procedural->is_object()) {
        renderer::SkySettings& target = asset->sky.procedural;
        const renderer::SkySettings proceduralDefaults;
        target.zenithColor = ReadFloat3(*procedural, "zenithColor", proceduralDefaults.zenithColor);
        target.horizonColor =
            ReadFloat3(*procedural, "horizonColor", proceduralDefaults.horizonColor);
        target.groundColor = ReadFloat3(*procedural, "groundColor", proceduralDefaults.groundColor);
        target.intensity = ReadFloat(*procedural, "intensity", proceduralDefaults.intensity);
    }
    return id;
}

// 天球アセットが無いプロジェクト（天球を入れる前の形式）から 1 つ作る。
// 当時は環境がビューポートに 1 つしか無く、preview 節に直接書かれていた。
void MigrateSkyFromPreview(const json& preview, renderer::SkyLibrary& skies,
                           const fs::path& baseDir) {
    const renderer::SkyDefinition defaults;
    const std::string hdri = ReadString(preview, "hdri");
    const renderer::SkyAssetId id = skies.Add("既定の空");
    renderer::SkyAsset* asset = skies.FindMutable(id);
    if (asset == nullptr) {
        return;
    }
    if (!hdri.empty()) {
        asset->sky.source = renderer::SkySource::Hdri;
        asset->sky.hdriPath = ResolvePath(hdri, baseDir);
        asset->name = ToUtf8Display(asset->sky.hdriPath.stem());
    }
    asset->sky.skyLuminance = ReadFloat(preview, "hdriSkyLuminance", defaults.skyLuminance);
    asset->sky.iblIntensity = ReadFloat(preview, "iblIntensity", defaults.iblIntensity);

    if (const json* sky = FindMember(preview, "sky"); sky != nullptr && sky->is_object()) {
        renderer::SkySettings& target = asset->sky.procedural;
        const renderer::SkySettings proceduralDefaults;
        target.zenithColor = ReadFloat3(*sky, "zenithColor", proceduralDefaults.zenithColor);
        target.horizonColor = ReadFloat3(*sky, "horizonColor", proceduralDefaults.horizonColor);
        target.groundColor = ReadFloat3(*sky, "groundColor", proceduralDefaults.groundColor);
        target.intensity = ReadFloat(*sky, "intensity", proceduralDefaults.intensity);
    }
    skies.SetActive(id);
}

// --- ファイル入出力 -------------------------------------------------------

bool WriteJsonFile(const fs::path& path, const json& document) {
    std::error_code error;
    if (const fs::path parent = path.parent_path(); !parent.empty()) {
        fs::create_directories(parent, error);
    }

    // いきなり上書きすると、ディスクフルなどで途中失敗したときに元のファイルが
    // 壊れたまま残る。一時ファイルへ書き切ってから rename で差し替える。
    const fs::path tempPath = path.wstring() + L".tmp";
    {
        std::ofstream stream(tempPath, std::ios::binary | std::ios::trunc);
        if (!stream.is_open()) {
            TG_LOG_ERROR("ファイルを開けませんでした: %s", ToUtf8Portable(tempPath).c_str());
            return false;
        }
        // 人が読める形で書く。差分も取りやすい。壊れた文字列が混ざっていても
        // 例外を出さない（不正な UTF-8 は置換文字にする）。
        stream << document.dump(2, ' ', false, json::error_handler_t::replace) << '\n';
        if (!stream.good()) {
            TG_LOG_ERROR("ファイルの書き込みに失敗しました: %s", ToUtf8Portable(tempPath).c_str());
            return false;
        }
    }

    std::error_code renameError;
    fs::rename(tempPath, path, renameError);
    if (renameError) {
        TG_LOG_ERROR("ファイルを差し替えられませんでした: %s", ToUtf8Portable(path).c_str());
        std::error_code removeError;
        fs::remove(tempPath, removeError);
        return false;
    }
    return true;
}

bool ReadJsonFile(const fs::path& path, const char* expectedFormat, int maxVersion,
                  json& outDocument) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        TG_LOG_ERROR("ファイルを開けませんでした: %s", ToUtf8Portable(path).c_str());
        return false;
    }

    // 例外は使わない方針なので、パース失敗は discarded で受ける。
    outDocument = json::parse(stream, nullptr, false);
    if (outDocument.is_discarded() || !outDocument.is_object()) {
        TG_LOG_ERROR("JSON として読めませんでした: %s", ToUtf8Portable(path).c_str());
        return false;
    }

    // material-mixer 時代のファイルは "material-mixer.project" などの形式名を持つ。
    // 中身は同じなので、旧形式名は新形式名へ読み替えて受け付ける（書くのは新形式名のみ）。
    std::string format = ReadString(outDocument, "format");
    if (format.rfind("material-mixer.", 0) == 0) {
        format = "terrain-graph." + format.substr(std::string("material-mixer.").size());
    }
    if (format != expectedFormat) {
        TG_LOG_ERROR("形式が違います（%s ではなく %s）: %s", expectedFormat, format.c_str(),
                     ToUtf8Portable(path).c_str());
        return false;
    }
    const int version = ReadInt(outDocument, "version", 0);
    if (version > maxVersion) {
        TG_LOG_ERROR("このバージョンでは読めません（ファイル %d > 対応 %d）: %s", version,
                     maxVersion, ToUtf8Portable(path).c_str());
        return false;
    }
    return true;
}

// ペイントマスクを置く場所。`<プロジェクト名>.assets/`。
fs::path PaintMaskDirectory(const fs::path& projectPath) {
    return projectPath.parent_path() / (projectPath.stem().wstring() + L".assets");
}

std::string PaintMaskFileName(size_t index) {
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "paint_%04zu.png", index);
    return buffer;
}

// 前回の保存で書いた PNG のうち、今回書かなかったものを消す。
// **自分が書いた名前（paint_*.png）だけ**を対象にし、他のファイルには触らない。
// 今回のぶんを書き終えてから呼ぶこと。先に消すと、書き出しに失敗したときに
// 元の PNG まで失われてしまう。
void RemoveStalePaintMasks(const fs::path& directory, const std::vector<fs::path>& keep) {
    std::error_code error;
    if (!fs::is_directory(directory, error)) {
        return;
    }
    for (const fs::directory_entry& entry : fs::directory_iterator(directory, error)) {
        if (!entry.is_regular_file(error)) {
            continue;
        }
        const fs::path& file = entry.path();
        if (file.extension() != L".png") {
            continue;
        }
        if (file.filename().wstring().rfind(L"paint_", 0) != 0) {
            continue;
        }
        bool kept = false;
        for (const fs::path& name : keep) {
            if (file.filename() == name) {
                kept = true;
                break;
            }
        }
        if (kept) {
            continue;
        }
        std::error_code removeError;
        fs::remove(file, removeError);
    }
}

}  // namespace

bool SaveProject(const std::filesystem::path& path, rhi::Device& device,
                 const ProjectRefs& refs) {
    // 裸のファイル名（親ディレクトリ無し）で保存すると相対パスが作れず、
    // 全参照が絶対パスで書かれてしまう。先に絶対化してから基準を取る。
    std::error_code absoluteError;
    const fs::path absolutePath = fs::absolute(path, absoluteError);
    const fs::path& savePath = absoluteError ? path : absolutePath;
    const fs::path baseDir = savePath.parent_path();

    json document;
    document["format"] = kProjectFormat;
    document["version"] = kProjectFormatVersion;
    document["app"] = TG_APP_VERSION;

    // --- テクスチャ（画像は参照。パスはプロジェクトからの相対） -----------
    // ファイルの中では通し番号で参照する。実行中の ID をそのまま書くと、
    // 削除して番号が飛んだときにファイルが読みにくくなる。
    std::unordered_map<compositor::TextureId, int> textureIndex;
    json textures = json::array();
    for (const compositor::LibraryTexture& entry : refs.textures.Entries()) {
        const int index = static_cast<int>(textures.size()) + 1;
        textureIndex[entry.id] = index;

        json node;
        node["id"] = index;
        node["name"] = entry.name;
        node["path"] = RelativePathString(entry.path, baseDir);
        textures.push_back(std::move(node));
    }
    document["textures"] = std::move(textures);

    const TextureWriter writeTexture = [&textureIndex](compositor::TextureId id) {
        const auto it = textureIndex.find(id);
        return (it != textureIndex.end()) ? json(it->second) : json();
    };

    // --- マテリアル（構造ごと埋め込む） -----------------------------------
    std::unordered_map<compositor::MaterialAssetId, int> materialIndex;
    json materials = json::array();
    for (const compositor::MaterialAsset& asset : refs.materials.Entries()) {
        const int index = static_cast<int>(materials.size()) + 1;
        materialIndex[asset.id] = index;

        json node = WriteMaterialBody(asset, writeTexture);
        node["id"] = index;
        materials.push_back(std::move(node));
    }
    document["materials"] = std::move(materials);

    // --- ペイントマスク（PNG でサイドカーへ） -----------------------------
    // 手続きで再現できないので画像として持ち出す。
    // 参照しているのはレイヤーだけなので、レイヤーから辿って集める。
    std::unordered_map<compositor::PaintMaskId, int> paintIndex;
    json paintMasks = json::array();
    const fs::path paintDir = PaintMaskDirectory(savePath);
    // 今回書いたファイル名を控えておき、書き終えてから前回の残りを片付ける。
    std::vector<fs::path> writtenPaintFiles;
    const auto collectPaintMask = [&](compositor::PaintMaskId id) {
        if (id == compositor::kNoPaintMask || paintIndex.count(id) != 0) {
            return;
        }
        const std::vector<uint8_t> pixels = refs.paintMasks.ReadPixels(device, id);
        const uint32_t resolution = refs.paintMasks.Resolution();
        if (pixels.empty()) {
            TG_LOG_WARN("ペイントマスクを読み出せませんでした（保存から外します）");
            return;
        }

        const int index = static_cast<int>(paintMasks.size()) + 1;
        const std::string fileName = PaintMaskFileName(static_cast<size_t>(index));
        std::error_code error;
        fs::create_directories(paintDir, error);
        if (!SaveGray8Png(paintDir / FromUtf8(fileName), resolution, resolution, resolution,
                          pixels.data())) {
            TG_LOG_WARN("ペイントマスクを書き出せませんでした: %s", fileName.c_str());
            return;
        }

        paintIndex[id] = index;
        writtenPaintFiles.push_back(FromUtf8(fileName));
        json node;
        node["id"] = index;
        node["resolution"] = resolution;
        // サイドカーの場所はプロジェクト名から決まるので、ファイル名だけ持つ。
        node["file"] = fileName;
        paintMasks.push_back(std::move(node));
    };
    // ペイントマスクはグラフのノードから辿って集める。
    for (const graph::Node& node : refs.graph.Nodes()) {
        if (const auto* settings = std::get_if<graph::LayerNodeSettings>(&node.settings)) {
            collectPaintMask(settings->layer.mask.paint);
        }
    }
    document["paintMasks"] = std::move(paintMasks);
    document["paintResolution"] = refs.paintMasks.Resolution();
    // マスクを減らしたときに前回の PNG が残らないよう、ここで片付ける。
    RemoveStalePaintMasks(paintDir, writtenPaintFiles);

    // --- ノードグラフ -----------------------------------------------------
    // 版 4 から layers 節は書かない。合成の構造はグラフだけが持つ。
    const std::function<json(compositor::MaterialAssetId)> writeMaterial =
        [&materialIndex](compositor::MaterialAssetId id) {
            const auto it = materialIndex.find(id);
            return (it != materialIndex.end()) ? json(it->second) : json();
        };
    const std::function<json(compositor::PaintMaskId)> writePaint =
        [&paintIndex](compositor::PaintMaskId id) {
            const auto it = paintIndex.find(id);
            return (it != paintIndex.end()) ? json(it->second) : json();
        };
    document["graph"] = WriteGraph(refs.graph, writeTexture, writeMaterial, writePaint);

    // 天球はマテリアルと同じく、構造ごと埋め込む（画像だけ相対パスの参照）。
    json skies = json::array();
    int activeSkyIndex = 0;
    for (const renderer::SkyAsset& asset : refs.skies.Entries()) {
        if (asset.id == refs.skies.ActiveId()) {
            activeSkyIndex = static_cast<int>(skies.size());
        }
        skies.push_back(WriteSky(asset, baseDir));
    }
    document["skies"] = std::move(skies);
    document["activeSky"] = activeSkyIndex;

    document["preview"] = WritePreview(refs.renderer);

    if (!WriteJsonFile(savePath, document)) {
        return false;
    }
    TG_LOG_INFO("プロジェクトを保存しました: %s", ToUtf8Portable(savePath).c_str());
    return true;
}

bool LoadProject(const std::filesystem::path& path, rhi::Device& device,
                 rhi::PipelineCache& pipelineCache, const ProjectRefs& refs) {
    json document;
    if (!ReadJsonFile(path, kProjectFormat, kProjectFormatVersion, document)) {
        return false;
    }

    const fs::path baseDir = path.parent_path();
    const fs::path paintDir = PaintMaskDirectory(path);

    // ここから先は現在の中身を捨てて入れ替える。読み込みは GPU 待機を伴うため、
    // 呼び出し側がフレームの外で呼んでいること。
    refs.paintMasks.Clear(device);
    refs.materials.Clear(device);
    refs.skies.Clear(device);
    refs.textures.Clear(device);

    // --- テクスチャ -------------------------------------------------------
    std::unordered_map<int, compositor::TextureId> textureIds;
    if (const json* textures = FindMember(document, "textures");
        textures != nullptr && textures->is_array()) {
        for (const json& node : *textures) {
            if (!node.is_object()) {
                continue;
            }
            const int index = ReadInt(node, "id", 0);
            const fs::path texturePath = ResolvePath(ReadString(node, "path"), baseDir);
            if (index <= 0 || texturePath.empty()) {
                continue;
            }
            const std::string name = ReadString(node, "name");
            compositor::TextureId id = refs.textures.Load(device, pipelineCache, texturePath);
            if (id == compositor::kNoTexture) {
                // 画像が見つからなくても、残りは読み込む。**参照は捨てない。**
                // パスと名前だけの「リンク切れ」として登録し、マテリアルやノードの
                // 割り当てはそこへ繋いでおく。消してしまうと、次に保存した時点で
                // どのファイルを指していたかが失われ、繋ぎ直せなくなる。
                TG_LOG_WARN("テクスチャが見つかりません（リンク切れ）: %s",
                            ToUtf8Portable(texturePath).c_str());
                id = refs.textures.AddMissing(texturePath, name);
                if (id == compositor::kNoTexture) {
                    continue;
                }
            }
            textureIds[index] = id;
            if (compositor::LibraryTexture* entry = refs.textures.FindMutable(id);
                entry != nullptr && !name.empty()) {
                entry->name = name;
            }
        }
    }
    const TextureReader readTexture = [&textureIds](const json& node) {
        if (!node.is_number_integer()) {
            return compositor::kNoTexture;
        }
        const auto it = textureIds.find(node.get<int>());
        return (it != textureIds.end()) ? it->second : compositor::kNoTexture;
    };

    // --- マテリアル -------------------------------------------------------
    std::unordered_map<int, compositor::MaterialAssetId> materialIds;
    if (const json* materials = FindMember(document, "materials");
        materials != nullptr && materials->is_array()) {
        for (const json& node : *materials) {
            if (!node.is_object()) {
                continue;
            }
            const int index = ReadInt(node, "id", 0);
            const compositor::MaterialAssetId id = refs.materials.Add("マテリアル");
            if (compositor::MaterialAsset* asset = refs.materials.FindMutable(id);
                asset != nullptr) {
                ReadMaterialBody(node, *asset, readTexture);
                asset->thumbnailDirty = true;
            }
            if (index > 0) {
                materialIds[index] = id;
            }
        }
    }

    // --- ペイントマスク ---------------------------------------------------
    std::unordered_map<int, compositor::PaintMaskId> paintIds;
    uint32_t paintResolution = ReadUInt(document, "paintResolution", 1024);
    if (const json* paintMasks = FindMember(document, "paintMasks");
        paintMasks != nullptr && paintMasks->is_array()) {
        for (const json& node : *paintMasks) {
            if (!node.is_object()) {
                continue;
            }
            const int index = ReadInt(node, "id", 0);
            const std::string fileName = ReadString(node, "file");
            if (index <= 0 || fileName.empty()) {
                continue;
            }

            LdrImage image;
            if (!LoadLdrImage(paintDir / FromUtf8(fileName), image) || !image.IsValid()) {
                TG_LOG_WARN("ペイントマスクを読み込めませんでした: %s", fileName.c_str());
                continue;
            }
            if (image.width != image.height) {
                TG_LOG_WARN("ペイントマスクが正方ではありません: %s", fileName.c_str());
                continue;
            }

            // LoadLdrImage は RGBA8 で返す。R だけ取り出す。
            std::vector<uint8_t> gray(static_cast<size_t>(image.width) * image.height);
            for (size_t i = 0; i < gray.size(); ++i) {
                gray[i] = image.pixels[i * 4];
            }
            const compositor::PaintMaskId id =
                refs.paintMasks.AddFromPixels(device, image.width, gray);
            if (id == compositor::kNoPaintMask) {
                continue;
            }
            paintIds[index] = id;
            paintResolution = image.width;
        }
    }
    // 解像度の要求も揃える。揃えないと、次の ProcessPendingWork が
    // 読み込む前の解像度へ戻そうとして全マスクをリサンプルしてしまう。
    refs.paintMasks.RequestResolution(paintResolution);

    // --- ノードグラフ（旧形式は layers[] から移行） -----------------------
    const std::function<compositor::MaterialAssetId(const json&)> readMaterial =
        [&materialIds](const json& value) {
            if (!value.is_number_integer()) {
                return compositor::kNoMaterialAsset;
            }
            const auto it = materialIds.find(value.get<int>());
            return (it != materialIds.end()) ? it->second : compositor::kNoMaterialAsset;
        };
    const std::function<compositor::PaintMaskId(const json&)> readPaint =
        [&paintIds](const json& value) {
            if (!value.is_number_integer()) {
                return compositor::kNoPaintMask;
            }
            const auto it = paintIds.find(value.get<int>());
            return (it != paintIds.end()) ? it->second : compositor::kNoPaintMask;
        };
    // 旧形式の layers[]（版 3 以前）。移行用に一旦読み込んでおく。
    std::vector<compositor::MaterialLayer> legacyLayers;
    if (const json* layers = FindMember(document, "layers");
        layers != nullptr && layers->is_array()) {
        for (const json& node : *layers) {
            if (!node.is_object()) {
                continue;
            }
            legacyLayers.push_back(ReadLayer(node, readTexture, readMaterial, readPaint));
        }
    }

    // 実寸（scale）を持たないソースが引き継ぐ値。実寸をノードへ移す前のファイルは、
    // プレビュー設定に平面のサイズと変位量を持っている。**そちらを正とする**
    // （既定値を入れると、開いただけで地形の大きさが変わってしまう）。
    graph::TerrainScale scaleFallback;
    if (const json* preview = FindMember(document, "preview");
        preview != nullptr && preview->is_object()) {
        const renderer::PreviewDefaults& previewDefaults = renderer::kPreviewDefaults;


        scaleFallback.sizeMeters = ReadFloat(*preview, "planeSize", previewDefaults.planeSize);
        scaleFallback.heightMeters =
            ReadFloat(*preview, "displacementScale", previewDefaults.displacementScale);
    }

    // グラフの決め方。
    //   版 4 以降: graph 節が唯一の合成（無ければ既定へ戻す）。
    //   版 3 以前: 「プレビューに適用」（apply）がオンで保存されていれば graph 節を、
    //             そうでなければ layers[] をグラフへ移行して使う
    //             （当時プレビューに出ていた側を正とする）。
    const int version = ReadInt(document, "version", 0);
    const json* graphNode = FindMember(document, "graph");
    bool graphLoaded = false;
    if (graphNode != nullptr && graphNode->is_object()) {
        const bool legacyApply = ReadBool(*graphNode, "apply", version >= 4);
        if (version >= 4 || legacyApply || legacyLayers.empty()) {
            graphLoaded =
                ReadGraph(*graphNode, refs.graph, readTexture, readMaterial, readPaint,
                          scaleFallback);
        }
    }
    if (!graphLoaded) {
        if (!legacyLayers.empty()) {
            TG_LOG_INFO("旧形式のレイヤーをノードグラフへ移行しました（%zu 枚）",
                        legacyLayers.size());
            refs.graph = MigrateLayersToGraph(std::move(legacyLayers));
        } else {
            refs.graph = graph::NodeGraph::CreateDefault();
        }
    }

    // preview が無い（または壊れている）プロジェクトでも必ず既定値で埋める。
    // 呼ばないと、前のプロジェクトのカメラ・ライト・露出が残ってしまう。
    const json* preview = FindMember(document, "preview");
    const json emptyPreview = json::object();
    const json& previewNode =
        (preview != nullptr && preview->is_object()) ? *preview : emptyPreview;
    ReadPreview(previewNode, refs.renderer);

    // 天球。無ければ preview 節から 1 つ作る（天球を入れる前のプロジェクト）。
    if (const json* skies = FindMember(document, "skies");
        skies != nullptr && skies->is_array() && !skies->empty()) {
        std::vector<renderer::SkyAssetId> ids;
        for (const json& sky : *skies) {
            if (!sky.is_object()) {
                continue;
            }
            ids.push_back(ReadSky(sky, refs.skies, baseDir));
        }
        const auto activeIndex = static_cast<size_t>(ReadUInt(document, "activeSky", 0));
        if (activeIndex < ids.size()) {
            refs.skies.SetActive(ids[activeIndex]);
        }
    } else {
        MigrateSkyFromPreview(previewNode, refs.skies, baseDir);
    }
    refs.skies.EnsureDefault();

    TG_LOG_INFO("プロジェクトを開きました: %s", ToUtf8Portable(path).c_str());
    return true;
}

bool SaveMaterial(const std::filesystem::path& path, const compositor::MaterialAsset& asset,
                  const compositor::TextureLibrary& textures) {
    // SaveProject と同じく、裸のファイル名でも相対パスが作れるよう絶対化する。
    std::error_code absoluteError;
    const fs::path absolutePath = fs::absolute(path, absoluteError);
    const fs::path& savePath = absoluteError ? path : absolutePath;
    const fs::path baseDir = savePath.parent_path();

    // 単体ファイルでは、テクスチャをこのファイルからの相対パスで参照する。
    const TextureWriter writeTexture = [&textures, &baseDir](compositor::TextureId id) {
        const compositor::LibraryTexture* entry = textures.Find(id);
        if (entry == nullptr) {
            return json();
        }
        return json(RelativePathString(entry->path, baseDir));
    };

    json document = WriteMaterialBody(asset, writeTexture);
    document["format"] = kMaterialFormat;
    document["version"] = kMaterialFormatVersion;
    document["app"] = TG_APP_VERSION;

    if (!WriteJsonFile(savePath, document)) {
        return false;
    }
    TG_LOG_INFO("マテリアルを書き出しました: %s", ToUtf8Portable(savePath).c_str());
    return true;
}

compositor::MaterialAssetId LoadMaterial(const std::filesystem::path& path, rhi::Device& device,
                                         rhi::PipelineCache& pipelineCache,
                                         compositor::TextureLibrary& textures,
                                         compositor::MaterialLibrary& materials) {
    json document;
    if (!ReadJsonFile(path, kMaterialFormat, kMaterialFormatVersion, document)) {
        return compositor::kNoMaterialAsset;
    }

    const fs::path baseDir = path.parent_path();
    // 参照している画像はその場で読み込む。すでに同じ画像があれば読み直さない。
    const TextureReader readTexture = [&](const json& node) {
        if (!node.is_string()) {
            return compositor::kNoTexture;
        }
        const fs::path texturePath = ResolvePath(node.get<std::string>(), baseDir);
        if (texturePath.empty()) {
            return compositor::kNoTexture;
        }
        const compositor::TextureId id = textures.Load(device, pipelineCache, texturePath);
        if (id == compositor::kNoTexture) {
            // プロジェクトと同じく、見つからない画像はリンク切れとして残す。
            TG_LOG_WARN("テクスチャが見つかりません（リンク切れ）: %s",
                        ToUtf8Portable(texturePath).c_str());
            return textures.AddMissing(texturePath, std::string());
        }
        return id;
    };

    const compositor::MaterialAssetId id = materials.Add("マテリアル");
    compositor::MaterialAsset* asset = materials.FindMutable(id);
    if (asset == nullptr) {
        return compositor::kNoMaterialAsset;
    }
    ReadMaterialBody(document, *asset, readTexture);
    asset->thumbnailDirty = true;

    TG_LOG_INFO("マテリアルを読み込みました: %s", ToUtf8Portable(path).c_str());
    return id;
}

}  // namespace tg::io
