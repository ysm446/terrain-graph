// ノードグラフパネル。imgui-node-editor によるエディタと、
// 選択中ノードのプロパティ（レイヤーパネルと共有）を持つ。
//
// エディタの作法（カード描画・丸ピン・ドット背景・リンクの作成 / 削除）は
// terrain-editor のノードエディタ UI から移植した。ノードそのものは
// このプロジェクト独自（サーフェス / シェイプ / 水面 / 出力）。

#include "app/Application.h"

#include "app/ApplicationUiHelpers.h"
#include "ui/UiStyle.h"

#include <imgui.h>
#include <imgui-node-editor/imgui_node_editor.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <variant>
#include <vector>

namespace ed = ax::NodeEditor;

namespace tg {
namespace {

ImU32 ColorToU32(const ImVec4& color) {
    return ImGui::ColorConvertFloat4ToU32(color);
}

// 種類ごとのアクセント色。グレー基調を崩さないよう彩度は低め。
ImVec4 NodeAccentColor(graph::NodeKind kind) {
    switch (kind) {
        case graph::NodeKind::Surface:
            return ImVec4(0.55f, 0.66f, 0.58f, 1.0f);
        case graph::NodeKind::Heightmap:
            return ImVec4(0.72f, 0.66f, 0.50f, 1.0f);
        case graph::NodeKind::Shape:
            return ImVec4(0.66f, 0.62f, 0.52f, 1.0f);
        case graph::NodeKind::Liquid:
            return ImVec4(0.50f, 0.62f, 0.70f, 1.0f);
        case graph::NodeKind::Output:
        default:
            return ImVec4(0.59f, 0.64f, 0.68f, 1.0f);
    }
}

ImVec4 PinTypeColor(graph::ValueType valueType) {
    switch (valueType) {
        case graph::ValueType::Material:
        default:
            return ImVec4(0.70f, 0.78f, 0.72f, 1.0f);
    }
}

// 丸ピン。ed::PinRect / PinPivotRect で当たり判定と接続点を指定する。
void DrawRoundPin(const graph::Pin& pin) {
    const ImVec2 size(14.0f, 20.0f);
    ImGui::Dummy(size);
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    const ImVec2 center((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
    ed::PinRect(min, max);
    ed::PinPivotRect(ImVec2(center.x - 6.0f, center.y - 6.0f),
                     ImVec2(center.x + 6.0f, center.y + 6.0f));
    ImGui::GetWindowDrawList()->AddCircle(center, 4.3f, ColorToU32(PinTypeColor(pin.valueType)),
                                          16, 1.6f);
}

// ドットグリッドの背景。既定のグリッド線は消して自前で描く。
void DrawGraphDots(const ImVec2& screenMin, const ImVec2& screenMax) {
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 canvasMin = ed::ScreenToCanvas(screenMin);
    const ImVec2 canvasMax = ed::ScreenToCanvas(screenMax);
    constexpr float kBaseSpacing = 24.0f;
    const ImU32 backgroundColor = ColorToU32(ImVec4(0.112f, 0.112f, 0.112f, 1.0f));
    const ImU32 dotColor = ColorToU32(ImVec4(0.26f, 0.26f, 0.26f, 0.46f));

    ed::Suspend();
    drawList->PushClipRect(screenMin, screenMax, true);
    drawList->AddRectFilled(screenMin, screenMax, backgroundColor);
    const ImVec2 screen0 = ed::CanvasToScreen(ImVec2(0.0f, 0.0f));
    const ImVec2 screenStep = ed::CanvasToScreen(ImVec2(kBaseSpacing, 0.0f));
    const float baseScreenSpacing = std::max(1.0f, std::abs(screenStep.x - screen0.x));
    // 引きで見たときにドットが密集しないよう、画面上の間隔が保たれる倍率へ広げる。
    const float spacing = kBaseSpacing * std::max(1.0f, std::ceil(12.0f / baseScreenSpacing));
    const float startX = std::floor(std::min(canvasMin.x, canvasMax.x) / spacing) * spacing;
    const float endX = std::ceil(std::max(canvasMin.x, canvasMax.x) / spacing) * spacing;
    const float startY = std::floor(std::min(canvasMin.y, canvasMax.y) / spacing) * spacing;
    const float endY = std::ceil(std::max(canvasMin.y, canvasMax.y) / spacing) * spacing;
    for (float y = startY; y <= endY; y += spacing) {
        for (float x = startX; x <= endX; x += spacing) {
            const ImVec2 screen = ed::CanvasToScreen(ImVec2(x, y));
            if (screen.x < screenMin.x || screen.x > screenMax.x || screen.y < screenMin.y ||
                screen.y > screenMax.y) {
                continue;
            }
            drawList->AddRectFilled(ImVec2(screen.x - 1.0f, screen.y - 1.0f),
                                    ImVec2(screen.x + 1.0f, screen.y + 1.0f), dotColor);
        }
    }
    drawList->PopClipRect();
    ed::Resume();
}

// ノードの表示名。レイヤー設定を持つ種類はレイヤー名を出す。
const char* NodeDisplayName(const graph::Node& node) {
    if (const auto* settings = std::get_if<graph::LayerNodeSettings>(&node.settings)) {
        if (!settings->layer.name.empty()) {
            return settings->layer.name.c_str();
        }
    }
    const graph::NodeDefinition* definition = graph::FindNodeDefinition(node.kind);
    return (definition != nullptr) ? definition->title : "?";
}

int ToGraphId(uintptr_t id) {
    return static_cast<int>(id);
}

// エディタへ渡してよい座標か。エディタは**知らないノードの位置を FLT_MAX で返す**ので、
// それを信じて書き戻す・流し込むとノードが無限遠へ飛び、キャンバスの座標計算が
// 壊れて操作できなくなる。読み込んだファイルの値の検証にも使う。
bool IsValidNodePosition(float x, float y) {
    constexpr float kMaxCoordinate = 1.0e6f;
    return std::isfinite(x) && std::isfinite(y) && std::abs(x) <= kMaxCoordinate &&
           std::abs(y) <= kMaxCoordinate;
}

}  // namespace

void Application::DestroyGraphEditor() {
    if (m_nodeEditor != nullptr) {
        ed::DestroyEditor(m_nodeEditor);
        m_nodeEditor = nullptr;
    }
}

void Application::RequestGraphNodePlacement(bool navigate) {
    m_graphNodesToPlace.clear();
    for (const graph::Node& node : m_graph.Nodes()) {
        m_graphNodesToPlace.push_back(node.id);
    }
    // 全体の流し込みの後だけ画面へ収め直す。1 個の追加やアンドゥでは
    // 視点を動かさない（そのたびに視点が飛ぶと編集にならない）。
    if (navigate) {
        m_graphNavigateCountdown = 3;
    }
}

void Application::SyncGraphStack() {
    // プレビューの対象。レイヤー系のノードを選んでいる間は**そのノードまで**を
    // 表示し、選択を外す（または出力ノードを選ぶ）と出力ノードのチェーンに戻る。
    graph::GraphId target = 0;
    if (const graph::Node* node = m_graph.FindNode(m_selectedGraphNode);
        node != nullptr && graph::IsLayerNodeKind(node->kind)) {
        target = node->id;
    }
    if (m_compiledGraphRevision == m_graph.Revision() && m_compiledGraphTarget == target) {
        return;
    }
    m_compiledGraphRevision = m_graph.Revision();
    m_compiledGraphTarget = target;
    m_graphStack.Layers() =
        (target != 0) ? m_graph.CompileLayersTo(target) : m_graph.CompileLayers();
    m_graphStack.MarkDirty();
}

void Application::DrawGraphNode(const graph::Node& node) {
    constexpr float kNodeWidth = 200.0f;
    const ImVec4 accent = NodeAccentColor(node.kind);
    const ImVec4 nodeBorderColor(0.22f, 0.22f, 0.22f, 1.0f);
    const ImVec4 activeNodeBorderColor(0.59f, 0.64f, 0.68f, 1.0f);
    ed::PushStyleVar(ed::StyleVar_NodePadding, ImVec4(12.0f, 10.0f, 12.0f, 10.0f));
    ed::PushStyleVar(ed::StyleVar_NodeRounding, 6.0f);
    ed::PushStyleVar(ed::StyleVar_NodeBorderWidth, 1.0f);
    ed::PushStyleVar(ed::StyleVar_SelectedNodeBorderWidth, 1.8f);
    ed::PushStyleColor(ed::StyleColor_NodeBg, ImVec4(0.150f, 0.150f, 0.150f, 0.98f));
    ed::PushStyleColor(ed::StyleColor_NodeBorder, nodeBorderColor);
    ed::PushStyleColor(ed::StyleColor_HovNodeBorder, activeNodeBorderColor);
    ed::PushStyleColor(ed::StyleColor_SelNodeBorder, activeNodeBorderColor);

    ed::BeginNode(ed::NodeId(node.id));

    // ヘッダ: 種類色の印 + 名前。レイヤーが無効なら名前を落とした色で描く。
    const auto* layerSettings = std::get_if<graph::LayerNodeSettings>(&node.settings);
    const bool enabled = (layerSettings == nullptr) || layerSettings->layer.enabled;
    {
        const ImVec2 cursor = ImGui::GetCursorScreenPos();
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(ImVec2(cursor.x, cursor.y + 3.0f),
                                ImVec2(cursor.x + 10.0f, cursor.y + 13.0f),
                                ColorToU32(accent), 2.0f);
        ImGui::Dummy(ImVec2(16.0f, 16.0f));
        ImGui::SameLine();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 2.0f);
        const ImVec4 titleColor =
            enabled ? ImVec4(0.88f, 0.88f, 0.88f, 1.0f) : ImVec4(0.55f, 0.55f, 0.55f, 1.0f);
        ImGui::TextColored(titleColor, "%s", NodeDisplayName(node));
        // 種類はヘッダの下に小さく添える。名前と種類の両方が分かるようにする。
        if (const graph::NodeDefinition* definition = graph::FindNodeDefinition(node.kind);
            definition != nullptr && layerSettings != nullptr) {
            ImGui::TextColored(ImVec4(0.55f, 0.57f, 0.55f, 1.0f), "%s%s", definition->title,
                               enabled ? "" : "（無効）");
        }
    }

    ImGui::Dummy(ImVec2(kNodeWidth, 8.0f));
    const float rowStartX = ImGui::GetCursorPosX();
    const float rowY = ImGui::GetCursorPosY();

    for (size_t inputIndex = 0; inputIndex < node.inputs.size(); ++inputIndex) {
        const graph::Pin& input = node.inputs[inputIndex];
        const float inputY = rowY + static_cast<float>(inputIndex) * 24.0f;
        ImGui::SetCursorPos(ImVec2(rowStartX, inputY));
        ed::BeginPin(ed::PinId(input.id), ed::PinKind::Input);
        DrawRoundPin(input);
        ed::EndPin();
        ImGui::SameLine();
        ImGui::SetCursorPosY(inputY + 2.0f);
        ImGui::TextColored(ImVec4(0.62f, 0.64f, 0.62f, 1.0f), "%s", input.label.c_str());
    }

    for (size_t outputIndex = 0; outputIndex < node.outputs.size(); ++outputIndex) {
        const graph::Pin& output = node.outputs[outputIndex];
        const float outputY = rowY + static_cast<float>(outputIndex) * 24.0f;
        const float labelWidth = ImGui::CalcTextSize(output.label.c_str()).x;
        ImGui::SetCursorPos(ImVec2(rowStartX + kNodeWidth - labelWidth - 22.0f, outputY + 2.0f));
        ImGui::TextColored(ImVec4(0.62f, 0.64f, 0.62f, 1.0f), "%s", output.label.c_str());
        ImGui::SameLine();
        ImGui::SetCursorPosY(outputY);
        ed::BeginPin(ed::PinId(output.id), ed::PinKind::Output);
        DrawRoundPin(output);
        ed::EndPin();
    }
    const size_t pinRowCount = std::max(node.inputs.size(), node.outputs.size());
    ImGui::Dummy(
        ImVec2(kNodeWidth, std::max(4.0f, static_cast<float>(pinRowCount) * 24.0f - 20.0f)));

    ed::EndNode();
    ed::PopStyleColor(4);
    ed::PopStyleVar(4);
}

void Application::DrawGraphEditor() {
    if (m_nodeEditor == nullptr) {
        ed::Config config{};
        // 位置は Node が持ち、プロジェクトに保存する。エディタ側の設定ファイルは使わない。
        config.SettingsFile = nullptr;
        config.NavigateButtonIndex = 2;
        m_nodeEditor = ed::CreateEditor(&config);
        RequestGraphNodePlacement();
    }

    static ImVec2 addNodePosition(0.0f, 0.0f);
    const ImVec2 canvasMin = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 canvasMax(canvasMin.x + avail.x, canvasMin.y + avail.y);
    // ホバー判定は ed::Begin より前に取る。フレーム内では io.MousePos が
    // キャンバス座標に差し替えられていて、スクリーン座標の矩形と比べられない。
    const bool canvasHovered = ImGui::IsMouseHoveringRect(canvasMin, canvasMax);

    ed::SetCurrentEditor(m_nodeEditor);
    ed::PushStyleColor(ed::StyleColor_Bg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ed::PushStyleColor(ed::StyleColor_Grid, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ed::Begin("terrainGraphEditor", avail);
    DrawGraphDots(canvasMin, canvasMax);


    // 積まれた位置要求をエディタへ流し込む。まだ位置を持たないノード
    // （と、壊れた座標を持つノード）には現在のビューの中央を与える。
    if (!m_graphNodesToPlace.empty()) {
        for (const graph::GraphId nodeId : m_graphNodesToPlace) {
            graph::Node* node = m_graph.FindMutableNode(nodeId);
            if (node == nullptr) {
                continue;
            }
            if (!node->positionValid || !IsValidNodePosition(node->posX, node->posY)) {
                const ImVec2 center = ed::ScreenToCanvas(
                    ImVec2((canvasMin.x + canvasMax.x) * 0.5f, (canvasMin.y + canvasMax.y) * 0.5f));
                node->posX = center.x;
                node->posY = center.y;
                node->positionValid = true;
            }
            ed::SetNodePosition(ed::NodeId(node->id), ImVec2(node->posX, node->posY));
            // 追加やアンドゥで選ばれたノードは、エディタ側の選択も合わせる。
            // 合わせないと、次のフレームの選択同期（未選択 → 0）に消されてしまう。
            if (nodeId == m_selectedGraphNode) {
                ed::SelectNode(ed::NodeId(nodeId));
            }
        }
        m_graphNodesToPlace.clear();
    }

    for (const graph::Node& node : m_graph.Nodes()) {
        DrawGraphNode(node);
    }

    // A でグラフ全体を画面に収める（ビューポートの A と同じ作法）。
    // 内容の矩形は live なノードから計算されるため、描画の後に呼ぶ。
    if (canvasHovered && !ImGui::GetIO().WantTextInput &&
        ImGui::IsKeyPressed(ImGuiKey_A, false)) {
        ed::NavigateToContent();
    }

    // 位置を流し込んだ後の整列。**ノードを描いた後**でないと内容の矩形が空で
    // 何も起きない（live なノードから計算されるため）。さらに、エディタは
    // キャンバスのサイズ変化のたびに前の表示領域を復元する（ed::Begin 内）ので、
    // ドックの確定を待ってサイズが安定してから寄せる。
    const bool canvasStable =
        (avail.x == m_graphCanvasSize.x && avail.y == m_graphCanvasSize.y);
    m_graphCanvasSize = avail;
    if (m_graphNavigateCountdown > 0 && m_graphNodesToPlace.empty() && canvasStable) {
        if (--m_graphNavigateCountdown == 0) {
            ed::NavigateToContent(0.0f);
        }
    }

    for (const graph::Link& link : m_graph.Links()) {
        ImVec4 color(0.52f, 0.60f, 0.55f, 1.0f);
        if (const graph::Pin* startPin = m_graph.FindPin(link.startPin)) {
            color = PinTypeColor(startPin->valueType);
        }
        ed::Link(ed::LinkId(link.id), ed::PinId(link.startPin), ed::PinId(link.endPin), color,
                 2.5f);
    }

    // --- リンクの作成 -------------------------------------------------------
    if (ed::BeginCreate(ImVec4(0.52f, 0.70f, 0.59f, 1.0f), 2.5f)) {
        ed::PinId startPinId;
        ed::PinId endPinId;
        if (ed::QueryNewLink(&startPinId, &endPinId)) {
            const int startPin = ToGraphId(startPinId.Get());
            const int endPin = ToGraphId(endPinId.Get());
            if (m_graph.CanCreateLink(startPin, endPin)) {
                if (ed::AcceptNewItem(ImVec4(0.70f, 0.78f, 0.72f, 1.0f), 3.0f)) {
                    if (m_graph.CreateLink(startPin, endPin)) {
                        MarkDocumentChanged();
                    }
                }
            } else {
                ed::RejectNewItem(ImVec4(0.78f, 0.28f, 0.24f, 1.0f), 2.0f);
            }
        }
    }
    ed::EndCreate();

    // --- リンクとノードの削除 -----------------------------------------------
    if (ed::BeginDelete()) {
        ed::LinkId deletedLinkId;
        while (ed::QueryDeletedLink(&deletedLinkId)) {
            if (ed::AcceptDeletedItem()) {
                if (m_graph.DeleteLink(ToGraphId(deletedLinkId.Get()))) {
                    MarkDocumentChanged();
                }
            }
        }
        ed::NodeId deletedNodeId;
        while (ed::QueryDeletedNode(&deletedNodeId)) {
            if (ed::AcceptDeletedItem()) {
                const int nodeId = ToGraphId(deletedNodeId.Get());
                if (m_graph.DeleteNode(nodeId)) {
                    MarkDocumentChanged();
                    if (m_selectedGraphNode == nodeId) {
                        m_selectedGraphNode = 0;
                    }
                }
            }
        }
    }
    ed::EndDelete();

    // --- 背景の右クリックでノードを追加 -------------------------------------
    if (ed::ShowBackgroundContextMenu()) {
        // エディタのフレーム内では io.MousePos が**キャンバス座標に差し替えられている**
        // （imgui_canvas が Begin で変換する）。そのまま使う。ScreenToCanvas を
        // 重ねると二重変換になり、ノードが視界の外へ飛ぶ（実際に踏んだ）。
        addNodePosition = ImGui::GetMousePos();
        // 念のため現在の視界へ収める。視界の外に生まれると見失う。
        // 深いズームでは上限が下限を割り得るので、max で順序を保証する。
        const ImVec2 viewMin = ed::ScreenToCanvas(canvasMin);
        const ImVec2 viewMax = ed::ScreenToCanvas(canvasMax);
        const float loX = viewMin.x + 16.0f;
        const float loY = viewMin.y + 16.0f;
        addNodePosition.x = std::clamp(addNodePosition.x, loX, std::max(loX, viewMax.x - 240.0f));
        addNodePosition.y = std::clamp(addNodePosition.y, loY, std::max(loY, viewMax.y - 120.0f));
        ed::Suspend();
        ImGui::OpenPopup("addGraphNode");
        ed::Resume();
    }
    ed::Suspend();
    if (ImGui::BeginPopup("addGraphNode")) {
        ImGui::TextDisabled("ノードを追加");
        ImGui::Separator();
        const auto addNodeMenuItem = [&](graph::NodeKind kind, const char* label) {
            if (!ImGui::MenuItem(label)) {
                return;
            }
            const graph::GraphId nodeId = m_graph.CreateNode(kind);
            graph::Node* node = m_graph.FindMutableNode(nodeId);
            if (node == nullptr) {
                TG_LOG_WARN("ノードを追加できませんでした（種類の定義が見つかりません）");
                return;
            }
            if (auto* settings = std::get_if<graph::LayerNodeSettings>(&node->settings)) {
                // 追加時の初期値は旧レイヤーパネルと同じ既定値を使う。
                settings->layer = (kind == graph::NodeKind::Heightmap)
                                      ? kDefaultHeightmapLayer
                                      : DefaultLayerFor(graph::LayerKindFor(kind));
                settings->layer.name +=
                    " " + std::to_string(m_graph.Nodes().size());
            }
            node->posX = addNodePosition.x;
            node->posY = addNodePosition.y;
            node->positionValid = true;
            m_graphNodesToPlace.push_back(nodeId);
            m_selectedGraphNode = nodeId;
            MarkDocumentChanged();
            // ステータスバーに残す。追加が効いたかを画面で確かめられるようにする。
            TG_LOG_INFO("ノードを追加しました: %s", NodeDisplayName(*node));
        };
        addNodeMenuItem(graph::NodeKind::Heightmap, "ハイトマップ — 画像を地形として読み込む");
        ImGui::Separator();
        addNodeMenuItem(graph::NodeKind::Surface, "サーフェス — 素材を高さで張り合わせる");
        addNodeMenuItem(graph::NodeKind::Shape, "シェイプ — 高さへ起伏を加算する");
        addNodeMenuItem(graph::NodeKind::Liquid, "水面 — 水位より低い所に水を張る");
        ImGui::Separator();
        addNodeMenuItem(graph::NodeKind::Output, "出力 — ここに繋いだ結果をプレビューする");
        ImGui::EndPopup();
    }
    ed::Resume();

    // --- 選択 ---------------------------------------------------------------
    // 選択はプレビューの対象を兼ねる。外したら 0 に戻す（出力ノードのチェーンへ）。
    // **配置待ちのノードがある間は消さない。** 追加した直後のフレームは
    // エディタ側の選択がまだ無く、ここで 0 に戻すと「追加 → 選択」が消える
    // （エディタへの選択の反映は次のフレームの流し込みで行う）。
    ed::NodeId selectedNodes[1];
    if (ed::GetSelectedNodes(selectedNodes, 1) > 0) {
        m_selectedGraphNode = ToGraphId(selectedNodes[0].Get());
    } else if (m_graphNodesToPlace.empty()) {
        m_selectedGraphNode = 0;
    }

    ed::End();

    // エディタが持つ位置をノードへ書き戻す（保存はここから読む）。
    // **このフレーム中に作られたばかりでエディタが知らないノードは飛ばす。**
    // エディタは知らないノードに FLT_MAX を返すため、書き戻すと次の流し込みで
    // ノードが無限遠へ飛び、キャンバスが操作不能になる（実際に踏んだ）。
    for (graph::Node& node : m_graph.MutableNodes()) {
        const ImVec2 position = ed::GetNodePosition(ed::NodeId(node.id));
        if (!IsValidNodePosition(position.x, position.y)) {
            continue;
        }
        node.posX = position.x;
        node.posY = position.y;
        node.positionValid = true;
    }
    ed::PopStyleColor(2);
    ed::SetCurrentEditor(nullptr);
}

void Application::DrawGraphPanel() {
    // 既定レイアウトを組んだ直後は、右カラムの前面タブをこのパネルにする。
    if (m_focusDefaultTabs > 0) {
        ImGui::SetNextWindowFocus();
    }
    if (!ImGui::Begin("グラフ")) {
        ImGui::End();
        return;
    }

    if (const graph::Node* selected = m_graph.FindNode(m_selectedGraphNode);
        selected != nullptr && graph::IsLayerNodeKind(selected->kind)) {
        ui::HintText("選択したノードまでを表示中（選択を外すと出力まで）");
    } else {
        ui::HintText("出力ノードへ繋いだチェーンがプレビューになる");
    }

    float editorHeight = ui::Scaled(m_graphEditorHeight);
    const float paneWidth = ImGui::GetContentRegionAvail().x;
    const float maxHeight =
        std::max(ui::Scaled(160.0f), ImGui::GetContentRegionAvail().y - ui::Scaled(120.0f));
    ImGui::BeginChild("graphEditorPane", ImVec2(0.0f, editorHeight));
    DrawGraphEditor();
    ImGui::EndChild();

    ui::HorizontalSplitter("graphSplitter", &editorHeight, ui::Scaled(160.0f), maxHeight,
                           paneWidth);
    m_graphEditorHeight = editorHeight / std::max(ui::Scaled(1.0f), 0.01f);

    ImGui::BeginChild("graphPropertyPane", ImVec2(0.0f, 0.0f));
    graph::Node* selected = m_graph.FindMutableNode(m_selectedGraphNode);
    if (selected == nullptr) {
        ui::HintText("ノードを選ぶと設定が出る。背景の右クリックで追加、"
                     "ピンをドラッグして接続");
    } else if (auto* settings = std::get_if<graph::LayerNodeSettings>(&selected->settings)) {
        bool changed = false;
        if (ui::BeginPropertyTable("graphNodeBasicRows")) {
            changed |= ui::PropertyBool("有効", &settings->layer.enabled, true,
                                        "無効にすると合成から外れる");
            ui::EndPropertyTable();
        }
        // 「下地」入力が繋がっていないノードは一番下のレイヤー扱い。
        // ソース（ハイトマップ）はそもそも入力を持たないので常にこちら。
        const bool isBase =
            selected->inputs.empty() ||
            m_graph.FindUpstreamNodeForPin(selected->inputs.front().id) == nullptr;
        changed |= DrawLayerSettings(settings->layer, isBase,
                                     graph::IsSourceNodeKind(selected->kind));
        if (changed) {
            m_graph.MarkDirty();
            MarkDocumentChanged();
        }
    } else {
        ui::HintText("出力ノード。「マテリアル」へ繋いだチェーンがプレビューになる");
    }
    ImGui::EndChild();

    ImGui::End();
}

}  // namespace tg
