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
const char* Kind(int component) { return component == 2 ? "atmosphere-sky" : component == 1 ? "cloud-graph" : "terrain-graph"; }
const char* Extension(int component) { return component == 1 ? ".tgcloud" : ".tgterrain"; }
const char* Role(int component) { return component == 1 ? "cloud" : "terrain"; }
const char* const AtmosphereKeys[] = {"azimuth", "elevation", "illuminance", "density", "mie",
    "eccentricity", "altitude", "groundAlbedo", "lowerHemisphere", "skylightIntensity",
    "nightEnabled", "moonAzimuth", "moonElevation", "moonIlluminance", "moonPhase", "starIntensity", "starRotation", "starLatitude"};
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

json AtmosphereAssetBody(const json& preview, const std::string& name) {
    json settings = json::object();
    const auto source = preview.is_object() ? preview.value("atmosphere", json::object()) : json::object();
    if (source.is_object()) for (const auto* key : AtmosphereKeys)
        if (source.contains(key)) settings[key] = source[key];
    return {{"name", name}, {"settings", settings}};
}

bool ExpandSceneAtmosphere(ProjectWorkspace& workspace, json& document) {
    if (!document.contains("atmosphere")) return true;
    const auto reference = document["atmosphere"];
    json body;
    const auto path = workspace.Resolve(reference);
    if (path.empty() || !workspace.ReadAsset(path, "atmosphere-sky", body) ||
        !body.contains("settings") || !body["settings"].is_object()) return false;
    auto& preview = document["preview"];
    if (preview.is_null()) preview = json::object();
    if (!preview.is_object()) return false;
    auto& settings = preview["atmosphere"];
    if (settings.is_null()) settings = json::object();
    if (!settings.is_object()) return false;
    // 新しいスカイに無い値は、前のスカイから引き継がず既定値へ戻す。
    for (const auto* key : AtmosphereKeys) settings.erase(key);
    for (const auto* key : AtmosphereKeys) if (body["settings"].contains(key)) settings[key] = body["settings"][key];
    preview["lightingMode"] = "atmospheric";
    document["_atmosphereAsset"] = reference;
    return true;
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
    if (!workspace.Contains(scene) || !document.is_object() ||
        (document.contains("preview") && !document["preview"].is_object())) return false;
    auto graph = document.value("graph", json());
    if (!AssignGraphComponents(graph)) return false;
    const auto previous = document.value("_components", json::array());
    if (!previous.is_array()) return false;
    if (document.contains("paintResolution") && !Integer(document["paintResolution"])) return false;
    // 参照の並びは書いた順ではなく地形・雲の順で固定する（読み手が位置で見分けられるように）。
    json componentEntries[2];
    struct Pending { fs::path path; json body; int component; };
    std::vector<Pending> pending;
    // only: 0 以上ならその部品だけを書き、シーン本体は書かない（2 は大気散乱スカイ）。
    // write: シーン全体の保存で書き直す部品のビット。無い部品はファイルが無いときだけ作る。
    // どちらもファイルには残さない。
    const int only = document.value("_componentOnly", -1);
    const int write = document.value("_componentWrite", 7);
    document.erase("_componentWrite");
    const auto previousWork = workspace.WorkEnvironment();
    auto work = previousWork;
    const auto preview = document.value("preview", json::object());
    if (only < 0 && preview.is_object()) {
        const auto mode = ProjectWorkspace::String(preview, "lightingMode");
        work["lightingMode"] = mode.empty() ? previousWork.value("lightingMode", "atmospheric") : mode;
        if (preview.contains("light")) work["light"] = preview["light"];
        const auto skies = document.value("skies", json::array());
        if (skies.is_array() && !skies.empty()) {
            const int active = document.value("activeSky", 0);
            const auto& sky = skies[active >= 0 && active < static_cast<int>(skies.size()) ? active : 0];
            if (sky.contains("asset")) work["sky"] = sky["asset"];
        }
    }
    if (only < 0 || only == 2) {
        auto body = AtmosphereAssetBody(document.value("preview", json::object()), ToUtf8Display(scene.stem()) + " スカイ");
        fs::path path;
        const auto reference = document.value("_atmosphereAsset", json());
        if (!reference.is_null()) {
            path = workspace.Resolve(reference);
            json existing;
            if (path.empty() || !workspace.ReadAsset(path, "atmosphere-sky", existing)) return false;
            body["uid"] = ProjectWorkspace::String(existing, "uid");
            body["name"] = existing.value("name", body["name"]);
        } else path = workspace.UniquePath(scene.parent_path(), ToUtf8Display(scene.stem()) + "_sky", ".tgatmosphere");
        if (path.empty()) return false;
        // 変更の無いスカイは書き直さず、既存の参照をそのまま持ち越す。
        if (!reference.is_null() && only < 0 && !(write & 4)) document["atmosphere"] = reference;
        else pending.push_back({path, std::move(body), 2});
    }
    for (int component = 0; component < 2; ++component) {
        if (only >= 0 && only != component) continue;
        // 配置済みの部品は元ファイルの ID と名前を引き継ぐ。
        fs::path path;
        json existing, existingReference;
        for (const auto& entry : previous) if (ProjectWorkspace::String(entry, "role") == Role(component)) {
            existingReference = entry.value("asset", json::object());
            path = workspace.Resolve(existingReference);
            if (path.empty() || !workspace.ReadAsset(path, Kind(component), existing)) return false;
        }
        // 変更の無い部品は書き直さない（バックアップも増やさない）。
        if (!path.empty() && only < 0 && !(write & (1 << component))) {
            componentEntries[component] = {{"role", Role(component)}, {"asset", existingReference}};
            continue;
        }
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
        if (!path.empty()) {
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
        if (item.component == 2) document["atmosphere"] = workspace.Reference(item.path);
        else componentEntries[item.component] = {{"role", Role(item.component)}, {"asset", workspace.Reference(item.path)}};
    }
    json components = json::array();
    for (const auto& entry : componentEntries) if (!entry.is_null()) components.push_back(entry);
    document["components"] = components;
    document.erase("_components"); document.erase("graph");
    for (const auto* key : Tables) document.erase(key);
    document.erase("paintResolution");
    document["version"] = 3;
    document.erase("_atmosphereAsset");
    document.erase("skies"); document.erase("activeSky");
    if (document.contains("preview") && document["preview"].is_object()) {
        auto& scenePreview = document["preview"];
        scenePreview.erase("lightingMode"); scenePreview.erase("light");
        if (scenePreview.contains("atmosphere") && scenePreview["atmosphere"].is_object())
            for (const auto* key : AtmosphereKeys) scenePreview["atmosphere"].erase(key);
    }
    json validation = document;
    if (!ExpandSceneComponents(workspace, validation) || !workspace.Expand(validation)) { rollback(); return false; }
    if (only < 0) {
        if (!workspace.SetWorkEnvironment(work)) { rollback(); return false; }
        if (!ProjectWorkspace::WriteJson(scene, document)) {
            if (!workspace.SetWorkEnvironment(previousWork)) TG_LOG_ERROR("作業環境の復元に失敗しました");
            rollback(); return false;
        }
    }
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

fs::path CreateGraphAsset(ProjectWorkspace& workspace, const fs::path& directory, bool cloud) {
    std::error_code error;
    if (!workspace.Contains(directory) || !fs::is_directory(directory, error) || error) return {};
    auto path = workspace.UniquePath(directory, cloud ? "新規雲グラフ" : "新規地形グラフ",
                                     cloud ? ".tgcloud" : ".tgterrain");
    if (path.empty()) return {};
    json body = {{"name", ToUtf8Display(path.stem())},
        {"graph", {{"nodes", json::array({{{"id", 1}, {"kind", cloud ? "cloudOutput" : "output"},
            {"position", {0.0f, 0.0f}}, {"inputs", {2}}, {"outputs", json::array()}}})},
            {"links", json::array()}}},
        {"textures", json::array()}, {"materials", json::array()}, {"models", json::array()},
        {"paintMasks", json::array()}, {"paintResolution", 1024}};
    if (!workspace.SaveAsset(path, cloud ? "cloud-graph" : "terrain-graph", body)) return {};
    return path;
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
