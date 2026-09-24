// ノードグラフパネル。imgui-node-editor によるエディタと、
// 選択中ノードのプロパティ（レイヤーパネルと共有）を持つ。
//
// エディタの作法（カード描画・丸ピン・ドット背景・リンクの作成 / 削除）は
// terrain-editor のノードエディタ UI から移植した。ノードそのものは
// このプロジェクト独自（サーフェス / シェイプ / 水面 / 出力）。

#include "app/Application.h"

#include "app/ApplicationUiHelpers.h"
#include "core/FileDialog.h"
#include "io/SceneComponents.h"
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
        case graph::NodeKind::Missing:
            return ImGui::ColorConvertU32ToFloat4(ui::ErrorColor());
        case graph::NodeKind::Surface:
            return ImVec4(0.55f, 0.66f, 0.58f, 1.0f);
        case graph::NodeKind::Heightmap:
            return ImVec4(0.72f, 0.66f, 0.50f, 1.0f);
        case graph::NodeKind::Shape:
            return ImVec4(0.66f, 0.62f, 0.52f, 1.0f);
        case graph::NodeKind::Liquid:
            return ImVec4(0.50f, 0.62f, 0.70f, 1.0f);
        case graph::NodeKind::Blur:
            return ImVec4(0.62f, 0.58f, 0.68f, 1.0f);
        case graph::NodeKind::Sediment:
            return ImVec4(0.70f, 0.62f, 0.52f, 1.0f);
        case graph::NodeKind::Crumbling:
            return ImVec4(0.74f, 0.58f, 0.50f, 1.0f);
        case graph::NodeKind::SnowCover:
        case graph::NodeKind::Snow:
            return ImVec4(0.72f, 0.76f, 0.82f, 1.0f);
        case graph::NodeKind::MeanderingRivers:
        case graph::NodeKind::Lake:
        case graph::NodeKind::River:
            return ImVec4(0.48f, 0.64f, 0.72f, 1.0f);
        case graph::NodeKind::FluvialErosion:
        case graph::NodeKind::FlattenBorders:
        case graph::NodeKind::MultiScaleErosion:
        case graph::NodeKind::Droplet:
            return ImVec4(0.56f, 0.66f, 0.62f, 1.0f);
        case graph::NodeKind::Scatter:
            return ImVec4(0.60f, 0.70f, 0.52f, 1.0f);
        case graph::NodeKind::MaskImage:
            return ImVec4(0.72f, 0.72f, 0.72f, 1.0f);
        case graph::NodeKind::MaskNoise:
            return ImVec4(0.68f, 0.72f, 0.62f, 1.0f);
        case graph::NodeKind::MaskFlowline:
        case graph::NodeKind::MaskFluvial:
        case graph::NodeKind::WindField:
            return ImVec4(0.55f, 0.68f, 0.74f, 1.0f);
        case graph::NodeKind::SnowPlume:
            return ImVec4(0.72f, 0.76f, 0.82f, 1.0f);
        case graph::NodeKind::MaskHeight:
            return ImVec4(0.74f, 0.70f, 0.60f, 1.0f);
        case graph::NodeKind::MaskSlope:
            return ImVec4(0.60f, 0.70f, 0.66f, 1.0f);
        case graph::NodeKind::MaskCurvature:
            return ImVec4(0.66f, 0.68f, 0.74f, 1.0f);
        case graph::NodeKind::MaskLevels:
            return ImVec4(0.78f, 0.76f, 0.70f, 1.0f);
        case graph::NodeKind::MaskBlur:
            return ImVec4(0.76f, 0.72f, 0.64f, 1.0f);
        case graph::NodeKind::MaskBlend:
            return ImVec4(0.74f, 0.70f, 0.78f, 1.0f);
        case graph::NodeKind::Path:
            return ImVec4(0.52f, 0.74f, 0.84f, 1.0f);
        case graph::NodeKind::MaskPath:
        case graph::NodeKind::MaskArea:
            return ImVec4(0.58f, 0.74f, 0.82f, 1.0f);
        case graph::NodeKind::Output:
        default:
            return ImVec4(0.59f, 0.64f, 0.68f, 1.0f);
    }
}

// ピンとリンクの色。**線が何を運んでいるかを色で見分ける。**
// 値は terrain-editor に合わせてある（あちらの HeightField がこちらの Material）。
// 緑とオレンジは明度が近く、色相だけが離れているので、
// 暗い盤面でどちらも同じ強さで読める。
ImVec4 PinTypeColor(graph::ValueType valueType) {
    switch (valueType) {
        // マスクはオレンジ。0〜1 の 1 チャンネル。
        case graph::ValueType::Points:
        case graph::ValueType::Instances:
        case graph::ValueType::CloudShape:
        case graph::ValueType::Volume:
            return ImGui::GetStyleColorVec4(ImGuiCol_Text);
        case graph::ValueType::Mask:
            return ImVec4(0.82f, 0.64f, 0.36f, 1.0f);
        // 風の場は薄い紫。3D の速度場が流れる。
        case graph::ValueType::Wind:
            return ImVec4(0.78f, 0.70f, 0.92f, 1.0f);
        // パスは水色。線（点とエッジ）が流れる。緑 / オレンジと色相が離れていて、
        // 明度は同じくらいなので暗い盤面で同じ強さで読める。
        case graph::ValueType::Path:
            return ImVec4(0.55f, 0.80f, 0.95f, 1.0f);
        // マテリアルは緑。4 チャンネル一式（ハイトを含む）。
        case graph::ValueType::Material:
        default:
            return ImVec4(0.70f, 0.93f, 0.78f, 1.0f);
    }
}

// ピンの矩形。当たり判定をラベルまで広げるので、丸の位置は別に持つ。
struct PinGeometry {
    ImVec2 min;     // 丸の矩形
    ImVec2 max;
    ImVec2 center;  // 接続点（リンクの端）
};

// 丸ピンを描いて矩形を返す。**当たり判定（ed::PinRect）は呼び出し側で決める。**
// ラベルまで含めて掴めるようにするため（出力ピンはクリックでプレビューも切り替える）。
// filled が真なら丸を塗る。**ビューポートに出ている出力**の印に使う。
PinGeometry DrawRoundPin(const graph::Pin& pin, bool filled = false) {
    const ImVec2 size(14.0f, 20.0f);
    ImGui::Dummy(size);
    PinGeometry geometry;
    geometry.min = ImGui::GetItemRectMin();
    geometry.max = ImGui::GetItemRectMax();
    geometry.center = ImVec2((geometry.min.x + geometry.max.x) * 0.5f,
                             (geometry.min.y + geometry.max.y) * 0.5f);
    ed::PinPivotRect(ImVec2(geometry.center.x - 6.0f, geometry.center.y - 6.0f),
                     ImVec2(geometry.center.x + 6.0f, geometry.center.y + 6.0f));
    const ImU32 pinColor = ColorToU32(PinTypeColor(pin.valueType));
    if (filled) {
        ImGui::GetWindowDrawList()->AddCircleFilled(geometry.center, 4.3f, pinColor, 16);
    } else {
        ImGui::GetWindowDrawList()->AddCircle(geometry.center, 4.3f, pinColor, 16, 1.6f);
    }
    return geometry;
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
    m_presetEditorId = 0;
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

// ビューポートに出すノードを決める。**選択とは別**に持つので、
// 結果を見ながら別のノードのプロパティをいじれる。
void Application::SetPreviewGraphNode(graph::GraphId nodeId, graph::GraphId outputPin) {
    const graph::Node* node = m_graph.FindNode(nodeId);
    // 出力ノードと、プレビューできない種類は「出力ノードのチェーン」に落とす。
    const bool previewable = (node != nullptr && graph::IsPreviewableNodeKind(node->kind));
    m_previewGraphNode = previewable ? node->id : 0;
    // 見る出力。そのノードの出力ピンでなければ 0（＝最初の出力）に落とす。
    m_previewGraphPin = 0;
    if (previewable) {
        for (const graph::Pin& pin : node->outputs) {
            if (pin.id == outputPin) {
                m_previewGraphPin = pin.id;
                break;
            }
        }
    }
}

void Application::SyncGraphStack() {
    // プレビューの対象。**出力ピンのクリックで決める**（選択とは別）。
    // 0 のときは出力ノードのチェーン。
    graph::GraphId target = 0;
    if (const graph::Node* node = m_graph.FindNode(m_previewGraphNode);
        node != nullptr && graph::IsPreviewableNodeKind(node->kind)) {
        target = node->id;
    } else {
        m_previewGraphNode = 0;
        m_previewGraphPin = 0;
    }
    // 地形の実寸はチェーンの根にある Heightmap ノードが持つ。
    // **読み込むときに一度決めたら、以後はプレビュー側で触らない。**
    if (const graph::TerrainScale* scale = m_graph.FindChainScale(target)) {
        m_renderer.PlaneSize() = scale->sizeMeters;
        m_renderer.DisplacementScale() = scale->heightMeters;
    }
    // 合成の法線は実寸の勾配から作るので、評価器にも同じ実寸を渡す。
    // ノードが実寸を持たないときはプレビュー設定がジオメトリを決めるので、
    // そちらに合わせる（押し出した形と陰影の起伏を一致させる）。
    // レイヤー列が変わらなくても実寸だけ動くことがあるため、早期 return より前に置く。
    m_graphStack.SetTerrainScale(m_renderer.PlaneSize(), m_renderer.DisplacementScale());

    // 描画側は「いまマスクを見ているか」を知らないと斜線を引けない。毎フレーム写す。
    m_renderer.MaskPreviewActive() = false;
    if (const graph::Node* node = m_graph.FindNode(target); node != nullptr) {
        for (const graph::Pin& pin : node->outputs) {
            const bool isPreviewed =
                (pin.id == m_previewGraphPin) ||
                (m_previewGraphPin == 0 && pin.id == node->outputs.front().id);
            if (isPreviewed && pin.valueType == graph::ValueType::Mask) {
                m_renderer.MaskPreviewActive() = true;
            }
        }
    }

    if (m_compiledGraphRevision == m_graph.TerrainRevision() && m_compiledGraphTarget == target &&
        m_compiledGraphTargetPin == m_previewGraphPin) {
        return;
    }
    m_compiledGraphRevision = m_graph.TerrainRevision();
    m_compiledGraphTarget = target;
    m_compiledGraphTargetPin = m_previewGraphPin;
    graph::CompiledGraph compiled = (target != 0)
                                        ? m_graph.CompileLayersTo(target, m_previewGraphPin)
                                        : m_graph.CompileLayers();
    m_graphStack.Layers() = std::move(compiled.layers);
    m_graphStack.MaskOps() = std::move(compiled.maskOps);
    m_graphStack.MarkDirty();

    // op の出どころを版ごとに控える。評価が追いつくまでの数版ぶんあれば足りる。
    constexpr size_t kKeepRevisions = 4;
    GraphMaskOpSources sources;
    sources.revision = m_graphStack.Revision();
    sources.ops = std::move(compiled.maskOpSources);
    sources.layers = std::move(compiled.layerSources);
    m_graphMaskOpSources.push_back(std::move(sources));
    while (m_graphMaskOpSources.size() > kKeepRevisions) {
        m_graphMaskOpSources.erase(m_graphMaskOpSources.begin());
    }
}

D3D12_GPU_DESCRIPTOR_HANDLE Application::GraphLayerThumbnail(graph::GraphId nodeId) const {
    const compositor::MaterialEvaluator& evaluator = m_renderer.Evaluator();
    const uint64_t revision = evaluator.EvaluatedRevision();
    for (const GraphMaskOpSources& sources : m_graphMaskOpSources) {
        if (sources.revision != revision) {
            continue;
        }
        // 同じノードが Mask だけの差し込み（maskOnly）で先に並ぶことがあるので、
        // 後ろから探して Result のほう（本流）を取る。
        for (size_t i = sources.layers.size(); i-- > 0;) {
            if (sources.layers[i] == nodeId) {
                return evaluator.LayerThumbnailHandle(i);
            }
        }
        break;
    }
    return D3D12_GPU_DESCRIPTOR_HANDLE{0};
}

D3D12_GPU_DESCRIPTOR_HANDLE Application::GraphMaskThumbnail(graph::GraphId nodeId,
                                                            size_t outputIndex) const {
    const compositor::MaterialEvaluator& evaluator = m_renderer.Evaluator();
    const uint64_t revision = evaluator.EvaluatedRevision();
    for (const GraphMaskOpSources& sources : m_graphMaskOpSources) {
        if (sources.revision != revision) {
            continue;
        }
        for (size_t i = 0; i < sources.ops.size(); ++i) {
            if (sources.ops[i].nodeId == nodeId && sources.ops[i].outputIndex == outputIndex) {
                return evaluator.MaskOpThumbnailHandle(i);
            }
        }
        break;
    }
    for (const auto& slot : m_cloudMasks) {
        if (!slot.pin || !slot.Ready()) continue;
        for (size_t i=0; i<slot.sources.size(); ++i) {
            if (slot.sources[i].nodeId == nodeId && slot.sources[i].outputIndex == outputIndex)
                return slot.evaluator.MaskOpThumbnailHandle(i);
        }
    }
    return D3D12_GPU_DESCRIPTOR_HANDLE{0};
}

// 選択中のノードを控える。**出力ノードは対象外**（1 つだけ繋ぐ前提のノードで、
// 増やしても迷うだけなので）。
void Application::CopySelectedGraphNodes() {
    std::vector<const graph::Node*> nodes;
    for (const graph::GraphId id : m_selectedGraphNodes) {
        const graph::Node* node = m_graph.FindNode(id);
        if (node != nullptr && node->kind != graph::NodeKind::Output && node->kind != graph::NodeKind::CloudOutput) {
            nodes.push_back(node);
        }
    }
    if (nodes.empty()) {
        return;
    }

    m_graphClipboard.clear();
    m_graphPasteCount = 0;
    for (const graph::Node* node : nodes) {
        GraphClipboardNode entry;
        entry.kind = node->kind;
        entry.settings = node->settings;
        entry.posX = node->posX;
        entry.posY = node->posY;
        const ImVec2 size = ed::GetNodeSize(ed::NodeId(node->id));
        entry.sizeX = size.x;
        entry.sizeY = size.y;
        for (const graph::Pin& pin : node->inputs) {
            GraphClipboardNode::Source source;
            // この入力へ繋がっているリンクの「出力ピン」を覚える。
            for (const graph::Link& link : m_graph.Links()) {
                if (link.endPin != pin.id) {
                    continue;
                }
                const graph::Pin* startPin = m_graph.FindPin(link.startPin);
                if (startPin == nullptr) {
                    break;
                }
                // コピーした集合の中を指しているなら、貼った側どうしで繋ぎ直す。
                for (size_t i = 0; i < nodes.size(); ++i) {
                    if (nodes[i]->id == startPin->nodeId) {
                        source.copiedIndex = static_cast<int>(i);
                        break;
                    }
                }
                // 集合の外なら、**元の親へ繋いだまま**にする。
                if (source.copiedIndex < 0) {
                    source.externalPin = link.startPin;
                }
                break;
            }
            entry.inputs.push_back(source);
        }
        m_graphClipboard.push_back(std::move(entry));
    }
    TG_LOG_INFO("ノードをコピーしました: %zu 個", m_graphClipboard.size());
}

void Application::PasteGraphNodes(const ImVec2& viewCenter) {
    if (m_graphClipboard.empty()) {
        return;
    }
    // 貼るたびに少しずらす。同じ場所に重ねると、貼れたのかどうか分からない。
    // **4 回で一巡させる。** 増やし続けると、貼るほど画面の中央から遠ざかる。
    ++m_graphPasteCount;
    const float offset = 28.0f * static_cast<float>(m_graphPasteCount % 4);

    // **貼る先は今見えている所**。コピー元が画面の外にあっても、貼ったノードが
    // どこかへ消えないように、集合の中心をキャンバスの中央へ持ってくる。
    // 集合の中の相対の配置はそのまま。
    float minX = m_graphClipboard.front().posX;
    float minY = m_graphClipboard.front().posY;
    float maxX = minX;
    float maxY = minY;
    for (const GraphClipboardNode& entry : m_graphClipboard) {
        minX = std::min(minX, entry.posX);
        minY = std::min(minY, entry.posY);
        maxX = std::max(maxX, entry.posX + entry.sizeX);
        maxY = std::max(maxY, entry.posY + entry.sizeY);
    }
    const float deltaX = viewCenter.x - (minX + maxX) * 0.5f + offset;
    const float deltaY = viewCenter.y - (minY + maxY) * 0.5f + offset;

    std::vector<graph::GraphId> created(m_graphClipboard.size(), 0);
    for (size_t i = 0; i < m_graphClipboard.size(); ++i) {
        const GraphClipboardNode& entry = m_graphClipboard[i];
        const graph::GraphId nodeId = m_graph.CreateNode(entry.kind);
        graph::Node* node = m_graph.FindMutableNode(nodeId);
        if (node == nullptr) {
            continue;
        }
        node->settings = entry.settings;
        node->component = std::max(0, m_editComponent);
        node->posX = entry.posX + deltaX;
        node->posY = entry.posY + deltaY;
        node->positionValid = true;
        created[i] = nodeId;
        m_graphNodesToPlace.push_back(nodeId);
    }

    // 接続を張り直す。集合の中どうしは貼った側で、外は**元の親のまま**繋ぐ。
    // 出力側（自分を使っていた下流）は繋がない。入力ピンは 1 本しか持てないので、
    // 繋ぐと元のノードから奪ってしまう。
    for (size_t i = 0; i < m_graphClipboard.size(); ++i) {
        const graph::Node* node = m_graph.FindNode(created[i]);
        if (node == nullptr) {
            continue;
        }
        const GraphClipboardNode& entry = m_graphClipboard[i];
        for (size_t pinIndex = 0; pinIndex < entry.inputs.size(); ++pinIndex) {
            if (node->kind!=graph::NodeKind::CloudMerge && node->kind!=graph::NodeKind::ModelMerge && pinIndex >= node->inputs.size()) {
                break;
            }
            const GraphClipboardNode::Source& source = entry.inputs[pinIndex];
            const graph::GraphId endPin = (node->kind==graph::NodeKind::CloudMerge || node->kind==graph::NodeKind::ModelMerge) ? node->inputs.back().id : node->inputs[pinIndex].id;
            if (source.copiedIndex >= 0 &&
                static_cast<size_t>(source.copiedIndex) < created.size()) {
                const graph::Node* upstream = m_graph.FindNode(created[source.copiedIndex]);
                if (upstream != nullptr && !upstream->outputs.empty()) {
                    m_graph.CreateLink(upstream->outputs.front().id, endPin);
                }
            } else if (source.externalPin != 0) {
                // 元のノードが消えていれば CanCreateLink が弾く（何も起きない）。
                m_graph.CreateLink(source.externalPin, endPin);
            }
        }
    }

    for (const graph::GraphId id : created) {
        if (id != 0) {
            m_selectedGraphNode = id;
            break;
        }
    }
    MarkDocumentChanged();
    TG_LOG_INFO("ノードを貼り付けました: %zu 個", m_graphClipboard.size());
}

void Application::DrawGraphNode(const graph::Node& node) {
    // ノードの幅。**ピンのラベルが重ならない幅まで広げる。**
    // 入力は左、出力は右へ寄せるので、同じ行に並ぶ 2 つのラベルの合計が要る幅になる。
    // 200px 固定にしていたときは、Mask Blend の Foreground / Background のような
    // 長い名前が出力の Mask と重なっていた。
    constexpr float kNodeMinWidth = 200.0f;
    // 丸ピン 1 つぶん（丸の幅 + ImGui の項目間隔）。
    const float pinWidth = 14.0f + ImGui::GetStyle().ItemSpacing.x;
    float rowWidth = 0.0f;
    for (size_t row = 0; row < std::max(node.inputs.size(), node.outputs.size()); ++row) {
        float width = 0.0f;
        if (row < node.inputs.size()) {
            width += pinWidth + ImGui::CalcTextSize(node.inputs[row].label.c_str()).x;
        }
        if (row < node.outputs.size()) {
            width += ImGui::CalcTextSize(node.outputs[row].label.c_str()).x + pinWidth;
        }
        rowWidth = std::max(rowWidth, width);
    }
    // 入力と出力のラベルの間に隙間を空ける。詰まっていると 1 語に見える。
    const float kNodeWidth = std::max(kNodeMinWidth, rowWidth + 24.0f);
    const ImVec4 accent = NodeAccentColor(node.kind);
    // **プレビュー中のノードは枠を明るくする。** 選択（プロパティ）と
    // プレビューは別なので、どれが画面に出ているのかが分かるようにする。
    const bool isPreview = (node.id == m_previewGraphNode) ||
                           (m_previewGraphNode == 0 && node.kind == graph::NodeKind::Output);
    const auto* missingSettings = std::get_if<graph::MissingNodeSettings>(&node.settings);
    // 扱えないノードはエラー色の枠で目立たせる。
    const ImVec4 nodeBorderColor = missingSettings != nullptr ? ImGui::ColorConvertU32ToFloat4(ui::ErrorColor())
                                 : isPreview ? ImVec4(0.72f, 0.76f, 0.62f, 1.0f)
                                             : ImVec4(0.22f, 0.22f, 0.22f, 1.0f);
    const ImVec4 activeNodeBorderColor(0.59f, 0.64f, 0.68f, 1.0f);
    ed::PushStyleVar(ed::StyleVar_NodePadding, ImVec4(12.0f, 10.0f, 12.0f, 10.0f));
    ed::PushStyleVar(ed::StyleVar_NodeRounding, 6.0f);
    ed::PushStyleVar(ed::StyleVar_NodeBorderWidth, isPreview ? 2.0f : 1.0f);
    ed::PushStyleVar(ed::StyleVar_SelectedNodeBorderWidth, 1.8f);
    ed::PushStyleColor(ed::StyleColor_NodeBg, ImVec4(0.150f, 0.150f, 0.150f, 0.98f));
    ed::PushStyleColor(ed::StyleColor_NodeBorder, nodeBorderColor);
    ed::PushStyleColor(ed::StyleColor_HovNodeBorder, activeNodeBorderColor);
    ed::PushStyleColor(ed::StyleColor_SelNodeBorder, activeNodeBorderColor);

    ed::BeginNode(ed::NodeId(node.id));

    // ヘッダ: 種類色の印 + 名前。レイヤーが無効なら名前を落とした色で描く。
    const auto* layerSettings = std::get_if<graph::LayerNodeSettings>(&node.settings);
    const auto* cloudSettings = std::get_if<graph::CloudNodeSettings>(&node.settings);
    const bool enabled = ((layerSettings == nullptr) || layerSettings->layer.enabled) &&
                         ((cloudSettings == nullptr) || cloudSettings->enabled);
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
            missingSettings != nullptr ? ImGui::ColorConvertU32ToFloat4(ui::ErrorColor())
            : enabled ? ImVec4(0.88f, 0.88f, 0.88f, 1.0f) : ImVec4(0.55f, 0.55f, 0.55f, 1.0f);
        ImGui::TextColored(titleColor, "%s", missingSettings != nullptr ? missingSettings->kindName.c_str() : NodeDisplayName(node));
        if (missingSettings != nullptr) {
            ImGui::TextColored(ImVec4(0.55f, 0.57f, 0.55f, 1.0f), "扱えないノード（この版では未対応）");
        }
        if (isPreview) {
            // ビューポートに出ている印。名前の右に小さく添える。
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.72f, 0.76f, 0.62f, 1.0f), "●");
        }
        // 種類はヘッダの下に小さく添える。名前と種類の両方が分かるようにする。
        if (const graph::NodeDefinition* definition = graph::FindNodeDefinition(node.kind);
            definition != nullptr && layerSettings != nullptr) {
            ImGui::TextColored(ImVec4(0.55f, 0.57f, 0.55f, 1.0f), "%s%s", definition->title,
                               enabled ? "" : "（無効）");
        }
    }

    // サムネイル。**繋ぎ替えずに中身が分かる**ようにするためのもの。
    //   - レイヤーのノード（Heightmap / Surface / Shape / 加工…）: そのレイヤーまで
    //     合成した結果（アルベドに Height の勾配で陰影を付けたもの）。
    //     Mask 出力を持つ加工ノードは、隣に最初の Mask（白黒）も出す。
    //   - マスクのノード: 焼いたマスク（白黒）。
    //   - プレビューしていない枝は評価されないので、枠だけの空き（未評価）になる。
    {
        const float thumbnailSize = ui::Scaled(ui::kNodeThumbnail);
        if (graph::IsMaskNodeKind(node.kind)) {
            ImGui::Dummy(ImVec2(kNodeWidth, 2.0f));
            const D3D12_GPU_DESCRIPTOR_HANDLE handle = GraphMaskThumbnail(node.id, 0);
            ui::ThumbnailImage(static_cast<ImTextureID>(handle.ptr), thumbnailSize);
        } else if (layerSettings != nullptr) {
            ImGui::Dummy(ImVec2(kNodeWidth, 2.0f));
            const D3D12_GPU_DESCRIPTOR_HANDLE result = GraphLayerThumbnail(node.id);
            ui::ThumbnailImage(static_cast<ImTextureID>(result.ptr), thumbnailSize);
            // マテリアル一覧からサムネイルへ落とすと、そのノードに割り当たる
            // （Surface だけ）。ID の無いアイテムでも BeginDragDropTarget は矩形から
            // ID を作るので受けられる。
            if (node.kind == graph::NodeKind::Surface && ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload =
                        ImGui::AcceptDragDropPayload(kMaterialDragDropType);
                    payload != nullptr) {
                    const auto dropped =
                        *static_cast<const compositor::MaterialAssetId*>(payload->Data);
                    if (graph::Node* mutableNode = m_graph.FindMutableNode(node.id)) {
                        if (auto* mutableSettings =
                                std::get_if<graph::LayerNodeSettings>(&mutableNode->settings);
                            mutableSettings != nullptr &&
                            mutableSettings->layer.material != dropped) {
                            mutableSettings->layer.material = dropped;
                            m_graph.MarkDirty();
                            MarkDocumentChanged();
                        }
                    }
                }
                ImGui::EndDragDropTarget();
            }
            if (graph::IsLayerMaskSourceKind(node.kind)) {
                ImGui::SameLine();
                const D3D12_GPU_DESCRIPTOR_HANDLE mask = GraphMaskThumbnail(node.id, 0);
                ui::ThumbnailImage(static_cast<ImTextureID>(mask.ptr), thumbnailSize);
            }
        }
    }

    ImGui::Dummy(ImVec2(kNodeWidth, 8.0f));
    const float rowStartX = ImGui::GetCursorPosX();
    const float rowY = ImGui::GetCursorPosY();

    // **ラベルもピンの当たり判定に入れる。** 丸だけだと小さく、
    // 出力ピンのクリック（プレビューの切り替え）も接続も狙いにくい。
    const ImVec4 pinLabelColor(0.62f, 0.64f, 0.62f, 1.0f);

    for (size_t inputIndex = 0; inputIndex < node.inputs.size(); ++inputIndex) {
        const graph::Pin& input = node.inputs[inputIndex];
        const float inputY = rowY + static_cast<float>(inputIndex) * 24.0f;
        ImGui::SetCursorPos(ImVec2(rowStartX, inputY));
        ed::BeginPin(ed::PinId(input.id), ed::PinKind::Input);
        const PinGeometry geometry = DrawRoundPin(input);
        ImGui::SameLine();
        ImGui::SetCursorPosY(inputY + 2.0f);
        ImGui::TextColored(pinLabelColor, "%s", input.label.c_str());
        // 丸からラベルの右端まで。**縦は丸の高さに揃える**（行が重ならないように）。
        ed::PinRect(geometry.min, ImVec2(ImGui::GetItemRectMax().x, geometry.max.y));
        ed::EndPin();
    }

    for (size_t outputIndex = 0; outputIndex < node.outputs.size(); ++outputIndex) {
        const graph::Pin& output = node.outputs[outputIndex];
        // **どの出力を見ているか**を丸の塗りで示す。堆積のように出力が 2 つある
        // ノードでは、Result と Mask のどちらが画面に出ているのかが要る。
        const bool previewOutput = isPreview && ((output.id == m_previewGraphPin) ||
                                                 (m_previewGraphPin == 0 && outputIndex == 0));
        const float outputY = rowY + static_cast<float>(outputIndex) * 24.0f;
        const float labelWidth = ImGui::CalcTextSize(output.label.c_str()).x;
        ImGui::SetCursorPos(ImVec2(rowStartX + kNodeWidth - labelWidth - 22.0f, outputY + 2.0f));
        ed::BeginPin(ed::PinId(output.id), ed::PinKind::Output);
        ImGui::TextColored(pinLabelColor, "%s", output.label.c_str());
        const ImVec2 labelMin = ImGui::GetItemRectMin();
        ImGui::SameLine();
        ImGui::SetCursorPosY(outputY);
        const PinGeometry geometry = DrawRoundPin(output, previewOutput);
        // ラベルの左端から丸まで。
        ed::PinRect(ImVec2(labelMin.x, geometry.min.y), geometry.max);
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
            if (node == nullptr || (m_editComponent >= 0 && node->component != m_editComponent)) {
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
        if (m_editComponent >= 0 && node.component != m_editComponent) continue;
        DrawGraphNode(node);
    }

    // A でグラフ全体を画面に収める（ビューポートの A と同じ作法）。
    // 内容の矩形は live なノードから計算されるため、描画の後に呼ぶ。
    const ImGuiIO& io = ImGui::GetIO();
    if (canvasHovered && !io.WantTextInput && !io.KeyCtrl &&
        ImGui::IsKeyPressed(ImGuiKey_A, false)) {
        ed::NavigateToContent();
    }

    // Ctrl+C / Ctrl+V でノードをコピーする。**キャンバスの上にいるときだけ**
    // 拾う（名前の入力中や他のパネルの操作を横取りしない）。
    if (canvasHovered && !io.WantTextInput && io.KeyCtrl) {
        if (ImGui::IsKeyPressed(ImGuiKey_C, false)) {
            CopySelectedGraphNodes();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_V, false)) {
            PasteGraphNodes(ed::ScreenToCanvas(ImVec2((canvasMin.x + canvasMax.x) * 0.5f,
                                                      (canvasMin.y + canvasMax.y) * 0.5f)));
        }
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
        const auto* pin = m_graph.FindPin(link.startPin);
        const auto* owner = pin ? m_graph.FindNode(pin->nodeId) : nullptr;
        if (m_editComponent >= 0 && (!owner || owner->component != m_editComponent)) continue;
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
                    if (m_previewGraphNode == nodeId) {
                        m_previewGraphNode = 0;
                        m_previewGraphPin = 0;
                    }
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
            const auto* definition = graph::FindNodeDefinition(kind);
            const std::string name = definition ? definition->name : "";
            if (m_editComponent == 0 && name.starts_with("cloud")) return;
            if (m_editComponent == 1 && !name.starts_with("cloud") && !name.starts_with("mask") && name != "path" &&
                name != "terrain" && name != "windField") return;
            const bool available = kind != graph::NodeKind::CloudOutput || !m_graph.CompileCloud().hasOutput;
            if (!ImGui::MenuItem(label, nullptr, false, available)) {
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
            node->component = std::max(0, m_editComponent);
            node->posX = addNodePosition.x;
            node->posY = addNodePosition.y;
            node->positionValid = true;
            m_graphNodesToPlace.push_back(nodeId);
            m_selectedGraphNode = nodeId;
            // 作った直後は、その結果を見たいはず。プレビューも移す。
            SetPreviewGraphNode(nodeId);
            MarkDocumentChanged();
            // ステータスバーに残す。追加が効いたかを画面で確かめられるようにする。
            TG_LOG_INFO("ノードを追加しました: %s", NodeDisplayName(*node));
        };
        addNodeMenuItem(graph::NodeKind::Heightmap, "Heightmap — 画像を地形として読み込む");
        ImGui::Separator();
        addNodeMenuItem(graph::NodeKind::Surface, "Surface — 素材を高さで張り合わせる");
        addNodeMenuItem(graph::NodeKind::Shape, "Shape — 高さへ起伏を加算する");
        addNodeMenuItem(graph::NodeKind::Liquid, "Liquid — 水位より低い所に水を張る");
        ImGui::Separator();
        addNodeMenuItem(graph::NodeKind::Blur, "Heightmap Blur — ハイトをぼかしてならす");
        addNodeMenuItem(graph::NodeKind::Sediment,
                        "Sediment — 土砂を重力で再分配して谷に積もらせる");
        addNodeMenuItem(graph::NodeKind::Crumbling,
                        "Crumbling — 崩れた岩屑を斜面下へ流して積む");
        addNodeMenuItem(graph::NodeKind::SnowCover, "Snow Cover — 積雪と薄雪を生成し、被覆・雪深・流動量を出す");
        addNodeMenuItem(graph::NodeKind::Snow,
                        "Snow — 雪を降らせ、急な雪面から落として積もらせる");
        addNodeMenuItem(graph::NodeKind::MeanderingRivers, "Meandering Rivers — Path を蛇行させ、河床を掘る");
        addNodeMenuItem(graph::NodeKind::Lake, "Lake — 窪みに水を溜め、湖の範囲・水深・水位を出す");
        addNodeMenuItem(graph::NodeKind::River,
                        "River — 川筋から河床を掘り、下流へ下がる水面を張る");
        addNodeMenuItem(graph::NodeKind::FluvialErosion, "Fluvial Erosion — 流れに沿って谷を刻み、細部を戻しながら侵食する");
        addNodeMenuItem(graph::NodeKind::FlattenBorders, "Flatten Borders — 地形の外周を指定した標高へならす");
        addNodeMenuItem(graph::NodeKind::MultiScaleErosion,
                        "Multi-Scale Erosion — 大きな谷から細かな溝まで段階的に侵食する");
        addNodeMenuItem(graph::NodeKind::Droplet,
                        "Droplet Erosion — 水滴を流して谷を刻み、土砂を運んで積む");
        addNodeMenuItem(graph::NodeKind::Scatter,
                        "Scatter — 単純な形をばら撒き、分布のマスクを出す");
        ImGui::Separator();
        addNodeMenuItem(graph::NodeKind::MaskImage,
                        "Mask Image — 画像をマスクにする（白い所だけ乗る）");
        addNodeMenuItem(graph::NodeKind::MaskNoise,
                        "Mask Noise — ノイズをマスクにする（下地に依らない）");
        addNodeMenuItem(graph::NodeKind::MaskFlowline, "Mask Flowline — 地形に沿う流跡をマスクにする");
        addNodeMenuItem(graph::NodeKind::MaskFluvial,
                        "Mask Fluvial — 下地の川筋をマスクにする");
        addNodeMenuItem(graph::NodeKind::MaskHeight,
                        "Mask Height — 下地の標高帯（m）をマスクにする");
        addNodeMenuItem(graph::NodeKind::MaskSlope,
                        "Mask Slope — 下地の傾斜（角度）をマスクにする");
        addNodeMenuItem(graph::NodeKind::WindField,
                        "Wind Field — 地形全体の風の場。地表の風速と粉雪の発生量をマスクにする");
        addNodeMenuItem(graph::NodeKind::SnowPlume,
                        "Snow Plume — 稜線から風下へ雪煙をなびかせる（Spindrift を Source に繋ぐ）");
        addNodeMenuItem(graph::NodeKind::MaskCurvature,
                        "Mask Curvature — 下地の凹凸（尾根 / 谷）をマスクにする");
        addNodeMenuItem(graph::NodeKind::MaskLevels,
                        "Mask Levels — マスクの黒点 / 白点 / ガンマを調整する");
        addNodeMenuItem(graph::NodeKind::MaskBlur,
                        "Mask Blur — マスクをぼかして境界をなだらかにする");
        addNodeMenuItem(graph::NodeKind::MaskBlend,
                        "Mask Blend — マスク 2 枚を合成する");
        ImGui::Separator();
        addNodeMenuItem(graph::NodeKind::Path,
                        "Path — 地形の上に線を引く（道路 / 川 / 氷河のガイド）");
        addNodeMenuItem(graph::NodeKind::MaskPath,
                        "Mask Path — パスの足跡をマスクにする");
        addNodeMenuItem(graph::NodeKind::MaskArea,
                        "Mask Area — パスの閉じた鎖の内側をマスクにする（エリア選択）");
        ImGui::Separator();
        ImGui::Separator();
        addNodeMenuItem(graph::NodeKind::CloudWeatherLayer, "Cloud Weather Layer — 雲量と雲種のマップで広域の雲層を作る");
        addNodeMenuItem(graph::NodeKind::CloudShapeGenerate, "Cloud Shape Generate (Experimental) — 単独の積雲を生成する");
        addNodeMenuItem(graph::NodeKind::CloudMapGenerate, "Cloud Map Generate (Experimental) — ポイントの分布から雲形状を生成する");
        addNodeMenuItem(graph::NodeKind::CloudMerge, "Cloud Merge (Experimental) — 基本形状を統合する");
        addNodeMenuItem(graph::NodeKind::CloudTransform, "Cloud Transform (Experimental) — 雲形状を移動する");
        addNodeMenuItem(graph::NodeKind::CloudAnimation, "Cloud Animation — 指定範囲で雲を繰り返し移動する");
        addNodeMenuItem(graph::NodeKind::CloudNoise, "Cloud Noise (Experimental) — 輪郭と密度を作る");
        addNodeMenuItem(graph::NodeKind::CloudOutput, "Cloud Output — Volume を繋いで雲を表示する");
        addNodeMenuItem(graph::NodeKind::Terrain, "Terrain — 地形グラフの結果を取り出す（マスクの Base に繋ぐ）");
        ImGui::Separator();
        addNodeMenuItem(graph::NodeKind::ModelScatter, "Model Scatter — Points にモデルをランダム配置する");
        addNodeMenuItem(graph::NodeKind::ModelMerge, "Model Merge — 複数のモデル配置をまとめる");
        addNodeMenuItem(graph::NodeKind::ModelOutput, "Model Output — モデル配置をビューポートへ出す");
        addNodeMenuItem(graph::NodeKind::Output, "Output — ここに繋いだ結果をプレビューする");
        ImGui::EndPopup();
    }
    ed::Resume();

    // --- プレビュー対象の切り替え -------------------------------------------
    // **選択とは別。** ノードを選んでプロパティをいじりながら、別のノードの
    // 出力をビューポートに出しておけるようにする（terrain-editor と同じ作法）。
    //
    // 出力ピンは押した瞬間からリンクのドラッグが始まるので、
    // **同じピンの上でほとんど動かずに離したとき**だけクリックとみなす。
    {
        const graph::GraphId hoveredPin = ToGraphId(ed::GetHoveredPin().Get());
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            m_graphPressedPin = hoveredPin;
            m_graphPressedPinPos = ImGui::GetMousePos();
        }
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            const ImVec2 released = ImGui::GetMousePos();
            const float moved = std::abs(released.x - m_graphPressedPinPos.x) +
                                std::abs(released.y - m_graphPressedPinPos.y);
            if (m_graphPressedPin != 0 && hoveredPin == m_graphPressedPin && moved < 6.0f) {
                if (const graph::Pin* pin = m_graph.FindPin(m_graphPressedPin);
                    pin != nullptr && pin->kind == graph::PinKind::Output) {
                    // 押したピンそのものを見る（堆積の Mask をクリックすれば
                    // 積もった厚みが白黒で出る）。
                    SetPreviewGraphNode(pin->nodeId, pin->id);
                }
            }
            m_graphPressedPin = 0;
        }
        // ノードのダブルクリックでも切り替える（ピンが小さいときの逃げ道）。
        if (const ed::NodeId doubleClicked = ed::GetDoubleClickedNode()) {
            SetPreviewGraphNode(ToGraphId(doubleClicked.Get()));
        }
        // 背景のダブルクリックで出力ノードのチェーンへ戻す。
        if (ed::IsBackgroundDoubleClicked()) {
            SetPreviewGraphNode(0);
        }
    }

    // --- 選択 ---------------------------------------------------------------
    // 選択はプロパティに出すノード。外したら 0 に戻す（プレビューには影響しない）。
    // **配置待ちのノードがある間は消さない。** 追加した直後のフレームは
    // エディタ側の選択がまだ無く、ここで 0 に戻すと「追加 → 選択」が消える
    // （エディタへの選択の反映は次のフレームの流し込みで行う）。
    // コピーは複数選択（枠で囲む）にも効かせたいので、全部控えておく。
    ed::NodeId selectedNodes[64];
    const int selectedCount = ed::GetSelectedNodes(selectedNodes, IM_ARRAYSIZE(selectedNodes));
    // 起動引数で指定したノード（--select-node）は、エディタがそのノードを選ぶまで
    // 毎フレーム選び直す。エディタがまだノードを知らないフレームでは選択が付かない。
    if (m_pendingSelectGraphNode != 0) {
        const auto* pending = m_graph.FindNode(m_pendingSelectGraphNode);
        if (pending == nullptr || (m_editComponent >= 0 && pending->component != m_editComponent)) {
            m_pendingSelectGraphNode = 0;
        } else if (selectedCount > 0) {
            m_pendingSelectGraphNode = 0;
        } else {
            ed::SelectNode(ed::NodeId(m_pendingSelectGraphNode));
            m_selectedGraphNode = m_pendingSelectGraphNode;
        }
    }
    if (selectedCount > 0) {
        m_selectedGraphNodes.clear();
        for (int i = 0; i < selectedCount; ++i) {
            m_selectedGraphNodes.push_back(ToGraphId(selectedNodes[i].Get()));
        }
        m_selectedGraphNode = m_selectedGraphNodes.front();
    } else if (m_graphNodesToPlace.empty() && m_pendingSelectGraphNode == 0) {
        m_selectedGraphNodes.clear();
        m_selectedGraphNode = 0;
    }

    ed::End();

    // エディタが持つ位置をノードへ書き戻す（保存はここから読む）。
    // **このフレーム中に作られたばかりでエディタが知らないノードは飛ばす。**
    // エディタは知らないノードに FLT_MAX を返すため、書き戻すと次の流し込みで
    // ノードが無限遠へ飛び、キャンバスが操作不能になる（実際に踏んだ）。
    for (graph::Node& node : m_graph.MutableNodes()) {
        if (m_editComponent >= 0 && node.component != m_editComponent) continue;
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

void Application::OpenComponentEditor(int component) {
    if (m_editComponent == component && m_nodeEditor) return;
    m_editComponent = component;
    m_graphClipboard.clear();
    // 開くコンポーネントのノードを選んでいれば、その選択は保つ（--select-node や
    // 読み込み直後の選択がここで消えないように）。別のコンポーネントの選択は外す。
    const auto* selected = m_graph.FindNode(m_selectedGraphNode);
    if (selected == nullptr || selected->component != component) {
        m_selectedGraphNode = 0; m_selectedGraphNodes.clear();
    }
    m_previewGraphNode = 0; m_previewGraphPin = 0;
    m_presetEditorId = 0;
    if (m_nodeEditor) { ed::DestroyEditor(m_nodeEditor); m_nodeEditor = nullptr; }
    RequestGraphNodePlacement();
}

std::filesystem::path Application::SceneAssetDirectory() {
    if (m_projectPath.extension() == L".tgscene") return m_projectPath.parent_path();
    const auto directory = m_workspace.Root() / L"Scenes";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    return error ? m_workspace.Root() : directory;
}

void Application::DrawSceneHierarchy() {
    if (!ImGui::Begin("シーン階層")) { ImGui::End(); return; }
    const bool components = m_sceneComponents.is_array();
    // 一時プレビュー中は保存先がプレビューのグラフになるので、項目の印と保存は出さない。
    const bool previewing = m_componentPreview >= 0;
    // 旧形式のシーンは全部が 1 つのファイル。シーンの行にまとめて印を出す。
    const unsigned sceneDirty = components ? (m_sceneDirty & kDirtyScene) : (m_sceneDirty & ~kDirtyShared);

    // 行の右端に置く「未保存の印 + 保存ボタン（フロッピー）」の幅。変更のある行にだけ出す。
    const ImGuiStyle& style = ImGui::GetStyle();
    // 印とアイコンは文字の高さの枠に収める。フレームの高さにすると、印の付いた行だけ
    // 縦に太り、目のアイコンや文字の位置が他の行とずれる。
    const float saveSize = ImGui::GetTextLineHeight();
    const float controlsWidth = saveSize + style.ItemSpacing.x + saveSize;
    const auto unsavedControls = [&](bool dirty, int item, const char* tooltip) {
        if (!dirty || previewing) return;
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - controlsWidth);
        ui::UnsavedMark("未保存の変更があります", saveSize);
        ImGui::SameLine();
        ImGui::PushID(item);
        if (ui::SaveIconButton("##save", saveSize, tooltip)) RequestComponentSave(item);
        ImGui::PopID();
    };
    // 見出しは Selectable の幅を控えの分だけ縮め、右端の操作を覆わないようにする。
    const auto rowWidth = [&](bool dirty) {
        return (dirty && !previewing) ? ImVec2(ImGui::GetContentRegionAvail().x - controlsWidth - style.ItemSpacing.x, 0.0f)
                                      : ImVec2(0.0f, 0.0f);
    };
    // 行の左端の目のアイコン。その部品の描画だけを切り替え、グラフやスカイの設定には触れない。
    // 値はプレビュー設定（雲を描画 / 背景を表示）と同じものなので、シーンに保存される。
    // 文字より少し小さくし、行の高さは変えない（縦は文字の中心に揃える）。
    const float eyeSize = ui::Scaled(14.0f);
    const auto eye = [&](const char* id, bool* value, const char* tooltip) {
        const float rowY = ImGui::GetCursorPosY();
        ImGui::SetCursorPosY(rowY + (ImGui::GetTextLineHeight() - eyeSize) * 0.5f);
        if (ui::EyeToggle(id, value, eyeSize)) MarkDocumentChanged(false);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", tooltip);
        ImGui::SameLine();
        ImGui::SetCursorPosY(rowY);
    };

    ImGui::TextUnformatted(m_projectPath.empty() ? "新規シーン" : ToUtf8Display(m_projectPath.stem()).c_str());
    unsavedControls(sceneDirty != 0, 3,
                    components ? "シーン本体だけを保存する。部品のファイルは書き換えない" : "シーンを保存する");
    if (!components) {
        ui::HintText("旧形式のシーンです。保存済みの元データを残して部品へ分離できます。");
        if (ui::Button("部品へ分離…", ui::kWideButtonWidth)) m_pendingComponentMigration = true;
    }
    // 行の右クリックに置く改名。ファイルがある行だけ。F2 でも同じ。
    const auto renameMenuItem = [&](const std::filesystem::path& file) {
        if (ImGui::MenuItem("名前を変更…", "F2", false, !file.empty())) {
            OpenAssetRename(file);
            m_assetRenameInHierarchy = true;
        }
    };

    // 部品 1 つにつき 1 行。「ファイル名（種類）」で並べ、ファイルがまだ無ければ種類だけ出す。
    // 地形・大気散乱スカイ・雲は「シーンが参照するアセット 1 つ」という同じ立場なので同列に置く。
    // 見出しは空の Selectable を敷いた上に文字を描く（ファイル名と種類で色を分けるため）。
    // 長いファイル名は種類と右端の操作を残して中央を省略する。戻り値はダブルクリックされたか。
    // 改名中の行は見出しの代わりにその場の入力欄を出す（アセットブラウザの F2 と同じ経路）。
    // 拡張子は変えられないので、欄には拡張子を除いた名前だけを入れる。
    const auto row = [&](const char* id, const std::filesystem::path& file, const char* kind,
                         bool selected, bool dirty, const char* tooltip) {
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImVec2 size = rowWidth(dirty);
        if (size.x <= 0.0f) size.x = ImGui::GetContentRegionAvail().x;
        if (m_assetRenameInHierarchy && !file.empty() && m_assetRenameTarget == file) {
            const auto edit = ui::InlineNameInput(id, m_assetRenameBuffer, sizeof(m_assetRenameBuffer),
                                                  size.x, &m_assetRenameFocus);
            if (edit != ui::CaptionEdit::Editing) FinishAssetRename(edit == ui::CaptionEdit::Commit);
            return false;
        }
        const bool doubleClicked =
            ImGui::Selectable(id, selected, ImGuiSelectableFlags_AllowDoubleClick, size) &&
            ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", tooltip);
            if (!file.empty() && !ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F2, false)) {
                OpenAssetRename(file);
                m_assetRenameInHierarchy = true;
            }
        }
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 textPos(origin.x + style.FramePadding.x, origin.y);
        const float innerWidth = size.x - style.FramePadding.x * 2.0f;
        if (file.empty()) {
            draw->AddText(textPos, ImGui::GetColorU32(ImGuiCol_Text), kind);
        } else {
            const std::string suffix = std::string("（") + kind + "）";
            const float suffixWidth = ImGui::CalcTextSize(suffix.c_str()).x;
            const std::string name = ui::EllipsizeMiddle(ToUtf8Display(file.filename()).c_str(),
                                                         innerWidth - suffixWidth);
            draw->AddText(textPos, ImGui::GetColorU32(ImGuiCol_Text), name.c_str());
            draw->AddText(ImVec2(textPos.x + ImGui::CalcTextSize(name.c_str()).x, textPos.y),
                          ImGui::GetColorU32(ImGuiCol_TextDisabled), suffix.c_str());
        }
        return doubleClicked;
    };

    ImGui::BeginDisabled(previewing);
    const auto graphEntry = [&](int component) {
        ImGui::PushID(component);
        const bool dirty = components && (m_sceneDirty & (component ? kDirtyCloud : kDirtyTerrain));
        if (component) eye("##eyeCloud", &m_renderer.ShowClouds(), "雲と雲影の表示。雲グラフの設定は保持する");
        else eye("##eyeTerrain", &m_renderer.ShowTerrain(), "地形と配置したモデルの表示。影も一緒に消える");
        std::filesystem::path placed;
        if (components) for (const auto& entry : m_sceneComponents)
            if (io::ProjectWorkspace::String(entry, "role") == (component ? "cloud" : "terrain"))
                placed = m_workspace.Resolve(entry.value("asset", nlohmann::json::object()));
        if (row("##graphRow", placed, component ? "雲グラフ" : "地形グラフ", m_editComponent == component,
                dirty, "ダブルクリックで編集。右クリックで改名・新規作成・入れ替え"))
            OpenComponentEditor(components ? component : -1);
        // 新規作成と入れ替え。差し替える部品に未保存の編集があれば一時プレビューへ回る。
        if (components && ImGui::BeginPopupContextItem("componentMenu")) {
            renameMenuItem(placed);
            ImGui::Separator();
            if (ImGui::MenuItem("新規作成して配置")) {
                const auto path = io::CreateGraphAsset(m_workspace, SceneAssetDirectory(), component == 1);
                if (path.empty()) TG_LOG_ERROR("グラフを作成できませんでした");
                else { m_pendingComponentPlace = path; m_assetRefresh = true; }
            }
            if (ImGui::MenuItem("入れ替え…")) {
                const auto selected = component ? ShowOpenFileDialog(L"雲グラフを選ぶ", {{L"雲グラフ", L"*.tgcloud"}})
                                               : ShowOpenFileDialog(L"地形グラフを選ぶ", {{L"地形グラフ", L"*.tgterrain"}});
                if (!selected.empty()) m_pendingComponentPlace = selected;
            }
            if (ImGui::MenuItem("アセットブラウザで表示", nullptr, false, !placed.empty())) m_pendingAssetReveal = placed;
            ImGui::EndPopup();
        }
        unsavedControls(dirty, component, "このグラフのファイルだけを保存する");
        ImGui::PopID();
    };
    graphEntry(0);
    {
        const bool atmosphereDirty = components && (m_sceneDirty & kDirtyAtmosphere);
        eye("##eyeSky", &m_renderer.ShowSkybox(), "空の背景の表示。環境光と地形の手前の雲は残る");
        const auto path = m_sceneAtmosphere.is_null() ? std::filesystem::path{} : m_workspace.Resolve(m_sceneAtmosphere);
        if (row("##atmosphereRow", path, path.empty() && components ? "大気散乱スカイ（シーン保存時に作成）" : "大気散乱スカイ",
                false, atmosphereDirty, "ダブルクリックでライティングへ。右クリックで改名・新規作成・読み込み")) {
            m_renderer.AtmosphericMode() = true; m_pendingWorkEnvironmentSave = true; m_focusLighting = true;
        }
        if (components && ImGui::BeginPopupContextItem("atmosphereMenu")) {
            renameMenuItem(path);
            ImGui::Separator();
            if (ImGui::MenuItem("新規作成して配置")) {
                auto created = m_workspace.UniquePath(SceneAssetDirectory(), "大気散乱スカイ", ".tgatmosphere");
                auto body = io::AtmosphereAssetBody(nlohmann::json::object(), "大気散乱スカイ");
                if (!created.empty() && m_workspace.SaveAsset(created, "atmosphere-sky", body)) {
                    m_pendingComponentPlace = created; m_assetRefresh = true;
                } else TG_LOG_ERROR("大気散乱スカイを作成できませんでした");
            }
            if (ImGui::MenuItem("読み込む…")) {
                const auto selected = ShowOpenFileDialog(L"大気散乱スカイを読み込む", {{L"大気散乱スカイ", L"*.tgatmosphere"}});
                if (!selected.empty()) m_pendingComponentPlace = selected;
            }
            if (ImGui::MenuItem("アセットブラウザで表示", nullptr, false, !path.empty())) m_pendingAssetReveal = path;
            ImGui::EndPopup();
        }
        unsavedControls(atmosphereDirty, 2, m_sceneAtmosphere.is_null()
                                                ? "スカイのアセットを作り、シーン本体と一緒に保存する"
                                                : "大気散乱スカイのファイルだけを保存する");
    }
    graphEntry(1);
    ImGui::EndDisabled();
    if ((m_sceneDirty & kDirtyShared) && !previewing) {
        ui::HintText("マテリアル・モデルに未保存の変更があります（Ctrl+S でまとめて保存）。");
    }
    ui::HintText("編集は保存するまでファイルへ書き込まれません。Ctrl+S で変更のある項目をまとめて保存します。");
    ImGui::End();
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

    if (m_componentPreview >= 0) {
        ui::HintText("未配置のアセットをシーン内で一時プレビュー中");
        if (ui::Button("保存")) RequestSaveProject(false);
        ImGui::SameLine();
        if (ui::Button("元を保存して配置", ui::kWideButtonWidth)) m_pendingPreviewFinish = 1;
        ImGui::SameLine();
        if (ui::Button("破棄して戻る", ui::kWideButtonWidth)) m_pendingPreviewFinish = 2;
    }
    if (m_sceneComponents.is_array()) {
        ImGui::BeginDisabled(m_componentPreview >= 0);
        if (ui::Button("地形グラフ", ui::kWideButtonWidth)) OpenComponentEditor(0);
        ImGui::SameLine();
        if (ui::Button("雲グラフ", ui::kWideButtonWidth)) OpenComponentEditor(1);
        ImGui::EndDisabled();
        ImGui::TextUnformatted(m_editComponent == 1 ? "編集中：雲" : "編集中：地形");
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

    // **プレビュー対象は選択とは別。** どれが画面に出ているかをここに出し、
    // 出力へ戻す手段も置く（出力ピンのクリックで切り替わる、と気づけるように）。
    {
        const graph::Node* previewNode = m_graph.FindNode(m_previewGraphNode);
        const char* previewName =
            (previewNode != nullptr) ? NodeDisplayName(*previewNode) : "Output";
        // 出力が 2 つ以上あるノードは、どちらを見ているのかも出す。
        const graph::Pin* previewPin = m_graph.FindPin(m_previewGraphPin);
        if (ui::BeginPropertyTable("graphPreviewRow")) {
            if (previewPin != nullptr && previewNode != nullptr &&
                previewNode->outputs.size() > 1) {
                ui::PropertyValue("プレビュー", "%s（%s）", previewName,
                                  previewPin->label.c_str());
            } else {
                ui::PropertyValue("プレビュー", "%s", previewName);
            }
            ui::EndPropertyTable();
        }
        if (previewNode != nullptr) {
            if (ui::Button("出力へ戻す", ui::kWideButtonWidth)) {
                SetPreviewGraphNode(0);
            }
        }
        ui::HintText("出力ピンをクリック（またはノードをダブルクリック）で、"
                     "ビューポートに出す出力を切り替える。"
                     "Mask の出力を選ぶと、そのマスクが白黒で貼られる");
        ImGui::Spacing();
    }

    graph::Node* selected = m_graph.FindMutableNode(m_selectedGraphNode);
    if (selected == nullptr) {
        ui::HintText("ノードを選ぶと設定が出る。背景の右クリックで追加、"
                     "ピンをドラッグして接続、Ctrl+C / Ctrl+V でコピー");
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
        const bool isSource = graph::IsSourceNodeKind(selected->kind);
        // Mask 入力にノードが繋がっていれば、マスクの出どころはそちら。
        bool maskFromNode = false;
        for (const graph::Pin& pin : selected->inputs) {
            if (pin.valueType == graph::ValueType::Mask &&
                m_graph.FindUpstreamNodeForPin(pin.id) != nullptr) {
                maskFromNode = true;
            }
        }
        if (selected->kind == graph::NodeKind::MeanderingRivers) {
            const auto* source = m_graph.FindUpstreamNodeForPin(selected->inputs[1].id);
            const auto* path = source ? std::get_if<graph::PathNodeSettings>(&source->settings) : nullptr;
            const auto* scale = m_graph.FindChainScale(selected->id);
            const auto& p = settings->layer.meanderingRivers;
            if (!path || graph::BuildMeanderPoints(path->path,
                scale ? scale->sizeMeters : graph::TerrainScale{}.sizeMeters,
                p.riverWidth * p.meanderScale * 2.0f).empty()) {
                ui::HintText("有効な川筋がないため地形は変わらず、River は空になる。"
                    "独立した開いた Path を矢印が揃う向きで接続する（合計 2048 標本まで）。");
            }
        }
        // Surface の UV Path に Path が繋がっていれば、帯の座標で貼る設定を出す。
        bool pathUvConnected = false;
        if (selected->kind == graph::NodeKind::Surface) {
            for (const graph::Pin& pin : selected->inputs) {
                if (pin.valueType == graph::ValueType::Path &&
                    m_graph.FindUpstreamNodeForPin(pin.id) != nullptr) {
                    pathUvConnected = true;
                }
            }
        }
        changed |= DrawLayerSettings(settings->layer, isBase, isSource, maskFromNode,
                                     m_graph.MaskSourceResolves(*selected), pathUvConnected);

        // 地形の実寸。**ソースだけが持ち、読み込むときに一度だけ決める。**
        // プレビュー設定ではなくここに置くのは、実寸が見え方の設定ではなく
        // 読み込んだデータそのものの性質だから。
        if (isSource) {
            ui::SectionHeader("スケール");
            if (ui::BeginPropertyTable("graphNodeScaleRows")) {
                const graph::TerrainScale defaults;
                changed |= ui::PropertyFloat(
                    "サイズ", &settings->scale.sizeMeters, 0.5f, 32768.0f, defaults.sizeMeters,
                    "地形の一辺の長さ（m）。カメラと影の範囲もこれに追従する", "%.1f m",
                    ImGuiSliderFlags_Logarithmic);
                changed |= ui::PropertyFloat(
                    "標高差", &settings->scale.heightMeters, 0.0f,
                    std::max(1.0f, settings->scale.sizeMeters * 0.5f), defaults.heightMeters,
                    "ハイト 0〜1 の全幅が何 m になるか（最低地点から最高地点までの差）",
                    "%.1f m", ImGuiSliderFlags_Logarithmic);
                ui::EndPropertyTable();
            }
            ui::HintText("読み込んだ地形の実寸。プレビュー設定の平面のサイズと変位量はこれに従う");
        }
        if (changed) {
            m_graph.MarkDirty();
            MarkDocumentChanged();
        }
    } else if (auto* mask = std::get_if<graph::MaskNodeSettings>(&selected->settings)) {
        bool changed = false;
        const char* header = "マスク画像";
        const char* hint =
            "レイヤーの Mask 入力へ繋ぐと、白い所にだけそのレイヤーが乗る。"
            "効き方（係数 / カーブ / レベル）はレイヤー側で決める";
        switch (selected->kind) {
            case graph::NodeKind::MaskNoise:
                header = "ノイズ";
                hint = "下地に依らないノイズをマスクにする。"
                       "周波数は整数へ丸めて使うので、出力は必ずタイルする";
                break;
            case graph::NodeKind::MaskFlowline:
                header = "流跡";
                hint = "Base の地形に沿って粒子を流し、通った場所をマスクにする。地形の高さは変えない。"
                       "Source は発生範囲と強さ、Outflow は途中で流れを弱める範囲を指定する";
                break;
            case graph::NodeKind::WindField:
                header = "風の場";
                hint = "Base の地形の周囲の風を粗い 3D 格子で近似する。Speed は地表から鉛直半セル上の風速、"
                       "Spindrift は風下の稜線で風速がしきい値を"
                       "超える所。Wind は 3D の速度場で、今は繋ぐ先がない。Spindrift を Snow Plume の Source に繋ぐと雪煙になる。"
                       "矢印の根元は風を取得した位置、向きは空中の風向、長さは風速（入力風速の 2 倍で頭打ち）。"
                       "斜面に沿う流れや粒子の軌跡ではない。最後に評価した風を表示する"
                       "（Speed か Spindrift の接続が必要）";
                break;
            case graph::NodeKind::MaskFluvial:
                header = "川筋";
                hint = "下地の高さから水の集まる所（川筋）を作る。"
                       "Base にどこまでのハイトを使うかを繋ぐ";
                break;
            case graph::NodeKind::MaskHeight:
                header = "標高";
                hint = "下地の標高帯（m）をマスクにする。"
                       "0 m は地形の一番低い所で、標高差 m が一番高い所。"
                       "Base にどこまでのハイトを使うかを繋ぐ";
                break;
            case graph::NodeKind::MaskSlope:
                header = "傾斜";
                hint = "下地の傾斜（角度）をマスクにする。"
                       "Base にどこまでのハイトを使うかを繋ぐ";
                break;
            case graph::NodeKind::MaskCurvature:
                header = "曲率";
                hint = "下地の凹凸をマスクにする。周りの平均と比べて、"
                       "高い所（尾根）か低い所（谷）を拾う。"
                       "Base にどこまでのハイトを使うかを繋ぐ";
                break;
            case graph::NodeKind::MaskLevels:
                header = "レベル";
                hint = "入力のマスクの黒点 / 白点 / ガンマを整える";
                break;
            case graph::NodeKind::MaskBlur:
                header = "ぼかし";
                hint = "入力のマスクをぼかす。境界のギザギザや、"
                       "しきい値で二値になったマスクを馴染ませるのに使う";
                break;
            case graph::NodeKind::MaskBlend:
                header = "合成";
                hint = "マスク 2 枚を合成する。片方だけ繋いだときはそれを通す";
                break;
            case graph::NodeKind::MaskPath:
                header = "パスの足跡";
                hint = "Path 入力の線を、点ごとの幅とフェザーでマスクにする。"
                       "形はパスの点が持ち、ここでは調整だけ";
                break;
            case graph::NodeKind::MaskArea:
                header = "パスの面";
                hint = "Path 入力の閉じた鎖を多角形とみなし、内側を 1 にする。"
                       "輪の中に輪を描けば穴になる。開いた鎖と点ごとの幅は読まない";
                break;
            default:
                break;
        }
        ui::SectionHeader(header);
        if (ui::BeginPropertyTable("graphMaskRows")) {
            switch (selected->kind) {
                case graph::NodeKind::MaskNoise:
                    changed |= DrawNoiseRows(mask->noise, graph::MaskNodeSettings().noise);
                    break;
                case graph::NodeKind::MaskFlowline:
                    changed |= DrawFlowlineRows(mask->flowline);
                    break;
                case graph::NodeKind::MaskFluvial:
                    changed |= DrawFluvialRows(mask->fluvial);
                    break;
                case graph::NodeKind::WindField:
                    changed |= DrawWindRows(mask->wind);
                    break;
                case graph::NodeKind::MaskHeight:
                    changed |= DrawHeightMaskRows(mask->height);
                    break;
                case graph::NodeKind::MaskSlope:
                    changed |= DrawSlopeRows(mask->slope);
                    break;
                case graph::NodeKind::MaskCurvature:
                    changed |= DrawCurvatureRows(mask->curvature);
                    break;
                case graph::NodeKind::MaskLevels:
                    changed |= DrawLevelsRows(mask->levels);
                    break;
                case graph::NodeKind::MaskBlur:
                    changed |= DrawMaskBlurRows(mask->blur);
                    break;
                case graph::NodeKind::MaskBlend:
                    changed |= DrawBlendRows(mask->blend);
                    break;
                case graph::NodeKind::MaskPath:
                    changed |= DrawPathMaskRows(mask->pathMask);
                    break;
                case graph::NodeKind::MaskArea:
                    changed |= DrawAreaMaskRows(mask->areaMask);
                    break;
                default:
                    changed |= DrawMapSlotRow("画像", mask->map, m_textureLibrary, m_pendingAssetReveal);
                    break;
            }
            ui::EndPropertyTable();
        }
        ui::HintText("%s", hint);
        // Mask Area は閉じた鎖しか読まない。無いと黙って空のマスクになるので注意書きを出す。
        if (selected->kind == graph::NodeKind::MaskArea) {
            const graph::Node* pathNode = m_graph.FindUpstreamNodeForPin(selected->inputs.front().id);
            const auto* pathSettings =
                (pathNode != nullptr) ? std::get_if<graph::PathNodeSettings>(&pathNode->settings)
                                      : nullptr;
            bool hasClosed = false;
            if (pathSettings != nullptr) {
                for (const graph::PathStrand& strand : graph::BuildPathStrands(pathSettings->path)) {
                    hasClosed |= strand.closed;
                }
            }
            if (pathSettings == nullptr) {
                ui::HintText("Path 入力が繋がっていないので、マスクは空になる");
            } else if (!hasClosed) {
                ui::HintText("閉じた鎖が無いので、マスクは空になる。端の点を始点へ重ねるか、"
                             "鎖を右クリック → 閉じる");
            }
        }
        if (changed) {
            m_graph.MarkDirty();
            MarkDocumentChanged();
        }
    } else if (const auto* missing = std::get_if<graph::MissingNodeSettings>(&selected->settings)) {
        ui::SectionHeader("扱えないノード");
        if (ui::BeginPropertyTable("missingNodeRows")) {
            ui::PropertyValue("保存名", "%s", missing->kindName.c_str());
            ui::EndPropertyTable();
        }
        ui::HintText("この版では扱えない種類のノードです。廃止された種類か、新しい版で追加された種類です。"
                     "評価には使われず、保存時は元の種類名のまま書き戻します。不要なら削除してください。");
    } else if (auto* cloudMerge = std::get_if<graph::CloudMergeSettings>(&selected->settings)) {
        const graph::CloudMergeSettings defaults;
        bool changed = false;
        ui::HintText("接続した形状を同じ座標で統合します。接続すると次の空き入力が増えます。同じ形状の重複接続は1回だけ。入れ子の滑らかさは最大値を全体へ適用します。");
        if (ui::BeginPropertyTable("CloudMergeRows", "横方向のばらつき")) {
            changed |= ui::PropertyFloat("つなぎの滑らかさ", &cloudMerge->smoothness, 0.0f, 500.0f, defaults.smoothness, "値はメートル単位。形状の膨らみは最大でこの1/4。", "%.0f m");
            ui::EndPropertyTable();
        }
        if (changed) { m_graph.MarkCloudDirty(); MarkDocumentChanged(false); }
    } else if (auto* weather = std::get_if<graph::CloudWeatherSettings>(&selected->settings)) {
        const graph::CloudWeatherSettings defaults;
        bool changed=false;
        ui::HintText("雲量と雲種のマップで広域の雲層を作ります。Coverage / Type にマスクを接続すると場所ごとに変わり、未接続はスライダーの値を全域に使います。Volume を Cloud Output へ接続。");
        ui::SectionHeader("動き");
        if (ui::BeginPropertyTable("CloudWeatherMotionRows", "積乱雲の広がり")) {
            float position = m_renderer.WeatherLoopPosition(static_cast<uint32_t>(selected->id), weather->loopPosition);
            if (ui::PropertyBool("再生", &weather->animate, defaults.animate, "指定した区間を繰り返し再生します。")) {
                weather->loopPosition = position;
                changed = true;
            }
            if (ui::PropertyFloat("ループ位置", &position, 0.0f, 1.0f, defaults.loopPosition,
                                  "0が開始、1が終端（開始と同じ状態）。再生中も操作でき、指定位置から続けます。", "%.3f")) {
                weather->loopPosition = position;
                m_renderer.SeekWeatherLoop(static_cast<uint32_t>(selected->id), position);
                changed = true;
            }
            changed |= ui::PropertyFloat("ループ時間", &weather->loopDuration, 0.1f, 3600.0f, defaults.loopDuration,
                                         "1周の秒数。終端で先頭へ戻ります。風向や模様の速度比によって継ぎ目が生じます。", "%.1f s");
            changed |= ui::PropertyFloat("風速", &weather->windSpeed, 0.0f, 1000.0f, defaults.windSpeed, nullptr, "%.1f m/s");
            changed |= ui::PropertyFloat("風向", &weather->windDirection, 0.0f, 360.0f, defaults.windDirection, "0 は +Z、90 は +X。", "%.0f °");
            changed |= ui::PropertyBool("模様を変化", &weather->evolveNoise, defaults.evolveNoise, "オフでは現在の形を保ったまま移動します。オンでは雲量の分布は移動し、形状と細部の模様が別の速度で流れて形が変わります。");
            if (weather->evolveNoise)
                changed |= ui::PropertyFloat("模様の速度比", &weather->noiseSpeedRatio, 0.0f, 1.0f, defaults.noiseSpeedRatio, "1 で雲と同じ速度（形を維持）、0 で模様を空間に固定。小さいほど移動に伴う形の変化が速くなります。", "%.2f");
            ui::EndPropertyTable();
        }
        ui::SectionHeader("雲種");
        if (ui::BeginPropertyTable("CloudWeatherTypeRows", "積乱雲の広がり")) {
            changed |= ui::PropertyFloat("雲量", &weather->coverage, 0.0f, 1.0f, defaults.coverage, "雲の占有率。Coverage マスクの値を掛けます。", "%.2f");
            changed |= ui::PropertyFloat("雲種", &weather->cloudType, 0.0f, 1.0f, defaults.cloudType, "0 で層雲、0.5 で積雲、1 で積乱雲。高さプロファイルを連続的に補間します。Type マスクの値を掛けます。", "%.2f");
            changed |= ui::PropertyFloat("積乱雲の広がり", &weather->anvil, 0.0f, 1.0f, defaults.anvil, "雲種 0.5 以上で上部を横へ広げます（かなとこ雲）。", "%.2f");
            changed |= ui::PropertyFloat("雲底のほつれ", &weather->wisp, 0.0f, 1.0f, defaults.wisp, "雲底付近の細部の削りを強めます。", "%.2f");
            changed |= ui::PropertyFloat("帯状の伸び", &weather->streets, 0.0f, 1.0f, defaults.streets, "雲量の分布を風向に沿って引き伸ばし、列状の並びを作ります。", "%.2f");
            changed |= ui::PropertyFloat("塊の密度差", &weather->variation, 0.0f, 1.0f, defaults.variation, "塊ごとに厚い雲と薄い雲を混ぜます。0 で均一。", "%.2f");
            ui::EndPropertyTable();
        }
        ui::SectionHeader("範囲");
        if (ui::BeginPropertyTable("CloudWeatherRangeRows", "積乱雲の広がり")) {
            changed |= ui::PropertyFloat("中心 X", &weather->centerX, -100000.0f, 100000.0f, defaults.centerX, "メートル単位。", "%.0f m");
            changed |= ui::PropertyFloat("中心 Z", &weather->centerZ, -100000.0f, 100000.0f, defaults.centerZ, "メートル単位。", "%.0f m");
            changed |= ui::PropertyFloat("範囲幅", &weather->width, 100.0f, 200000.0f, defaults.width, "マスク全体をこの範囲に割り当てます。範囲を広げると遠くの雲が地平線へ沈みます（100 km で約 800 m）。", "%.0f m");
            changed |= ui::PropertyFloat("範囲奥行き", &weather->depth, 100.0f, 200000.0f, defaults.depth, "マスク全体をこの範囲に割り当てます。", "%.0f m");
            changed |= ui::PropertyFloat("雲底高度", &weather->bottomHeight, -10000.0f, 20000.0f, defaults.bottomHeight, "メートル単位。", "%.0f m");
            changed |= ui::PropertyFloat("最大厚さ", &weather->maxThickness, 100.0f, 20000.0f, defaults.maxThickness, "雲種 1（積乱雲）で使う厚さ。層雲はこの約2割、積雲は約5割の高さまで。", "%.0f m");
            changed |= ui::PropertyFloat("端のフェード", &weather->edgeSoftness, 0.01f, 1.0f, defaults.edgeSoftness, "範囲の端で密度を落とす幅（範囲に対する比率）。", "%.2f");
            ui::EndPropertyTable();
        }
        ui::SectionHeader("形状と密度");
        if (ui::BeginPropertyTable("CloudWeatherShapeRows", "積乱雲の広がり")) {
            static const char* const kNoise[]={"Perlin fBM","Perlin-Worley"};
            changed |= ui::PropertyCombo("ノイズの種類", &weather->noiseType, kNoise, 2, defaults.noiseType);
            changed |= ui::PropertyFloat("模様の大きさ", &weather->noiseScale, 100.0f, 50000.0f, defaults.noiseScale, "形状ノイズの周期。", "%.0f m");
            changed |= ui::PropertyFloat("細部の大きさ", &weather->detailScale, 20.0f, 5000.0f, defaults.detailScale, "細部ノイズの周期。小さいほど縁が細かくなり、近景の刻み幅もこれに合わせて細かくなります。12〜30 km より遠くでは細部を省きます。", "%.0f m");
            changed |= ui::PropertyFloat("細部の削り", &weather->detailStrength, 0.0f, 1.0f, defaults.detailStrength, nullptr, "%.2f");
            changed |= ui::PropertyFloat("遠景の開始距離", &weather->farDistance, 0.0f, 200000.0f, defaults.farDistance, "この距離より先の雲を 1/4 解像度の別パスで描き、手前と合成します。0 で無効。層は地球と同心の球殻として扱い、遠くの雲は地平線へ沈みます。", "%.0f m");
            changed |= ui::PropertyBool("距離 LOD", &weather->distanceLod, defaults.distanceLod, "オンで遠景ほど刻みを伸ばし、刻みより細かいノイズを平均へ寄せ、25〜60 km より先で細部を省きます。オフは全距離を近景の刻みで評価するため負荷が上がります。");
            changed |= ui::PropertyFloat("密度", &weather->extinction, 0.0001f, 0.2f, defaults.extinction,
                "雲の中を進む光の減衰の強さ（消散係数、1/m）。密度 1 の場所で 1 m 進むごとに光がこの割合で失われます。\n"
                "大きいほど光を通しにくく、輪郭が硬く雲影が濃くなります。実際の積雲はおよそ 0.02〜0.1 /m。", "%.4f", ImGuiSliderFlags_Logarithmic);
            changed |= ui::PropertyFloat("Indirect Light", &weather->indirectLight, 0.0f, 1.0f, defaults.indirectLight,
                "雲の中で繰り返し散乱する太陽光の反射率。1 で各次数のエネルギーが単散乱と同じ上限、\n"
                "0 は太陽光の多重散乱なし。光を足す方向には働かず、密度・透過率・地形への雲影は変えません。", "%.2f");
            changed |= ui::PropertyFloat("多重散乱の広がり", &weather->scatterSpread, 0.05f, 0.9f, defaults.scatterSpread,
                "多重散乱した光が雲の中へ行き渡る度合い。高次ほど太陽方向の減衰を (1-広がり) の累乗で縮めます。\n"
                "大きいほど陰の側まで光が回って柔らかく、小さいほど陰が締まります。エネルギーは増えません。", "%.2f");
            changed |= ui::PropertyFloat("Ambient Light", &weather->ambientLight, 0.0f, 5.0f, defaults.ambientLight, nullptr, "%.2f");
            changed |= ui::PropertyInt("シード", &weather->seed, 0, 10000, defaults.seed);
            ui::EndPropertyTable();
        }
        if (changed) { m_graph.MarkCloudDirty(); MarkDocumentChanged(false); }
    } else if (auto* generate = std::get_if<graph::CloudShapeGenerateSettings>(&selected->settings)) {
        const graph::CloudShapeGenerateSettings defaults;
        bool changed=false;
        ui::HintText("単独の積雲を球の集合として生成。Shape を Cloud Replicate / Cloud Merge / Cloud Noise へ接続します。");
        if (ui::BeginPropertyTable("CloudShapeGenerateRows", "二次形状の繰り返し")) {
            static const char* const kSpecies[]={"Humilis（扁平）","Mediocris（中程度）","Congestus（塔状）"};
            changed |= ui::PropertyCombo("雲種", &generate->species, kSpecies, 3, defaults.species, "土台の厚さと塔の高さ・本数を切り替えます。");
            changed |= ui::PropertyInt("シード", &generate->seed, 0, 10000, defaults.seed);
            changed |= ui::PropertyFloat("中心 X", &generate->centerX, -100000.0f, 100000.0f, defaults.centerX, "メートル単位。", "%.0f m");
            changed |= ui::PropertyFloat("中心 Y", &generate->centerY, -10000.0f, 20000.0f, defaults.centerY, "土台の中心高度。メートル単位。", "%.0f m");
            changed |= ui::PropertyFloat("中心 Z", &generate->centerZ, -100000.0f, 100000.0f, defaults.centerZ, "メートル単位。", "%.0f m");
            changed |= ui::PropertyFloat("サイズ", &generate->size, 10.0f, 5000.0f, defaults.size, "基本半径。長さ・幅・球の間隔・塔の高さの基準になります。", "%.0f m");
            changed |= ui::PropertyFloat("長さ (X)", &generate->length, 0.1f, 5.0f, defaults.length, "サイズに対するX方向の倍率。", "%.2f");
            changed |= ui::PropertyFloat("幅 (Z)", &generate->width, 0.1f, 5.0f, defaults.width, "サイズに対するZ方向の倍率。", "%.2f");
            changed |= ui::PropertyFloat("球の間隔", &generate->pointSeparation, 0.05f, 1.0f, defaults.pointSeparation, "サイズに対する比率。小さいほど細かく多数の球を敷き詰めます。", "%.2f");
            changed |= ui::PropertyFloat("乱れ", &generate->distortion, 0.0f, 1.0f, defaults.distortion, "配置と半径のランダムな乱れ。", "%.2f");
            changed |= ui::PropertyFloat("下側の切り取り", &generate->flattenBottom, 0.0f, 0.9f, defaults.flattenBottom, "土台の高さに対する割合。平面より下の球を除き、かかる球を持ち上げます。", "%.2f");
            changed |= ui::PropertyFloat("回転", &generate->rotation, -180.0f, 180.0f, defaults.rotation, "上方向まわりの回転。", "%.0f °");
            changed |= ui::PropertyBool("半径をランダム化", &generate->randomScale, defaults.randomScale, "球ごとに半径へ乱数倍率を掛けます。");
            if (generate->randomScale) {
                changed |= ui::PropertyFloat("倍率 最小", &generate->scaleMin, 0.1f, 3.0f, defaults.scaleMin, nullptr, "%.2f");
                changed |= ui::PropertyFloat("倍率 最大", &generate->scaleMax, 0.1f, 3.0f, defaults.scaleMax, nullptr, "%.2f");
            }
            changed |= ui::PropertyBool("二次形状", &generate->secondaryShapes, defaults.secondaryShapes, "既存の球の上側へ小さな球を積み、輪郭を細かくします。");
            if (generate->secondaryShapes) {
                changed |= ui::PropertyInt("二次形状の繰り返し", &generate->iterations, 1, 3, defaults.iterations, "繰り返すごとに球数が約3倍になります。");
                changed |= ui::PropertyFloat("押し出し", &generate->displacement, 0.0f, 1.0f, defaults.displacement, "親の半径に対する子球の距離。", "%.2f");
                changed |= ui::PropertyFloat("広がり", &generate->spread, 0.0f, 1.0f, defaults.spread, "0で真上、1で水平まで方向が散らばります。", "%.2f");
            }
            changed |= ui::PropertyFloat("つなぎの滑らかさ", &generate->smoothnessRatio, 0.0f, 3.0f, defaults.smoothnessRatio, "球の半径（サイズ×球の間隔）に対する比率。実際の値は下に表示します。球のくびれは最大で実際の値の1/4埋まります。", "%.2f");
            const auto generated=m_graph.CompileCloudShapes(selected->id);
            char text[48]; std::snprintf(text,sizeof(text),"%.0f m",generated.smoothness);
            ui::PropertyValue("実際の滑らかさ",text);
            std::snprintf(text,sizeof(text),"%zu 個",generated.primitives.size());
            ui::PropertyValue("合計形状数",text);
            changed |= ui::PropertyBool("配置ガイド", &generate->showGuides, defaults.showGuides, "選択中に元形状の球を大円で表示します。多数の場合は表示のみ間引きます。");
            ui::EndPropertyTable();
            if (generated.shapeOverflow) ui::HintText("作業メモリ予算を超えました。球の間隔を大きくするか二次形状の繰り返しを減らしてください。");
        }
        if (changed) { m_graph.MarkCloudDirty(); MarkDocumentChanged(false); }
    } else if (auto* map = std::get_if<graph::CloudMapSettings>(&selected->settings)) {
        const graph::CloudMapSettings defaults;
        bool changed=false;
        ui::HintText("近傍点を結ぶ線から雲底と上向きの球列を生成。Shape を Cloud Replicate または Cloud Noise へ接続します。");
        if (ui::BeginPropertyTable("CloudMapRows", "接続線 / 成長ライン")) {
            changed |= ui::PropertyInt("ポイント数", &map->pointCount, 0, 10000, defaults.pointCount, "指定範囲へ一様ランダムに散布します。");
            changed |= ui::PropertyInt("シード", &map->seed, 0, 10000, defaults.seed);
            changed |= ui::PropertyFloat("範囲幅", &map->width, 100.0f, 100000.0f, defaults.width, "メートル単位。", "%.0f m");
            changed |= ui::PropertyFloat("範囲奥行き", &map->depth, 100.0f, 100000.0f, defaults.depth, "メートル単位。", "%.0f m");
            changed |= ui::PropertyFloat("中心 X", &map->centerX, -100000.0f, 100000.0f, defaults.centerX, "メートル単位。", "%.0f m");
            changed |= ui::PropertyFloat("中心 Z", &map->centerZ, -100000.0f, 100000.0f, defaults.centerZ, "メートル単位。", "%.0f m");
            changed |= ui::PropertyFloat("接続距離", &map->connectionDistance, 1.0f, 10000.0f, defaults.connectionDistance, "接続距離以内の全ペアを1回ずつ結びます。", "%.0f m");
            changed |= ui::PropertyFloat("雲底高度", &map->bottomHeight, -10000.0f, 20000.0f, defaults.bottomHeight, "メートル単位。", "%.0f m");
            changed |= ui::PropertyFloat("雲底の厚さ", &map->bottomThickness, 2.0f, 2000.0f, defaults.bottomThickness, "厚さの半分が楕円体の垂直半径になります。", "%.0f m");
            changed |= ui::PropertyFloat("厚さ／横幅の上限", &map->maxThicknessRatio, 0.01f, 1.0f, defaults.maxThicknessRatio, "雲底の厚さを水平直径×比率以下に抑えます。底面高度は共通。最小厚さは2mです。", "%.2f");
            changed |= ui::PropertyFloat("成長ライン密度", &map->columnsPerKm, 0.0f, 50.0f, defaults.columnsPerKm, "線の長さに比例した本数。端数は確率で生成します。0で雲底のみ。", "%.1f 本/km");
            changed |= ui::PropertyFloat("成長高さ 最小", &map->minGrowth, 0.0f, 5000.0f, defaults.minGrowth, "メートル単位。成長高さの最小・最大が逆の場合は入れ替えて評価します。", "%.0f m");
            changed |= ui::PropertyFloat("成長高さ 最大", &map->maxGrowth, 0.0f, 5000.0f, defaults.maxGrowth, "メートル単位。成長高さの最小・最大が逆の場合は入れ替えて評価します。", "%.0f m");
            changed |= ui::PropertyFloat("高さ／横幅の上限", &map->maxHeightRatio, 0.0f, 1.0f, defaults.maxHeightRatio, "接続線の長さ×比率で成長ラインを制限します。成長高さの最小値より優先。0で成長なし。雲底の厚さ・球の半径は含みません。", "%.2f");
            changed |= ui::PropertyFloat("成長球の半径", &map->columnRadius, 10.0f, 1000.0f, defaults.columnRadius, "メートル単位。", "%.0f m");
            changed |= ui::PropertyBool("孤立点を除外", &map->removeIsolated, defaults.removeIsolated, "他のポイントとつながらない点を雲形状と分布ガイドから除外します。");
            if (!map->removeIsolated) changed |= ui::PropertyFloat("孤立点の半径", &map->isolatedRadius, 1.0f, 1000.0f, defaults.isolatedRadius, "メートル単位。", "%.0f m");
            changed |= ui::PropertyFloat("つなぎの滑らかさ", &map->smoothness, 0.0f, 500.0f, defaults.smoothness, "メートル単位。", "%.0f m");
            changed |= ui::PropertyBool("分布ガイド", &map->showGuides, defaults.showGuides, "散布範囲・ポイント・接続線・成長ラインを表示します。多数の場合は表示のみ間引きます。");
            const auto generated=m_graph.CompileCloudShapes(selected->id);
            if (generated.mapGuide) {
                char text[96];
                std::snprintf(text,sizeof(text),"%u / %u",generated.mapGuide->edgeCount,generated.mapGuide->columnCount);
                ui::PropertyValue("接続線 / 成長ライン",text);
            }
            char text[48]; std::snprintf(text,sizeof(text),"%zu 個",generated.primitives.size());
            ui::PropertyValue("合計形状数",text);
            ui::EndPropertyTable();
            if (generated.shapeOverflow) ui::HintText("作業メモリ予算を超えました。ポイント数・接続距離・成長ライン密度を下げてください。");
        }
        if (changed) { m_graph.MarkCloudDirty(); MarkDocumentChanged(false); }
    } else if (auto* animation = std::get_if<graph::CloudAnimationSettings>(&selected->settings)) {
        const graph::CloudAnimationSettings defaults;
        bool changed=false;
        ui::HintText("Cloud Noise → Cloud Animation → Cloud Output と接続します。指定範囲の雲を水平に循環させます。範囲外の元形状は使いません。");
        if (ui::BeginPropertyTable("CloudAnimationRows", "模様の速度比")) {
            changed |= ui::PropertyBool("再生", &animation->playing, defaults.playing, "オフで現在位置に一時停止します。");
            changed |= ui::PropertyFloat("中心 X", &animation->centerX, -100000, 100000, defaults.centerX, "方向は0°が+Z、90°が+X。範囲はワールド座標で指定します。", "%.1f m");
            changed |= ui::PropertyFloat("中心 Z", &animation->centerZ, -100000, 100000, defaults.centerZ, "方向は0°が+Z、90°が+X。範囲はワールド座標で指定します。", "%.1f m");
            changed |= ui::PropertyFloat("幅", &animation->width, 100, 100000, defaults.width, "方向は0°が+Z、90°が+X。範囲はワールド座標で指定します。", "%.1f m");
            changed |= ui::PropertyFloat("奥行き", &animation->depth, 100, 100000, defaults.depth, "方向は0°が+Z、90°が+X。範囲はワールド座標で指定します。", "%.1f m");
            changed |= ui::PropertyFloat("速度", &animation->speed, 0, 1000, defaults.speed, "方向は0°が+Z、90°が+X。範囲はワールド座標で指定します。", "%.1f m/s");
            changed |= ui::PropertyFloat("方向", &animation->direction, 0, 360, defaults.direction, "方向は0°が+Z、90°が+X。範囲はワールド座標で指定します。", "%.1f °");
            changed |= ui::PropertyBool("模様を変化", &animation->evolveNoise, defaults.evolveNoise,
                "雲の移動に対して模様を遅く流します。オフにすると現在の模様を保って移動します。");
            if (animation->evolveNoise)
                changed |= ui::PropertyFloat("模様の速度比", &animation->noiseSpeedRatio, 0, 1, defaults.noiseSpeedRatio,
                    "1で模様を維持。小さいほど移動に伴う変化が速くなります。再生停止中は模様も停止します。", "%.2f");
            ui::PropertyLabelEmpty("resetCloudAnimation");
            if (ui::Button("開始位置へ戻す", ui::kWideButtonWidth)) {
                animation->playing=false;
                if (m_graph.CompileCloud().sourceId==selected->id) m_renderer.ResetCloudMotion();
                changed=true;
            }
            ui::PropertyEnd();
            ui::EndPropertyTable();
        }
        ui::HintText("高さは入力の雲を維持します。複数接続時は出力に近い Animation の設定を使います。再生位置は保存しません。");
        if (changed) { m_graph.MarkCloudDirty(); MarkDocumentChanged(false); }
    } else if (auto* transform = std::get_if<graph::CloudTransformSettings>(&selected->settings)) {
        const graph::CloudTransformSettings defaults;
        bool changed=false;
        ui::HintText("Shape を接続すると、形状全体を移動できます。軸を左ドラッグで移動、Esc で取消。Alt + ドラッグは視点操作です。");
        if (ui::BeginPropertyTable("CloudTransformRows", "移動 X")) {
            changed |= ui::PropertyFloat("移動 X", &transform->translateX, -10000, 10000, defaults.translateX, "元形状からの移動量。", "%.1f m");
            changed |= ui::PropertyFloat("移動 Y", &transform->translateY, -10000, 10000, defaults.translateY, "元形状からの移動量。", "%.1f m");
            changed |= ui::PropertyFloat("移動 Z", &transform->translateZ, -10000, 10000, defaults.translateZ, "元形状からの移動量。", "%.1f m");
            changed |= ui::PropertyBool("配置ガイド", &transform->showGuides, defaults.showGuides, "選択中に移動後の球を大円で表示します。軸ギズモは残ります。");
            ui::EndPropertyTable();
        }
        if (changed) { m_graph.MarkCloudDirty(); MarkDocumentChanged(false); }
    } else if (auto* cloudNoise = std::get_if<graph::CloudNoiseSettings>(&selected->settings)) {
        const graph::CloudNoiseSettings defaults;
        bool changed = false;
        ui::HintText("形状を一体にしてノイズと密度を評価します。Volume をCloud Output へ接続。");
        if (ui::BeginPropertyTable("CloudNoiseRows", "横方向のばらつき")) {
            const char* noiseTypes[] = {"Perlin（従来）", "Perlin fBM", "Perlin-Worley"};
            changed |= ui::PropertyCombo("ノイズの種類", &cloudNoise->noiseType, noiseTypes, 3, defaults.noiseType,
                "Perlin は従来の輪郭。Perlin fBM は複数スケールの模様、Perlin-Worley は丸い膨らみと細部の削りを使います。雲本体と影に共通です。");
            changed |= ui::PropertyFloat("模様の大きさ", &cloudNoise->scale, 10.0f, 5000.0f, defaults.scale, "値はメートル単位。", "%.0f m");
            changed |= ui::PropertyFloat("輪郭の変位", &cloudNoise->displacement, 0.0f, 500.0f, defaults.displacement, "値はメートル単位。", "%.0f m");
            changed |= ui::PropertyFloat("細部の削り", &cloudNoise->detail, 0.0f, 200.0f, defaults.detail, "値はメートル単位。", "%.0f m");
            changed |= ui::PropertyFloat("境界の柔らかさ", &cloudNoise->feather, 1.0f, 300.0f, defaults.feather, "値はメートル単位。", "%.0f m");
            changed |= ui::PropertyBool("雲底を平らにする", &cloudNoise->flattenBottom, defaults.flattenBottom,
                "指定した高さより下の密度を削ります。球のない隙間を埋める機能ではありません。");
            if (cloudNoise->flattenBottom) {
                changed |= ui::PropertyFloat("雲底の高さ", &cloudNoise->bottomHeight, -20000.0f, 20000.0f, defaults.bottomHeight,
                    "ワールド座標の高さ。雲の内部を横切る高さに設定すると平らな雲底になります。", "%.0f m");
                changed |= ui::PropertyFloat("雲底のぼかし幅", &cloudNoise->bottomFeather, 0.0f, 300.0f, defaults.bottomFeather,
                    "雲底から上方向に密度を立ち上げる幅。0では水平面で切り取ります。", "%.0f m");
            }
            changed |= ui::PropertyFloat("密度", &cloudNoise->extinction, 0.0001f, 0.2f, defaults.extinction,
                "雲の中を進む光の減衰の強さ（消散係数、1/m）。密度 1 の場所で 1 m 進むごとに光がこの割合で失われます。\n"
                "大きいほど光を通しにくく、輪郭が硬く雲影が濃くなります。実際の積雲はおよそ 0.02〜0.1 /m。", "%.4f", ImGuiSliderFlags_Logarithmic);
            changed |= ui::PropertyFloat("Indirect Light", &cloudNoise->indirectLight, 0.0f, 1.0f, defaults.indirectLight,
                "雲の中で繰り返し散乱する太陽光の反射率。1 で各次数のエネルギーが単散乱と同じ上限、\n"
                "0 は太陽光の多重散乱なし。光を足す方向には働かず、密度・透過率・地形への雲影は変えません。", "%.2f");
            changed |= ui::PropertyFloat("多重散乱の広がり", &cloudNoise->scatterSpread, 0.05f, 0.9f, defaults.scatterSpread,
                "多重散乱した光が雲の中へ行き渡る度合い。高次ほど太陽方向の減衰を (1-広がり) の累乗で縮めます。\n"
                "大きいほど陰の側まで光が回って柔らかく、小さいほど陰が締まります。エネルギーは増えません。", "%.2f");
            changed |= ui::PropertyFloat("Ambient Light", &cloudNoise->ambientLight, 0.0f, 5.0f, defaults.ambientLight, "形状を一体にしてノイズと密度を評価します。Volume をCloud Output へ接続。", "%.2f");
            changed |= ui::PropertyInt("シード", &cloudNoise->seed, 0, 10000, defaults.seed, "形状を一体にしてノイズと密度を評価します。Volume をCloud Output へ接続。");
            ui::EndPropertyTable();
        }
        const auto compiled = m_graph.CompileCloud();
        if (compiled.shapeOverflow) ui::HintText("形状生成の作業メモリ予算を超えています。球の数や配置密度を下げてください。");
        if (!m_renderer.AtmosphericMode() && ui::Button("大気散乱へ切替", ui::kWideButtonWidth)) {
            m_renderer.AtmosphericMode() = true;
            MarkDocumentChanged(false);
        }
        if (changed) { m_graph.MarkCloudDirty(); MarkDocumentChanged(false); }
    } else if (auto* cloud = std::get_if<graph::CloudNodeSettings>(&selected->settings)) {
        bool changed = false;
        graph::CloudNodeSettings defaults;
        const bool isCloudLayer = selected->kind == graph::NodeKind::CloudLayer;
        if (isCloudLayer) {
            defaults.width = defaults.depth = 12000.0f;
            defaults.thickness = 600.0f;
            defaults.centerY = 500.0f;
            defaults.noiseScale = 1500.0f;
            defaults.motionMode = 1;
            ui::HintText("Distribution にマスクを接続：白に雲、黒は空。未接続は全面。マスク全体を雲層の範囲に割り当てます");
        }
        ui::SectionHeader("動き");
        if (ui::BeginPropertyTable("cloudMotionRows")) {
            changed |= ui::PropertyBool("再生", &cloud->animate, defaults.animate,
                "風で雲を動かします。オフでその位置に一時停止します。");
            const char* modes[] = {"雲全体を移動", "範囲内で模様を流す", "流れながら変化"};
            changed |= ui::PropertyCombo("動かし方", &cloud->motionMode, modes, 3, defaults.motionMode,
                "全体移動、範囲内の移流、流れながら変化を選べます。変化では模様を雲より遅く進めます。切替時は開始位置へ戻ります。");
            if (cloud->motionMode == 2 || (isCloudLayer && cloud->motionMode == 1)) {
                changed |= ui::PropertyFloat("模様の速度比", &cloud->noiseSpeedRatio, 0.0f, 1.0f, defaults.noiseSpeedRatio,
                    "1 で雲と同じ速度（形を維持）、0 で模様を空間に固定。小さいほど移動に伴う形の変化が速くなります。", "%.2f");
            }
            changed |= ui::PropertyFloat("風速", &cloud->windSpeed, 0.0f, 1000.0f, defaults.windSpeed,
                "雲が進む速さ（m/s）。大きいほど速く動きます。", "%.1f m/s");
            changed |= ui::PropertyFloat("風向", &cloud->windDirection, -180.0f, 180.0f, defaults.windDirection,
                "進行方向。0 度は +Z、90 度は +X です。", "%.0f deg");
            ui::PropertyLabelEmpty("resetCloudMotion");
            if (ui::Button("開始位置へ戻す", ui::kWideButtonWidth)) {
                cloud->animate = false;
                if (m_graph.CompileCloud().sourceId == selected->id)
                    m_renderer.ResetCloudMotion();
                changed = true;
            }
            ui::PropertyEnd();
            ui::EndPropertyTable();
        }
        ui::HintText("再生中の環境光・反射は固定し、停止後に更新します");
        ui::SectionHeader("ライティング");
        if (ui::BeginPropertyTable("cloudLightingRows")) {
            changed |= ui::PropertyFloat("Indirect Light", &cloud->indirectLight, 0.0f, 1.0f, defaults.indirectLight,
                "雲の中で繰り返し散乱する太陽光の反射率。1 で各次数のエネルギーが単散乱と同じ上限、\n"
                "0 は太陽光の多重散乱なし。光を足す方向には働かず、密度・透過率・地形への雲影は変えません。", "%.2f");
            changed |= ui::PropertyFloat("多重散乱の広がり", &cloud->scatterSpread, 0.05f, 0.9f, defaults.scatterSpread,
                "多重散乱した光が雲の中へ行き渡る度合い。高次ほど太陽方向の減衰を (1-広がり) の累乗で縮めます。\n"
                "大きいほど陰の側まで光が回って柔らかく、小さいほど陰が締まります。エネルギーは増えません。", "%.2f");
            changed |= ui::PropertyFloat("Ambient Light", &cloud->ambientLight, 0.0f, 5.0f, defaults.ambientLight,
                "雲が空と地面反射から受ける環境光の倍率。スカイライト強度に掛け合わせます。\n"
                "0 は影響なし、1 は従来どおり。地形のスカイライト強度や雲の太陽光・密度は変えません。", "%.2f");
            ui::EndPropertyTable();
        }
        ui::SectionHeader("形と配置");
        if (ui::BeginPropertyTable("cloudNodeRows")) {
            changed |= ui::PropertyBool("有効", &cloud->enabled, defaults.enabled);
            changed |= ui::PropertyFloat("中心 X", &cloud->centerX, -10000.0f, 10000.0f, defaults.centerX,
                                         "雲の中心位置（m）。地形原点からの位置です。", "%.1f m");
            changed |= ui::PropertyFloat("中心 Z", &cloud->centerZ, -10000.0f, 10000.0f, defaults.centerZ,
                                         "雲の中心位置（m）。地形原点からの位置です。", "%.1f m");
            changed |= ui::PropertyFloat("中心高度", &cloud->centerY, -10000.0f, 10000.0f, defaults.centerY,
                                         "雲の中心の高さ（m）。低くすると山腹や谷へ移ります。", "%.1f m");
            changed |= ui::PropertyFloat("横幅", &cloud->width, 10.0f, 20000.0f, defaults.width,
                                         "雲の X 方向の全幅。大きくすると横へ広がります。", "%.1f m", ImGuiSliderFlags_Logarithmic);
            changed |= ui::PropertyFloat("奥行き", &cloud->depth, 10.0f, 20000.0f, defaults.depth,
                                         "雲の Z 方向の全幅。大きくすると奥へ広がります。", "%.1f m", ImGuiSliderFlags_Logarithmic);
            changed |= ui::PropertyFloat("厚さ", &cloud->thickness, 10.0f, 20000.0f, defaults.thickness,
                                         "雲の上下方向の全幅。中心高度の上下へ半分ずつ広がります。", "%.1f m", ImGuiSliderFlags_Logarithmic);
            changed |= ui::PropertyBool("平らな雲底", &cloud->flatBottom, defaults.flatBottom,
                "底を中心高度 − 厚さの半分にそろえ、境界を薄くぼかします。上部の凹凸は残します。");
            if (cloud->flatBottom) {
                changed |= ui::PropertyFloat("雲底の平らさ", &cloud->bottomFlatness, 0.0f, 1.0f, defaults.bottomFlatness,
                    "0 で丸い雲底、1 で従来の平らな雲底。中間値で底の丸みを調整します。", "%.2f");
            }
            changed |= ui::PropertyFloat("消散係数", &cloud->extinction, 0.0001f, 0.2f, defaults.extinction,
                "雲の中を進む光の減衰の強さ（消散係数、1/m）。密度 1 の場所で 1 m 進むごとに光がこの割合で失われます。\n"
                "大きいほど光を通しにくく、輪郭が硬く雲影が濃くなります。実際の積雲はおよそ 0.02〜0.1 /m。", "%.4f", ImGuiSliderFlags_Logarithmic);
            const char* noiseTypes[] = {"Perlin fBM", "Perlin-Worley"};
            changed |= ui::PropertyCombo("ノイズの種類", &cloud->noiseType, noiseTypes, 2, defaults.noiseType,
                "Perlin fBM は従来の模様。Perlin-Worley は丸い細胞状の膨らみと、Worley による細部の崩しを使います。雲本体と影に共通です。");
            changed |= ui::PropertyFloat("模様の大きさ", &cloud->noiseScale, 10.0f, 20000.0f, defaults.noiseScale,
                                         isCloudLayer
                                             ? "配置の1セルあたりの長さ（m）。繰り返しセル数との積が配置の周期になります。表面ノイズの大きさにも影響します。"
                                             : "ノイズの基準となる長さ（m）。大きいほど大きな膨らみになります。",
                                         "%.1f m", ImGuiSliderFlags_Logarithmic);
            if (isCloudLayer) {
                changed |= ui::PropertyInt("繰り返しセル数", &cloud->cellCount, 1, 32, defaults.cellCount,
                    "横・奥行きに共通の配置周期。範囲内の雲の個数ではありません。大きくすると配置が繰り返すまでの距離が長くなります。高さ方向のセルはありません。");
                changed |= ui::PropertyFloat("雲量", &cloud->coverage, 0.0f, 1.0f, defaults.coverage,
                    "分布の中を雲で覆う量。0 で雲なし、1 で隙間が少なくなります。", "%.2f");
            } else changed |= ui::PropertyFloat("形の崩し", &cloud->shapeStrength, 0.0f, 1.0f, defaults.shapeStrength,
                                         "楕円体の輪郭をノイズで削ります。大きいほど形が崩れます。", "%.2f");
            changed |= ui::PropertyFloat("細部の崩し", &cloud->detailStrength, 0.0f, 1.0f, defaults.detailStrength,
                                         "細かな密度の抜けを加えます。大きいほど縁がほぐれます。", "%.2f");
            changed |= ui::PropertyFloat("縁の柔らかさ", &cloud->edgeSoftness, 0.02f, 1.0f, defaults.edgeSoftness,
                                         "輪郭から内側へ密度が増す幅の割合。大きいほど薄く柔らかくなります。", "%.2f");
            changed |= ui::PropertyInt("シード", &cloud->seed, 0, 10000, defaults.seed);
            ui::EndPropertyTable();
        }
        ui::HintText("Volume をCloud Output へ接続して表示。太陽と照明はライティング設定を共有します");
        if (!m_renderer.AtmosphericMode() && ui::Button("大気散乱へ切替", ui::kWideButtonWidth)) {
            m_renderer.AtmosphericMode() = true;
            MarkDocumentChanged(false);
        }
        if (changed) {
            m_graph.MarkCloudDirty();
            MarkDocumentChanged(false);
        }
    } else if (auto* plume = std::get_if<graph::SnowPlumeSettings>(&selected->settings)) {
        const graph::SnowPlumeSettings defaults;
        bool changed = false;
        ui::HintText("Source のマスクが強い所から、風下へ半透明の帯を伸ばして雪煙を描く。"
                     "Wind Field の Spindrift を繋ぐと、稜線の風下から出て Wind Field の風に流れる。"
                     "ビューポートだけの表示で、合成結果や書き出しには入らない");
        const auto compiled = m_graph.CompileSnowPlumes();
        const auto found = std::find_if(compiled.begin(), compiled.end(),
                                        [&](const auto& c) { return c.node == selected->id; });
        const bool fromWind = found != compiled.end() && found->fromWindField;
        if (found != compiled.end() && found->maskPin == 0) ui::HintText("Source が未接続なので何も出ない");
        ui::SectionHeader("発生");
        if (ui::BeginPropertyTable("snowPlumeSource")) {
            changed |= ui::PropertyFloat("しきい値", &plume->threshold, 0.0f, 0.99f, defaults.threshold,
                                         "Source がこれ以下の所からは出さない。上の値ほど強い所に絞られる", "%.2f");
            changed |= ui::PropertyFloat("発生の割合", &plume->coverage, 0.0f, 1.0f, defaults.coverage,
                                         "Source が 1 の所で帯が出る割合。下げると間引かれてまばらになる", "%.2f");
            changed |= ui::PropertyInt("種の数（一辺）", &plume->seedsPerSide, 4, 256, defaults.seedsPerSide,
                                       "地形の一辺をこの数に分け、各マスから最大 1 本の帯を出す。多いほど密で重い");
            changed |= ui::PropertyInt("シード", &plume->seed, 0, 1000000, defaults.seed);
            ui::EndPropertyTable();
        }
        ui::SectionHeader("形");
        if (ui::BeginPropertyTable("snowPlumeShape")) {
            changed |= ui::PropertyFloat("長さ", &plume->lengthMeters, 1.0f, 5000.0f, defaults.lengthMeters,
                                         "風下へ伸びる長さ。帯ごとに ±10% ばらつく", "%.0f m", ImGuiSliderFlags_Logarithmic);
            changed |= ui::PropertyFloat("根元の幅", &plume->widthStart, 0.1f, 2000.0f, defaults.widthStart,
                                         "種の間隔（地形の一辺 ÷ 種の数）の 1.3 倍が下限。それより細くしても変わらない"
                                         "（隣の帯と必ず重ねるため）。細くしたいときは種の数を増やす",
                                         "%.1f m", ImGuiSliderFlags_Logarithmic);
            changed |= ui::PropertyFloat("先端の幅", &plume->widthEnd, 0.1f, 2000.0f, defaults.widthEnd,
                                         "風下へ行くほどこの幅まで広がる。根元の幅（下限を掛けた後）より細くはならない",
                                         "%.1f m", ImGuiSliderFlags_Logarithmic);
            changed |= ui::PropertyFloat("持ち上がり", &plume->lift, 0.0f, 1000.0f, defaults.lift,
                                         "稜線を越えてから浮き上がる高さ", "%.0f m");
            changed |= ui::PropertyFloat("沈み込み", &plume->sink, 0.0f, 1000.0f, defaults.sink,
                                         "先端までに風下の斜面側へ下がる高さ。地形の下へは潜らない", "%.0f m");
            changed |= ui::PropertyFloat("風上の助走", &plume->upwind, 0.0f, 1000.0f, defaults.upwind,
                                         "稜線の風上側へ帯を延ばす長さ。斜面を這って立ち上がり、稜線でいちばん濃くなって剥がれる。"
                                         "0 だと稜線の風下から急に湧いて見える", "%.0f m");
            changed |= ui::PropertyFloat("斜面に沿う", &plume->slopeFollow, 0.0f, 1.0f, defaults.slopeFollow,
                                         "風下の斜面が落ちるぶんを追う割合。0 でまっすぐ流れ、1 で斜面に沿って谷へ下りる。"
                                         "上のシートほど追わないので、谷へ覆いかぶさる楔の形になる", "%.2f");
            changed |= ui::PropertyInt("シートの数", &plume->sheets, 1, 3, defaults.sheets,
                                       "1 本の帯に重ねる層の数。2 枚目以降は上に薄く広く重なり、厚みが出る");
            ui::EndPropertyTable();
        }
        ui::SectionHeader("見え方");
        if (ui::BeginPropertyTable("snowPlumeLook")) {
            changed |= ui::PropertyFloat("不透明度", &plume->opacity, 0.0f, 1.0f, defaults.opacity, nullptr, "%.2f");
            changed |= ui::PropertyFloat("塊の大きさ", &plume->puffSize, 1.0f, 1000.0f, defaults.puffSize,
                                         "雪煙の濃淡の基本の大きさ", "%.0f m", ImGuiSliderFlags_Logarithmic);
            changed |= ui::PropertyFloat("乱れ", &plume->turbulence, 0.0f, 1.0f, defaults.turbulence,
                                         "帯の蛇行と、輪郭のちぎれ具合", "%.2f");
            changed |= ui::PropertyFloat("前方散乱", &plume->anisotropy, 0.0f, 0.95f, defaults.anisotropy,
                                         "逆光で縁が光る強さ（位相関数の g）", "%.2f");
            ui::EndPropertyTable();
        }
        ui::SectionHeader("動き");
        if (ui::BeginPropertyTable("snowPlumeMotion")) {
            changed |= ui::PropertyFloat("突風", &plume->gust, 0.0f, 1.0f, defaults.gust,
                                         "帯ごとに位相をずらして、長さと濃さを時間で揺らす", "%.2f");
            changed |= ui::PropertyFloat("ループ長", &plume->loopSeconds, 1.0f, 600.0f, defaults.loopSeconds,
                                         "この秒数で動きが完全に一巡する（継ぎ目のないループ）", "%.1f 秒");
            if (fromWind) {
                ui::PropertyValue("風", "%.0f° / %.1f m/s（Wind Field）", found->windDirection, found->windSpeed);
            } else {
                changed |= ui::PropertyFloat("風向", &plume->windDirection, 0.0f, 360.0f, defaults.windDirection,
                                             "Source の上流に Wind Field が無いときの風向。0 が +Z、90 が +X", "%.0f°");
                changed |= ui::PropertyFloat("風速", &plume->windSpeed, 0.0f, 100.0f, defaults.windSpeed,
                                             "雪煙の濃淡が流れる速さ", "%.1f m/s");
            }
            ui::EndPropertyTable();
        }
        // 評価に効くのは Source の接続だけで、設定は描画の値。グラフの版は上げない。
        if (changed) MarkDocumentChanged(false);
    } else if (auto* scatter = std::get_if<graph::ModelScatterSettings>(&selected->settings)) {
        bool changed = false;
        ui::HintText("Crumbling の Points を接続し、Instances をModel Outputへ接続します");
        if (ui::BeginPropertyTable("modelScatterSettings", "LOD 距離の倍率")) {
            changed |= ui::PropertyInt("シード",&scatter->seed,0,1000000,1);
            changed |= ui::PropertyFloat("描画距離", &scatter->maxDistance, 0, 100000, 0,
                "この距離より遠いモデルを描画対象から外します。0は距離制限なし。影にも適用します", "%.0f m");
            changed |= ui::PropertyBool("ポイントの大きさ",&scatter->usePointSize,true,"モデルの最大寸法を岩片の直径に合わせます");
            // 対数のスライダーにして 1.0 付近を細かく動かせるようにする。
            constexpr const char* kScaleTooltip = "株ごとの倍率はこの範囲から選ぶ。大きくすると描画と影の負荷が増える";
            changed |= ui::PropertyFloat("最小スケール",&scatter->scaleMin,graph::kModelScatterScaleMin,graph::kModelScatterScaleMax,
                                         0.8f,kScaleTooltip,"%.2f",ImGuiSliderFlags_Logarithmic);
            changed |= ui::PropertyFloat("最大スケール",&scatter->scaleMax,graph::kModelScatterScaleMin,graph::kModelScatterScaleMax,
                                         1.2f,kScaleTooltip,"%.2f",ImGuiSliderFlags_Logarithmic);
            changed |= ui::PropertyFloat("地表に沿う",&scatter->alignToNormal,0,1,1);
            changed |= ui::PropertyFloat("接地オフセット",&scatter->offset,-10000,10000,0,"負の値で地面へ埋め込みます","%.3f m");
            changed |= ui::PropertyBool("LOD 自動",&scatter->autoLod,true,
                "カメラからの距離で LOD を切り替えます。切り替え距離はモデルのプロパティで設定します");
            if (scatter->autoLod)
                changed |= ui::PropertyFloat("LOD 距離の倍率",&scatter->lodBias,0.01f,100.0f,1.0f,
                    "モデルの切り替え距離に掛けます。小さくすると近くから簡略な段階になり軽くなります","%.2f");
            else
                changed |= ui::PropertyInt("LOD",&scatter->lod,0,16,0,"モデルにないLODは最も近い段階を使います");
            ui::EndPropertyTable();
        }
        std::vector<const char*> names{"未指定"};
        for (const auto& model : m_models) names.push_back(model.name.c_str());
        int remove = -1;
        if (ui::BeginPropertyTable("scatterModels")) {
            for (size_t i=0;i<scatter->models.size();++i) {
                ImGui::PushID(static_cast<int>(i));
                auto& choice=scatter->models[i]; int index=0;
                for(size_t j=0;j<m_models.size();++j) if(m_models[j].id==choice.model) index=static_cast<int>(j)+1;
                const auto label="モデル "+std::to_string(i+1);
                ui::PropertyLabel(label.c_str());
                ImGui::SetNextItemWidth(AssetReferenceWidth(std::min(ui::Scaled(ui::kComboMaxWidth), ImGui::GetContentRegionAvail().x)));
                if (ImGui::BeginCombo("##model", !index && choice.model ? "見つからないモデル" : names[index])) {
                    for (size_t j=0; j<names.size(); ++j) {
                        ImGui::PushID(static_cast<int>(j));
                        if (ImGui::Selectable(names[j], index == static_cast<int>(j))) {
                            index = static_cast<int>(j);
                            choice.model = j ? m_models[j-1].id : 0; changed = true;
                        }
                        ImGui::PopID();
                    }
                    // プロジェクト内の未読み込みモデルも選べる。読み込みは描画の外で行う。
                    for (const auto& path : m_workspace.AssetsWithExtension(L".tgmodel")) {
                        if (std::any_of(m_models.begin(), m_models.end(), [&](const auto& model) { return model.assetPath == path; })) continue;
                        const auto relative = ToUtf8Display(path.lexically_relative(m_workspace.Root()));
                        ImGui::PushID(relative.c_str());
                        if (ImGui::Selectable(relative.c_str())) {
                            m_pendingScatterModel = path;
                            m_pendingScatterNode = selected->id;
                            m_pendingScatterChoice = i;
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndCombo();
                }
                DrawAssetSourceButton(index ? (m_models[index-1].assetPath.empty() ? m_models[index-1].path : m_models[index-1].assetPath)
                                            : std::filesystem::path{}, m_pendingAssetReveal);
                ui::PropertyEnd();
                changed |= ui::PropertyFloat("出現比率",&choice.weight,0,1000,1);
                ui::PropertyLabel("候補"); if(ui::Button("削除")) remove=static_cast<int>(i); ui::PropertyEnd();
                ImGui::PopID();
            }
            ui::EndPropertyTable();
        }
        if(remove>=0) { scatter->models.erase(scatter->models.begin()+remove); changed=true; }
        if(ui::Button("モデルを追加",ui::kWideButtonWidth)) {
            scatter->models.push_back({m_models.empty()?0:m_models.front().id,1}); changed=true;
        }
        if(changed) { m_graph.MarkDirty(); MarkDocumentChanged(false); }
    } else if (selected->kind == graph::NodeKind::ModelMerge) {
        ui::HintText("複数のInstancesをまとめます。接続すると入力が増え、各Model Scatterの設定を保持します");
    } else if (selected->kind == graph::NodeKind::ModelOutput) {
        ui::HintText("Model Scatter / Model Merge のInstancesを接続します。ハイトマップとは独立してモデルを描画します");
    } else if (selected->kind == graph::NodeKind::Terrain) {
        ui::HintText("地形グラフの Output に繋いだ結果を Result に出します。Mask Slope / Mask Height などの "
                     "Base に繋ぐと、雲グラフのマスクを実際の地形から作れます");
        if (m_graph.CompileLayers().layers.empty() || m_graph.FindChainScale(0) == nullptr) {
            ui::HintText("地形グラフの Output に何も繋がっていないので、Result は空になる");
        }
    } else if (selected->kind == graph::NodeKind::CloudOutput) {
        ui::HintText("Cloud Noise の Volume を接続して表示します。Cloud Animation を挟むと移動できます。未接続なら雲は表示しません");
        ui::HintText("保存済みの Cloud / Cloud Layer (Legacy) も引き続き表示できます");
        if (!m_renderer.AtmosphericMode() && ui::Button("大気散乱へ切替", ui::kWideButtonWidth)) {
            m_renderer.AtmosphericMode() = true; m_pendingWorkEnvironmentSave = true;
            MarkDocumentChanged();
        }
    } else if (std::get_if<graph::PathNodeSettings>(&selected->settings) != nullptr) {
        if (DrawPathSettings(*selected)) {
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
