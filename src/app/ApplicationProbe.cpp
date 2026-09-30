// ノードの設定の行（ui::Property*）から、範囲・既定値・単位を取り出す。
//
// 設定の範囲はプロパティ UI の呼び出しの中にしか書かれていないので、見えないウィンドウで
// ノードの複製の設定を描き、ヘルパーに渡った値を記録する（ui::SetPropertyRecorder）。
// 行と保存のキーは、値を少し動かして書き出し（io::WriteNodeJson）のどこが変わるかで対応づける。
// ノードカタログ（--dump-catalog）と評価のレポート（--evaluate-report）の範囲の検査が同じ行を読む。
#include "app/Application.h"

#include <nlohmann/json.hpp>

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <set>

namespace tg {
using nlohmann::json;

namespace {

// 書式（"%.1f m" など）から数値の部分を除いた単位。
std::string UnitOf(const std::string& format) {
    std::string unit;
    for (size_t i = 0; i < format.size(); ++i) {
        if (format[i] != '%') {
            unit += format[i];
            continue;
        }
        if (i + 1 < format.size() && format[i + 1] == '%') {
            unit += '%';
            ++i;
            continue;
        }
        size_t j = i + 1;
        while (j < format.size() && std::strchr("-+ #0123456789.", format[j]) != nullptr) ++j;
        i = j;  // 変換指定子（f / d など）まで飛ばす
    }
    const size_t first = unit.find_first_not_of(' ');
    const size_t last = unit.find_last_not_of(' ');
    return first == std::string::npos ? std::string() : unit.substr(first, last - first + 1);
}

const char* TypeName(ui::PropertyRecord::Type type) {
    switch (type) {
    case ui::PropertyRecord::Type::Float: return "float";
    case ui::PropertyRecord::Type::Int: return "int";
    case ui::PropertyRecord::Type::Bool: return "bool";
    case ui::PropertyRecord::Type::Combo: return "enum";
    case ui::PropertyRecord::Type::Color: return "color";
    }
    return "";
}

// JSON Pointer（"/layer/height/scale"）を "." 区切りのパスへ。
std::string DottedPath(const std::string& pointer) {
    std::string path = pointer.empty() ? pointer : pointer.substr(1);
    std::replace(path.begin(), path.end(), '/', '.');
    return path;
}

}  // namespace

std::vector<ui::PropertyRecord> Application::RecordNodeProperties(graph::Node& node) {
    std::vector<ui::PropertyRecord> records;
    ImGui::PushID(++m_probeId);
    ui::SetPropertyRecorder(&records);
    DrawNodeProperties(&node);
    ui::SetPropertyRecorder(nullptr);
    ImGui::PopID();
    return records;
}

// 行の値を動かしてよいか。ノードの複製の中か、スタックの外（ノードが持つ配列など）だけ。
// UI が一時的な局所変数に写して渡した行は、描き終わった時点で場所が無効になっている。
bool Application::CanTouchProperty(const graph::Node& node, const ui::PropertyRecord& record) const {
    const auto* address = static_cast<const char*>(record.value);
    const auto* begin = reinterpret_cast<const char*>(&node);
    if (address >= begin && address < begin + sizeof(graph::Node)) return true;
    ULONG_PTR low = 0, high = 0;
    ::GetCurrentThreadStackLimits(&low, &high);
    const auto value = reinterpret_cast<ULONG_PTR>(address);
    return address != nullptr && (value < low || value >= high);
}

std::string Application::FindPropertyPath(graph::Node& node, const ui::PropertyRecord& record,
                                          const json& baseFlat) {
    if (!CanTouchProperty(node, record)) return {};
    const auto changedPaths = [&] {
        const json flat = io::WriteNodeJson(node).flatten();
        std::vector<std::string> changed;
        for (const auto& [key, value] : flat.items()) {
            const auto found = baseFlat.find(key);
            if (found == baseFlat.end() || *found != value) changed.push_back(key);
        }
        for (const auto& [key, value] : baseFlat.items()) {
            if (!flat.contains(key)) changed.push_back(key);
        }
        return changed;
    };
    std::vector<std::string> changed;
    switch (record.type) {
    case ui::PropertyRecord::Type::Float: {
        float& value = *static_cast<float*>(record.value);
        const float old = value;
        float step = static_cast<float>((record.maxValue - record.minValue) * 0.1);
        if (!(step > 0.0f) || !std::isfinite(step)) step = 0.5f;
        value = (old + step <= record.maxValue) ? old + step : old - step;
        changed = changedPaths();
        value = old;
        break;
    }
    case ui::PropertyRecord::Type::Int:
    case ui::PropertyRecord::Type::Combo: {
        int& value = *static_cast<int*>(record.value);
        const int old = value;
        if (record.maxValue <= record.minValue) return {};
        value = (old + 1 <= record.maxValue) ? old + 1 : old - 1;
        changed = changedPaths();
        value = old;
        break;
    }
    case ui::PropertyRecord::Type::Bool: {
        bool& value = *static_cast<bool*>(record.value);
        value = !value;
        changed = changedPaths();
        value = !value;
        break;
    }
    case ui::PropertyRecord::Type::Color: {
        float& red = static_cast<float*>(record.value)[0];
        const float old = red;
        red = old > 0.5f ? old - 0.25f : old + 0.25f;
        changed = changedPaths();
        red = old;
        // 色は配列の 1 要素だけが変わる。キーは配列そのもの。
        if (changed.size() == 1) changed.front() = changed.front().substr(0, changed.front().rfind('/'));
        break;
    }
    }
    // 1 つに決まらない（何も変わらない・連動して複数変わる）行は対応づけない。
    return changed.size() == 1 ? DottedPath(changed.front()) : std::string();
}

json Application::DescribeProperty(graph::Node& node, const ui::PropertyRecord& record, const std::string& path) {
    json item;
    item["label"] = record.label;
    item["type"] = TypeName(record.type);
    if (!record.tooltip.empty()) item["tooltip"] = record.tooltip;
    switch (record.type) {
    case ui::PropertyRecord::Type::Float:
    case ui::PropertyRecord::Type::Int: {
        item["min"] = record.minValue;
        item["max"] = record.maxValue;
        item["default"] = record.defaultValue;
        if (const std::string unit = UnitOf(record.format); !unit.empty()) item["unit"] = unit;
        if (record.logarithmic) item["logarithmic"] = true;
        break;
    }
    case ui::PropertyRecord::Type::Bool:
        item["default"] = record.defaultValue != 0.0;
        break;
    case ui::PropertyRecord::Type::Combo: {
        // 選択肢ごとに書き出して、ファイルに書く値を集める。
        int& value = *static_cast<int*>(record.value);
        const int old = value;
        json values = json::array();
        const std::string pointer = "/" + [&] {
            std::string text = path;
            std::replace(text.begin(), text.end(), '.', '/');
            return text;
        }();
        for (int i = 0; i < static_cast<int>(record.items.size()); ++i) {
            value = i;
            const json flat = io::WriteNodeJson(node).flatten();
            const auto found = flat.find(pointer);
            values.push_back({{"value", found != flat.end() ? *found : json()}, {"label", record.items[i]}});
        }
        value = old;
        item["values"] = std::move(values);
        const auto index = static_cast<size_t>(record.defaultValue);
        if (index < item["values"].size()) item["default"] = item["values"][index]["value"];
        break;
    }
    case ui::PropertyRecord::Type::Color:
        item["space"] = record.format;  // "linear" / "srgb"
        break;
    }
    return item;
}

json Application::ProbeNodeParameters(const graph::Node& original) {
    // 選択肢とオン・オフを切り替えた状態も辿る（条件付きで出る行を拾うため）。切り替えは 2 段まで、
    // 描く状態の数にも上限を置く。列挙は UI が一時的な値を介して描くことが多く行の場所を動かせないので、
    // 保存の JSON の値を書き換えて読み戻した状態を作る。
    constexpr size_t kMaxStates = 80;
    constexpr int kMaxDepth = 2;
    struct State {
        graph::Node node;
        int depth = 0;
    };
    json parameters = json::object();
    std::set<std::string> mapped, unmapped;
    std::deque<State> pending{{original, 0}};
    std::set<std::string> seen{io::WriteNodeJson(original).dump()};
    const auto push = [&](graph::Node&& node, int depth) {
        if (seen.insert(io::WriteNodeJson(node).dump()).second) pending.push_back({std::move(node), depth});
    };
    size_t drawn = 0;
    while (!pending.empty() && drawn < kMaxStates) {
        State state = std::move(pending.front());
        pending.pop_front();
        ++drawn;
        graph::Node& node = state.node;
        const std::vector<ui::PropertyRecord> records = RecordNodeProperties(node);
        const json baseFlat = io::WriteNodeJson(node).flatten();
        const bool expand = state.depth < kMaxDepth;
        for (const ui::PropertyRecord& record : records) {
            const std::string path = FindPropertyPath(node, record, baseFlat);
            if (path.empty()) {
                unmapped.insert(record.label);
                continue;
            }
            mapped.insert(record.label);
            if (!parameters.contains(path)) parameters[path] = DescribeProperty(node, record, path);
            // オン・オフを切り替えた状態を積む。
            if (expand && record.type == ui::PropertyRecord::Type::Bool) {
                bool& value = *static_cast<bool*>(record.value);
                value = !value;
                graph::Node variant = node;
                value = !value;
                push(std::move(variant), state.depth + 1);
            }
        }
        // 列挙を別の値にした状態を積む。
        if (!expand) continue;
        const json settings = io::WriteNodeJson(node);
        // 一時オブジェクトの items() を回すと、ループの間に元が消える。変数に受ける。
        const json enums = io::EnumFieldsOf(node);
        for (const auto& [path, values] : enums.items()) {
            std::string pointer = "/" + path;
            std::replace(pointer.begin(), pointer.end(), '.', '/');
            const json::json_pointer at(pointer);
            for (const json& value : values) {
                if (settings.at(at) == value) continue;
                json changed = settings;
                changed[at] = value;
                graph::Node variant;
                if (io::ReadNodeJson(node, changed, variant)) push(std::move(variant), state.depth + 1);
            }
        }
    }
    // 保存のキーに対応づけられなかった行（ラベル）。一時的な値を介して描く行（多くは列挙。
    // 値はカタログの enums にある）や、保存しない表示の設定。どの状態でも対応づかなかったものだけ残す。
    json unmappedRows = json::array();
    for (const std::string& label : unmapped) {
        if (!mapped.contains(label)) unmappedRows.push_back(label);
    }
    return {{"parameters", std::move(parameters)}, {"unmappedRows", std::move(unmappedRows)}};
}

void Application::RunPropertyProbes() {
    m_propertyProbeDone = true;
    // 画面の外のウィンドウで描く。表は画面外でも中身を描く（スクロールする子ウィンドウでなければ）。
    // 透明（Alpha 0）にすると ImGui が中身ごと飛ばすので、位置で隠す。
    ImGui::SetNextWindowPos(ImVec2(-20000.0f, -20000.0f));
    ImGui::SetNextWindowSize(ImVec2(720.0f, 540.0f));
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                        ImGuiWindowFlags_NoBackground;
    if (ImGui::Begin("##propertyProbe", nullptr, kFlags)) {
        if (!m_options.catalogPath.empty()) {
            // 種類ごとに 1 つだけ置いたノードを描く。今のグラフの ID と混ざらないよう、空のグラフに差し替える。
            graph::NodeGraph saved = std::move(m_graph);
            m_graph = graph::NodeGraph();
            for (const graph::NodeDefinition& definition : graph::NodeDefinitions()) {
                if (definition.kind == graph::NodeKind::Missing) continue;
                graph::NodeGraph scratch;
                const graph::Node* node = scratch.FindNode(scratch.CreateNode(definition.kind));
                if (node != nullptr) m_catalogParameters[definition.name] = ProbeNodeParameters(*node);
            }
            m_graph = std::move(saved);
        }
        if (!m_options.reportPath.empty()) {
            // 設定の値が UI の範囲の外にあるノード。エディタで選ぶと範囲へ丸まる値。
            for (const graph::Node& original : m_graph.Nodes()) {
                graph::Node node = original;
                const std::vector<ui::PropertyRecord> records = RecordNodeProperties(node);
                json baseFlat;
                for (const ui::PropertyRecord& record : records) {
                    if (record.type != ui::PropertyRecord::Type::Float && record.type != ui::PropertyRecord::Type::Int) continue;
                    const double tolerance = 1.0e-5 * std::max(1.0, std::max(std::abs(record.minValue), std::abs(record.maxValue)));
                    if (record.shown >= record.minValue - tolerance && record.shown <= record.maxValue + tolerance) continue;
                    if (baseFlat.is_null()) baseFlat = io::WriteNodeJson(node).flatten();
                    const std::string path = FindPropertyPath(node, record, baseFlat);
                    char text[512];
                    std::snprintf(text, sizeof(text), "「%s」の値 %g は範囲 %g〜%g の外（エディタで開くと丸まる）",
                                  record.label.c_str(), record.shown, record.minValue, record.maxValue);
                    json issue = {{"node", node.id}, {"message", text}};
                    if (!path.empty()) issue["path"] = path;
                    m_reportRangeIssues.push_back(std::move(issue));
                }
            }
        }
    }
    ImGui::End();
}

}  // namespace tg
