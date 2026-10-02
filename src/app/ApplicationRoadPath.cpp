// Road Path の縦断曲線・バンク角（プロパティ、縦断図、ビューポートの表示）。
//
// 平面の点とエッジの編集は Path と共通（ApplicationPathEdit.cpp が EditablePathSettings を通して
// Road Path も扱う）。ここは道路の線形だけ。
//
// 中心線の高さは **Road Path の Base に繋いだチェーン**の高さから引く。Base までのレイヤー列を
// Road Path ごとの評価器（512²。配置の点の評価器と同じ流儀）で評価し、その CPU 側のハイトを
// 読む。最終出力（Path の表示が読むもの）ではないので、この先の地形の均し（切土・盛土）が
// 路面の高さを地形へ書き戻しても、中心線がそれを読み直して浮いていく循環にはならない。
// 縦断ポイントが無ければ中心線は地形に沿い、置くとその間を縦断曲線でつなぐ。
// 地形との差（切土・盛土）は Road Mesh の段で地形を均すときの量になる。

#include "app/Application.h"

#include "app/ApplicationUiHelpers.h"
#include "core/Log.h"
#include "graph/RoadPath.h"
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

// 縦断図の高さ（96 DPI 基準）。
constexpr float kProfilePlotHeight = 150.0f;
// 最大勾配を測る区間（m）。1 m ごとの差では地形の細かい凹凸を拾いすぎる。
constexpr float kGradeWindowMeters = 10.0f;
// ビューポートに切土・盛土の目安を立てる間隔（m）と、これより小さい差は描かない（m）。
constexpr float kCutFillTickMeters = 10.0f;
constexpr float kCutFillMinMeters = 0.3f;

// 区間 window ごとの勾配の最大（%）。
float MaxGradePercent(const graph::RoadProfileCurve& curve) {
    float best = 0.0f;
    const float total = curve.TotalLength();
    for (float d = 0.0f; d + kGradeWindowMeters <= total; d += kGradeWindowMeters * 0.5f) {
        const XMFLOAT3 a = curve.At(d);
        const XMFLOAT3 b = curve.At(d + kGradeWindowMeters);
        const float run = std::hypot(b.x - a.x, b.z - a.z);
        if (run > 1e-3f) best = std::max(best, std::abs(b.y - a.y) / run * 100.0f);
    }
    return best;
}

// 縦断ポイントを置く位置。今あるポイント（と両端）の間で一番広く空いた所の真ん中。
float NextProfilePointU(std::vector<float> taken) {
    taken.push_back(0.0f);
    taken.push_back(1.0f);
    std::sort(taken.begin(), taken.end());
    float bestU = 0.5f;
    float bestGap = -1.0f;
    for (size_t i = 1; i < taken.size(); ++i) {
        const float gap = taken[i] - taken[i - 1];
        if (gap > bestGap) {
            bestGap = gap;
            bestU = (taken[i] + taken[i - 1]) * 0.5f;
        }
    }
    return bestU;
}

// 位置の行。中心線の長さが分かれば m、分からなければ % で出す（保存は 0〜1）。
bool PropertyAlong(const char* label, float* u, float total, const char* tooltip) {
    if (total > 0.0f) {
        float meters = *u * total;
        if (ui::PropertyFloat(label, &meters, 0.0f, total, 0.5f * total, tooltip, "%.0f m")) {
            *u = std::clamp(meters / total, 0.0f, 1.0f);
            return true;
        }
        return false;
    }
    float percent = *u * 100.0f;
    if (ui::PropertyFloat(label, &percent, 0.0f, 100.0f, 50.0f, tooltip, "%.0f %%")) {
        *u = std::clamp(percent / 100.0f, 0.0f, 1.0f);
        return true;
    }
    return false;
}

}  // namespace

bool Application::RoadBaseConnected(const graph::Node& node) const {
    for (const graph::Pin& pin : node.inputs) {
        if (pin.valueType != graph::ValueType::Material) continue;
        for (const graph::Link& link : m_graph.Links()) {
            if (link.endPin == pin.id) return true;
        }
    }
    return false;
}

const compositor::CpuHeightfield* Application::RoadBaseHeightfield(graph::GraphId roadPathId) const {
    const auto found = m_roadBaseSlots.find(roadPathId);
    if (found == m_roadBaseSlots.end() || found->second->evaluator.Resolution() == 0) return nullptr;
    const compositor::CpuHeightfield& heightfield = found->second->evaluator.Heightfield();
    return heightfield.IsValid() ? &heightfield : nullptr;
}

void Application::PrepareRoadBaseTerrain() {
    // Base の地形の写しは 512²（パスの表示や経路探索の写しと同じ。2 km の地形で 4 m）。
    constexpr uint32_t kResolution = 512;
    std::vector<graph::GraphId> alive;
    for (const graph::Node& node : m_graph.Nodes()) {
        if (node.kind != graph::NodeKind::RoadPath) continue;
        alive.push_back(node.id);
        auto& slot = m_roadBaseSlots[node.id];
        if (!slot) slot = std::make_unique<RoadBaseSlot>();
        if (slot->evaluator.Resolution() == 0) {
            if (!slot->evaluator.Create(m_device, kResolution, /*asynchronous=*/true)) {
                TG_LOG_WARN("Road Path の Base の地形の評価器を作れませんでした");
                continue;
            }
            slot->evaluator.SetTileSize(kResolution);
            slot->evaluator.SetMeshFootprints(&m_meshFootprints);
        }
        const graph::TerrainScale* scale = m_graph.FindChainScale(node.id);
        slot->stack.SetTerrainScale(scale ? scale->sizeMeters : m_renderer.PlaneSize(),
                                    scale ? scale->heightMeters : m_renderer.DisplacementScale());
        if (slot->graphRevision == m_graph.TerrainRevision() && slot->paintRevision == m_paintMasks.Revision()) {
            continue;
        }
        slot->graphRevision = m_graph.TerrainRevision();
        slot->paintRevision = m_paintMasks.Revision();
        // Base までのレイヤー列（Base が繋がっていなければ変位 0 の平面）。点を動かしただけでは
        // 列は変わらないので、中身のハッシュが同じなら評価し直さない。
        graph::CompiledGraph compiled = m_graph.CompileLayersTo(node.id);
        compositor::MaterialStack candidate;
        candidate.Layers() = std::move(compiled.layers);
        candidate.MaskOps() = std::move(compiled.maskOps);
        candidate.SetTerrainScale(slot->stack.SizeMeters(), slot->stack.HeightMeters());
        uint64_t hash = compositor::HashStackHeightState(candidate);
        const uint64_t paintRevision = m_paintMasks.Revision();
        hash ^= paintRevision * 0x9E3779B97F4A7C15ull;
        if (hash == slot->stackHash && slot->evaluator.Heightfield().IsValid()) continue;
        slot->stackHash = hash;
        slot->stack.Layers() = std::move(candidate.Layers());
        slot->stack.MaskOps() = std::move(candidate.MaskOps());
        slot->stack.MarkDirty();
        slot->evaluator.Invalidate();
    }
    for (auto it = m_roadBaseSlots.begin(); it != m_roadBaseSlots.end();) {
        if (std::find(alive.begin(), alive.end(), it->first) == alive.end()) {
            if (it->second->evaluator.Resolution() != 0) it->second->evaluator.Destroy(m_device);
            it = m_roadBaseSlots.erase(it);
        } else {
            ++it;
        }
    }
}

bool Application::BuildRoadCenterline(const graph::Node& node, graph::RoadProfileCurve& base,
                                      graph::RoadProfileCurve& centerline, std::string* error,
                                      bool* pending) const {
    if (pending) *pending = false;
    const auto* settings = std::get_if<graph::RoadPathNodeSettings>(&node.settings);
    if (settings == nullptr) {
        if (error) *error = "Road Path ではありません";
        return false;
    }
    const compositor::CpuHeightfield* heightfield = RoadBaseHeightfield(node.id);
    if (heightfield == nullptr) {
        if (pending) *pending = true;
        if (error) *error = "Base の地形を評価中";
        return false;
    }
    const float scale = m_renderer.DisplacementScale();
    // ワールドの高さ（PathWorldPosition と同じ換算。読む元だけが Base の地形）。
    const graph::RoadHeightSampler height = [heightfield, scale](float u, float v) {
        return (heightfield->Sample(u, v) - 0.5f) * scale;
    };
    if (!graph::BuildRoadBaseline(settings->road, m_renderer.PlaneSize(), height, base, error)) {
        return false;
    }
    const std::vector<float> heights = graph::EvaluateVerticalProfile(settings->road, base);
    std::vector<XMFLOAT3> points = base.points;
    for (size_t i = 0; i < points.size(); ++i) points[i].y = heights[i];
    centerline = graph::BuildRoadProfileCurve(points);
    return true;
}

bool Application::DrawRoadPathSettings(graph::Node& node) {
    auto* settings = std::get_if<graph::RoadPathNodeSettings>(&node.settings);
    if (settings == nullptr) {
        return false;
    }
    graph::RoadPathSettings& road = settings->road;
    const graph::RoadPathSettings defaults;
    bool changed = false;

    graph::RoadProfileCurve base;
    graph::RoadProfileCurve centerline;
    std::string error;
    const bool valid = BuildRoadCenterline(node, base, centerline, &error);
    const float total = valid ? centerline.TotalLength() : 0.0f;
    // 標高で出す（Heightmap の最低標高が分かれば）。ワールドの Y はハイト 0.5 が 0。
    const graph::TerrainScale* scale = m_graph.FindChainScale(0);
    const float elevationOffset =
        scale != nullptr ? scale->baseElevationMeters + 0.5f * m_renderer.DisplacementScale() : 0.0f;

    // --- 縦断 -----------------------------------------------------------------
    ui::SectionHeader("縦断");
    if (!valid) {
        ui::HintText("縦断とバンク角は、分岐・閉ループ・孤立点のない 1 本の線で求める（%s）",
                     error.c_str());
    }
    // 中心線は Base に繋いだ地形に沿う（最終出力ではない）。繋いでいなければ平らな板の上。
    if (!RoadBaseConnected(node)) {
        ui::HintText("Base が繋がっていないので、中心線は変位 0 の平面に沿う。"
                     "均す前の地形を Base に繋ぐと、その地形に沿う");
    }
    if (valid && ui::BeginPropertyTable("roadProfileRows")) {
        float low = centerline.points.front().y;
        float high = low;
        for (const XMFLOAT3& p : centerline.points) {
            low = std::min(low, p.y);
            high = std::max(high, p.y);
        }
        ui::PropertyValue("延長", "%.0f m", total);
        ui::PropertyValue(scale != nullptr ? "標高" : "高さ", "%.0f 〜 %.0f m", low + elevationOffset,
                          high + elevationOffset);
        ui::PropertyValue("最大勾配", "%.1f %%（地形のまま %.1f %%）", MaxGradePercent(centerline),
                          MaxGradePercent(base));
        ui::EndPropertyTable();
    }

    // 縦断図。横が道のり、縦が高さ。地形（+ 点のずれ）を淡く、縦断を反映した高さを強く描く。
    // 図の上で縦断ポイントを直接編集する: 空いた所をクリックで追加、菱形をクリックで選択、
    // ドラッグで位置（横）と高さ（縦）を動かす。菱形は曲線ではなく縦断曲線の交点（制御点）に描く
    // （曲線長があると、曲線は交点の内側を通る）。
    RoadProfileEditState& edit = m_roadProfileEdit;
    if (edit.nodeId != node.id) {
        const graph::RoadVerticalAutoParams keep = edit.autoParams;
        edit = RoadProfileEditState{};
        edit.nodeId = node.id;
        edit.autoParams = keep;
    }
    if (edit.selected != 0 && graph::FindVerticalPoint(road, edit.selected) == nullptr) edit.selected = 0;
    if (edit.dragging != 0 && graph::FindVerticalPoint(road, edit.dragging) == nullptr) edit.dragging = 0;

    // 縦断ポイントの位置の順（見出しの番号と、勾配線を引く順）。
    const auto sortedPoints = [&road]() {
        std::vector<size_t> order(road.verticalPoints.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            return road.verticalPoints[a].u < road.verticalPoints[b].u;
        });
        return order;
    };
    graph::PathElementId removeId = 0;

    if (valid && centerline.points.size() >= 2) {
        const float width = ImGui::GetContentRegionAvail().x;
        const float height = ui::Scaled(kProfilePlotHeight);
        const ImVec2 min = ImGui::GetCursorScreenPos();
        const ImVec2 max(min.x + width, min.y + height);
        ImGui::InvisibleButton("##roadProfilePlot", ImVec2(width, height));
        const bool hovered = ImGui::IsItemHovered();
        const bool pressed = ImGui::IsItemActivated();
        const bool active = ImGui::IsItemActive();
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(min, max, ImGui::GetColorU32(ImGuiCol_FrameBg), ImGui::GetStyle().FrameRounding);
        drawList->AddRect(min, max, ImGui::GetColorU32(ImGuiCol_Border), ImGui::GetStyle().FrameRounding);

        // 交点（制御点）の高さ = その位置の地形 + 高さのずれ（EvaluateVerticalProfile と同じ）。
        const auto handleHeight = [&](const graph::RoadVerticalPoint& point) {
            return base.At(std::clamp(point.u, 0.0f, 1.0f) * total).y + point.offsetMeters;
        };
        // 縦の範囲。ドラッグ中は掴んだときの範囲のまま。
        if (edit.dragging == 0) {
            float low = centerline.points.front().y;
            float high = low;
            for (size_t i = 0; i < centerline.points.size(); ++i) {
                low = std::min({low, centerline.points[i].y, base.points[i].y});
                high = std::max({high, centerline.points[i].y, base.points[i].y});
            }
            for (const graph::RoadVerticalPoint& point : road.verticalPoints) {
                low = std::min(low, handleHeight(point));
                high = std::max(high, handleHeight(point));
            }
            if (high - low < 2.0f) {
                const float mid = (high + low) * 0.5f;
                low = mid - 1.0f;
                high = mid + 1.0f;
            }
            const float margin = (high - low) * 0.08f;
            edit.plotLow = low - margin;
            edit.plotHigh = high + margin;
        }
        const float low = edit.plotLow;
        const float high = edit.plotHigh;
        const float pad = ui::Scaled(6.0f);
        const float innerWidth = width - pad * 2.0f;
        const float innerHeight = height - pad * 2.0f;
        const auto toScreen = [&](float distance, float y) {
            return ImVec2(min.x + pad + (distance / total) * innerWidth,
                          max.y - pad - (y - low) / (high - low) * innerHeight);
        };
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const float mouseDistance = (mouse.x - min.x - pad) / innerWidth * total;
        const float mouseHeight = low + (max.y - pad - mouse.y) / innerHeight * (high - low);

        // カーソルの下の菱形（一番近いもの）。
        const float pickRadius = ui::Scaled(7.0f);
        graph::PathElementId hoverPoint = 0;
        float hoverDistance = pickRadius;
        if (hovered || active) {
            for (const graph::RoadVerticalPoint& point : road.verticalPoints) {
                const ImVec2 at = toScreen(std::clamp(point.u, 0.0f, 1.0f) * total, handleHeight(point));
                const float distance = std::hypot(at.x - mouse.x, at.y - mouse.y);
                if (distance <= hoverDistance) {
                    hoverDistance = distance;
                    hoverPoint = point.id;
                }
            }
        }

        // 押したとき: 菱形なら選んで掴む、空いた所ならその道のり・高さにポイントを置いて掴む。
        if (pressed) {
            if (hoverPoint != 0) {
                const graph::RoadVerticalPoint* point = graph::FindVerticalPoint(road, hoverPoint);
                edit.selected = edit.dragging = hoverPoint;
                edit.grabDistance = std::clamp(point->u, 0.0f, 1.0f) * total - mouseDistance;
                edit.grabHeight = handleHeight(*point) - mouseHeight;
            } else {
                const float distance = std::clamp(mouseDistance, 0.0f, total);
                const graph::PathElementId id = graph::AddVerticalPoint(road, distance / total);
                if (graph::RoadVerticalPoint* point = graph::FindVerticalPoint(road, id)) {
                    point->offsetMeters = mouseHeight - base.At(distance).y;
                }
                edit.selected = edit.dragging = id;
                edit.grabDistance = 0.0f;
                edit.grabHeight = 0.0f;
                changed = true;
            }
        }
        // ドラッグ: 位置と、その位置の地形からの高さのずれを置き直す。
        if (active && edit.dragging != 0) {
            if (graph::RoadVerticalPoint* point = graph::FindVerticalPoint(road, edit.dragging)) {
                const float distance = std::clamp(mouseDistance + edit.grabDistance, 0.0f, total);
                const float u = distance / total;
                const float offset = (mouseHeight + edit.grabHeight) - base.At(distance).y;
                if (u != point->u || offset != point->offsetMeters) {
                    point->u = u;
                    point->offsetMeters = offset;
                    changed = true;
                }
            }
        }
        if (!active) edit.dragging = 0;
        if (hovered && !active && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) edit.selected = 0;
        if ((hovered || active) && edit.selected != 0 && ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
            removeId = edit.selected;
        }
        if (edit.dragging != 0) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        } else if (hoverPoint != 0) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }

        // 編集を反映した線で描く（このフレームで動かした分も遅れずに見せる）。
        graph::RoadProfileCurve design = centerline;
        if (changed) {
            const std::vector<float> heights = graph::EvaluateVerticalProfile(road, base);
            for (size_t i = 0; i < design.points.size(); ++i) design.points[i].y = heights[i];
            design = graph::BuildRoadProfileCurve(design.points);
        }
        // 横の画素ごとに 1 点。中心線は 1 m ごとなので、長い道路では間引く。
        const int columns = std::max(2, static_cast<int>(innerWidth));
        std::vector<ImVec2> terrainLine;
        std::vector<ImVec2> designLine;
        terrainLine.reserve(columns + 1);
        designLine.reserve(columns + 1);
        for (int i = 0; i <= columns; ++i) {
            const float d = total * static_cast<float>(i) / static_cast<float>(columns);
            terrainLine.push_back(toScreen(d, base.At(d).y));
            designLine.push_back(toScreen(d, design.At(d).y));
        }
        drawList->PushClipRect(min, max, true);
        const std::vector<size_t> order = sortedPoints();
        // 勾配線（始点・交点・終点を結ぶ折れ線）。縦断曲線はこの折れ線の角を丸めたもの。
        if (!order.empty()) {
            std::vector<ImVec2> tangent;
            tangent.push_back(toScreen(0.0f, base.points.front().y));
            for (const size_t index : order) {
                const graph::RoadVerticalPoint& point = road.verticalPoints[index];
                tangent.push_back(toScreen(std::clamp(point.u, 0.0f, 1.0f) * total, handleHeight(point)));
            }
            tangent.push_back(toScreen(total, base.points.back().y));
            drawList->AddPolyline(tangent.data(), static_cast<int>(tangent.size()),
                                  ImGui::GetColorU32(ImGuiCol_Border), 0, ui::Scaled(1.0f));
        }
        drawList->AddPolyline(terrainLine.data(), static_cast<int>(terrainLine.size()),
                              ImGui::GetColorU32(ImGuiCol_TextDisabled), 0, ui::Scaled(1.0f));
        drawList->AddPolyline(designLine.data(), static_cast<int>(designLine.size()),
                              ImGui::GetColorU32(ImGuiCol_CheckMark), 0, ui::Scaled(2.0f));
        // 縦断ポイント。縦の細線と、交点の菱形。選んでいるものは大きく、本文の色で縁取る。
        for (const graph::RoadVerticalPoint& point : road.verticalPoints) {
            const float d = std::clamp(point.u, 0.0f, 1.0f) * total;
            const ImVec2 at = toScreen(d, handleHeight(point));
            const bool isSelected = point.id == edit.selected;
            drawList->AddLine(ImVec2(at.x, min.y + pad), ImVec2(at.x, max.y - pad),
                              ImGui::GetColorU32(isSelected ? ImGuiCol_TextDisabled : ImGuiCol_Border),
                              ui::Scaled(1.0f));
            const float r = ui::Scaled(isSelected ? 6.0f : (point.id == hoverPoint ? 5.0f : 4.0f));
            if (isSelected) {
                const float o = r + ui::Scaled(1.5f);
                drawList->AddQuadFilled(ImVec2(at.x, at.y - o), ImVec2(at.x + o, at.y), ImVec2(at.x, at.y + o),
                                        ImVec2(at.x - o, at.y), ImGui::GetColorU32(ImGuiCol_Text));
            }
            drawList->AddQuadFilled(ImVec2(at.x, at.y - r), ImVec2(at.x + r, at.y), ImVec2(at.x, at.y + r),
                                    ImVec2(at.x - r, at.y), ImGui::GetColorU32(ImGuiCol_CheckMark));
        }
        char text[64] = {};
        std::snprintf(text, sizeof(text), "%.0f m", high + elevationOffset);
        drawList->AddText(ImVec2(min.x + pad, min.y + pad * 0.5f), ImGui::GetColorU32(ImGuiCol_TextDisabled), text);
        std::snprintf(text, sizeof(text), "%.0f m", low + elevationOffset);
        drawList->AddText(ImVec2(min.x + pad, max.y - pad * 0.5f - ImGui::GetTextLineHeight()),
                          ImGui::GetColorU32(ImGuiCol_TextDisabled), text);
        std::snprintf(text, sizeof(text), "%.0f m", total);
        drawList->AddText(ImVec2(max.x - pad - ImGui::CalcTextSize(text).x, max.y - pad * 0.5f - ImGui::GetTextLineHeight()),
                          ImGui::GetColorU32(ImGuiCol_TextDisabled), text);
        // カーソルの位置の値。ポイントの上・ドラッグ中はそのポイントの値、ほかは道のり・設計の高さ・
        // 地形との差（正が盛土）・勾配。
        const graph::PathElementId shown = edit.dragging != 0 ? edit.dragging : hoverPoint;
        if (shown != 0) {
            const graph::RoadVerticalPoint* point = graph::FindVerticalPoint(road, shown);
            const float d = std::clamp(point->u, 0.0f, 1.0f) * total;
            ImGui::SetTooltip("縦断ポイント\n%.0f m\n交点の%s %.1f m\n地形との差 %+.1f m\n"
                              "ドラッグで位置と高さ、Delete で削除",
                              d, scale != nullptr ? "標高" : "高さ", handleHeight(*point) + elevationOffset,
                              point->offsetMeters);
        } else if (hovered) {
            const float d = std::clamp(mouseDistance, 0.0f, total);
            drawList->AddLine(ImVec2(mouse.x, min.y), ImVec2(mouse.x, max.y), ImGui::GetColorU32(ImGuiCol_Border));
            const float value = design.At(d).y;
            const float terrain = base.At(d).y;
            const float half = kGradeWindowMeters * 0.5f;
            const XMFLOAT3 a = design.At(std::max(0.0f, d - half));
            const XMFLOAT3 b = design.At(std::min(total, d + half));
            const float run = std::hypot(b.x - a.x, b.z - a.z);
            const float grade = run > 1e-3f ? (b.y - a.y) / run * 100.0f : 0.0f;
            ImGui::SetTooltip("%.0f m\n%s %.1f m\n地形との差 %+.1f m（%s）\n勾配 %+.1f %%\n"
                              "クリックで縦断ポイントを追加",
                              d, scale != nullptr ? "標高" : "高さ", value + elevationOffset, value - terrain,
                              value >= terrain ? "盛土" : "切土", grade);
        }
        drawList->PopClipRect();
    }

    // 選んでいる縦断ポイントだけプロパティを出す。
    if (graph::RoadVerticalPoint* point = graph::FindVerticalPoint(road, edit.selected)) {
        const std::vector<size_t> order = sortedPoints();
        size_t number = 0;
        for (size_t n = 0; n < order.size(); ++n) {
            if (road.verticalPoints[order[n]].id == point->id) number = n + 1;
        }
        ImGui::PushID(point->id);
        ImGui::TextDisabled("縦断ポイント %zu / %zu", number, order.size());
        if (ui::BeginPropertyTable("roadVerticalPoint")) {
            changed |= PropertyAlong("位置", &point->u, total, "始点からの道のり");
            changed |= ui::PropertyFloat("縦断曲線長", &point->vclMeters, 0.0f, 500.0f, 50.0f,
                                         "前後の勾配を放物線でつなぐ長さ（m）。0 で折れ線のまま。"
                                         "隣のポイントとの間より長くはならない",
                                         "%.0f m");
            changed |= ui::PropertyFloat("高さのずれ", &point->offsetMeters, -100.0f, 100.0f, 0.0f,
                                         "この位置の設計の高さ（交点）を、地形（+ 点のずれ）からどれだけ上げ下げするか（m）",
                                         "%+.1f m");
            ui::PropertyLabelEmpty("roadVerticalPointDelete");
            if (ui::Button("削除")) removeId = point->id;
            ui::PropertyEnd();
            ui::EndPropertyTable();
        }
        ImGui::PopID();
    } else if (!road.verticalPoints.empty()) {
        ImGui::TextDisabled("縦断ポイント %zu 個（図の菱形をクリックで選ぶ）", road.verticalPoints.size());
    }
    if (ui::Button("縦断ポイントを追加", ui::kWideButtonWidth)) {
        std::vector<float> taken;
        for (const graph::RoadVerticalPoint& point : road.verticalPoints) taken.push_back(point.u);
        edit.selected = graph::AddVerticalPoint(road, NextProfilePointU(taken));
        edit.autoReport.clear();
        changed = true;
    }

    // 自動で作成。地形の縦断をならして単純化し、特徴点を縦断ポイントにする（今のポイントは置き換える）。
    if (valid && ui::BeginPropertyTable("roadVerticalAuto", "縦断曲線長")) {
        graph::RoadVerticalAutoParams& p = edit.autoParams;
        const graph::RoadVerticalAutoParams d;
        ui::PropertyFloat("許容差", &p.toleranceMeters, 0.5f, 50.0f, d.toleranceMeters,
                          "交点を結ぶ折れ線が、ならした地形から離れてよい量（m）。小さいほど地形に沿い、"
                          "大きいほど均した道になる（切土・盛土の目安）",
                          "%.1f m");
        ui::PropertyFloat("最大勾配", &p.maxGradePercent, 1.0f, 30.0f, d.maxGradePercent,
                          "交点を結ぶ勾配の上限（%）。超える所は交点の高さを動かして収める。"
                          "両端の高さの差が急すぎると収まりきらない",
                          "%.1f %%");
        if (ui::PropertyFloat("縦断曲線長", &p.vclMeters, 0.0f, 500.0f, d.vclMeters,
                              "作る縦断ポイントの縦断曲線長（m）。点どうしの間隔もこれ以上空ける",
                              "%.0f m")) {
            p.minSpacingMeters = p.vclMeters;
        }
        ui::EndPropertyTable();
    }
    ImGui::BeginDisabled(!valid);
    if (ui::Button("縦断ポイントを自動で作成", ui::kWideButtonWidth)) {
        edit.autoParams.minSpacingMeters = std::max(edit.autoParams.vclMeters, 1.0f);
        const std::vector<graph::RoadVerticalPoint> generated = graph::GenerateVerticalPoints(base, edit.autoParams);
        graph::ReplaceVerticalPoints(road, generated);
        edit.selected = 0;
        // 結果（点の数、交点を結ぶ勾配の最大、地形との差の最大）。
        graph::RoadProfileCurve design = base;
        const std::vector<float> heights = graph::EvaluateVerticalProfile(road, base);
        float largestCut = 0.0f;
        float largestFill = 0.0f;
        for (size_t i = 0; i < design.points.size(); ++i) {
            largestFill = std::max(largestFill, heights[i] - base.points[i].y);
            largestCut = std::max(largestCut, base.points[i].y - heights[i]);
            design.points[i].y = heights[i];
        }
        design = graph::BuildRoadProfileCurve(design.points);
        char report[160] = {};
        std::snprintf(report, sizeof(report), "%zu 点を作成。最大勾配 %.1f %%、盛土 最大 %.1f m、切土 最大 %.1f m",
                      generated.size(), MaxGradePercent(design), largestFill, largestCut);
        edit.autoReport = report;
        changed = true;
    }
    ImGui::EndDisabled();
    if (!edit.autoReport.empty()) ui::HintText("%s", edit.autoReport.c_str());
    ui::HintText("図の空いた所をクリックで縦断ポイントを追加、菱形をクリックで選び、ドラッグで位置と高さを"
                 "動かす（Delete で削除、Esc で選択を外す）。ポイントが無ければ道路の高さは地形に沿う。"
                 "置くと、両端とポイントの間の勾配を縦断曲線でつなぐ（地形との差が切土・盛土になる）");

    // --- バンク角 --------------------------------------------------------------
    ui::SectionHeader("バンク角");
    if (ui::BeginPropertyTable("roadBankRows")) {
        changed |= ui::PropertyBool("バンク角を付ける", &road.bankEnabled, defaults.bankEnabled,
                                    "カーブで路面を内側へ傾ける。切ると手動のポイントがあっても水平のまま");
        ImGui::BeginDisabled(!road.bankEnabled);
        changed |= ui::PropertyFloat("設計速度", &road.designSpeedKmh, 10.0f, 150.0f, defaults.designSpeedKmh,
                                     "ポイントの無い所の設計速度（km/h）。速いほど同じカーブで大きく傾く",
                                     "%.0f km/h");
        changed |= ui::PropertyFloat("摩擦係数", &road.frictionCoefficient, 0.0f, 0.5f,
                                     defaults.frictionCoefficient,
                                     "横方向の摩擦係数。大きいほど、傾けずに済む分が増える", "%.2f");
        changed |= ui::PropertyBool("なめらかにする", &road.smoothBank, defaults.smoothBank,
                                    "距離方向にぼかし、傾きの変わり目を緩やかにする");
        if (road.smoothBank) {
            changed |= ui::PropertyFloat("なめらかにする距離", &road.bankSmoothMeters, 1.0f, 200.0f,
                                         defaults.bankSmoothMeters, "ぼかす範囲（m）", "%.0f m");
        }
        if (valid && road.bankEnabled) {
            float largest = 0.0f;
            for (float d = 0.0f; d <= total; d += std::max(1.0f, total / 200.0f)) {
                largest = std::max(largest, std::abs(graph::EvaluateBankAngleRadians(road, centerline, d)));
            }
            ui::PropertyValue("最大", "%.1f°", XMConvertToDegrees(largest));
        }
        ImGui::EndDisabled();
        ui::EndPropertyTable();
    }
    ImGui::BeginDisabled(!road.bankEnabled);
    std::vector<size_t> bankOrder(road.bankPoints.size());
    for (size_t i = 0; i < bankOrder.size(); ++i) bankOrder[i] = i;
    std::sort(bankOrder.begin(), bankOrder.end(), [&](size_t a, size_t b) {
        return road.bankPoints[a].u < road.bankPoints[b].u;
    });
    for (size_t n = 0; n < bankOrder.size(); ++n) {
        graph::RoadBankPoint& point = road.bankPoints[bankOrder[n]];
        ImGui::PushID(point.id);
        ImGui::TextDisabled("バンクポイント %zu", n + 1);
        if (ui::BeginPropertyTable("roadBankPoint")) {
            changed |= PropertyAlong("位置", &point.u, total, "始点からの道のり");
            changed |= ui::PropertyFloat("設計速度", &point.designSpeedKmh, 10.0f, 150.0f, road.designSpeedKmh,
                                         "この位置の設計速度（km/h）。自動の傾きはポイントの間で速度を補間して求める",
                                         "%.0f km/h");
            changed |= ui::PropertyBool("手動", &point.manual, false,
                                        "傾きを直接決める。手動のポイントの間は角度を直線で補間する");
            if (point.manual) {
                changed |= ui::PropertyFloat("角度", &point.angleDegrees, -30.0f, 30.0f, 0.0f,
                                             "正で進行方向に向かって左側が上がる（°）", "%+.1f°");
            }
            ui::PropertyLabelEmpty("roadBankPointDelete");
            if (ui::Button("削除")) removeId = point.id;
            ui::PropertyEnd();
            ui::EndPropertyTable();
        }
        ImGui::PopID();
    }
    if (ui::Button("バンクポイントを追加", ui::kWideButtonWidth)) {
        std::vector<float> taken;
        for (const graph::RoadBankPoint& point : road.bankPoints) taken.push_back(point.u);
        graph::AddBankPoint(road, NextProfilePointU(taken));
        changed = true;
    }
    ImGui::EndDisabled();
    ui::HintText("ポイントが無ければ、傾きは曲率・設計速度・摩擦係数から自動で決まる");

    if (removeId != 0 && graph::DeleteRoadProfilePoint(road, removeId)) {
        changed = true;
    }
    return changed;
}

void Application::DrawRoadPathOverlay(const graph::Node& node, const ImVec2& viewportMin,
                                      const ImVec2& viewportMax) {
    const auto* settings = std::get_if<graph::RoadPathNodeSettings>(&node.settings);
    if (settings == nullptr) {
        return;
    }
    const graph::RoadPathSettings& road = settings->road;
    graph::RoadProfileCurve base;
    graph::RoadProfileCurve centerline;
    if (!BuildRoadCenterline(node, base, centerline, nullptr) || centerline.points.size() < 2) {
        return;
    }
    const ImVec2 size(viewportMax.x - viewportMin.x, viewportMax.y - viewportMin.y);
    if (size.x <= 0.0f || size.y <= 0.0f) {
        return;
    }
    const renderer::Camera& camera = m_renderer.GetCamera();
    const XMMATRIX viewProjection = camera.ViewMatrix() * camera.ProjectionMatrix();
    const auto project = [&](const XMFLOAT3& world) {
        return ProjectToViewport(viewProjection, world, viewportMin, size);
    };

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->PushClipRect(viewportMin, viewportMax, true);
    // 色は Path の表示（ApplicationPathEdit.cpp）と同じ流儀で、Road Path のピンの淡い黄色に揃える。
    const ImU32 designColor = IM_COL32(245, 225, 140, 235);
    const ImU32 shadow = IM_COL32(0, 0, 0, 130);
    const ImU32 fillColor = IM_COL32(245, 190, 120, 200);  // 盛土（道路が地形より上）
    const ImU32 cutColor = IM_COL32(150, 200, 245, 200);   // 切土（道路が地形より下）

    const float total = centerline.TotalLength();
    // 縦断を反映した中心線。2 m ごと（長い道路では画面に効かない細かさを間引く）。
    const float step = std::max(2.0f, total / 4000.0f);
    ProjectedPoint previous = project(centerline.At(0.0f));
    for (float d = step; ; d += step) {
        const float at = std::min(d, total);
        const ProjectedPoint current = project(centerline.At(at));
        if (previous.visible && current.visible) {
            drawList->AddLine(previous.screen, current.screen, shadow, ui::Scaled(4.0f));
            drawList->AddLine(previous.screen, current.screen, designColor, ui::Scaled(2.0f));
        }
        previous = current;
        if (at >= total) break;
    }
    // 切土・盛土の目安。設計の高さと地形の間に縦の線を立てる（差が小さい所は描かない）。
    for (float d = 0.0f; d <= total; d += kCutFillTickMeters) {
        const XMFLOAT3 design = centerline.At(d);
        const XMFLOAT3 terrain = base.At(d);
        if (std::abs(design.y - terrain.y) < kCutFillMinMeters) continue;
        const ProjectedPoint a = project(design);
        const ProjectedPoint b = project(terrain);
        if (a.visible && b.visible) {
            drawList->AddLine(a.screen, b.screen, design.y > terrain.y ? fillColor : cutColor, ui::Scaled(1.5f));
        }
    }
    // 縦断ポイント（菱形）。縦断図で選んでいるものは大きく描く。
    for (const graph::RoadVerticalPoint& point : road.verticalPoints) {
        const ProjectedPoint p = project(centerline.At(std::clamp(point.u, 0.0f, 1.0f) * total));
        if (!p.visible) continue;
        const bool isSelected = m_roadProfileEdit.nodeId == node.id && m_roadProfileEdit.selected == point.id;
        const float r = ui::Scaled(isSelected ? 9.0f : 6.0f);
        const ImVec2 c = p.screen;
        drawList->AddQuadFilled(ImVec2(c.x, c.y - r - 1), ImVec2(c.x + r + 1, c.y), ImVec2(c.x, c.y + r + 1),
                                ImVec2(c.x - r - 1, c.y), shadow);
        drawList->AddQuadFilled(ImVec2(c.x, c.y - r), ImVec2(c.x + r, c.y), ImVec2(c.x, c.y + r),
                                ImVec2(c.x - r, c.y), designColor);
    }
    // バンク。ポイントの位置（無ければ一定間隔）に、道路の幅ぶんの傾いた横棒を描く。
    if (road.bankEnabled) {
        const float halfWidth = road.path.defaultWidthMeters * 0.5f;
        const auto drawBank = [&](float u, ImU32 color) {
            const graph::RoadProfileFrame frame = graph::EvaluateRoadProfileFrame(road, centerline, u);
            const float c = std::cos(frame.bankRadians) * halfWidth;
            const float s = std::sin(frame.bankRadians) * halfWidth;
            // 正のバンクで Left（-right）側が上がる。
            const XMFLOAT3 left{frame.position.x - frame.right.x * c, frame.position.y + s,
                                frame.position.z - frame.right.z * c};
            const XMFLOAT3 right{frame.position.x + frame.right.x * c, frame.position.y - s,
                                 frame.position.z + frame.right.z * c};
            const ProjectedPoint a = project(left);
            const ProjectedPoint b = project(right);
            if (a.visible && b.visible) {
                drawList->AddLine(a.screen, b.screen, shadow, ui::Scaled(4.0f));
                drawList->AddLine(a.screen, b.screen, color, ui::Scaled(2.0f));
            }
        };
        const float interval = std::max(25.0f, total / 80.0f);
        for (float d = 0.0f; d <= total; d += interval) drawBank(d / total, IM_COL32(245, 225, 140, 140));
        for (const graph::RoadBankPoint& point : road.bankPoints) drawBank(point.u, designColor);
    }
    drawList->PopClipRect();
}

}  // namespace tg
