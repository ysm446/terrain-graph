// ビューポートパネルと、その上の入力（軌道 / ライトドラッグ / ブラシ）、
// 重ねて描くギズモ類。

#include "app/Application.h"

#include "app/ApplicationUiHelpers.h"
#include "core/FileDialog.h"
#include "core/Log.h"
#include "io/ProjectIo.h"
#include "ui/UiStyle.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <DirectXMath.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace tg {

compositor::MaterialLayer* Application::CurrentPaintLayer() {
    if (!m_paintMode) {
        return nullptr;
    }

    // 選択中ノードのレイヤーがペイントの対象。
    graph::Node* node = m_graph.FindMutableNode(m_selectedGraphNode);
    auto* settings =
        (node != nullptr) ? std::get_if<graph::LayerNodeSettings>(&node->settings) : nullptr;
    compositor::MaterialLayer* layer = (settings != nullptr) ? &settings->layer : nullptr;

    if (layer == nullptr || layer->mask.source != compositor::MaskSource::Paint ||
        layer->mask.paint == compositor::kNoPaintMask) {
        return nullptr;
    }
    return layer;
}

// 3 桁ごとに区切る。**桁数の多い数はそのままだと読めない。**
std::string GroupDigits(uint64_t value) {
    std::string digits = std::to_string(value);
    for (int i = static_cast<int>(digits.size()) - 3; i > 0; i -= 3) {
        digits.insert(static_cast<size_t>(i), ",");
    }
    return digits;
}

// ビューポートに重ねる操作。表示モードの切り替えと、重ねる情報の切り替え。
//
// トップメニューではなくビューポートの中に置く。見ている場所から目を離さずに
// 切り替えられ、いまどの表示なのかも常に見える。
void Application::DrawViewportOverlay(const ImVec2& viewportMin, const ImVec2& viewportMax) {
    const float margin = ui::Scaled(10.0f);
    ImGui::SetCursorScreenPos(ImVec2(viewportMin.x + margin, viewportMin.y + margin));

    renderer::DebugView& current = m_renderer.Debug();
    const char* label = kDebugViewLabels[static_cast<size_t>(current)];

    // 既定以外の表示は見落としやすいので、ボタンの文字を強調する。
    const bool highlighted = (current != renderer::DebugView::Shaded);
    if (highlighted) {
        ImGui::PushStyleColor(ImGuiCol_Text, ui::WarnColor());
    }
    if (ImGui::Button(label)) {
        ImGui::OpenPopup("##viewportViewMenu");
    }
    if (highlighted) {
        ImGui::PopStyleColor();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("ビューポートに何を表示するか");
    }

    if (ImGui::BeginPopup("##viewportViewMenu")) {
        for (int i = 0; i < IM_ARRAYSIZE(kDebugViewLabels); ++i) {
            const auto view = static_cast<renderer::DebugView>(i);
            if (ImGui::Selectable(kDebugViewLabels[i], current == view)) {
                current = view;
            }
        }
        ImGui::EndPopup();
    }

    // --- 重ねる情報の切り替え ------------------------------------------------
    // FPS / 統計 / ハイトの範囲。どれもビューポートに重ねて出すものなので、
    // トップメニューではなくここに置く。切り替えたその場で設定に覚える。
    ImGui::SameLine();
    if (ImGui::Button("表示")) {
        ImGui::OpenPopup("##viewportDisplayMenu");
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("ビューポートに重ねる情報");
    }
    if (ImGui::BeginPopup("##viewportDisplayMenu")) {
        io::DisplaySettings& settings = m_settings.Display();
        bool changed = false;
        changed |= ImGui::MenuItem("FPS", nullptr, &settings.showFps);
        changed |= ImGui::MenuItem("統計", nullptr, &settings.showStats);
        changed |= ImGui::MenuItem("ハイトの範囲", nullptr, &settings.showHeightGuide);
        if (changed) {
            m_settings.Save();
        }
        ImGui::EndPopup();
    }

    // --- FPS と描画の量 ------------------------------------------------------
    // **右上へ置く。** 左上は表示モードの切り替えとライトの数値で埋まっている。
    // ボタンではなく描き込みにする。押すものではないので、枠を持たせない。
    const io::DisplaySettings& display = m_settings.Display();
    if (!display.showFps && !display.showStats) {
        return;
    }

    // 行を組み立ててから 1 つの下地にまとめて描く。**枠を 2 つ並べない。**
    // FPS と統計で別々の箱にすると、片方だけ出したときに位置が揃わない。
    std::vector<std::string> lines;
    if (display.showFps) {
        // 1 桁台では小数まで出す。整数だけだと 0.8fps が「0 FPS」になり、
        // 止まっているのか極端に遅いのかが読めない。
        const float framerate = ImGui::GetIO().Framerate;
        char text[32] = {};
        std::snprintf(text, sizeof(text), (framerate < 10.0f) ? "%.1f FPS" : "%.0f FPS",
                      framerate);
        lines.emplace_back(text);
    }
    if (display.showStats) {
        const renderer::RenderStats& stats = m_renderer.Stats();
        char text[96] = {};
        const double gpuMilliseconds = m_device.GpuFrameMilliseconds();
        if (gpuMilliseconds >= 0.0) {
            std::snprintf(text, sizeof(text), "GPU 処理 %.2f ms", gpuMilliseconds);
        } else {
            std::snprintf(text, sizeof(text), "GPU 処理 -- ms");
        }
        lines.emplace_back(text);
        std::snprintf(text, sizeof(text), "ドローコール %u", stats.drawCalls);
        lines.emplace_back(text);
        std::snprintf(text, sizeof(text), "頂点%s %s", stats.instanceUpperBounds ? "（上限）" : "", GroupDigits(stats.vertices).c_str());
        lines.emplace_back(text);
        // テセレーション中は、三角形はドメインシェーダが決めるので CPU では分からない。
        // **数えられないものを数えたふりをしない。** 投入したパッチ数と上限を出す。
        if (stats.tessellation) {
            std::snprintf(text, sizeof(text), "パッチ %s (x%.0f まで)",
                          GroupDigits(stats.patches).c_str(), stats.tessellationFactor);
        } else {
            std::snprintf(text, sizeof(text), "三角形%s %s", stats.instanceUpperBounds ? "（上限）" : "", GroupDigits(stats.triangles).c_str());
        }
        lines.emplace_back(text);
        // VRAM はプロセス全体の使用量とバジェット。合成の解像度を上げたときに
        // どれだけ余裕が残っているかを、その場で見えるようにする。
        const rhi::Device::VideoMemory vram = m_device.QueryVideoMemory();
        constexpr double kMegaBytes = 1024.0 * 1024.0;
        std::snprintf(text, sizeof(text), "VRAM %.0f / %.0f MB",
                      static_cast<double>(vram.usage) / kMegaBytes,
                      static_cast<double>(vram.budget) / kMegaBytes);
        lines.emplace_back(text);
    }

    const ImVec2 padding(ui::Scaled(8.0f), ui::Scaled(4.0f));
    const float lineHeight = ImGui::GetTextLineHeight();
    const float spacing = ImGui::GetStyle().ItemSpacing.y * 0.5f;
    float widest = 0.0f;
    for (const std::string& line : lines) {
        widest = std::max(widest, ImGui::CalcTextSize(line.c_str()).x);
    }
    const float height = lineHeight * static_cast<float>(lines.size()) +
                         spacing * static_cast<float>(lines.size() - 1);

    const ImVec2 boxMax(viewportMax.x - margin,
                        viewportMin.y + margin + height + padding.y * 2.0f);
    const ImVec2 boxMin(boxMax.x - widest - padding.x * 2.0f, viewportMin.y + margin);

    // 明るい素材の上でも読めるように、暗い下地を敷く。
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(boxMin, boxMax, IM_COL32(8, 10, 12, 190), ui::Scaled(4.0f));

    float y = boxMin.y + padding.y;
    for (const std::string& line : lines) {
        // **右へ揃える。** 桁数が変わるたびに数字の頭が動くと、目で追えない。
        const float width = ImGui::CalcTextSize(line.c_str()).x;
        drawList->AddText(ImVec2(boxMax.x - padding.x - width, y),
                          IM_COL32(235, 235, 235, 255), line.c_str());
        y += lineHeight + spacing;
    }
}

namespace {

int DaysInMonth(int year, int month) {
    static constexpr int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return (month == 2 && leap) ? 29 : kDays[std::clamp(month, 1, 12) - 1];
}

// 時刻を 0〜24 時に収め、はみ出したぶんだけ日付を進める / 戻す。
void NormalizeCelestialTime(renderer::CelestialSettings& c) {
    while (c.hour >= 24.0f) {
        c.hour -= 24.0f;
        if (++c.day > DaysInMonth(c.year, c.month)) {
            c.day = 1;
            if (++c.month > 12) { c.month = 1; ++c.year; }
        }
    }
    while (c.hour < 0.0f) {
        c.hour += 24.0f;
        if (--c.day < 1) {
            if (--c.month < 1) { c.month = 12; --c.year; }
            c.day = DaysInMonth(c.year, c.month);
        }
    }
}

}  // namespace

// L + 左ドラッグでライトの向きを変える。
// 「緯度経度と日時」のときは向きを直接動かせない（毎フレーム日時から計算される）ので、
// 横ドラッグで時刻を進める。縦は使わない。
//
// 修飾キー（Ctrl / Shift / Alt）は付けない。Alt は軌道、Ctrl は数値の直接入力に
// 使っているので、それらと重ならないようにする。
bool Application::HandleLightDrag(bool itemActive) {
    const ImGuiIO& io = ImGui::GetIO();
    const bool shortcut =
        ImGui::IsKeyDown(ImGuiKey_L) && !io.KeyCtrl && !io.KeyShift && !io.KeyAlt;
    if (!shortcut || !itemActive || !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        // 離したときにまとめて 1 段。ドラッグ中に毎フレーム積むと履歴が埋まる。
        if (m_lightDragChangedTime) MarkDocumentChanged(false);
        m_lightDragChangedTime = false;
        m_lightDragActive = false;
        return false;
    }

    m_lightDragActive = true;
    m_lightGizmoUntil = ImGui::GetTime() + kLightGizmoFadeSeconds;

    if (m_renderer.AtmosphericMode() && m_renderer.Celestial().mode == 1) {
        if (io.MouseDelta.x != 0.0f) {
            auto& celestial = m_renderer.Celestial();
            celestial.hour += io.MouseDelta.x * kLightMinutesPerPixel / 60.0f;
            NormalizeCelestialTime(celestial);
            // 次のフレームを待たず、掴んだまま太陽が動くようにする。
            ApplyCelestialSettings();
            m_lightDragChangedTime = true;
        }
        return true;
    }

    renderer::LightSettings& light = m_renderer.Light();
    const float step = DegreesToRadians(kLightDegreesPerPixel);
    // 方位角は一周させる。仰角は UI のスライダーと同じ範囲に収める。
    light.azimuth = WrapAngle(light.azimuth + io.MouseDelta.x * step);
    // 真下からの光も見たいので、下は -89 度まで許す。
    light.elevation = std::clamp(light.elevation - io.MouseDelta.y * step,
                                 DegreesToRadians(-89.0f), DegreesToRadians(89.0f));
    return true;
}

// F でメッシュを画面の中心へ戻し、A でさらに全体が収まる距離まで引く。
// DCC の「選択をフレーム / 全体をフレーム」に倣った割り当て。
//
// 修飾キーは付けない（Ctrl は数値の直接入力、Alt は軌道に使っている）。
// カーソルがビューポートの上にあるときだけ効かせ、
// **テキスト入力中は無視する**。レイヤー名を打っている最中に視点が飛ぶのを防ぐ。
void Application::HandleCameraShortcuts(bool itemHovered) {
    const ImGuiIO& io = ImGui::GetIO();
    if (!itemHovered || io.WantTextInput || io.KeyCtrl || io.KeyShift || io.KeyAlt) {
        return;
    }

    // プレビューのメッシュはどれも原点中心（モデル行列は単位行列）。
    constexpr DirectX::XMFLOAT3 kMeshCenter{0.0f, 0.0f, 0.0f};
    renderer::Camera& camera = m_renderer.GetCamera();

    if (ImGui::IsKeyPressed(ImGuiKey_F, false)) {
        camera.Focus(kMeshCenter);
    } else if (ImGui::IsKeyPressed(ImGuiKey_A, false)) {
        camera.Frame(kMeshCenter, m_renderer.BoundingRadius());
    }
}

// ライトの向きを示すギズモ。地面のリング、水平方向、仰角の弧、光が来る向きの矢印。
//
// 色はテーマから引かない。座標軸ギズモと同じく「意味を持つ色」として固定する。
void Application::DrawCloudShapeGizmo(const ImVec2& viewportMin, const ImVec2& viewportMax) {
    const auto* node=m_graph.FindNode(m_selectedGraphNode);
    if (!node) return;
    using namespace DirectX;
    const auto& camera=m_renderer.GetCamera();
    const XMMATRIX viewProjection=camera.ViewMatrix()*camera.ProjectionMatrix();
    const ImVec2 size(viewportMax.x-viewportMin.x,viewportMax.y-viewportMin.y);
    if (size.x<=0 || size.y<=0) return;
    ImDrawList* drawList=ImGui::GetWindowDrawList();
    const ImU32 color=ImGui::GetColorU32(ImGuiCol_PlotLinesHovered);
    drawList->PushClipRect(viewportMin,viewportMax,true);
    const auto segment=[&](const XMFLOAT3& a,const XMFLOAT3& b) {
        const auto pa=ProjectToViewport(viewProjection,a,viewportMin,size);
        const auto pb=ProjectToViewport(viewProjection,b,viewportMin,size);
        if (pa.visible && pb.visible) drawList->AddLine(pa.screen,pb.screen,color,ui::Scaled(1.25f));
    };
    if (const auto* animation=std::get_if<graph::CloudAnimationSettings>(&node->settings)) {
        const auto cloud=m_graph.CompileCloud();
        if (cloud.connected && cloud.sourceId==node->id) {
            const float x=animation->centerX,z=animation->centerZ,w=animation->width*0.5f,d=animation->depth*0.5f;
            const float bottom=cloud.cloud.centerY-cloud.cloud.thickness*0.5f,top=bottom+cloud.cloud.thickness;
            const XMFLOAT3 corners[]{{x-w,bottom,z-d},{x+w,bottom,z-d},{x+w,bottom,z+d},{x-w,bottom,z+d},
                {x-w,top,z-d},{x+w,top,z-d},{x+w,top,z+d},{x-w,top,z+d}};
            for (int i=0;i<4;++i) {
                segment(corners[i],corners[(i+1)%4]);
                segment(corners[i+4],corners[(i+1)%4+4]);
                segment(corners[i],corners[i+4]);
            }
        }
        drawList->PopClipRect();
        return;
    }
    // Wind Field は地表直上の風を矢印で示す。読み戻した格子（最後に走った Wind の op）を
    // 使うので、どこにも繋いでいない Wind Field は評価されず矢印も出ない。
    if (node->kind==graph::NodeKind::WindField) {
        const auto* settings=std::get_if<graph::MaskNodeSettings>(&node->settings);
        const compositor::CpuWindField* field=nullptr;
        for (const compositor::CpuWindField* candidate : {&m_renderer.Evaluator().WindField(),
                                                          &m_cloudMasks[0].evaluator.WindField(),
                                                          &m_cloudMasks[1].evaluator.WindField()}) {
            if (candidate->IsValid()) { field=candidate; break; }
        }
        if (settings && field) {
            // 色は雪面や岩と混ざらない橙。座標軸やライトのギズモと同じく意味を持つ固定色。
            m_renderer.SetGuideLineColor(1.0f,0.62f,0.2f);
            const float planeSize=std::max(m_renderer.PlaneSize(),1e-3f);
            const float displacement=m_renderer.DisplacementScale();
            // 高さは CPU のハイト（512²）があればそちらを使う。読み戻しの 64² では
            // 山頂の間に埋まって地形の下に隠れる。
            const compositor::CpuHeightfield& heightfield=m_renderer.Evaluator().Heightfield();
            constexpr int kArrows=28;
            const float cell=planeSize/float(kArrows);
            const float reference=std::max(field->windSpeed,1e-3f);
            for (int gy=0;gy<kArrows;++gy) for (int gx=0;gx<kArrows;++gx) {
                const float u=(float(gx)+0.5f)/float(kArrows), v=(float(gy)+0.5f)/float(kArrows);
                const uint32_t sx=std::min<uint32_t>(uint32_t(u*float(field->resolution)),field->resolution-1);
                const uint32_t sy=std::min<uint32_t>(uint32_t(v*float(field->resolution)),field->resolution-1);
                const XMFLOAT4 sample=field->values[size_t(sy)*field->resolution+sx];
                const float speed=std::sqrt(sample.x*sample.x+sample.y*sample.y+sample.z*sample.z);
                if (speed<1e-3f) continue;
                // 長さは風速に比例（一様風で格子の 0.8 倍）、地形から少し浮かせる。
                const float length=cell*0.8f*std::min(speed/reference,2.0f);
                const XMFLOAT3 direction{sample.x/speed,sample.y/speed,sample.z/speed};
                const float terrain=heightfield.IsValid() ? heightfield.Sample(u,v) : sample.w;
                const XMFLOAT3 origin{(u-0.5f)*planeSize,(terrain-0.5f)*displacement+cell*0.12f,(v-0.5f)*planeSize};
                const XMFLOAT3 tip{origin.x+direction.x*length,origin.y+direction.y*length,origin.z+direction.z*length};
                m_renderer.AddGuideLine(origin,tip,1.0f);
                // 矢じり。進行方向を水平面内で ±150 度回した短い線。
                const float hx=direction.x, hz=direction.z;
                const float horizontal=std::sqrt(hx*hx+hz*hz);
                if (horizontal>1e-4f) {
                    const float nx=hx/horizontal, nz=hz/horizontal, head=length*0.3f;
                    const float c=std::cos(2.6f), s=std::sin(2.6f);
                    const XMFLOAT3 left{tip.x+(nx*c-nz*s)*head,tip.y,tip.z+(nx*s+nz*c)*head};
                    const XMFLOAT3 right{tip.x+(nx*c+nz*s)*head,tip.y,tip.z+(-nx*s+nz*c)*head};
                    m_renderer.AddGuideLine(tip,left,1.0f);
                    m_renderer.AddGuideLine(tip,right,1.0f);
                }
            }
        }
        drawList->PopClipRect();
        return;
    }
    // 天候層は範囲（幅 × 奥行き）と雲底〜雲底 + 最大厚さの直方体で示す。
    // 球殻の沈み（遠方で層が下がるぶん）は描かない。設定した値がそのまま読める箱にする。
    // **ImGui ではなくレンダラのガイド線で描く。** 深度でテストするので地形の向こう側は隠れ、
    // カメラの後ろに端点がある辺も切れない（雲層は視点を囲む大きさになりがち）。
    if (const auto* weather=std::get_if<graph::CloudWeatherSettings>(&node->settings)) {
        const float x=weather->centerX,z=weather->centerZ,w=weather->width*0.5f,d=weather->depth*0.5f;
        const float bottom=weather->bottomHeight,top=bottom+weather->maxThickness;
        const XMFLOAT3 corners[]{{x-w,bottom,z-d},{x+w,bottom,z-d},{x+w,bottom,z+d},{x-w,bottom,z+d},
            {x-w,top,z-d},{x+w,top,z-d},{x+w,top,z+d},{x-w,top,z+d}};
        const ImVec4 guide=ImGui::GetStyleColorVec4(ImGuiCol_PlotLinesHovered);
        m_renderer.SetGuideLineColor(guide.x,guide.y,guide.z);
        for (int i=0;i<4;++i) {
            m_renderer.AddGuideLine(corners[i],corners[(i+1)%4]);
            m_renderer.AddGuideLine(corners[i+4],corners[(i+1)%4+4]);
            m_renderer.AddGuideLine(corners[i],corners[i+4]);
        }
        drawList->PopClipRect();
        return;
    }
    if (const auto* map=std::get_if<graph::CloudMapSettings>(&node->settings)) {
        if (map->showGuides) {
            const float x=map->centerX,z=map->centerZ,w=map->width*0.5f,d=map->depth*0.5f,y=map->bottomHeight;
            const XMFLOAT3 corners[]{{x-w,y,z-d},{x+w,y,z-d},{x+w,y,z+d},{x-w,y,z+d}};
            for (int i=0;i<4;++i) segment(corners[i],corners[(i+1)%4]);
            const auto generated=m_graph.CompileCloudShapes(node->id);
            if (generated.mapGuide) {
                const auto& guide=*generated.mapGuide;
                const size_t stride=std::max(size_t(1),(guide.points.size()+1023)/1024);
                for (size_t i=0;i<guide.points.size();i+=stride) {
                    if (map->removeIsolated && !guide.pointConnected[i]) continue;
                    const auto& point=guide.points[i];
                    const auto projected=ProjectToViewport(viewProjection,{point.x,point.y,point.z},viewportMin,size);
                    if (projected.visible) drawList->AddCircleFilled(projected.screen,ui::Scaled(2.5f),color);
                }
                for (const auto& edge:guide.edges) {
                    const auto& a=guide.points[edge.a]; const auto& b=guide.points[edge.b];
                    segment({a.x,a.y,a.z},{b.x,b.y,b.z});
                }
                for (const auto& line:guide.columns) segment({line.start.x,line.start.y,line.start.z},{line.end.x,line.end.y,line.end.z});
            }
        }
        drawList->PopClipRect();
        return;
    }
    const auto shapes=m_graph.CompileCloudShapes(node->id);
    // 配置ガイドだけを間引く。生成・ベイクには全形状を使う。
    const size_t stride=std::max(size_t(1),(shapes.primitives.size()+255)/256);
    for (size_t i=0;i<shapes.primitives.size();i+=stride) {
        const auto& shape=shapes.primitives[i];
        // 元形状を3つの大円で表示。ノイズ適用後の輪郭とは独立した配置ガイド。
        for (int plane=0;plane<3;++plane) {
            XMFLOAT3 previous{};
            for (int sample=0;sample<=64;++sample) {
                const float angle=XM_2PI*float(sample)/64;
                const float c=std::cos(angle),v=std::sin(angle);
                XMFLOAT3 point{shape.centerX,shape.centerY,shape.centerZ};
                if (plane==0) { point.x+=shape.radiusX*c; point.y+=shape.radiusY*v; }
                if (plane==1) { point.x+=shape.radiusX*c; point.z+=shape.radiusZ*v; }
                if (plane==2) { point.y+=shape.radiusY*c; point.z+=shape.radiusZ*v; }
                if (sample>0) segment(previous,point);
                previous=point;
            }
        }
    }
    drawList->PopClipRect();
}

bool Application::HandleCloudTransformGizmo(bool itemActive, bool itemHovered, const ImVec2& viewportMin, const ImVec2& viewportMax) {
    auto* node=m_graph.FindMutableNode(m_selectedGraphNode);
    auto* transform=node ? std::get_if<graph::CloudTransformSettings>(&node->settings) : nullptr;
    auto& drag=m_cloudTransformDrag;
    const auto& io=ImGui::GetIO();
    if (!transform || drag.node!=m_selectedGraphNode) drag={};
    if (!transform) return false;
    float* values[]{&transform->translateX,&transform->translateY,&transform->translateZ};
    const bool cancel=drag.axis>=0 && ImGui::IsKeyPressed(ImGuiKey_Escape);
    if (drag.Update(io.MousePos,ImGui::IsMouseDown(ImGuiMouseButton_Left),cancel,io.KeyAlt,values)) {
        m_graph.MarkCloudDirty(); MarkDocumentChanged(false);
    }
    if (cancel) return true;
    const auto shapes=m_graph.CompileCloudShapes(node->id);
    if (!shapes.connected || shapes.primitives.empty()) { drag={}; return false; }
    using namespace DirectX;
    XMFLOAT3 lower{FLT_MAX,FLT_MAX,FLT_MAX},upper{-FLT_MAX,-FLT_MAX,-FLT_MAX};
    for (const auto& p:shapes.primitives) {
        lower.x=std::min(lower.x,p.centerX-p.radiusX); upper.x=std::max(upper.x,p.centerX+p.radiusX);
        lower.y=std::min(lower.y,p.centerY-p.radiusY); upper.y=std::max(upper.y,p.centerY+p.radiusY);
        lower.z=std::min(lower.z,p.centerZ-p.radiusZ); upper.z=std::max(upper.z,p.centerZ+p.radiusZ);
    }
    const XMFLOAT3 center{(lower.x+upper.x)*0.5f,(lower.y+upper.y)*0.5f,(lower.z+upper.z)*0.5f};
    const auto& camera=m_renderer.GetCamera();
    const XMMATRIX vp=camera.ViewMatrix()*camera.ProjectionMatrix();
    const ImVec2 size{viewportMax.x-viewportMin.x,viewportMax.y-viewportMin.y};
    if (size.x<=0 || size.y<=0) return false;
    const auto projected=ProjectToViewport(vp,center,viewportMin,size);
    if (!projected.visible) return drag.axis>=0;
    XMFLOAT3 viewCenter;
    XMStoreFloat3(&viewCenter,XMVector3TransformCoord(XMLoadFloat3(&center),camera.ViewMatrix()));
    const float worldLength=std::max(0.001f,std::abs(viewCenter.z)*2*std::tan(camera.FovY()*0.5f)*ui::Scaled(90)/size.y);
    ImVec2 tips[3]{},directions[3]{};
    float lengths[3]{};
    int hover=-1;
    float best=ui::Scaled(9);
    for (int axis=0;axis<3;++axis) {
        XMFLOAT3 endpoint=center;
        if (axis==0) endpoint.x+=worldLength;
        if (axis==1) endpoint.y+=worldLength;
        if (axis==2) endpoint.z+=worldLength;
        const auto tip=ProjectToViewport(vp,endpoint,viewportMin,size);
        if (!tip.visible) continue;
        const float x=tip.screen.x-projected.screen.x,y=tip.screen.y-projected.screen.y;
        const float length=std::sqrt(x*x+y*y);
        // 視線に重なった軸は操作できないため表示しない。視点を回すか数値欄を使う。
        if (length<ui::Scaled(12)) continue;
        tips[axis]=tip.screen; lengths[axis]=length; directions[axis]={x/length,y/length};
        const float mx=io.MousePos.x-projected.screen.x,my=io.MousePos.y-projected.screen.y;
        const float t=std::clamp((mx*x+my*y)/(length*length),0.15f,1.0f);
        const float distance=std::hypot(mx-t*x,my-t*y);
        if (itemHovered && !io.KeyAlt && !ImGui::IsKeyDown(ImGuiKey_L) && distance<best) { best=distance; hover=axis; }
    }
    if (drag.axis<0 && hover>=0 && itemActive && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        drag.node=node->id; drag.axis=hover; drag.press=io.MousePos;
        drag.direction=directions[hover]; drag.metersPerPixel=worldLength/lengths[hover]; drag.start=*values[hover];
    }
    auto* draw=ImGui::GetWindowDrawList();
    draw->PushClipRect(viewportMin,viewportMax,true);
    const char* labels[]{"X","Y","Z"};
    for (int axis=0;axis<3;++axis) {
        if (lengths[axis]==0) continue;
        const bool active=drag.axis==axis || hover==axis;
        const ImU32 color=active ? ImGui::GetColorU32(ImGuiCol_PlotLinesHovered) : ui::TransformAxisColor(axis);
        draw->AddLine(projected.screen,tips[axis],color,ui::Scaled(active?4:2.5f));
        const auto d=directions[axis]; const auto tip=tips[axis]; const float arrow=ui::Scaled(9);
        draw->AddTriangleFilled(tip,{tip.x-d.x*arrow-d.y*arrow*0.45f,tip.y-d.y*arrow+d.x*arrow*0.45f},
            {tip.x-d.x*arrow+d.y*arrow*0.45f,tip.y-d.y*arrow-d.x*arrow*0.45f},color);
        draw->AddText({tip.x+d.x*ui::Scaled(7),tip.y+d.y*ui::Scaled(7)},color,labels[axis]);
    }
    draw->PopClipRect();
    if (hover>=0 || drag.axis>=0) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    return drag.axis>=0 || hover>=0;
}

void Application::DrawLightGizmo(const ImVec2& viewportMin, const ImVec2& viewportMax) {
    const double now = ImGui::GetTime();
    if (!m_lightDragActive && now >= m_lightGizmoUntil) {
        return;
    }
    const float fade =
        m_lightDragActive
            ? 1.0f
            : static_cast<float>(std::clamp((m_lightGizmoUntil - now) / kLightGizmoFadeSeconds,
                                            0.0, 1.0));
    if (fade <= 0.001f) {
        return;
    }

    using namespace DirectX;
    const renderer::Camera& camera = m_renderer.GetCamera();
    const XMMATRIX viewProjection = camera.ViewMatrix() * camera.ProjectionMatrix();
    const ImVec2 size(viewportMax.x - viewportMin.x, viewportMax.y - viewportMin.y);
    if (size.x <= 0.0f || size.y <= 0.0f) {
        return;
    }

    const renderer::LightSettings& light = m_renderer.Light();
    const XMFLOAT3 direction = light.Direction();
    // **見ているものの実寸に合わせる。** 素材（2m 角）でも地形（2km 角）でも
    // 同じ見え方になるよう、平面の一辺（m）から決める。固定値にすると、
    // 地形では原点の一点に潰れて見えなくなる。
    // 包む球（対角）ではなく**辺の半分**にしてあるのは、対角だと視界からはみ出して
    // リングが読めなくなるため。地形の縁に接するくらいがちょうどいい。
    const float gizmoRadius = m_renderer.PlaneSize() * 0.5f;
    const XMFLOAT3 origin{0.0f, 0.0f, 0.0f};
    const XMFLOAT3 horizontal{std::sin(light.azimuth), 0.0f, std::cos(light.azimuth)};

    const auto color = [fade](int r, int g, int b, int a) {
        return IM_COL32(r, g, b, static_cast<int>(static_cast<float>(a) * fade));
    };
    const auto offset = [](const XMFLOAT3& base, const XMFLOAT3& dir, float amount) {
        return XMFLOAT3{base.x + dir.x * amount, base.y + dir.y * amount,
                        base.z + dir.z * amount};
    };
    const auto project = [&](const XMFLOAT3& world) {
        return ProjectToViewport(viewProjection, world, viewportMin, size);
    };

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->PushClipRect(viewportMin, viewportMax, true);

    const auto drawWorldLine = [&](const XMFLOAT3& a, const XMFLOAT3& b, ImU32 lineColor,
                                   float thickness) {
        const ProjectedPoint pa = project(a);
        const ProjectedPoint pb = project(b);
        if (pa.visible && pb.visible) {
            drawList->AddLine(pa.screen, pb.screen, lineColor, thickness);
        }
    };

    // 地面のリング。方位角の目安になる。
    constexpr int kRingSegments = 72;
    ProjectedPoint previous;
    for (int i = 0; i <= kRingSegments; ++i) {
        const float t = (static_cast<float>(i) / kRingSegments) * 2.0f * 3.14159265f;
        const ProjectedPoint current =
            project(XMFLOAT3{std::sin(t) * gizmoRadius, 0.0f, std::cos(t) * gizmoRadius});
        if (i > 0 && previous.visible && current.visible) {
            drawList->AddLine(previous.screen, current.screen, color(150, 160, 175, 130), 1.6f);
        }
        previous = current;
    }

    // 水平方向への投影と、そこから仰角ぶんの弧。
    drawWorldLine(origin, offset(origin, horizontal, gizmoRadius), color(150, 160, 175, 170), 1.8f);

    constexpr int kArcSegments = 32;
    ProjectedPoint previousArc;
    for (int i = 0; i <= kArcSegments; ++i) {
        const float angle = light.elevation * (static_cast<float>(i) / kArcSegments);
        const float ring = std::cos(angle) * gizmoRadius;
        const ProjectedPoint current = project(
            XMFLOAT3{horizontal.x * ring, std::sin(angle) * gizmoRadius, horizontal.z * ring});
        if (i > 0 && previousArc.visible && current.visible) {
            drawList->AddLine(previousArc.screen, current.screen, color(255, 206, 112, 150), 1.6f);
        }
        previousArc = current;
    }

    // 光が来る向きの矢印。ライトの位置から原点へ向ける。
    const ProjectedPoint arrowStart = project(offset(origin, direction, gizmoRadius));
    const ProjectedPoint arrowEnd = project(offset(origin, direction, gizmoRadius * 0.22f));
    if (arrowStart.visible && arrowEnd.visible) {
        const ImU32 lightColor = color(255, 188, 76, 245);
        ImVec2 screenDir(arrowEnd.screen.x - arrowStart.screen.x,
                         arrowEnd.screen.y - arrowStart.screen.y);
        const float length = std::sqrt(screenDir.x * screenDir.x + screenDir.y * screenDir.y);
        if (length > 0.001f) {
            screenDir.x /= length;
            screenDir.y /= length;
            const ImVec2 side(-screenDir.y, screenDir.x);
            const float head = ui::Scaled(14.0f);
            const float halfWidth = ui::Scaled(6.0f);
            const ImVec2 base(arrowEnd.screen.x - screenDir.x * head,
                              arrowEnd.screen.y - screenDir.y * head);
            drawList->AddLine(arrowStart.screen, base, lightColor, ui::Scaled(3.5f));
            drawList->AddTriangleFilled(
                arrowEnd.screen, ImVec2(base.x + side.x * halfWidth, base.y + side.y * halfWidth),
                ImVec2(base.x - side.x * halfWidth, base.y - side.y * halfWidth), lightColor);
        }
    }

    if (const ProjectedPoint center = project(origin); center.visible) {
        drawList->AddCircle(center.screen, ui::Scaled(5.0f), color(200, 210, 220, 200), 20, 1.6f);
    }

    // いまの値。掴んだまま数字を確かめられるようにする。
    // 日時モードでは動かしているのが時刻なので、日付と時刻を先頭に出す。
    char text[96] = {};
    if (m_renderer.AtmosphericMode() && m_renderer.Celestial().mode == 1) {
        const auto& c = m_renderer.Celestial();
        const int minutes = static_cast<int>(std::lround(c.hour * 60.0f));
        std::snprintf(text, sizeof(text), "%d/%d %02d:%02d   方位角 %.0f 度   仰角 %.0f 度",
                      c.month, c.day, (minutes / 60) % 24, minutes % 60,
                      RadiansToDegrees(light.azimuth), RadiansToDegrees(light.elevation));
    } else {
        std::snprintf(text, sizeof(text), "方位角 %.0f 度   仰角 %.0f 度",
                      RadiansToDegrees(light.azimuth), RadiansToDegrees(light.elevation));
    }
    const ImVec2 textSize = ImGui::CalcTextSize(text);
    const ImVec2 padding(ui::Scaled(8.0f), ui::Scaled(5.0f));
    // 左上には表示モードのボタンがあるので、その下へ置く。
    const ImVec2 textMin(viewportMin.x + ui::Scaled(10.0f),
                         viewportMin.y + ui::Scaled(10.0f) + ImGui::GetFrameHeight() +
                             ui::Scaled(6.0f));
    const ImVec2 textMax(textMin.x + textSize.x + padding.x * 2.0f,
                         textMin.y + textSize.y + padding.y * 2.0f);
    drawList->AddRectFilled(textMin, textMax, color(8, 10, 12, 190), ui::Scaled(4.0f));
    drawList->AddText(ImVec2(textMin.x + padding.x, textMin.y + padding.y),
                      color(235, 235, 235, 255), text);

    drawList->PopClipRect();
}

// ハイトの範囲のラベル（0.0 / 0.5 / 1.0）。
//
// **枠の線はレンダラが描く**（`PreviewRenderer::DrawHeightGuideOverlay`）。
// シーンの深度でテストしてメッシュの向こう側を隠すためで、ImGui の
// オーバーレイでは深度が使えない。文字だけは ImGui で重ねる
// （手前の角に添えるので、隠れてもラベルの意味は保たれる）。
void Application::DrawHeightGuide(const ImVec2& viewportMin, const ImVec2& viewportMax) {
    if (!m_settings.Display().showHeightGuide) {
        return;
    }
    using namespace DirectX;
    const renderer::Camera& camera = m_renderer.GetCamera();
    const XMMATRIX viewProjection = camera.ViewMatrix() * camera.ProjectionMatrix();
    const ImVec2 size(viewportMax.x - viewportMin.x, viewportMax.y - viewportMin.y);
    if (size.x <= 0.0f || size.y <= 0.0f) {
        return;
    }

    const float half = m_renderer.PlaneSize() * 0.5f;
    const float scale = m_renderer.DisplacementScale();

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->PushClipRect(viewportMin, viewportMax, true);

    struct Level {
        float height;
        ImU32 color;
        const char* label;
    };
    // 0.5（元の面）は基準なので、レンダラの線と同じく少し落とした色にする。
    const Level levels[] = {
        {0.0f, IM_COL32(150, 160, 175, 200), "0.0"},
        {0.5f, IM_COL32(150, 160, 175, 110), "0.5"},
        {1.0f, IM_COL32(150, 160, 175, 200), "1.0"},
    };

    for (const Level& level : levels) {
        const float y = (level.height - 0.5f) * scale;
        // ラベルは手前の角（+X, +Z）へ。線と重ならないよう少し外へずらす。
        const ProjectedPoint corner =
            ProjectToViewport(viewProjection, XMFLOAT3{half, y, half}, viewportMin, size);
        if (corner.visible) {
            drawList->AddText(ImVec2(corner.screen.x + ui::Scaled(6.0f),
                                     corner.screen.y - ImGui::GetTextLineHeight() * 0.5f),
                              level.color, level.label);
        }
    }

    drawList->PopClipRect();
}

void Application::HandlePaintInput(compositor::MaterialLayer& layer, bool itemActive,
                                   const ImVec2& imageOrigin, const ImVec2& imageSize) {
    const ImGuiIO& io = ImGui::GetIO();

    const bool addPressed = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    const bool erasePressed = ImGui::IsMouseDown(ImGuiMouseButton_Right);
    if (!itemActive || (!addPressed && !erasePressed)) {
        m_strokeActive = false;
        return;
    }

    // 画像はコンテンツ領域に合わせて拡縮して描いているため、
    // ImGui の座標をレンダーターゲットのピクセル座標へ換算する。
    const float scaleX = (imageSize.x > 0.0f)
                             ? (static_cast<float>(m_renderer.Width()) / imageSize.x)
                             : 1.0f;
    const float scaleY = (imageSize.y > 0.0f)
                             ? (static_cast<float>(m_renderer.Height()) / imageSize.y)
                             : 1.0f;
    const float x = (io.MousePos.x - imageOrigin.x) * scaleX;
    const float y = (io.MousePos.y - imageOrigin.y) * scaleY;

    if (!m_strokeActive) {
        // ストロークを始める前の内容をアンドゥ履歴へ積む。
        // アンドゥの単位は「1 ストローク」で、押しっぱなしの間は 1 段に収まる。
        m_paintMasks.QueueSnapshot(m_device, layer.mask.paint);
        m_strokeActive = true;
        m_strokeLastX = x;
        m_strokeLastY = y;
        // 筆跡は GPU 上にあって文書の指紋に映らない。塗ったグラフを未保存として覚える。
        // 対象は選択中ノードのレイヤー（CurrentPaintLayer と同じ）。
        if (const graph::Node* node = m_graph.FindNode(m_selectedGraphNode); node != nullptr) {
            m_paintDirty |= node->component == 1 ? 2u : 1u;
            RefreshSceneDirty();
        }
    }

    compositor::BrushStroke stroke;
    stroke.target = layer.mask.paint;
    stroke.fromX = m_strokeLastX;
    stroke.fromY = m_strokeLastY;
    stroke.toX = x;
    stroke.toY = y;
    stroke.brush = m_brush;
    // 右ドラッグは加算 / 減算を入れ替える。消しゴムへ切り替えずに消せるようにするため。
    stroke.brush.erase = erasePressed ? !m_brush.erase : m_brush.erase;
    m_paintMasks.QueueStroke(stroke);

    m_strokeLastX = x;
    m_strokeLastY = y;
}

void Application::DrawViewportPanel() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    // ホイールでウィンドウがスクロールしないようにする（ズームに使うため）。
    const bool open = ImGui::Begin("ビューポート", nullptr,
                                   ImGuiWindowFlags_NoScrollbar |
                                       ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();

    if (open) {
        const ImVec2 available = ImGui::GetContentRegionAvail();
        // 8 の倍数に丸め、ドラッグ中の作り直しを減らす。
        const auto snap = [](float value) {
            const int clamped = std::clamp(static_cast<int>(value), 64, 4096);
            return static_cast<uint32_t>((clamped / 8) * 8);
        };
        m_requestedViewportWidth = snap(available.x);
        m_requestedViewportHeight = snap(available.y);

        if (m_renderer.HasOutput()) {
            // テクスチャの実サイズではなくコンテンツ領域に合わせて描く。
            // 実サイズで描くとパネルからはみ出し、スクロールバーの出入りで
            // 要求サイズが振動してしまう。作り直しは 1 フレーム遅れる。
            const ImVec2 imageOrigin = ImGui::GetCursorScreenPos();
            ImGui::Image(static_cast<ImTextureID>(m_renderer.OutputHandle().ptr), available);

            // ImGui::Image は入力を消費しないため、そのままだと画像上のドラッグが
            // 「ウィンドウの余白のドラッグ」と解釈されてパネルごと動いてしまう。
            // 同じ矩形に不可視ボタンを重ねてドラッグを受け止める。
            ImGui::SetCursorScreenPos(imageOrigin);
            // ビューポート内に重ねるボタン（表示モード）へ入力を譲る。
            ImGui::SetNextItemAllowOverlap();
            ImGui::InvisibleButton("##viewportInput", available,
                                   ImGuiButtonFlags_MouseButtonLeft |
                                       ImGuiButtonFlags_MouseButtonMiddle |
                                       ImGuiButtonFlags_MouseButtonRight);

            const ImGuiIO& io = ImGui::GetIO();
            renderer::Camera& camera = m_renderer.GetCamera();
            const bool itemActive = ImGui::IsItemActive();
            const bool itemHovered = ImGui::IsItemHovered();

            // L + 左ドラッグはライトの向き。ブラシや軌道より先に見る。
            const bool lightDragging = HandleLightDrag(itemActive);
            const ImVec2 imageMax(imageOrigin.x + available.x, imageOrigin.y + available.y);
            // ガイド線は毎フレーム積み直す（選択を外したら消える）。
            m_renderer.ClearGuideLines();
            DrawCloudShapeGizmo(imageOrigin, imageMax);
            const bool cloudDragging=HandleCloudTransformGizmo(itemActive && !lightDragging,itemHovered && !lightDragging,imageOrigin,imageMax);

            // ペイントモードの間は左 / 右ドラッグをブラシが受け取る。
            // 視点操作を残すため、軌道は Alt + 左ドラッグへ移す。
            compositor::MaterialLayer* paintLayer = CurrentPaintLayer();
            const bool brushEnabled = (paintLayer != nullptr) && !io.KeyAlt && !lightDragging && !cloudDragging;

            if (brushEnabled) {
                HandlePaintInput(*paintLayer, itemActive, imageOrigin, available);
            } else {
                m_strokeActive = false;
            }

            // Path ノードを選んでいる間は、左クリック / ドラッグと右クリックがパスの編集。
            // ペイントと同じく、視点は Alt を押している間だけ動く。
            graph::Node* pathNode = CurrentPathNode();
            const bool pathEnabled = (pathNode != nullptr) && !brushEnabled && !lightDragging;
            if (pathEnabled && !io.KeyAlt) {
                HandlePathInput(*pathNode, itemActive, itemHovered, imageOrigin, imageMax);
            } else if (!pathEnabled) {
                m_pathEdit.dragging = false;
                m_pathEdit.dragPoint = 0;
            }

            // 視点操作は Alt を押している間だけ受ける（Maya と同じ割り当て）。
            //
            // Alt なしのドラッグは、将来の選択や範囲選択のために空けてある。
            // Alt を押している間はブラシもライトも無効になる（上の brushEnabled と
            // HandleLightDrag が !io.KeyAlt を見る）ので、ここで競合は起きない。
            if (itemActive && io.KeyAlt) {
                if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                    camera.Orbit(io.MouseDelta.x * 0.006f, io.MouseDelta.y * 0.006f);
                } else if (ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
                    camera.Pan(io.MouseDelta.x, io.MouseDelta.y);
                } else if (ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
                    // 右へ引くと寄る。縦は見ない（斜めに引いたときに暴れるため）。
                    camera.Dolly(io.MouseDelta.x);
                }
            }

            if (itemHovered && !cloudDragging && io.MouseWheel != 0.0f) {
                camera.Zoom(io.MouseWheel);
            }

            HandleCameraShortcuts(itemHovered);

            DrawAxisGizmo(camera, imageOrigin, imageMax);
            DrawHeightGuide(imageOrigin, imageMax);
            DrawLightGizmo(imageOrigin, imageMax);
            if (pathNode != nullptr) {
                DrawPathOverlay(*pathNode, imageOrigin, imageMax);
            }

            // ビューポートに重ねる操作。左上に表示モードの切り替え、右上に FPS。
            DrawViewportOverlay(imageOrigin, imageMax);

            // ブラシの当たる範囲を円で示す。半径はビューポートのピクセル単位なので、
            // 表示倍率で割って ImGui の座標へ戻す。
            if (brushEnabled && itemHovered && available.x > 0.0f) {
                const float displayScale =
                    available.x / static_cast<float>(std::max(m_renderer.Width(), 1u));
                const float radius = m_brush.radiusPixels * displayScale;
                ImDrawList* drawList = ImGui::GetWindowDrawList();
                drawList->AddCircle(io.MousePos, radius + 1.0f, IM_COL32(0, 0, 0, 140), 0, 3.0f);
                drawList->AddCircle(io.MousePos, radius, IM_COL32(235, 235, 235, 200), 0, 1.5f);
            }
        }
    }
    ImGui::End();
}

}  // namespace tg
