#include "renderer/ModelAsset.h"

#include <ufbx.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <limits>
#include <string_view>
#include <unordered_map>

namespace tg::renderer {
using namespace DirectX;
namespace {
// 名前の末尾が _LOD<n>（大文字小文字は問わない）なら n。Blender の FBX 書き出しは
// LODGroup を作れないので、Unreal などと同じ命名規約でも段階を受け付ける。
int LodFromName(const ufbx_string& name) {
    const std::string_view text(name.data, name.length);
    const size_t mark = text.rfind('_');
    if (mark == std::string_view::npos || text.size() - mark < 5) return -1;
    const auto tag = text.substr(mark + 1, 3);
    if (!(std::tolower(static_cast<unsigned char>(tag[0])) == 'l' &&
          std::tolower(static_cast<unsigned char>(tag[1])) == 'o' &&
          std::tolower(static_cast<unsigned char>(tag[2])) == 'd'))
        return -1;
    int lod = 0;
    for (const char c : text.substr(mark + 4)) {
        if (c < '0' || c > '9' || lod > 64) return -1;
        lod = lod * 10 + (c - '0');
    }
    return lod;
}
}  // namespace
float LodStartDistance(const ModelAsset& asset, size_t lod) {
    if (lod == 0) return 0.0f;
    if (lod - 1 < asset.lodDistances.size()) return asset.lodDistances[lod - 1];
    // 未設定なら**描画効率を優先する**。最大寸法の 4 倍から段ごとに 3 倍ずつ遠くし、
    // どの段も kDefaultLodMaxDistance で頭打ちにする。以前の「10 倍から」では、背の高い木
    // （高さ 24 m のカラマツ）がインポスターへ替わるのが 2 km 先になり、数 km 先まで
    // メッシュのまま大量に描かれて重かった。インポスター段（メッシュ段の数と同じ番号）も同じ式。
    constexpr float kFirstScale = 4.0f, kStep = 3.0f, kDefaultLodMaxDistance = 300.0f;
    float size = 1.0f;
    if (asset.geometry) {
        const auto& g = *asset.geometry;
        size = std::max({g.maximum.x - g.minimum.x, g.maximum.y - g.minimum.y,
                         g.maximum.z - g.minimum.z, 0.01f});
    }
    return std::min(size * kFirstScale * std::pow(kStep, static_cast<float>(lod - 1)),
                    kDefaultLodMaxDistance);
}
bool LoadModel(const std::filesystem::path& path, ModelAsset& asset) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        asset.error = "モデルファイルを開けません";
        return false;
    }
    const auto size = stream.tellg();
    if (size <= 0) {
        asset.error = "モデルファイルが空です";
        return false;
    }
    std::vector<char> bytes(static_cast<size_t>(size));
    stream.seekg(0);
    if (!stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))) {
        asset.error = "モデルファイルを読み取れません";
        return false;
    }
    ufbx_load_opts opts{};
    opts.target_axes = ufbx_axes_right_handed_y_up;
    opts.target_unit_meters = 1.0;
    opts.generate_missing_normals = true;
    opts.ignore_animation = true;
    opts.ignore_embedded = true;
    ufbx_error error{};
    std::unique_ptr<ufbx_scene, decltype(&ufbx_free_scene)> scene(
        ufbx_load_memory(bytes.data(), bytes.size(), &opts, &error), &ufbx_free_scene);
    if (!scene) {
        asset.error.assign(error.description.data, error.description.length);
        return false;
    }
    auto geometry = std::make_shared<ModelGeometry>();
    const float limit = std::numeric_limits<float>::max();
    geometry->minimum = {limit, limit, limit};
    geometry->maximum = {-limit, -limit, -limit};
    std::unordered_map<uint32_t, uint32_t> slotIds;
    for (ufbx_node* node : scene->nodes) {
        if (!node->mesh) continue;
        size_t lod = 0;
        bool grouped = false;
        for (ufbx_node* child = node; child->parent; child = child->parent) {
            if (child->parent->attrib_type == UFBX_ELEMENT_LOD_GROUP) {
                auto children = child->parent->children;
                for (size_t i = 0; i < children.count; ++i)
                    if (children.data[i] == child) lod = i;
                grouped = true;
                break;
            }
        }
        if (!grouped)
            for (ufbx_node* named = node; named && !named->is_root; named = named->parent)
                if (const int value = LodFromName(named->name); value >= 0) {
                    lod = static_cast<size_t>(value);
                    break;
                }
        geometry->lods.resize(std::max(geometry->lods.size(), lod + 1));
        auto& level = geometry->lods[lod];
        const ufbx_mesh& mesh = *node->mesh;
        const auto normalMatrix = ufbx_matrix_for_normals(&node->geometry_to_world);
        std::vector<uint32_t> indices(mesh.max_face_triangles * 3);
        std::unordered_map<uint32_t, size_t> parts;
        for (size_t faceIndex = 0; faceIndex < mesh.faces.count; ++faceIndex) {
            const auto face = mesh.faces.data[faceIndex];
            const uint32_t count =
                ufbx_triangulate_face(indices.data(), indices.size(), &mesh, face);
            if (!count) continue;
            const uint32_t mat = mesh.face_material.count ? mesh.face_material.data[faceIndex] : 0;
            std::string slotName = "Default";
            uint32_t materialId = std::numeric_limits<uint32_t>::max();
            if (mat < node->materials.count) {
                materialId = node->materials.data[mat]->typed_id;
                const auto name = node->materials.data[mat]->name;
                slotName.assign(name.data, name.length);
            }
            auto [slot, added] =
                slotIds.emplace(materialId, static_cast<uint32_t>(geometry->slots.size()));
            if (added) geometry->slots.push_back(slotName);
            auto [part, newPart] = parts.emplace(slot->second, level.parts.size());
            if (newPart) level.parts.push_back({{}, slot->second});
            auto& dst = level.parts[part->second].mesh;
            for (uint32_t t = 0; t < count; ++t) {
                MeshVertex vertices[3]{};
                for (uint32_t j = 0; j < 3; ++j) {
                    const uint32_t index = indices[t * 3 + j];
                    const auto p =
                        ufbx_transform_position(&node->geometry_to_world,
                                                ufbx_get_vertex_vec3(&mesh.vertex_position, index));
                    const auto n = ufbx_transform_direction(
                        &normalMatrix, ufbx_get_vertex_vec3(&mesh.vertex_normal, index));
                    auto& v = vertices[j];
                    v.position = {float(p.x), float(p.y), float(p.z)};
                    v.normal = {float(n.x), float(n.y), float(n.z)};
                    XMStoreFloat3(&v.normal, XMVector3Normalize(XMLoadFloat3(&v.normal)));
                    if (mesh.vertex_uv.exists) {
                        const auto uv = ufbx_get_vertex_vec2(&mesh.vertex_uv, index);
                        v.uv = {float(uv.x), float(1.0 - uv.y)};
                    }
                    for (int k = 0; k < 3; ++k) {
                        const float value = (&v.position.x)[k];
                        if (!std::isfinite(value)) {
                            asset.error = "頂点座標が不正です";
                            return false;
                        }
                        (&geometry->minimum.x)[k] = std::min((&geometry->minimum.x)[k], value);
                        (&geometry->maximum.x)[k] = std::max((&geometry->maximum.x)[k], value);
                    }
                }
                // 負のスケールも含め、面の向きを変換後の法線へ合わせる。
                auto e1 = XMVectorSubtract(XMLoadFloat3(&vertices[1].position),
                                           XMLoadFloat3(&vertices[0].position));
                auto e2 = XMVectorSubtract(XMLoadFloat3(&vertices[2].position),
                                           XMLoadFloat3(&vertices[0].position));
                if (XMVectorGetX(XMVector3Dot(XMVector3Cross(e1, e2),
                                              XMLoadFloat3(&vertices[0].normal))) < 0) {
                    std::swap(vertices[1], vertices[2]);
                    std::swap(e1, e2);
                }
                const float du1 = vertices[1].uv.x - vertices[0].uv.x,
                            dv1 = vertices[1].uv.y - vertices[0].uv.y;
                const float du2 = vertices[2].uv.x - vertices[0].uv.x,
                            dv2 = vertices[2].uv.y - vertices[0].uv.y;
                const float det = du1 * dv2 - du2 * dv1;
                auto tangent = e1, bitangent = e2;
                if (std::abs(det) > 1e-12f) {
                    tangent = XMVectorScale(
                        XMVectorSubtract(XMVectorScale(e1, dv2), XMVectorScale(e2, dv1)), 1 / det);
                    bitangent = XMVectorScale(
                        XMVectorSubtract(XMVectorScale(e2, du1), XMVectorScale(e1, du2)), 1 / det);
                }
                for (auto& v : vertices) {
                    auto n = XMLoadFloat3(&v.normal);
                    auto tx =
                        XMVectorSubtract(tangent, XMVectorMultiply(n, XMVector3Dot(n, tangent)));
                    if (XMVectorGetX(XMVector3LengthSq(tx)) < 1e-20f) {
                        const auto axis = std::abs(v.normal.y) < 0.9f ? XMVectorSet(0, 1, 0, 0)
                                                                      : XMVectorSet(1, 0, 0, 0);
                        tx = XMVector3Cross(axis, n);
                    }
                    tx = XMVector3Normalize(tx);
                    XMStoreFloat4(&v.tangent, tx);
                    v.tangent.w = XMVectorGetX(XMVector3Dot(XMVector3Cross(n, tx), bitangent)) < 0
                                      ? -1.0f
                                      : 1.0f;
                    dst.indices.push_back(static_cast<uint32_t>(dst.vertices.size()));
                    dst.vertices.push_back(v);
                }
            }
            level.triangles += count;
        }
    }
    // 番号が飛んでいても段を詰める（_LOD0 と _LOD2 だけなら 2 段）。
    std::erase_if(geometry->lods, [](const ModelLod& level) { return level.triangles == 0; });
    if (geometry->lods.empty() || !geometry->lods[0].triangles) {
        asset.error = "表示できるメッシュがありません";
        return false;
    }
    asset.path = path;
    asset.geometry = std::move(geometry);
    asset.materials.resize(asset.geometry->slots.size());
    asset.error.clear();
    return true;
}
}  // namespace tg::renderer
