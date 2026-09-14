#include "io/SceneComponents.h"
#include "core/PathUtf8.h"
#include "core/Log.h"
#include <algorithm>
#include <map>
#include <set>
#include <functional>
#include <limits>

namespace tg::io {
namespace fs = std::filesystem;
using nlohmann::json;
namespace {
bool Integer(const json& j) { return j.is_number_integer() && j >= 0 && j < 100000000; }
bool GraphValid(const json& graph) {
    if (!graph.is_object() || !graph.contains("nodes") || !graph["nodes"].is_array() ||
        !graph.contains("links") || !graph["links"].is_array()) return false;
    std::set<int> ids, inputs, outputs;
    const auto add = [&](const json& value) {
        return Integer(value) && value > 0 && ids.insert(value.get<int>()).second;
    };
    for (const auto& n : graph["nodes"]) {
        if (!n.is_object() || !n.contains("id") || !add(n["id"]) ||
            !n.contains("kind") || !n["kind"].is_string()) return false;
        if (n.contains("component") && (!Integer(n["component"]) || n["component"] > 1)) return false;
        for (const auto* key : {"inputs", "outputs"}) {
            if (!n.contains(key) || !n[key].is_array()) return false;
            for (const auto& pin : n[key]) {
                if (!add(pin)) return false;
                (std::string_view(key) == "inputs" ? inputs : outputs).insert(pin.get<int>());
            }
        }
    }
    for (const auto& link : graph["links"]) {
        if (!link.is_object() || !link.contains("id") || !add(link["id"]) ||
            !link.contains("start") || !Integer(link["start"]) ||
            !link.contains("end") || !Integer(link["end"]) ||
            !outputs.contains(link["start"].get<int>()) || !inputs.contains(link["end"].get<int>())) return false;
    }
    return true;
}
const char* Kind(int component) { return component == 1 ? "cloud-graph" : "terrain-graph"; }
const char* Extension(int component) { return component == 1 ? ".tgcloud" : ".tgterrain"; }
const char* Role(int component) { return component == 1 ? "cloud" : "terrain"; }
const char* const Tables[] = {"textures", "materials", "models", "paintMasks"};
const char* const RefKeys[] = {"texture", "material", "model", "paint"};
// グラフ設定内の資源参照だけを変換する。Pathの点IDなどには触れない。
bool RemapResources(json& value, const std::map<int, int> (&maps)[4]) {
    if (value.is_array()) {
        for (auto& child : value) if (!RemapResources(child, maps)) return false;
    } else if (value.is_object()) {
        for (auto& [key, child] : value.items()) {
            bool mapped = false;
            for (int i = 0; i < 4; ++i) if (key == RefKeys[i] && child.is_number_integer()) {
                if (!Integer(child)) return false;
                const int id = child.get<int>();
                if (id == 0) { mapped = true; break; }
                const auto found = maps[i].find(id);
                if (found == maps[i].end()) return false;
                child = found->second; mapped = true; break;
            }
            if (!mapped && !RemapResources(child, maps)) return false;
        }
    }
    return true;
}
}

bool AssignGraphComponents(json& graph) {
    if (!GraphValid(graph)) return false;
    auto& nodes = graph["nodes"];
    std::map<int, size_t> pinNodes;
    std::vector<std::set<size_t>> neighbors(nodes.size());
    for (size_t i = 0; i < nodes.size(); ++i)
        for (const auto* key : {"inputs", "outputs"})
            for (const auto& pin : nodes[i][key]) pinNodes[pin.get<int>()] = i;
    for (const auto& link : graph["links"]) {
        const auto a = pinNodes.at(link["start"].get<int>()), b = pinNodes.at(link["end"].get<int>());
        neighbors[a].insert(b); neighbors[b].insert(a);
    }
    std::set<size_t> visited;
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (visited.contains(i)) continue;
        std::vector<size_t> group{i}; visited.insert(i);
        bool cloud = false, terrain = false;
        for (size_t j = 0; j < group.size(); ++j) {
            const auto index = group[j];
            const auto kind = ProjectWorkspace::String(nodes[index], "kind");
            const bool cloudKind = kind.starts_with("cloud");
            cloud |= cloudKind || nodes[index].value("component", 0) == 1;
            terrain |= !cloudKind && !kind.starts_with("mask") && kind != "path";
            for (const auto next : neighbors[index]) if (visited.insert(next).second) group.push_back(next);
        }
        if (cloud && terrain) {
            TG_LOG_ERROR("地形と雲をまたぐ接続があります。接続を保持するため自動分離を中止しました");
            return false;
        }
        for (const auto index : group) nodes[index]["component"] = cloud ? 1 : 0;
    }
    return true;
}

bool SaveSceneComponents(ProjectWorkspace& workspace, const fs::path& scene, json& document) {
    if (!workspace.Contains(scene) || !document.is_object()) return false;
    auto graph = document.value("graph", json());
    if (!AssignGraphComponents(graph)) return false;
    const auto previous = document.value("_components", json::array());
    if (!previous.is_array()) return false;
    if (document.contains("paintResolution") && !Integer(document["paintResolution"])) return false;
    json components = json::array();
    struct Pending { fs::path path; json body; int component; };
    std::vector<Pending> pending;
    const int only = document.value("_componentOnly", -1);
    for (int component = 0; component < 2; ++component) {
        if (only >= 0 && only != component) continue;
        json part = {{"nodes", json::array()}, {"links", json::array()}};
        std::set<int> pins;
        for (auto node : graph["nodes"]) if (node["component"] == component) {
            node.erase("component");
            for (const auto* key : {"inputs", "outputs"})
                for (const auto& pin : node[key]) pins.insert(pin.get<int>());
            part["nodes"].push_back(std::move(node));
        }
        for (const auto& link : graph["links"]) if (pins.contains(link["start"].get<int>()))
            part["links"].push_back(link);
        // 空の雲も保持する。後でノードを追加できる編集対象になる。
        json body = {{"name", ToUtf8Display(scene.stem()) + (component ? " 雲" : " 地形")}, {"graph", part}};
        for (const auto* key : Tables) body[key] = document.value(key, json::array());
        // 各グラフが直接使う参照だけを持つ。素材の依存画像等はExpandで辿る。
        std::set<int> used[4];
        const std::function<void(const json&)> collect = [&](const json& value) {
            if (value.is_array()) for (const auto& child : value) collect(child);
            else if (value.is_object()) for (const auto& [key, child] : value.items()) {
                for (int i = 0; i < 4; ++i) if (key == RefKeys[i] && Integer(child) && child > 0) used[i].insert(child.get<int>());
                collect(child);
            }
        };
        collect(part["nodes"]);
        for (int i = 0; i < 4; ++i) {
            if (!body[Tables[i]].is_array()) return false;
            auto& entries = body[Tables[i]].get_ref<json::array_t&>();
            std::set<int> found;
            for (const auto& entry : entries) {
                if (!entry.is_object() || !entry.contains("id") || !Integer(entry["id"])) return false;
                found.insert(entry["id"].get<int>());
            }
            for (const int id : used[i]) if (!found.contains(id)) return false;
            std::erase_if(entries, [&](const auto& entry) { return !used[i].contains(entry["id"].template get<int>()); });
        }
        body["paintResolution"] = document.value("paintResolution", 1024);
        if (!component && document.contains("preview")) {
            for (const auto* key : {"mesh", "planeSize", "displacementScale", "tessellation", "tessellationFactor", "materialResolution", "meshSubdivisions"})
                if (document["preview"].contains(key)) body["geometry"][key] = document["preview"][key];
        }
        fs::path path;
        for (const auto& entry : previous) if (ProjectWorkspace::String(entry, "role") == Role(component)) {
            path = workspace.Resolve(entry.value("asset", json::object()));
            json existing;
            if (path.empty() || !workspace.ReadAsset(path, Kind(component), existing)) return false;
            body["uid"] = ProjectWorkspace::String(existing, "uid");
            body["name"] = existing.value("name", body["name"]);
        }
        if (path.empty()) path = workspace.UniquePath(scene.parent_path(), ToUtf8Display(scene.stem()) + (component ? "_clouds" : "_terrain"), Extension(component));
        if (path.empty()) return false;
        // ペイントはコンポーネントの原本として独立コピーする。旧シーンの整理で消えない。
        for (auto& paint : body["paintMasks"]) {
            const auto input = scene.parent_path() / (scene.stem().wstring() + L".assets") / FromUtf8(ProjectWorkspace::String(paint, "file"));
            const auto directory = path.parent_path() / (path.stem().wstring() + L".assets");
            const auto output = workspace.UniquePath(directory, "paint", ".png");
            std::error_code error;
            if (output.empty() || !fs::is_regular_file(input, error)) return false;
            fs::create_directories(directory, error);
            if (error || !fs::copy_file(input, output, fs::copy_options::none, error)) return false;
            paint["source"] = workspace.Reference(output);
            if (paint["source"].is_null()) return false;
            paint.erase("file");
        }
        pending.push_back({path, std::move(body), component});
    }
    // 既存アセットは更新前にバックアップ。保存失敗時も元の内容を戻せる。
    std::vector<std::pair<fs::path, json>> originals;
    for (auto& item : pending) {
        json original;
        std::error_code error;
        if (fs::exists(item.path, error)) {
            if (!ProjectWorkspace::ReadJson(item.path, original)) return false;
            const auto backup = workspace.UniquePath(workspace.Root() / L".terrain-graph/component-backups", ToUtf8Display(item.path.stem()), ToUtf8Portable(item.path.extension()).c_str());
            if (backup.empty() || !ProjectWorkspace::WriteJson(backup, original)) return false;
        }
        originals.emplace_back(item.path, original);
    }
    const auto rollback = [&]() {
        for (const auto& [path, original] : originals) {
            if (!original.is_null()) {
                if (!ProjectWorkspace::WriteJson(path, original)) TG_LOG_ERROR("コンポーネントの復元に失敗しました。component-backupsを確認してください");
            } else { std::error_code error; fs::remove(path, error); }
        }
        workspace.Scan();
    };
    for (size_t i = 0; i < pending.size(); ++i) {
        auto& item = pending[i];
        if (!workspace.SaveAsset(item.path, Kind(item.component), item.body)) {
            rollback(); return false;
        }
        components.push_back({{"role", Role(item.component)}, {"asset", workspace.Reference(item.path)}});
    }
    document["components"] = components;
    document.erase("_components"); document.erase("graph");
    for (const auto* key : Tables) document.erase(key);
    document.erase("paintResolution");
    document["version"] = 2;
    json validation = document;
    if (!ExpandSceneComponents(workspace, validation) || !workspace.Expand(validation)) { rollback(); return false; }
    if (only < 0 && !ProjectWorkspace::WriteJson(scene, document)) { rollback(); return false; }
    return true;
}

bool ExpandSceneComponents(ProjectWorkspace& workspace, json& document) {
    if (!document.contains("components") || !document["components"].is_array()) return false;
    json result = document;
    result["graph"] = {{"nodes", json::array()}, {"links", json::array()}};
    for (const auto* key : Tables) result[key] = json::array();
    int nextGraphId = 1;
    std::map<std::string, int> knownResources[4];
    std::set<std::string> roles;
    for (const auto& entry : document["components"]) {
        const auto role = ProjectWorkspace::String(entry, "role");
        if ((role != "terrain" && role != "cloud") || !roles.insert(role).second) return false;
        const int component = role == "cloud" ? 1 : 0;
        const auto path = workspace.Resolve(entry.value("asset", json::object()));
        json body;
        if (path.empty() || !workspace.ReadAsset(path, Kind(component), body) ||
            !GraphValid(body.value("graph", json()))) return false;
        std::map<int, int> resources[4];
        for (int table = 0; table < 4; ++table) {
            const auto values = body.value(Tables[table], json::array());
            if (!values.is_array()) return false;
            auto& destination = result[Tables[table]];
            for (auto value : values) {
                if (!value.is_object() || !value.contains("id") || !Integer(value["id"])) return false;
                const auto reference = value.value(table == 0 || table == 3 ? "source" : "asset", json::object());
                const auto uid = ProjectWorkspace::String(reference, "uid");
                if (uid.empty()) return false;
                const auto known = knownResources[table].find(uid);
                const int id = known == knownResources[table].end() ? static_cast<int>(destination.size()) + 1 : known->second;
                if (!resources[table].emplace(value["id"].get<int>(), id).second) return false;
                if (known != knownResources[table].end()) continue;
                knownResources[table][uid] = id;
                value["id"] = id;
                if (table == 3) {
                    const auto file = workspace.Resolve(value.value("source", json::object()));
                    std::error_code error;
                    if (file.empty() || !fs::is_regular_file(file, error)) return false;
                    value["file"] = ToUtf8Portable(file);
                }
                destination.push_back(std::move(value));
            }
        }
        auto graph = body["graph"];
        if (!RemapResources(graph["nodes"], resources)) return false;
        std::map<int, int> ids;
        const auto remap = [&](json& id) {
            const int old = id.get<int>();
            if (!ids.contains(old)) ids[old] = nextGraphId++;
            id = ids.at(old);
        };
        for (auto& node : graph["nodes"]) {
            remap(node["id"]); node["component"] = component;
            for (const auto* key : {"inputs", "outputs"}) for (auto& pin : node[key]) remap(pin);
            result["graph"]["nodes"].push_back(node);
        }
        for (auto& link : graph["links"]) {
            for (const auto* key : {"id", "start", "end"}) remap(link[key]);
            result["graph"]["links"].push_back(link);
        }
        result["paintResolution"] = body.value("paintResolution", 1024);
        if (!component && body.contains("geometry") && body["geometry"].is_object())
            for (const auto& [key, value] : body["geometry"].items()) result["preview"][key] = value;
    }
    result["_components"] = document["components"];
    document = std::move(result);
    return true;
}

fs::path MigrateSceneComponents(ProjectWorkspace& workspace, const fs::path& source) {
    json document;
    if (!workspace.Contains(source) || !workspace.Scan() || !ProjectWorkspace::ReadJson(source, document) ||
        ProjectWorkspace::String(document, "format") != "terrain-graph.scene" || document["version"] != 1) return {};
    // 参照欠落と不正なグラフを、書き出し前に確認する。
    json validation = document;
    if (!workspace.Expand(validation) || !AssignGraphComponents(document["graph"])) return {};
    const auto destination = workspace.UniquePath(source.parent_path(), ToUtf8Display(source.stem()) + "_migrated", ".tgscene");
    if (destination.empty()) return {};
    // 元のペイントへの絶対パスは保存処理だけに渡す。
    for (auto& paint : document["paintMasks"])
        paint["file"] = ToUtf8Portable(source.parent_path() / (source.stem().wstring() + L".assets") / FromUtf8(ProjectWorkspace::String(paint, "file")));
    document.erase("sceneUid");
    if (!SaveSceneComponents(workspace, destination, document)) return {};
    TG_LOG_INFO("元のシーンを保持して分離しました: %s", ToUtf8Display(destination).c_str());
    return destination;
}
}
