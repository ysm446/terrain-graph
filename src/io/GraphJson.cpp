#include "io/GraphJson.h"
#include "io/JsonRead.h"

#include "core/Log.h"
#include "graph/NodeGraph.h"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <optional>
#include <string>
#include <tuple>

namespace tg::io {
using nlohmann::json;

namespace {

// Cloud Merge / Model Merge の入力は可変長。
constexpr size_t kMaxVariadicInputs = 64;

bool IsVariadic(const graph::NodeDefinition* definition, graph::PinKind kind) {
    return definition != nullptr && kind == graph::PinKind::Input &&
           (definition->kind == graph::NodeKind::CloudMerge || definition->kind == graph::NodeKind::ModelMerge);
}

size_t PinCount(const graph::NodeDefinition* definition, graph::PinKind kind) {
    if (definition == nullptr) {
        return 0;
    }
    return static_cast<size_t>(std::count_if(definition->pins.begin(), definition->pins.end(),
                                             [kind](const graph::PinDefinition& pin) { return pin.kind == kind; }));
}

const char* PinKey(graph::PinKind kind) {
    return kind == graph::PinKind::Input ? "inputs" : "outputs";
}

std::string PinLabels(const graph::NodeDefinition* definition, graph::PinKind kind) {
    std::string labels;
    if (definition != nullptr) {
        for (const graph::PinDefinition& pin : definition->pins) {
            if (pin.kind == kind) {
                labels += labels.empty() ? "" : ", ";
                labels += pin.label;
            }
        }
    }
    if (IsVariadic(definition, kind)) {
        labels += "（番号を増やして足せる）";
    }
    return labels.empty() ? "なし" : labels;
}

}  // namespace

void NormalizeGraphJson(json& graph) {
    if (!graph.is_object() || !graph.contains("nodes") || !graph["nodes"].is_array()) {
        return;
    }
    json& nodes = graph["nodes"];
    if (!graph.contains("links") || !graph["links"].is_array()) {
        graph["links"] = json::array();
    }
    json& links = graph["links"];

    // 新しい ID は、ファイルに出てくるどの ID よりも大きく振る（ノード / ピン / リンクは同じ ID 空間）。
    int maxId = 0;
    const auto consider = [&maxId](const json& value) {
        if (value.is_number_integer()) {
            maxId = std::max(maxId, value.get<int>());
        }
    };
    for (const json& node : nodes) {
        if (!node.is_object()) {
            continue;
        }
        if (node.contains("id")) consider(node["id"]);
        for (const char* key : {"inputs", "outputs"}) {
            if (node.contains(key) && node[key].is_array()) {
                for (const json& pin : node[key]) consider(pin);
            }
        }
    }
    for (const json& link : links) {
        if (!link.is_object()) {
            continue;
        }
        for (const char* key : {"id", "start", "end"}) {
            if (link.contains(key)) consider(link[key]);
        }
    }

    // ピンの ID を書いていないノードには、定義のピンの数だけ振る。
    std::map<int, json*> nodeById;
    for (json& node : nodes) {
        if (!node.is_object() || !node.contains("id") || !node["id"].is_number_integer()) {
            continue;
        }
        nodeById[node["id"].get<int>()] = &node;
        const graph::NodeDefinition* definition =
            node.contains("kind") && node["kind"].is_string()
                ? graph::FindNodeDefinitionByName(node["kind"].get<std::string>())
                : nullptr;
        for (const graph::PinKind kind : {graph::PinKind::Input, graph::PinKind::Output}) {
            if (node.contains(PinKey(kind)) && node[PinKey(kind)].is_array()) {
                continue;
            }
            json pins = json::array();
            for (size_t i = 0; i < PinCount(definition, kind); ++i) {
                pins.push_back(++maxId);
            }
            node[PinKey(kind)] = std::move(pins);
        }
    }

    const auto resolve = [&](const json& reference, graph::PinKind kind) -> std::optional<int> {
        if (!reference.is_array() || reference.size() != 2 || !reference[0].is_number_integer() ||
            !(reference[1].is_string() || reference[1].is_number_integer())) {
            TG_LOG_WARN("リンクのピンは [ノード ID, ピン名 か 番号] で書いてください: %s", reference.dump().c_str());
            return std::nullopt;
        }
        const int nodeId = reference[0].get<int>();
        const auto found = nodeById.find(nodeId);
        if (found == nodeById.end()) {
            TG_LOG_WARN("リンクの先のノード %d がありません", nodeId);
            return std::nullopt;
        }
        json& node = *found->second;
        // 手書きのグラフでは "kind" が文字列でないこともある。value() は型違いで投げるので使わない。
        const std::string kindName = ReadString(node, "kind");
        const graph::NodeDefinition* definition = graph::FindNodeDefinitionByName(kindName);
        const bool variadic = IsVariadic(definition, kind);

        std::optional<size_t> index;
        if (reference[1].is_number_integer()) {
            if (reference[1].get<int>() >= 0) index = static_cast<size_t>(reference[1].get<int>());
        } else {
            const std::string name = reference[1].get<std::string>();
            if (definition != nullptr) index = graph::FindPinDefinitionIndex(*definition, kind, name);
            // 可変入力は "Shape 3" / "Instances 3" のように番号付きの名前でも指せる（1 始まり）。
            if (!index && variadic) {
                const size_t last = name.find_last_not_of("0123456789");
                if (last != std::string::npos && last + 1 < name.size()) {
                    const int number = std::atoi(name.c_str() + last + 1);
                    if (number >= 1) index = static_cast<size_t>(number - 1);
                }
            }
        }

        json& pins = node[PinKey(kind)];
        // 古いファイルで定義よりピンが少ない / 可変入力の番号が大きい、ときは足す。
        const size_t limit = variadic ? kMaxVariadicInputs : PinCount(definition, kind);
        if (index && *index < limit) {
            while (pins.size() <= *index) {
                pins.push_back(++maxId);
            }
        }
        if (index && *index < pins.size() && pins[*index].is_number_integer()) {
            return pins[*index].get<int>();
        }
        TG_LOG_WARN("ノード %d（%s）に%sピン %s がありません（あるのは: %s）", nodeId, kindName.c_str(),
                    kind == graph::PinKind::Input ? "入力" : "出力", reference[1].dump().c_str(),
                    PinLabels(definition, kind).c_str());
        return std::nullopt;
    };

    json normalized = json::array();
    for (json link : links) {
        if (!link.is_object()) {
            continue;
        }
        bool resolved = true;
        for (const auto& [named, key, kind] : {std::tuple{"from", "start", graph::PinKind::Output},
                                               std::tuple{"to", "end", graph::PinKind::Input}}) {
            if (!link.contains(named)) {
                continue;
            }
            if (const std::optional<int> pin = resolve(link[named], kind)) {
                link[key] = *pin;
            } else {
                resolved = false;
            }
            link.erase(named);
        }
        if (!resolved) {
            continue;
        }
        if (!link.contains("id") || !link["id"].is_number_integer() || link["id"].get<int>() <= 0) {
            link["id"] = ++maxId;
        }
        normalized.push_back(std::move(link));
    }
    links = std::move(normalized);
}

}  // namespace tg::io
