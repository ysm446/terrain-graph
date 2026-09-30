// --evaluate-report: 読み込んだグラフを評価しきってから、結果を JSON に書いて終了する。
// スクリプトや LLM が UI を見ずに「エラーは無いか・寸法は合っているか・何が作られたか」を
// 確かめるための経路。形式は docs/reference/file-format.md の「評価レポート」。
#include "app/Application.h"

#include "core/PathUtf8.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace tg {
using nlohmann::json;

bool Application::EvaluationSettled(bool evaluationIdle) {
    // --open-graph は 2 フレーム目に積むので、それより後から見る。
    if (m_frameCounter < std::max<uint32_t>(m_options.screenshotFrame, 3u)) return false;
    if (!m_pendingProjectOpen.empty() || !m_pendingAssetOpen.empty()) return false;
    const compositor::MaterialEvaluator& evaluator = m_renderer.Evaluator();
    if (!evaluationIdle || evaluator.HasPendingPostprocess() ||
        evaluator.EvaluatedRevision() != m_graphStack.Revision()) {
        return false;
    }
    // 配置の点は本体の評価の後に 1 つずつ作り、残った数は読み戻しで届く。
    for (const graph::CompiledModelScatter& scatter : m_modelScatters) {
        const compositor::PlacementPointSet* points = nullptr;
        const PlacementPointsState state = PlacementPointsOf(scatter.source, &points);
        if (state == PlacementPointsState::Evaluating) return false;
        if (state == PlacementPointsState::Ready && points != nullptr && !points->countReady) return false;
    }
    return true;
}

bool Application::WriteEvaluationReport(bool timedOut, bool& reportOk) {
    json errors = json::array();
    json warnings = json::array();
    const auto addIssue = [](json& list, graph::GraphId node, const std::string& message) {
        json item = {{"message", message}};
        if (node != 0) item["node"] = node;
        list.push_back(std::move(item));
    };
    if (timedOut) addIssue(errors, 0, "評価が時間内に落ち着かなかった（結果は途中のもの）");
    if (m_projectPath.empty()) addIssue(errors, 0, "シーンを読み込めていない");

    // --- ノード ---------------------------------------------------------
    const std::vector<graph::GraphId>& cycle = GraphCycleNodes();
    json nodes = json::array();
    for (const graph::Node& node : m_graph.Nodes()) {
        json item;
        item["id"] = node.id;
        const graph::NodeDefinition* definition = graph::FindNodeDefinition(node.kind);
        item["kind"] = definition != nullptr ? definition->name : "";
        item["component"] = node.component == 1 ? "cloud" : "terrain";
        if (const auto* layer = std::get_if<graph::LayerNodeSettings>(&node.settings)) item["name"] = layer->layer.name;
        if (graph::IsBypassed(node)) item["bypass"] = true;
        json nodeErrors = json::array();
        if (const auto* missing = std::get_if<graph::MissingNodeSettings>(&node.settings)) {
            item["kind"] = missing->kindName;
            nodeErrors.push_back("扱えないノードの種類: " + missing->kindName);
        }
        if (std::binary_search(cycle.begin(), cycle.end(), node.id)) nodeErrors.push_back("接続が循環している");
        if (const auto road = m_roadNodeStatus.find(node.id); road != m_roadNodeStatus.end()) {
            const RoadNodeStatus& status = road->second;
            if (!status.error.empty()) nodeErrors.push_back(status.error);
            if (!status.materialError.empty()) nodeErrors.push_back(status.materialError);
            item["stats"] = {{"lengthMeters", status.lengthMeters},
                             {"vertices", status.vertices},
                             {"triangles", status.triangles}};
        }
        for (const auto& message : nodeErrors) addIssue(errors, node.id, message.get<std::string>());
        if (!nodeErrors.empty()) item["errors"] = std::move(nodeErrors);
        nodes.push_back(std::move(item));
    }

    // --- 地形 -----------------------------------------------------------
    json terrain;
    terrain["sizeMeters"] = m_renderer.PlaneSize();
    terrain["heightMeters"] = m_renderer.DisplacementScale();
    terrain["resolution"] = m_renderer.MaterialResolution();
    float baseElevation = 0.0f;
    for (const graph::Node& node : m_graph.Nodes()) {
        if (node.kind != graph::NodeKind::Output) continue;
        if (const graph::TerrainScale* scale = m_graph.FindChainScale(node.id)) {
            baseElevation = scale->baseElevationMeters;
            if (scale->hasLocation) terrain["location"] = {{"latitude", scale->latitude}, {"longitude", scale->longitude}};
        }
        break;
    }
    terrain["baseElevationMeters"] = baseElevation;
    // 評価器が CPU へ写した縮小ハイト（0〜1）から標高の範囲を出す。
    if (const compositor::CpuHeightfield& field = m_renderer.Evaluator().Heightfield(); field.IsValid()) {
        float low = std::numeric_limits<float>::max(), high = std::numeric_limits<float>::lowest();
        double sum = 0.0;
        for (const float value : field.values) {
            low = std::min(low, value);
            high = std::max(high, value);
            sum += value;
        }
        const float scale = m_renderer.DisplacementScale();
        terrain["heightfield"] = {
            {"resolution", field.resolution},
            {"min", low}, {"max", high}, {"mean", sum / double(field.values.size())},
            {"minElevationMeters", baseElevation + low * scale},
            {"maxElevationMeters", baseElevation + high * scale},
        };
        if (!(high > low)) addIssue(warnings, 0, "地形が平ら（ハイトに起伏が無い）");
    } else {
        addIssue(warnings, 0, "地形のハイトを読めていない");
    }

    // --- 配置 -----------------------------------------------------------
    json scatters = json::array();
    uint64_t instanceTotal = 0;
    for (const graph::CompiledModelScatter& scatter : m_modelScatters) {
        json item = {{"node", scatter.node}, {"source", scatter.source}, {"outputs", scatter.outputs},
                     {"models", scatter.settings.models.size()}};
        const compositor::PlacementPointSet* points = nullptr;
        const PlacementPointsState state = PlacementPointsOf(scatter.source, &points);
        if (state == PlacementPointsState::Ready && points != nullptr && points->countReady) {
            item["candidates"] = points->count;
            item["instances"] = points->activeCount;
            instanceTotal += points->activeCount;
            if (points->activeCount == 0) addIssue(warnings, scatter.node, "配置の点が 1 つも残っていない");
        } else {
            addIssue(warnings, scatter.node, "配置の点を数えられなかった");
        }
        if (scatter.settings.models.empty()) addIssue(warnings, scatter.node, "配置するモデルが選ばれていない");
        scatters.push_back(std::move(item));
    }

    // --- メッシュ（道路・路肩・区画線） ----------------------------------
    size_t meshVertices = 0, meshTriangles = 0, meshNodes = 0;
    for (const auto& [id, status] : m_roadNodeStatus) {
        if (status.triangles == 0) continue;
        ++meshNodes;
        meshVertices += status.vertices;
        meshTriangles += status.triangles;
    }

    // --- 参照しているアセット -------------------------------------------
    for (const auto& texture : m_textureLibrary.Entries()) {
        if (texture.missing) addIssue(errors, 0, "テクスチャが見つからない: " + ToUtf8Portable(texture.path));
    }
    for (const auto& material : m_materialLibrary.Entries()) {
        if (!material.layerError.empty()) addIssue(errors, 0, "マテリアル " + material.name + ": " + material.layerError);
    }
    for (const auto& model : m_models) {
        if (!model.error.empty()) addIssue(errors, 0, "モデル " + model.name + ": " + model.error);
    }

    // --- ログ -----------------------------------------------------------
    json log = json::array();
    for (const auto& [level, text] : m_reportLog) {
        log.push_back({{"level", level == LogLevel::Error ? "error" : "warn"}, {"message", text}});
        addIssue(level == LogLevel::Error ? errors : warnings, 0, text);
    }

    json report;
    report["format"] = "terrain-graph.evaluate-report";
    report["version"] = 1;
    report["scene"] = ToUtf8Portable(m_projectPath);
    if (m_componentPreview >= 0) report["graph"] = ToUtf8Portable(m_componentPreviewPath);
    report["timedOut"] = timedOut;
    report["frames"] = m_frameCounter;
    report["seconds"] = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_reportStart).count();
    report["terrain"] = std::move(terrain);
    report["counts"] = {
        {"nodes", m_graph.Nodes().size()},
        {"links", m_graph.Links().size()},
        {"meshNodes", meshNodes},
        {"meshVertices", meshVertices},
        {"meshTriangles", meshTriangles},
        {"instances", instanceTotal},
    };
    report["scatters"] = std::move(scatters);
    report["nodes"] = std::move(nodes);
    report["log"] = std::move(log);

    if (!m_options.reportThumbnailPath.empty()) {
        const std::filesystem::path thumbnail = std::filesystem::absolute(m_options.reportThumbnailPath);
        if (m_renderer.SaveOutputToPng(m_device, thumbnail, 512)) report["thumbnail"] = ToUtf8Portable(thumbnail);
        else addIssue(warnings, 0, "サムネイルを書けなかった");
    }

    reportOk = errors.empty();
    report["ok"] = reportOk;
    report["errors"] = std::move(errors);
    report["warnings"] = std::move(warnings);

    const std::filesystem::path path = std::filesystem::absolute(m_options.reportPath);
    if (!io::ProjectWorkspace::WriteJson(path, report)) {
        TG_LOG_ERROR("評価のレポートを書けませんでした: %s", ToUtf8Portable(path).c_str());
        return false;
    }
    TG_LOG_INFO("評価のレポートを書きました: %s", ToUtf8Portable(path).c_str());
    return true;
}

}  // namespace tg
