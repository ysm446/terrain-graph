#include "renderer/ModelAsset.h"

#include <ufbx.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <unordered_map>

namespace tg::renderer {
using namespace DirectX;
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
        for (ufbx_node* child = node; child->parent; child = child->parent) {
            if (child->parent->attrib_type == UFBX_ELEMENT_LOD_GROUP) {
                auto children = child->parent->children;
                for (size_t i = 0; i < children.count; ++i)
                    if (children.data[i] == child) lod = i;
                break;
            }
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
