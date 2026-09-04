// ビューポートでのパス（Path ノード）の編集。
//
// 操作は「クリックは選ぶ、Ctrl を押している間だけ伸ばす、ドラッグして重ねれば繋がる」
// の 3 つを軸にする。設計の経緯は docs/design/node-graph.md の「パス」。
//
//   点をクリック            選択する（伸ばす起点になる）
//   エッジをクリック        鎖（分岐から分岐まで）を選ぶ。向きと曲線の種類はここで変える。
//                           エッジ 1 本の選択は無い（曲線も向きも鎖の性質で、1 本だけ変えると
//                           鎖の中で食い違う。1 本だけ消す / 切り離すは右クリックのメニュー）
//   空をクリック            選択を外す
//   Ctrl + 空をクリック     選択した点から新しい点へ線を伸ばす（選択が無ければ新しい線の始点）
//   Ctrl + 点をクリック     選択した点とその点を繋ぐ
//   Ctrl + エッジをクリック そこに点を挿入して選択（そのままドラッグできる。点を選んで
//                           いればその点と繋ぐ）
//   点をドラッグ            地形の表面に沿って動かす。他の点や線に重ねると吸着して結合
//   右クリック              点 / エッジ / 空のメニュー（分離、削除、反転、挿入…）
//   Delete                  選択した点（またはエッジ）を消す
//   R                       選択した点に付くエッジ（または選択した鎖）の向きを反転
//   Esc                     選択を外す
//   Shift                   吸着しない
//
// **ふつうのクリックはデータを変えない**（選ぶだけ）。増やす / 繋ぐは Ctrl、動かすは
// ドラッグ、消す / 分ける / 反転はキーとメニュー。線のそばに点を置きたかったのに
// 挿入されてしまう、という誤操作を無くすため。
//
// **仮のエッジ（選択した点からカーソルへ）は Ctrl を押している間だけ出す。**
// 常に出ていると「まだ終わっていない」と急かされる感じになる。伸ばす意思があるときにだけ
// 現れ、Ctrl を離しても状態は変わらない（選択は残る）。
// 操作の案内はビューポートの下端に出す（プロパティに書くと目を離さないと読めない）。
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

// 鎖の曲線（曲線の鎖だけ）。ガイドの折れ線とは別に、本線として描く。
struct PathScreenCurve {
    std::vector<ImVec2> polyline;
    std::vector<bool> visible;
};

struct PathScreenCache {
    std::vector<PathScreenPoint> points;
    std::vector<PathScreenEdge> edges;
    std::vector<graph::PathStrand> strands;
    std::vector<PathScreenCurve> curves;  // strands と同じ並び。直線の鎖は空

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
    // 曲線の鎖。制御点の区間ごとに割り、各標本を地形の高さで描く。
    cache.strands = graph::BuildPathStrands(path);
    cache.curves.resize(cache.strands.size());
    for (size_t i = 0; i < cache.strands.size(); ++i) {
        if (graph::StrandCurve(path, cache.strands[i], nullptr, nullptr) == graph::PathCurve::Line) {
            continue;
        }
        constexpr int kSamplesPerSpan = 12;
        for (const graph::PathCurveSample& sample :
             graph::SamplePathStrand(path, cache.strands[i], kSamplesPerSpan)) {
            const ProjectedPoint projected = ProjectToViewport(
                viewProjection, worldOf(sample.u, sample.v, sample.heightOffsetMeters),
                viewportMin, size);
            cache.curves[i].polyline.push_back(projected.screen);
            cache.curves[i].visible.push_back(projected.visible);
        }
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
    std::erase_if(state.selected, [&path](graph::PathElementId id) {
        return path.FindPoint(id) == nullptr;
    });
    std::erase_if(state.selectedEdges, [&path](graph::PathElementId id) {
        return path.FindEdge(id) == nullptr;
    });
    std::erase_if(state.selectedStrandInterior, [&path](graph::PathElementId id) {
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
    // 点の選択とエッジの選択は排他。
    const auto selectOnly = [&](graph::PathElementId id) {
        state.selected.clear();
        state.selectedEdges.clear();
        state.selectedStrandInterior.clear();
        if (id != 0) {
            state.selected.push_back(id);
        }
    };
    // 鎖を丸ごと選ぶ。内側の点も覚えておき、Delete で一緒に消す。
    const auto selectStrand = [&](graph::PathElementId edgeId) {
        state.selected.clear();
        state.selectedEdges.clear();
        state.selectedStrandInterior.clear();
        const graph::PathStrand* strand = graph::FindStrandOfEdge(cache.strands, edgeId);
        if (strand == nullptr) {
            state.selectedEdges.push_back(edgeId);
            return;
        }
        state.selectedEdges = strand->edges;
        state.selectedStrandInterior.clear();
        for (size_t i = 1; i + 1 < strand->points.size(); ++i) {
            state.selectedStrandInterior.push_back(strand->points[i]);
        }
    };
    // 伸ばす起点。選択している点（複数なら先頭）。
    const graph::PathElementId anchor = state.selected.empty() ? 0 : state.selected.front();
    // Ctrl を押している間が「伸ばす」モード。
    const bool extend = io.KeyCtrl;
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
        if (extend) {
            // 伸ばす。起点（選択した点）から、クリックした所へ繋ぐ。
            if (state.hoverPoint != 0) {
                if (anchor != 0 && anchor != state.hoverPoint) {
                    changed |= graph::ConnectPathPoints(path, anchor, state.hoverPoint);
                }
                selectOnly(state.hoverPoint);
            } else if (state.hoverEdge != 0) {
                // エッジの途中に点を挿入して掴む。点を選んでいれば、その点とも繋ぐ。
                const graph::PathElementId inserted =
                    graph::InsertPathPointOnEdge(path, state.hoverEdge, state.hoverEdgeT);
                if (inserted != 0) {
                    if (anchor != 0) {
                        graph::ConnectPathPoints(path, anchor, inserted);
                    }
                    selectOnly(inserted);
                    state.dragPoint = inserted;
                    state.dragging = true;
                    changed = true;
                }
            } else if (onTerrain) {
                // 空の所。選択が無ければ新しい線の始点になる。
                const graph::PathElementId added =
                    graph::AddPathPoint(path, terrainU, terrainV, anchor);
                if (added != 0) {
                    selectOnly(added);
                    state.dragPoint = added;
                    state.dragging = true;
                    changed = true;
                }
            }
        } else if (state.hoverPoint != 0) {
            // 点を選ぶ。そのままドラッグで動かせる。
            selectOnly(state.hoverPoint);
            state.dragPoint = state.hoverPoint;
            state.dragging = true;
        } else if (state.hoverEdge != 0) {
            // エッジを選ぶ = その鎖を選ぶ。挿入は Ctrl + クリック。
            selectStrand(state.hoverEdge);
        } else {
            // 空の所。選択を外す（次の Ctrl + クリックは新しい線の始点）。
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
                    changed = true;
                }
            } else if (state.snapEdge != 0) {
                // 線の上へ落とした。そこに点を挿入して合体する。
                const graph::PathElementId inserted =
                    graph::InsertPathPointOnEdge(path, state.snapEdge, state.snapEdgeT);
                if (inserted != 0 && graph::MergePathPoints(path, state.dragPoint, inserted)) {
                    selectOnly(inserted);
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
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            selectOnly(0);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
            if (!state.selectedEdges.empty()) {
                for (const graph::PathElementId id : state.selectedEdges) {
                    changed |= graph::DeletePathEdge(path, id);
                }
                // 鎖の内側の点は、エッジが無くなれば一緒に消す。
                for (const graph::PathElementId id : state.selectedStrandInterior) {
                    if (path.EdgeCount(id) == 0) {
                        changed |= graph::DeletePathPoint(path, id);
                    }
                }
                selectOnly(0);
            } else if (!state.selected.empty()) {
                for (const graph::PathElementId id : state.selected) {
                    changed |= graph::DeletePathPoint(path, id);
                }
                selectOnly(0);
            }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_R, false) && !io.KeyCtrl) {
            for (const graph::PathElementId id : state.selectedEdges) {
                changed |= graph::ReversePathEdge(path, id);
            }
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
        } else if (state.menuEdge != 0) {
            selectStrand(state.menuEdge);
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
                    changed = true;
                }
            }
            ImGui::EndDisabled();
            ImGui::BeginDisabled(path.EdgeCount(pointId) == 0);
            if (ImGui::MenuItem("向きを反転")) {
                changed |= graph::ReversePathEdgesAt(path, pointId);
            }
            ImGui::EndDisabled();
            if (ImGui::MenuItem("削除")) {
                changed |= graph::DeletePathPoint(path, pointId);
                selectOnly(0);
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
                        changed = true;
                    }
                }
            }
            // 向きは鎖の性質。1 本だけ反転すると鎖の中で食い違うので、鎖ごと反転する。
            if (ImGui::MenuItem("鎖の向きを反転")) {
                for (const graph::PathElementId id : state.selectedEdges) {
                    changed |= graph::ReversePathEdge(path, id);
                }
            }
            if (ImGui::MenuItem("このエッジを消す（ここで切る）")) {
                changed |= graph::DeletePathEdge(path, edgeId);
                selectOnly(0);
            }
        } else {
            ImGui::TextDisabled("パス");
            ImGui::Separator();
            ImGui::BeginDisabled(!state.menuOnTerrain);
            if (ImGui::MenuItem("ここに線を始める")) {
                const graph::PathElementId added =
                    graph::AddPathPoint(path, state.menuU, state.menuV, 0);
                if (added != 0) {
                    selectOnly(added);
                    changed = true;
                }
            }
            ImGui::EndDisabled();
            ImGui::BeginDisabled(state.selected.empty());
            if (ImGui::MenuItem("選択を外す")) {
                selectOnly(0);
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

    // 色。ピンの色（水色）と同じ系統で、状態は明るさで分ける。
    const ImU32 lineColor = IM_COL32(120, 200, 240, 220);
    const ImU32 lineShadow = IM_COL32(0, 0, 0, 120);
    const ImU32 hoverColor = IM_COL32(255, 245, 255, 255);
    const ImU32 snapColor = IM_COL32(255, 220, 120, 255);
    const ImU32 pointColor = IM_COL32(205, 235, 250, 240);
    const ImU32 selectedColor = IM_COL32(255, 255, 255, 255);
    const ImU32 tailColor = IM_COL32(255, 220, 120, 255);
    const float lineWidth = ui::Scaled(2.0f);

    // --- エッジ ---------------------------------------------------------------
    for (const PathScreenEdge& edge : cache.edges) {
        const bool hovered = (edge.id == state.hoverEdge && !state.dragging);
        const bool snapping = (edge.id == state.snapEdge);
        const bool selectedEdge =
            std::find(state.selectedEdges.begin(), state.selectedEdges.end(), edge.id) !=
            state.selectedEdges.end();
        // 曲線の鎖のエッジはガイド。薄く細く描き、本線は曲線のほうに任せる。
        const graph::PathStrand* strand = graph::FindStrandOfEdge(cache.strands, edge.id);
        const bool guide =
            strand != nullptr &&
            graph::StrandCurve(path, *strand, nullptr, nullptr) != graph::PathCurve::Line;
        const ImU32 baseColor = guide ? IM_COL32(120, 200, 240, 110) : lineColor;
        const ImU32 color = snapping ? snapColor
                                     : ((hovered || selectedEdge) ? hoverColor : baseColor);
        const float width = selectedEdge ? lineWidth + ui::Scaled(1.5f)
                                         : (guide ? ui::Scaled(1.0f) : lineWidth);
        for (size_t i = 0; i + 1 < edge.polyline.size(); ++i) {
            if (!edge.visible[i] || !edge.visible[i + 1]) {
                continue;
            }
            drawList->AddLine(edge.polyline[i], edge.polyline[i + 1], lineShadow,
                              width + ui::Scaled(2.0f));
            drawList->AddLine(edge.polyline[i], edge.polyline[i + 1], color, width);
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

    // --- 曲線（本線） -----------------------------------------------------------
    for (size_t s = 0; s < cache.curves.size(); ++s) {
        const PathScreenCurve& curve = cache.curves[s];
        const graph::PathStrand& strand = cache.strands[s];
        // 鎖が選ばれていれば本線も明るく太く。
        const bool selectedStrand =
            !strand.edges.empty() &&
            std::find(state.selectedEdges.begin(), state.selectedEdges.end(),
                      strand.edges.front()) != state.selectedEdges.end() &&
            state.selectedEdges.size() == strand.edges.size();
        const ImU32 color = selectedStrand ? hoverColor : lineColor;
        const float width = selectedStrand ? lineWidth + ui::Scaled(1.5f) : lineWidth;
        for (size_t i = 0; i + 1 < curve.polyline.size(); ++i) {
            if (!curve.visible[i] || !curve.visible[i + 1]) {
                continue;
            }
            drawList->AddLine(curve.polyline[i], curve.polyline[i + 1], lineShadow,
                              width + ui::Scaled(2.0f));
            drawList->AddLine(curve.polyline[i], curve.polyline[i + 1], color, width);
        }
    }

    // --- 仮のエッジ（選択した点からカーソルへ） ----------------------------------
    // **Ctrl を押している間だけ出す。** 次のクリックで何が起きるかを先に見せる。
    const ImGuiIO& io = ImGui::GetIO();
    const bool viewportHovered = ImGui::IsMouseHoveringRect(viewportMin, viewportMax);
    const graph::PathElementId anchor = state.selected.empty() ? 0 : state.selected.front();
    if (io.KeyCtrl && anchor != 0 && viewportHovered && !state.dragging) {
        const PathScreenPoint* tail = cache.Find(anchor);
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
            if (PickTerrainUv(ImGui::GetIO().MousePos, viewportMin, viewportMax, u, v)) {
                // 地形に沿わせて描く（空中を横切る直線だと距離感が狂う）。
                const graph::PathPoint* from = path.FindPoint(anchor);
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
                                              IM_COL32(120, 200, 240, 120), lineWidth);
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
        const float radius = ui::Scaled(selected ? 5.5f : 4.5f);
        drawList->AddCircleFilled(point.screen, radius + ui::Scaled(1.5f), lineShadow, 16);
        drawList->AddCircleFilled(point.screen,
                                  radius, snapping ? snapColor : (hovered ? hoverColor : pointColor),
                                  16);
        if (selected) {
            drawList->AddCircle(point.screen, radius + ui::Scaled(3.0f), selectedColor, 20,
                                ui::Scaled(1.5f));
        }
        // Ctrl を押している間は、伸ばす起点をもう 1 つの輪で示す。
        if (io.KeyCtrl && point.id == anchor) {
            drawList->AddCircle(point.screen, radius + ui::Scaled(6.0f), tailColor, 24,
                                ui::Scaled(1.5f));
        }
    }

    // --- 操作の案内 ---------------------------------------------------------------
    // ビューポートの右下に、いまの状態でできることを「操作 → 意味」の小さな表で出す。
    // プロパティに書くと視線を外さないと読めないので、見ている場所へ重ねる。
    struct HintRow {
        const char* key;
        const char* action;
    };
    std::vector<HintRow> rows;
    if (io.KeyCtrl) {
        if (anchor != 0) {
            rows = {{"クリック", "点を置いて伸ばす"},
                    {"点をクリック", "繋ぐ"},
                    {"線をクリック", "点を挿入して繋ぐ"},
                    {"Ctrl を離す", "戻る"}};
        } else {
            rows = {{"クリック", "線を始める"},
                    {"線をクリック", "点を挿入"},
                    {"Ctrl を離す", "戻る"}};
        }
    } else if (state.dragging) {
        rows = {{"点 / 線に重ねる", "繋ぐ"}, {"Shift", "吸着しない"}};
    } else if (!state.selectedEdges.empty()) {
        rows = {{"プロパティ", "曲線の種類 / 丸め / 向き"},
                {"Ctrl + 線をクリック", "点を挿入"},
                {"右クリック", "挿入 / 切り離し / ここで切る"},
                {"Delete / R", "鎖を消す / 向きを反転"},
                {"Esc", "選択を外す"}};
    } else if (!state.selected.empty()) {
        rows = {{"Ctrl + クリック", "伸ばす（点や線の上で繋ぐ）"},
                {"ドラッグ", "動かす"},
                {"右クリック", "分離 / 反転 / 削除"},
                {"Delete / R", "消す / 向きを反転"},
                {"Esc", "選択を外す"}};
    } else {
        rows = {{"クリック", "点や線（鎖）を選ぶ"},
                {"Ctrl + クリック", "線を始める"},
                {"Ctrl + 線をクリック", "点を挿入"},
                {"Alt + ドラッグ", "視点"}};
    }
    {
        float keyWidth = 0.0f;
        float actionWidth = 0.0f;
        for (const HintRow& row : rows) {
            keyWidth = std::max(keyWidth, ImGui::CalcTextSize(row.key).x);
            actionWidth = std::max(actionWidth, ImGui::CalcTextSize(row.action).x);
        }
        const ImVec2 padding(ui::Scaled(10.0f), ui::Scaled(6.0f));
        const float gap = ui::Scaled(14.0f);
        const float margin = ui::Scaled(10.0f);
        const float lineHeight = ImGui::GetTextLineHeight();
        const float spacing = ImGui::GetStyle().ItemSpacing.y * 0.5f;
        const float width = keyWidth + gap + actionWidth + padding.x * 2.0f;
        const float height = lineHeight * static_cast<float>(rows.size()) +
                             spacing * static_cast<float>(rows.size() - 1) + padding.y * 2.0f;
        const ImVec2 boxMax(viewportMax.x - margin, viewportMax.y - margin);
        const ImVec2 boxMin(boxMax.x - width, boxMax.y - height);
        drawList->AddRectFilled(boxMin, boxMax, IM_COL32(8, 10, 12, 190), ui::Scaled(4.0f));
        float y = boxMin.y + padding.y;
        for (const HintRow& row : rows) {
            // 操作は右揃えで落とした色、意味は明るい色。列が揃うので読み飛ばしやすい。
            const float keyX = boxMin.x + padding.x + keyWidth - ImGui::CalcTextSize(row.key).x;
            drawList->AddText(ImVec2(keyX, y), IM_COL32(170, 175, 180, 255), row.key);
            drawList->AddText(ImVec2(boxMin.x + padding.x + keyWidth + gap, y),
                              IM_COL32(235, 235, 235, 255), row.action);
            y += lineHeight + spacing;
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

    if (m_pathEdit.nodeId == node.id && !m_pathEdit.selectedEdges.empty()) {
        std::vector<graph::PathEdge*> edges;
        for (const graph::PathElementId id : m_pathEdit.selectedEdges) {
            for (graph::PathEdge& edge : path.edges) {
                if (edge.id == id) {
                    edges.push_back(&edge);
                }
            }
        }
        if (!edges.empty()) {
            ui::SectionHeader("選択した鎖");
            // 曲線の種類と丸め。鎖なら全エッジに同じ値を入れる（表示は先頭。混在なら注記）。
            graph::PathCurve curve = edges.front()->curve;
            float rounding = edges.front()->rounding;
            float clothoidRatio = edges.front()->clothoidRatio;
            bool mixed = false;
            for (const graph::PathEdge* edge : edges) {
                if (edge->curve != curve || std::abs(edge->rounding - rounding) > 1e-4f ||
                    std::abs(edge->clothoidRatio - clothoidRatio) > 1e-4f) {
                    mixed = true;
                }
            }
            if (ui::BeginPropertyTable("graphPathEdgeRows")) {
                ui::PropertyValue("エッジ", "%zu 本（点 %d → 点 %d）", edges.size(),
                                  edges.front()->from, edges.back()->to);
                static const char* const kCurveLabels[] = {"直線", "2 次ベジェ",
                                                           "3 次 B スプライン", "クロソイド"};
                int curveIndex = static_cast<int>(curve);
                bool curveChanged = ui::PropertyCombo(
                    "曲線", &curveIndex, kCurveLabels, IM_ARRAYSIZE(kCurveLabels), 0,
                    "折れ線をガイドにして、その内側に描く曲線。点は通らない。"
                    "2 次は角ごとに丸めて両端だけ通る。3 次はさらに滑らかだが折れ線からより離れる。"
                    "クロソイドは角ごとに緩和曲線 → 円弧 → 緩和曲線で、曲率が 0 から連続的に"
                    "立ち上がる（道路 / 鉄道の線形）");
                curve = static_cast<graph::PathCurve>(curveIndex);
                curveChanged |= ui::PropertyFloat(
                    "丸め", &rounding, 0.0f, 1.0f, 1.0f,
                    "どれだけ角を取るか。0 で折れ線のまま、1 で最大（線分の中点まで）", "%.2f");
                if (curve == graph::PathCurve::Clothoid) {
                    curveChanged |= ui::PropertyFloat(
                        "クロソイド比", &clothoidRatio, 0.0f, 1.0f, 0.5f,
                        "交角のうち緩和曲線 2 本が受け持つ割合。0 で純粋な円弧、"
                        "1 で円弧なし（緩和曲線だけ）",
                        "%.2f");
                }
                if (curveChanged) {
                    for (graph::PathEdge* edge : edges) {
                        edge->curve = curve;
                        edge->rounding = rounding;
                        edge->clothoidRatio = clothoidRatio;
                    }
                    changed = true;
                }
                ui::PropertyLabelEmpty("pathEdgeButtons");
                if (ui::Button("向きを反転")) {
                    for (graph::PathEdge* edge : edges) {
                        changed |= graph::ReversePathEdge(path, edge->id);
                    }
                }
                ImGui::SameLine();
                if (ui::Button("削除")) {
                    for (const graph::PathElementId id : m_pathEdit.selectedEdges) {
                        changed |= graph::DeletePathEdge(path, id);
                    }
                    for (const graph::PathElementId id : m_pathEdit.selectedStrandInterior) {
                        if (path.EdgeCount(id) == 0) {
                            changed |= graph::DeletePathPoint(path, id);
                        }
                    }
                    m_pathEdit.selectedEdges.clear();
                    m_pathEdit.selectedStrandInterior.clear();
                }
                ui::PropertyEnd();
                ui::EndPropertyTable();
            }
            if (mixed) {
                ui::HintText("鎖の中で曲線の種類か丸めが混在している。変えると全部に入る");
            }
        }
    }

    ui::HintText("ビューポートで編集する（操作はビューポートの下に出る）。"
                 "地形はプレビュー中のものに沿う。Base に繋いだ地形を見るには、"
                 "このノードをダブルクリック");
    return changed;
}

}  // namespace tg
