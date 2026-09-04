// ビューポートでのパス（Path ノード）の編集。
//
// 操作は「クリックは伸ばす、既存のものをクリックすれば選ぶ、ドラッグして重ねれば繋がる」
// の 3 つを軸にする。設計の経緯は docs/design/node-graph.md の「パス」。
//
//   空をクリック        末尾の点から新しい点へ線を伸ばす（末尾が無ければ新しい線の始点）
//   点をクリック        選択して末尾にする（続きはそこから伸びる。枝分かれもこれ）
//   エッジをクリック    その位置に点を挿入して選択（そのままドラッグできる）
//   点をドラッグ        地形の表面に沿って動かす。他の点や線に重ねると吸着して結合
//   右クリック          点 / エッジ / 空のメニュー（分離、削除、反転、挿入…）
//   Enter / Esc         末尾を解除（次のクリックは新しい線の始点）
//   Delete              選択した点を消す
//   R                   選択した点に付くエッジの向きを反転
//   Shift               吸着しない
//
// 点は正規化 UV で持ち、高さは CPU 側のハイト（評価器が写したもの）から毎回引く。
// クリック位置の地形への投影も同じハイトへレイを飛ばして求める。

#include "app/Application.h"

#include "app/ApplicationUiHelpers.h"
#include "core/Log.h"
#include "graph/Path.h"
#include "ui/UiStyle.h"

#include <imgui.h>

#include <DirectXMath.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace tg {
namespace {

using namespace DirectX;

// 当たり判定の広さ（96 DPI 基準のピクセル）。
constexpr float kPointHitRadius = 9.0f;
constexpr float kEdgeHitRadius = 6.0f;
constexpr float kSnapRadius = 12.0f;
// 分離 / 切り離しで点を離す距離（画面上）。
constexpr float kDetachPixels = 14.0f;
// これより動いたらクリックではなくドラッグ。
constexpr float kDragThreshold = 3.0f;

float Distance(const ImVec2& a, const ImVec2& b) {
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y));
}

// 点 p から線分 ab までの距離と、最寄りの位置 t（0〜1）。
float DistanceToSegment(const ImVec2& p, const ImVec2& a, const ImVec2& b, float& outT) {
    const float abx = b.x - a.x;
    const float aby = b.y - a.y;
    const float lengthSq = abx * abx + aby * aby;
    outT = (lengthSq > 1e-6f) ? std::clamp(((p.x - a.x) * abx + (p.y - a.y) * aby) / lengthSq,
                                           0.0f, 1.0f)
                              : 0.0f;
    const ImVec2 closest(a.x + abx * outT, a.y + aby * outT);
    return Distance(p, closest);
}

// 画面へ投影したパス。当たり判定と描画の両方がこれを見る。
struct PathScreenPoint {
    graph::PathElementId id = 0;
    ImVec2 screen{};
    bool visible = false;
};

struct PathScreenEdge {
    graph::PathElementId id = 0;
    graph::PathElementId from = 0;
    graph::PathElementId to = 0;
    // 地形に沿わせるために細かく割った折れ線。t は from 側が 0。
    std::vector<ImVec2> polyline;
    std::vector<float> t;
    std::vector<bool> visible;
};

struct PathScreenCache {
    std::vector<PathScreenPoint> points;
    std::vector<PathScreenEdge> edges;

    const PathScreenPoint* Find(graph::PathElementId id) const {
        for (const PathScreenPoint& point : points) {
            if (point.id == id) {
                return &point;
            }
        }
        return nullptr;
    }
};

template <typename WorldFn>
PathScreenCache BuildPathScreenCache(const graph::PathSettings& path, const XMMATRIX& viewProjection,
                                     const ImVec2& viewportMin, const ImVec2& size,
                                     const WorldFn& worldOf) {
    PathScreenCache cache;
    cache.points.reserve(path.points.size());
    for (const graph::PathPoint& point : path.points) {
        const ProjectedPoint projected = ProjectToViewport(
            viewProjection, worldOf(point.u, point.v, point.heightOffsetMeters), viewportMin,
            size);
        cache.points.push_back({point.id, projected.screen, projected.visible});
    }
    cache.edges.reserve(path.edges.size());
    for (const graph::PathEdge& edge : path.edges) {
        const graph::PathPoint* a = path.FindPoint(edge.from);
        const graph::PathPoint* b = path.FindPoint(edge.to);
        const PathScreenPoint* sa = cache.Find(edge.from);
        const PathScreenPoint* sb = cache.Find(edge.to);
        if (a == nullptr || b == nullptr || sa == nullptr || sb == nullptr) {
            continue;
        }
        PathScreenEdge screenEdge;
        screenEdge.id = edge.id;
        screenEdge.from = edge.from;
        screenEdge.to = edge.to;
        // 画面上の長さで割る数を決める。長い線ほど細かく割って地形に沿わせる。
        const float length = (sa->visible && sb->visible) ? Distance(sa->screen, sb->screen)
                                                          : 400.0f;
        const int segments = std::clamp(static_cast<int>(length / ui::Scaled(14.0f)), 1, 32);
        for (int i = 0; i <= segments; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(segments);
            const float u = a->u + (b->u - a->u) * t;
            const float v = a->v + (b->v - a->v) * t;
            const float offset = a->heightOffsetMeters + (b->heightOffsetMeters - a->heightOffsetMeters) * t;
            const ProjectedPoint projected =
                ProjectToViewport(viewProjection, worldOf(u, v, offset), viewportMin, size);
            screenEdge.polyline.push_back(projected.screen);
            screenEdge.t.push_back(t);
            screenEdge.visible.push_back(projected.visible);
        }
        cache.edges.push_back(std::move(screenEdge));
    }
    return cache;
}

// 最寄りの点（半径内）。exclude は除外する点。
graph::PathElementId NearestPoint(const PathScreenCache& cache, const ImVec2& mouse, float radius,
                                  graph::PathElementId exclude) {
    graph::PathElementId best = 0;
    float bestDistance = radius;
    for (const PathScreenPoint& point : cache.points) {
        if (!point.visible || point.id == exclude) {
            continue;
        }
        const float distance = Distance(mouse, point.screen);
        if (distance <= bestDistance) {
            bestDistance = distance;
            best = point.id;
        }
    }
    return best;
}

// 最寄りのエッジ（半径内）と、その上の位置 t。excludePoint に付くエッジは除外する。
graph::PathElementId NearestEdge(const PathScreenCache& cache, const ImVec2& mouse, float radius,
                                 graph::PathElementId excludePoint, float& outT) {
    graph::PathElementId best = 0;
    float bestDistance = radius;
    outT = 0.0f;
    for (const PathScreenEdge& edge : cache.edges) {
        if (edge.from == excludePoint || edge.to == excludePoint) {
            continue;
        }
        for (size_t i = 0; i + 1 < edge.polyline.size(); ++i) {
            if (!edge.visible[i] || !edge.visible[i + 1]) {
                continue;
            }
            float t = 0.0f;
            const float distance =
                DistanceToSegment(mouse, edge.polyline[i], edge.polyline[i + 1], t);
            if (distance <= bestDistance) {
                bestDistance = distance;
                best = edge.id;
                outT = edge.t[i] + (edge.t[i + 1] - edge.t[i]) * t;
            }
        }
    }
    return best;
}

}  // namespace

graph::Node* Application::CurrentPathNode() {
    graph::Node* node = m_graph.FindMutableNode(m_selectedGraphNode);
    if (node == nullptr || node->kind != graph::NodeKind::Path) {
        return nullptr;
    }
    return node;
}

XMFLOAT3 Application::PathWorldPosition(float u, float v, float heightOffsetMeters) const {
    const float size = m_renderer.PlaneSize();
    const float scale = m_renderer.DisplacementScale();
    const float height = m_renderer.Evaluator().Heightfield().Sample(u, v);
    return XMFLOAT3{(u - 0.5f) * size, (height - 0.5f) * scale + heightOffsetMeters,
                    (v - 0.5f) * size};
}

// カーソルからレイを飛ばし、CPU 側のハイトと最初に交わる所を探す。
//
// 地形を包む箱の中だけを一定の歩幅で進み、レイが地形の下へ潜った区間を二分法で詰める。
// ハイトの写しは 512² なので、歩幅もその程度で足りる。
bool Application::PickTerrainUv(const ImVec2& mouse, const ImVec2& viewportMin,
                                const ImVec2& viewportMax, float& outU, float& outV) const {
    const ImVec2 size(viewportMax.x - viewportMin.x, viewportMax.y - viewportMin.y);
    if (size.x <= 0.0f || size.y <= 0.0f) {
        return false;
    }
    const renderer::Camera& camera = m_renderer.GetCamera();
    const XMMATRIX viewProjection = camera.ViewMatrix() * camera.ProjectionMatrix();
    XMVECTOR determinant;
    const XMMATRIX inverse = XMMatrixInverse(&determinant, viewProjection);
    if (XMVectorGetX(determinant) == 0.0f) {
        return false;
    }
    const float ndcX = ((mouse.x - viewportMin.x) / size.x) * 2.0f - 1.0f;
    const float ndcY = 1.0f - ((mouse.y - viewportMin.y) / size.y) * 2.0f;
    const XMVECTOR nearPoint = XMVector3TransformCoord(XMVectorSet(ndcX, ndcY, 0.0f, 1.0f), inverse);
    const XMVECTOR farPoint = XMVector3TransformCoord(XMVectorSet(ndcX, ndcY, 1.0f, 1.0f), inverse);
    const XMVECTOR direction = XMVector3Normalize(XMVectorSubtract(farPoint, nearPoint));

    XMFLOAT3 origin;
    XMFLOAT3 dir;
    XMStoreFloat3(&origin, nearPoint);
    XMStoreFloat3(&dir, direction);

    const float half = m_renderer.PlaneSize() * 0.5f;
    const float scale = m_renderer.DisplacementScale();
    const float yMin = -0.5f * scale - 1.0f;
    const float yMax = 0.5f * scale + 1.0f;

    // 箱との交差区間（スラブ法）。
    float tEnter = 0.0f;
    float tExit = 1.0e12f;
    const auto slab = [&](float o, float d, float lo, float hi) {
        if (std::abs(d) < 1e-9f) {
            return o >= lo && o <= hi;
        }
        float t0 = (lo - o) / d;
        float t1 = (hi - o) / d;
        if (t0 > t1) {
            std::swap(t0, t1);
        }
        tEnter = std::max(tEnter, t0);
        tExit = std::min(tExit, t1);
        return tEnter <= tExit;
    };
    if (!slab(origin.x, dir.x, -half, half) || !slab(origin.z, dir.z, -half, half) ||
        !slab(origin.y, dir.y, yMin, yMax)) {
        return false;
    }

    const compositor::CpuHeightfield& heightfield = m_renderer.Evaluator().Heightfield();
    const float sizeMeters = std::max(m_renderer.PlaneSize(), 1e-3f);
    const auto above = [&](float t, float& outUu, float& outVv) {
        const float x = origin.x + dir.x * t;
        const float y = origin.y + dir.y * t;
        const float z = origin.z + dir.z * t;
        outUu = std::clamp(x / sizeMeters + 0.5f, 0.0f, 1.0f);
        outVv = std::clamp(z / sizeMeters + 0.5f, 0.0f, 1.0f);
        const float terrainY = (heightfield.Sample(outUu, outVv) - 0.5f) * scale;
        return y - terrainY;
    };

    constexpr int kSteps = 384;
    float previousT = tEnter;
    float u = 0.0f;
    float v = 0.0f;
    float previous = above(previousT, u, v);
    for (int i = 1; i <= kSteps; ++i) {
        const float t = tEnter + (tExit - tEnter) * (static_cast<float>(i) / kSteps);
        const float current = above(t, u, v);
        if (previous > 0.0f && current <= 0.0f) {
            // 潜った区間を二分法で詰める。
            float lo = previousT;
            float hi = t;
            for (int k = 0; k < 12; ++k) {
                const float mid = (lo + hi) * 0.5f;
                if (above(mid, u, v) > 0.0f) {
                    lo = mid;
                } else {
                    hi = mid;
                }
            }
            above(hi, outU, outV);
            return true;
        }
        previous = current;
        previousT = t;
    }
    return false;
}

void Application::HandlePathInput(graph::Node& node, bool itemActive, bool itemHovered,
                                  const ImVec2& viewportMin, const ImVec2& viewportMax) {
    auto* settings = std::get_if<graph::PathNodeSettings>(&node.settings);
    if (settings == nullptr) {
        return;
    }
    graph::PathSettings& path = settings->path;
    PathEditState& state = m_pathEdit;
    const ImGuiIO& io = ImGui::GetIO();
    (void)itemActive;

    // ノードが変わったら状態を捨てる。消えた点の ID も落とす。
    if (state.nodeId != node.id) {
        state = PathEditState{};
        state.nodeId = node.id;
    }
    if (state.tail != 0 && path.FindPoint(state.tail) == nullptr) {
        state.tail = 0;
    }
    std::erase_if(state.selected, [&path](graph::PathElementId id) {
        return path.FindPoint(id) == nullptr;
    });
    if (state.dragPoint != 0 && path.FindPoint(state.dragPoint) == nullptr) {
        state.dragPoint = 0;
        state.dragging = false;
    }

    const ImVec2 size(viewportMax.x - viewportMin.x, viewportMax.y - viewportMin.y);
    const renderer::Camera& camera = m_renderer.GetCamera();
    const XMMATRIX viewProjection = camera.ViewMatrix() * camera.ProjectionMatrix();
    const auto worldOf = [this](float u, float v, float offset) {
        return PathWorldPosition(u, v, offset);
    };
    const PathScreenCache cache =
        BuildPathScreenCache(path, viewProjection, viewportMin, size, worldOf);

    const ImVec2 mouse = io.MousePos;
    const bool mouseInside = itemHovered;
    float terrainU = 0.0f;
    float terrainV = 0.0f;
    const bool onTerrain =
        mouseInside && PickTerrainUv(mouse, viewportMin, viewportMax, terrainU, terrainV);

    // --- ホバー ---------------------------------------------------------------
    state.hoverPoint = 0;
    state.hoverEdge = 0;
    if (mouseInside && !state.dragging) {
        state.hoverPoint = NearestPoint(cache, mouse, ui::Scaled(kPointHitRadius), 0);
        if (state.hoverPoint == 0) {
            state.hoverEdge =
                NearestEdge(cache, mouse, ui::Scaled(kEdgeHitRadius), 0, state.hoverEdgeT);
        }
    }

    bool changed = false;
    const auto selectOnly = [&](graph::PathElementId id) {
        state.selected.clear();
        if (id != 0) {
            state.selected.push_back(id);
        }
    };
    // 分離 / 切り離しで点を離す距離を、その点の位置での UV へ直す。
    const auto detachOffsetUv = [&](graph::PathElementId pointId) {
        const graph::PathPoint* point = path.FindPoint(pointId);
        if (point == nullptr) {
            return 0.01f;
        }
        const ProjectedPoint a = ProjectToViewport(
            viewProjection, worldOf(point->u, point->v, point->heightOffsetMeters), viewportMin,
            size);
        const ProjectedPoint b = ProjectToViewport(
            viewProjection, worldOf(std::min(point->u + 0.01f, 1.0f), point->v,
                                    point->heightOffsetMeters),
            viewportMin, size);
        if (!a.visible || !b.visible) {
            return 0.01f;
        }
        const float pixelsPerUv = Distance(a.screen, b.screen) / 0.01f;
        return (pixelsPerUv > 1e-3f) ? (ui::Scaled(kDetachPixels) / pixelsPerUv) : 0.01f;
    };

    // --- 押した -----------------------------------------------------------------
    if (mouseInside && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        state.pressPos = mouse;
        state.dragMoved = false;
        state.snapPoint = 0;
        state.snapEdge = 0;
        if (state.hoverPoint != 0) {
            // 点を選んで末尾にする。続きはここから伸びる。
            selectOnly(state.hoverPoint);
            state.tail = state.hoverPoint;
            state.dragPoint = state.hoverPoint;
            state.dragging = true;
        } else if (state.hoverEdge != 0) {
            // エッジの途中に点を挿入して、その点を掴む。
            const graph::PathElementId inserted =
                graph::InsertPathPointOnEdge(path, state.hoverEdge, state.hoverEdgeT);
            if (inserted != 0) {
                selectOnly(inserted);
                state.tail = inserted;
                state.dragPoint = inserted;
                state.dragging = true;
                changed = true;
            }
        } else if (onTerrain) {
            // 空の所。末尾から伸ばす（末尾が無ければ新しい線の始点）。
            const graph::PathElementId added =
                graph::AddPathPoint(path, terrainU, terrainV, state.tail);
            if (added != 0) {
                selectOnly(added);
                state.tail = added;
                state.dragPoint = added;
                state.dragging = true;
                changed = true;
            }
        } else {
            selectOnly(0);
        }
    }

    // --- ドラッグ -----------------------------------------------------------------
    if (state.dragging && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        if (!state.dragMoved && Distance(mouse, state.pressPos) > ui::Scaled(kDragThreshold)) {
            state.dragMoved = true;
        }
        if (state.dragMoved && onTerrain) {
            if (graph::PathPoint* point = path.FindPoint(state.dragPoint)) {
                point->u = terrainU;
                point->v = terrainV;
                changed = true;
            }
            // 吸着先。Shift を押している間は吸着しない。
            state.snapPoint = 0;
            state.snapEdge = 0;
            if (!io.KeyShift) {
                state.snapPoint =
                    NearestPoint(cache, mouse, ui::Scaled(kSnapRadius), state.dragPoint);
                if (state.snapPoint == 0) {
                    state.snapEdge = NearestEdge(cache, mouse, ui::Scaled(kSnapRadius),
                                                 state.dragPoint, state.snapEdgeT);
                }
            }
        }
    }

    // --- 離した -------------------------------------------------------------------
    if (state.dragging && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        if (state.dragMoved) {
            if (state.snapPoint != 0) {
                // 相手の点へ合体。位置と幅は相手のものが残る。
                if (graph::MergePathPoints(path, state.dragPoint, state.snapPoint)) {
                    selectOnly(state.snapPoint);
                    state.tail = state.snapPoint;
                    changed = true;
                }
            } else if (state.snapEdge != 0) {
                // 線の上へ落とした。そこに点を挿入して合体する。
                const graph::PathElementId inserted =
                    graph::InsertPathPointOnEdge(path, state.snapEdge, state.snapEdgeT);
                if (inserted != 0 && graph::MergePathPoints(path, state.dragPoint, inserted)) {
                    selectOnly(inserted);
                    state.tail = inserted;
                    changed = true;
                }
            }
        }
        state.dragging = false;
        state.dragPoint = 0;
        state.snapPoint = 0;
        state.snapEdge = 0;
    }

    // --- キー ---------------------------------------------------------------------
    if (mouseInside && !io.WantTextInput && !state.dragging) {
        if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
            ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) {
            state.tail = 0;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            state.tail = 0;
            selectOnly(0);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && !state.selected.empty()) {
            for (const graph::PathElementId id : state.selected) {
                changed |= graph::DeletePathPoint(path, id);
            }
            selectOnly(0);
            state.tail = 0;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_R, false) && !io.KeyCtrl) {
            for (const graph::PathElementId id : state.selected) {
                changed |= graph::ReversePathEdgesAt(path, id);
            }
        }
    }

    // --- 右クリックのメニュー --------------------------------------------------------
    if (mouseInside && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && !state.dragging) {
        state.menuPoint = state.hoverPoint;
        state.menuEdge = state.hoverEdge;
        state.menuEdgeT = state.hoverEdgeT;
        state.menuOnTerrain = onTerrain;
        state.menuU = terrainU;
        state.menuV = terrainV;
        if (state.menuPoint != 0) {
            selectOnly(state.menuPoint);
        }
        ImGui::OpenPopup("##pathContextMenu");
    }
    if (ImGui::BeginPopup("##pathContextMenu")) {
        if (state.menuPoint != 0 && path.FindPoint(state.menuPoint) != nullptr) {
            const graph::PathElementId pointId = state.menuPoint;
            ImGui::TextDisabled("点");
            ImGui::Separator();
            ImGui::BeginDisabled(path.EdgeCount(pointId) < 2);
            if (ImGui::MenuItem("分離")) {
                // エッジの本数ぶんに分ける。離した点をまとめて選択しておく。
                std::vector<graph::PathElementId> created;
                if (graph::SplitPathPoint(path, pointId, detachOffsetUv(pointId), &created)) {
                    state.selected = created;
                    state.tail = 0;
                    changed = true;
                }
            }
            ImGui::EndDisabled();
            if (ImGui::MenuItem("ここから線を伸ばす")) {
                state.tail = pointId;
            }
            ImGui::BeginDisabled(path.EdgeCount(pointId) == 0);
            if (ImGui::MenuItem("向きを反転")) {
                changed |= graph::ReversePathEdgesAt(path, pointId);
            }
            ImGui::EndDisabled();
            if (ImGui::MenuItem("削除")) {
                changed |= graph::DeletePathPoint(path, pointId);
                selectOnly(0);
                if (state.tail == pointId) {
                    state.tail = 0;
                }
            }
        } else if (state.menuEdge != 0 && path.FindEdge(state.menuEdge) != nullptr) {
            const graph::PathElementId edgeId = state.menuEdge;
            ImGui::TextDisabled("エッジ");
            ImGui::Separator();
            if (ImGui::MenuItem("点を挿入")) {
                const graph::PathElementId inserted =
                    graph::InsertPathPointOnEdge(path, edgeId, state.menuEdgeT);
                if (inserted != 0) {
                    selectOnly(inserted);
                    state.tail = inserted;
                    changed = true;
                }
            }
            if (ImGui::MenuItem("近いほうの点から切り離す")) {
                const graph::PathEdge* edge = path.FindEdge(edgeId);
                if (edge != nullptr) {
                    const graph::PathElementId end =
                        (state.menuEdgeT < 0.5f) ? edge->from : edge->to;
                    const graph::PathElementId created =
                        graph::DetachPathEdgeEnd(path, edgeId, end, detachOffsetUv(end));
                    if (created != 0) {
                        selectOnly(created);
                        state.tail = 0;
                        changed = true;
                    }
                }
            }
            if (ImGui::MenuItem("向きを反転")) {
                changed |= graph::ReversePathEdge(path, edgeId);
            }
            if (ImGui::MenuItem("削除")) {
                changed |= graph::DeletePathEdge(path, edgeId);
            }
        } else {
            ImGui::TextDisabled("パス");
            ImGui::Separator();
            ImGui::BeginDisabled(!state.menuOnTerrain);
            if (ImGui::MenuItem("新しい線を始める")) {
                const graph::PathElementId added =
                    graph::AddPathPoint(path, state.menuU, state.menuV, 0);
                if (added != 0) {
                    selectOnly(added);
                    state.tail = added;
                    changed = true;
                }
            }
            ImGui::EndDisabled();
            ImGui::BeginDisabled(state.tail == 0);
            if (ImGui::MenuItem("末尾を解除")) {
                state.tail = 0;
            }
            ImGui::EndDisabled();
        }
        ImGui::EndPopup();
    }

    if (changed) {
        m_graph.MarkDirty();
        MarkDocumentChanged();
    }
}

void Application::DrawPathOverlay(const graph::Node& node, const ImVec2& viewportMin,
                                  const ImVec2& viewportMax) {
    const auto* settings = std::get_if<graph::PathNodeSettings>(&node.settings);
    if (settings == nullptr) {
        return;
    }
    const graph::PathSettings& path = settings->path;
    const PathEditState& state = m_pathEdit;
    const ImVec2 size(viewportMax.x - viewportMin.x, viewportMax.y - viewportMin.y);
    if (size.x <= 0.0f || size.y <= 0.0f) {
        return;
    }
    const renderer::Camera& camera = m_renderer.GetCamera();
    const XMMATRIX viewProjection = camera.ViewMatrix() * camera.ProjectionMatrix();
    const auto worldOf = [this](float u, float v, float offset) {
        return PathWorldPosition(u, v, offset);
    };
    const PathScreenCache cache =
        BuildPathScreenCache(path, viewProjection, viewportMin, size, worldOf);

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->PushClipRect(viewportMin, viewportMax, true);

    // 色。ピンの色（薄い紫）と同じ系統で、状態は明るさで分ける。
    const ImU32 lineColor = IM_COL32(190, 165, 235, 220);
    const ImU32 lineShadow = IM_COL32(0, 0, 0, 120);
    const ImU32 hoverColor = IM_COL32(255, 245, 255, 255);
    const ImU32 snapColor = IM_COL32(255, 220, 120, 255);
    const ImU32 pointColor = IM_COL32(230, 220, 250, 240);
    const ImU32 selectedColor = IM_COL32(255, 255, 255, 255);
    const ImU32 tailColor = IM_COL32(255, 220, 120, 255);
    const float lineWidth = ui::Scaled(2.0f);

    // --- エッジ ---------------------------------------------------------------
    for (const PathScreenEdge& edge : cache.edges) {
        const bool hovered = (edge.id == state.hoverEdge && !state.dragging);
        const bool snapping = (edge.id == state.snapEdge);
        const ImU32 color = snapping ? snapColor : (hovered ? hoverColor : lineColor);
        for (size_t i = 0; i + 1 < edge.polyline.size(); ++i) {
            if (!edge.visible[i] || !edge.visible[i + 1]) {
                continue;
            }
            drawList->AddLine(edge.polyline[i], edge.polyline[i + 1], lineShadow,
                              lineWidth + ui::Scaled(2.0f));
            drawList->AddLine(edge.polyline[i], edge.polyline[i + 1], color, lineWidth);
        }
        // 向きの矢印。線の真ん中に小さく置く。
        const size_t mid = edge.polyline.size() / 2;
        if (mid > 0 && mid < edge.polyline.size() && edge.visible[mid - 1] && edge.visible[mid]) {
            const ImVec2 a = edge.polyline[mid - 1];
            const ImVec2 b = edge.polyline[mid];
            ImVec2 dir(b.x - a.x, b.y - a.y);
            const float length = std::sqrt(dir.x * dir.x + dir.y * dir.y);
            if (length > 1.0f) {
                dir.x /= length;
                dir.y /= length;
                const ImVec2 side(-dir.y, dir.x);
                const float head = ui::Scaled(9.0f);
                const float halfWidth = ui::Scaled(4.5f);
                const ImVec2 tip((a.x + b.x) * 0.5f + dir.x * head * 0.5f,
                                 (a.y + b.y) * 0.5f + dir.y * head * 0.5f);
                const ImVec2 base(tip.x - dir.x * head, tip.y - dir.y * head);
                drawList->AddTriangleFilled(
                    tip, ImVec2(base.x + side.x * halfWidth, base.y + side.y * halfWidth),
                    ImVec2(base.x - side.x * halfWidth, base.y - side.y * halfWidth), color);
            }
        }
    }

    // --- 仮のエッジ（末尾からカーソルへ） --------------------------------------
    // 末尾があるときだけ出す。次のクリックで何が起きるかを先に見せる。
    if (state.tail != 0 && !state.dragging) {
        const PathScreenPoint* tail = cache.Find(state.tail);
        ImVec2 target{};
        bool hasTarget = false;
        if (state.hoverPoint != 0) {
            if (const PathScreenPoint* point = cache.Find(state.hoverPoint)) {
                target = point->screen;
                hasTarget = point->visible;
            }
        } else if (state.hoverEdge != 0) {
            target = ImGui::GetIO().MousePos;
            hasTarget = true;
        } else {
            float u = 0.0f;
            float v = 0.0f;
            if (ImGui::IsMouseHoveringRect(viewportMin, viewportMax) &&
                PickTerrainUv(ImGui::GetIO().MousePos, viewportMin, viewportMax, u, v)) {
                // 地形に沿わせて描く（空中を横切る直線だと距離感が狂う）。
                const graph::PathPoint* from = path.FindPoint(state.tail);
                if (from != nullptr && tail != nullptr && tail->visible) {
                    constexpr int kSegments = 16;
                    ImVec2 previous = tail->screen;
                    bool previousVisible = true;
                    for (int i = 1; i <= kSegments; ++i) {
                        const float t = static_cast<float>(i) / kSegments;
                        const ProjectedPoint projected = ProjectToViewport(
                            viewProjection,
                            worldOf(from->u + (u - from->u) * t, from->v + (v - from->v) * t,
                                    from->heightOffsetMeters),
                            viewportMin, size);
                        if (previousVisible && projected.visible) {
                            drawList->AddLine(previous, projected.screen,
                                              IM_COL32(190, 165, 235, 120), lineWidth);
                        }
                        previous = projected.screen;
                        previousVisible = projected.visible;
                    }
                }
            }
        }
        if (hasTarget && tail != nullptr && tail->visible) {
            drawList->AddLine(tail->screen, target, IM_COL32(255, 220, 120, 160), lineWidth);
        }
    }

    // --- 点 ---------------------------------------------------------------------
    for (const PathScreenPoint& point : cache.points) {
        if (!point.visible) {
            continue;
        }
        const bool selected =
            std::find(state.selected.begin(), state.selected.end(), point.id) != state.selected.end();
        const bool hovered = (point.id == state.hoverPoint && !state.dragging);
        const bool snapping = (point.id == state.snapPoint);
        const bool isTail = (point.id == state.tail);
        const float radius = ui::Scaled(selected ? 5.5f : 4.5f);
        drawList->AddCircleFilled(point.screen, radius + ui::Scaled(1.5f), lineShadow, 16);
        drawList->AddCircleFilled(point.screen,
                                  radius, snapping ? snapColor : (hovered ? hoverColor : pointColor),
                                  16);
        if (selected) {
            drawList->AddCircle(point.screen, radius + ui::Scaled(3.0f), selectedColor, 20,
                                ui::Scaled(1.5f));
        }
        if (isTail) {
            // 末尾は輪で示す。次のクリックはここから伸びる。
            drawList->AddCircle(point.screen, radius + ui::Scaled(6.0f), tailColor, 24,
                                ui::Scaled(1.5f));
        }
    }

    drawList->PopClipRect();
}

bool Application::DrawPathSettings(graph::Node& node) {
    auto* settings = std::get_if<graph::PathNodeSettings>(&node.settings);
    if (settings == nullptr) {
        return false;
    }
    graph::PathSettings& path = settings->path;
    const graph::PathSettings defaults;
    bool changed = false;

    ui::SectionHeader("パス");
    if (ui::BeginPropertyTable("graphPathRows")) {
        ui::PropertyValue("要素", "点 %zu / エッジ %zu", path.points.size(), path.edges.size());
        changed |= ui::PropertyFloat("幅", &path.defaultWidthMeters, 0.5f, 2000.0f,
                                     defaults.defaultWidthMeters,
                                     "新しく置く点の幅（m）。マスクではこの幅の内側が 1 になる",
                                     "%.1f m", ImGuiSliderFlags_Logarithmic);
        changed |= ui::PropertyFloat("フェザー", &path.defaultFeatherMeters, 0.0f, 2000.0f,
                                     defaults.defaultFeatherMeters,
                                     "新しく置く点のフェザー（m）。幅の外側をこの距離で 0 へ落とす",
                                     "%.1f m", ImGuiSliderFlags_Logarithmic);
        changed |= ui::PropertyFloat("強さ", &path.defaultIntensity, 0.0f, 1.0f,
                                     defaults.defaultIntensity, "新しく置く点のマスクの強さ",
                                     "%.2f");
        ui::PropertyLabelEmpty("pathClear");
        ImGui::BeginDisabled(path.points.empty());
        if (ui::Button("全部消す", ui::kWideButtonWidth)) {
            path.points.clear();
            path.edges.clear();
            m_pathEdit = PathEditState{};
            m_pathEdit.nodeId = node.id;
            changed = true;
        }
        ImGui::EndDisabled();
        ui::PropertyEnd();
        ui::EndPropertyTable();
    }

    // 選択した点。複数選んでいれば全部に同じ値を入れる（表示は先頭の値）。
    std::vector<graph::PathPoint*> selectedPoints;
    if (m_pathEdit.nodeId == node.id) {
        for (const graph::PathElementId id : m_pathEdit.selected) {
            if (graph::PathPoint* point = path.FindPoint(id)) {
                selectedPoints.push_back(point);
            }
        }
    }
    if (!selectedPoints.empty()) {
        ui::SectionHeader(selectedPoints.size() == 1 ? "選択した点" : "選択した点（複数）");
        graph::PathPoint edit = *selectedPoints.front();
        bool pointChanged = false;
        if (ui::BeginPropertyTable("graphPathPointRows")) {
            pointChanged |= ui::PropertyFloat("幅", &edit.widthMeters, 0.5f, 2000.0f,
                                              path.defaultWidthMeters, "この点での幅（m）",
                                              "%.1f m", ImGuiSliderFlags_Logarithmic);
            pointChanged |= ui::PropertyFloat("フェザー", &edit.featherMeters, 0.0f, 2000.0f,
                                              path.defaultFeatherMeters,
                                              "この点でのフェザー（m）", "%.1f m",
                                              ImGuiSliderFlags_Logarithmic);
            pointChanged |= ui::PropertyFloat("強さ", &edit.intensity, 0.0f, 1.0f,
                                              path.defaultIntensity, "この点でのマスクの強さ",
                                              "%.2f");
            pointChanged |= ui::PropertyFloat(
                "高さのずれ", &edit.heightOffsetMeters, -200.0f, 200.0f, 0.0f,
                "地形からの高さのずれ（m）。表示と、高さを読むノードが使う。Mask Path は見ない",
                "%.1f m");
            ui::EndPropertyTable();
        }
        if (pointChanged) {
            for (graph::PathPoint* point : selectedPoints) {
                point->widthMeters = edit.widthMeters;
                point->featherMeters = edit.featherMeters;
                point->intensity = edit.intensity;
                point->heightOffsetMeters = edit.heightOffsetMeters;
            }
            changed = true;
        }
    }

    ui::HintText("ビューポートで編集する。クリックで点を置いて伸ばし、Enter で線を区切る。"
                 "点をクリックすると選択し、続きはそこから伸びる。"
                 "点をドラッグして他の点や線に重ねると繋がる（Shift で吸着なし）。"
                 "右クリックで分離 / 削除 / 反転。Delete で選択した点を消す。"
                 "視点は Alt + ドラッグ。地形はプレビュー中のものに沿う（Base に繋いだ地形を"
                 "見るには、このノードをダブルクリック）");
    return changed;
}

}  // namespace tg
