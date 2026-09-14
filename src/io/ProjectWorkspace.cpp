#include "io/ProjectWorkspace.h"
#include "io/SceneComponents.h"
#include "core/PathUtf8.h"
#include "core/Log.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <objbase.h>
#include <algorithm>
#include <fstream>
#include <functional>
#include <set>

namespace tg::io {
namespace fs = std::filesystem;
using nlohmann::json;
namespace {
std::string NewUid() {
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) return {};
    wchar_t buffer[40]{};
    StringFromGUID2(guid, buffer, 40);
    return ToUtf8Portable(fs::path(buffer));
}
fs::path Absolute(const fs::path& path) {
    std::error_code error;
    const auto result = fs::weakly_canonical(path, error);
    return error ? fs::path{} : result;
}
bool IsNative(const fs::path& path) {
    const auto ext = path.extension();
    return ext == L".tgmat" || ext == L".tgsky" || ext == L".tgmodel" || ext == L".tgterrain" || ext == L".tgcloud";
}
void MapTextures(json& material, const std::function<json(const json&)>& convert) {
    auto maps = material.find("maps");
    if (maps == material.end() || !maps->is_object()) return;
    for (auto& [key, value] : maps->items()) {
        if (key == "baseColor" || key == "normal") value = convert(value);
        else if (value.is_object() && value.contains("texture"))
            value["texture"] = convert(value["texture"]);
    }
}
}

std::string ProjectWorkspace::String(const json& value, const char* key) {
    const auto it = value.find(key);
    return it != value.end() && it->is_string() ? it->get<std::string>() : std::string{};
}
bool ProjectWorkspace::ReadJson(const fs::path& path, json& document) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return false;
    document = json::parse(stream, nullptr, false);
    return document.is_object();
}
bool ProjectWorkspace::WriteJson(const fs::path& path, const json& document) {
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    if (error) return false;
    const fs::path temp = path.wstring() + L".tmp";
    {
        std::ofstream stream(temp, std::ios::binary | std::ios::trunc);
        if (!stream) return false;
        stream << document.dump(2, ' ', false, json::error_handler_t::replace) << '\n';
        stream.flush();
        if (!stream) return false;
        stream.close();
        if (stream.fail()) return false;
    }
    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        TG_LOG_ERROR("保存先を更新できません: %s", ToUtf8Display(path).c_str());
        fs::remove(temp, error);
        return false;
    }
    return true;
}
bool ProjectWorkspace::Contains(const fs::path& path) const {
    if (m_root.empty() || path.empty()) return false;
    const auto target = Absolute(path);
    if (target.empty()) return false;
    auto a = m_root.begin();
    auto b = target.begin();
    for (; a != m_root.end(); ++a, ++b)
        if (b == target.end() || _wcsicmp(a->c_str(), b->c_str()) != 0) return false;
    return true;
}
bool ProjectWorkspace::Open(const fs::path& root) {
    ProjectWorkspace next;
    next.m_root = Absolute(root);
    if (next.m_root.empty()) return false;
    const auto projectPath = next.m_root / L"project.tgproj";
    std::error_code error;
    if (fs::exists(projectPath, error)) {
        if (!ReadJson(projectPath, next.m_project) ||
            String(next.m_project, "format") != "terrain-graph.workspace" ||
            !next.m_project.contains("version") || next.m_project["version"] != 1) {
            TG_LOG_ERROR("ルートのproject.tgprojが対応するプロジェクト形式ではありません");
            return false;
        }
    } else {
        next.m_project = {{"format", "terrain-graph.workspace"}, {"version", 1},
                          {"uid", NewUid()}, {"startupScene", ""}};
        if (!WriteJson(projectPath, next.m_project)) return false;
    }
    if (!next.Scan()) return false;
    *this = std::move(next);
    return true;
}
bool ProjectWorkspace::Scan() {
    m_paths.clear();
    std::error_code error;
    fs::recursive_directory_iterator it(m_root, fs::directory_options::skip_permission_denied, error), end;
    for (; it != end && !error; it.increment(error)) {
        if (it->is_symlink(error)) { it.disable_recursion_pending(); continue; }
        if (it->is_directory(error)) {
            if (it->path().filename().wstring().starts_with(L".")) it.disable_recursion_pending();
            continue;
        }
        const auto path = it->path();
        if (!IsNative(path) && path.extension() != L".meta") continue;
        json body;
        if (!ReadJson(path, body)) continue;
        const auto uid = String(body, "uid");
        if (uid.empty()) continue;
        auto target = path;
        if (path.extension() == L".meta") target.replace_extension();
        if (!m_paths.emplace(uid, target).second) {
            TG_LOG_ERROR("アセットIDが重複しています: %s", ToUtf8Display(path).c_str());
            return false;
        }
        m_knownUids[ToUtf8Portable(Absolute(target))] = uid;
    }
    return !error;
}
fs::path ProjectWorkspace::StartupScene() const {
    const auto text = String(m_project, "startupScene");
    const auto path = m_root / FromUtf8(text);
    return !text.empty() && Contains(path) ? path : fs::path{};
}
bool ProjectWorkspace::SetStartupScene(const fs::path& scene) {
    if (!Contains(scene)) return false;
    auto project = m_project;
    project["startupScene"] = ToUtf8Portable(Absolute(scene).lexically_relative(m_root));
    if (!WriteJson(m_root / L"project.tgproj", project)) return false;
    m_project = std::move(project);
    return true;
}
fs::path ProjectWorkspace::UniquePath(const fs::path& directory, const std::string& name,
                                      const char* extension) const {
    if (!Contains(directory)) return {};
    std::string safe = name.empty() ? "Asset" : name;
    for (char& c : safe) if (static_cast<unsigned char>(c) < 32 || std::string("<>:\"/\\|?*").find(c) != std::string::npos) c = '_';
    // 予約デバイス名や末尾の空白・ピリオドも避ける。
    while (!safe.empty() && (safe.back() == '.' || safe.back() == ' ')) safe.pop_back();
    if (safe.empty()) safe = "Asset";
    std::string upper = safe;
    std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return char(std::toupper(c)); });
    if (upper == "CON" || upper == "PRN" || upper == "AUX" || upper == "NUL" || upper.starts_with("COM") || upper.starts_with("LPT")) safe = "_" + safe;
    for (unsigned i = 0; i < 100000; ++i) {
        const auto path = directory / FromUtf8(safe + (i ? "_" + std::to_string(i) : "") + extension);
        std::error_code error;
        if (!fs::exists(path, error) && !error) return path;
    }
    return {};
}
fs::path ProjectWorkspace::Import(const fs::path& source, const fs::path& directory) {
    if (Contains(source)) return Absolute(source);
    const auto sourcePath = Absolute(source);
    const auto sourceKey = ToUtf8Portable(sourcePath);
    if (const auto found = m_imports.find(sourceKey); found != m_imports.end()) return found->second;
    const auto target = UniquePath(directory, ToUtf8Display(source.stem()), ToUtf8Portable(source.extension()).c_str());
    std::error_code error;
    if (target.empty()) return {};
    fs::create_directories(directory, error);
    if (error || !fs::copy_file(source, target, fs::copy_options::none, error)) {
        TG_LOG_ERROR("素材をコピーできません: %s", ToUtf8Display(source).c_str());
        return {};
    }
    m_imports[sourceKey] = target;
    return target;
}
json ProjectWorkspace::Reference(const fs::path& path) {
    if (!Contains(path)) return nullptr;
    const auto target = Absolute(path);
    if (const auto known = m_knownUids.find(ToUtf8Portable(target)); known != m_knownUids.end()) {
        if (const auto found = m_paths.find(known->second); found != m_paths.end())
            return {{"uid", known->second}, {"path", ToUtf8Portable(found->second.lexically_relative(m_root))}};
    }
    std::error_code error;
    // 移動前の既知のパスは上で追跡する。未知の欠落パスにIDを発行しない。
    if (!fs::is_regular_file(target, error) || error) return nullptr;
    const auto metadata = IsNative(target) ? target : fs::path(target.wstring() + L".meta");
    json body;
    if (fs::exists(metadata, error)) {
        if (!ReadJson(metadata, body)) return nullptr;
    } else body = json::object();
    auto uid = String(body, "uid");
    if (uid.empty()) {
        uid = NewUid();
        if (uid.empty()) return nullptr;
        body["uid"] = uid;
        if (!IsNative(target)) { body["format"] = "terrain-graph.source"; body["version"] = 1; }
        if (!WriteJson(metadata, body)) return nullptr;
    }
    if (const auto it = m_paths.find(uid); it != m_paths.end() && Absolute(it->second) != target) return nullptr;
    m_paths[uid] = target;
    m_knownUids[ToUtf8Portable(target)] = uid;
    return {{"uid", uid}, {"path", ToUtf8Portable(target.lexically_relative(m_root))}};
}
fs::path ProjectWorkspace::Resolve(const json& reference) const {
    const auto uid = String(reference, "uid");
    if (!uid.empty()) {
        const auto found = m_paths.find(uid);
        // IDがある参照は同名の別ファイルへ黙って付け替えない。
        return found == m_paths.end() ? fs::path{} : found->second;
    }
    const auto text = String(reference, "path");
    const auto path = m_root / FromUtf8(text);
    return !text.empty() && Contains(path) ? path : fs::path{};
}
bool ProjectWorkspace::SaveAsset(fs::path& path, const char* kind, json& body) {
    if (!Contains(path)) return false;
    auto uid = String(body, "uid");
    if (!uid.empty()) {
        const auto found = m_paths.find(uid);
        if (found != m_paths.end()) path = found->second;
    } else uid = NewUid();
    if (uid.empty()) return false;
    body["format"] = std::string("terrain-graph.") + kind;
    body["version"] = 1;
    body["uid"] = uid;
    body.erase("id"); body.erase("_assetPath");
    if (!WriteJson(path, body)) return false;
    path = Absolute(path);
    m_paths[uid] = path;
    return true;
}
fs::path ProjectWorkspace::FindIdenticalAsset(const char* kind, const json& body,
                                              std::string& uid) const {
    uid.clear();
    // 比べるのは中身だけ。ID と形式の印は持ち主ごとに違ってよい。
    json wanted = body;
    for (const char* key : {"uid", "format", "version", "id", "_assetPath"}) wanted.erase(key);
    // Scan() が拾った native アセットだけが対象。走査順に依らないよう、パスの小さい方を選ぶ。
    fs::path found;
    for (const auto& [knownUid, path] : m_paths) {
        if (!IsNative(path)) continue;
        json existing;
        if (!ReadAsset(path, kind, existing)) continue;
        for (const char* key : {"uid", "format", "version", "id", "_assetPath"}) existing.erase(key);
        if (existing != wanted) continue;
        if (found.empty() || path < found) { found = path; uid = knownUid; }
    }
    return found;
}
bool ProjectWorkspace::ReadAsset(const fs::path& path, const char* kind, json& body) const {
    return Contains(path) && ReadJson(path, body) &&
           String(body, "format") == std::string("terrain-graph.") + kind &&
           body.contains("version") && body["version"] == 1;
}

bool ProjectWorkspace::SaveScene(const fs::path& path, json& document) {
    if (path.extension() != L".tgscene" || !Contains(path) || !Scan()) return false;
    const auto baseDir = Absolute(path).parent_path();
    std::unordered_map<int, json> textures, materials;
    const auto sourceRef = [&](const json& value) -> json {
        if (!value.is_string() || value.get_ref<const std::string&>().empty()) return nullptr;
        const auto imported = Import(baseDir / FromUtf8(value.get<std::string>()), m_root / L"Imported");
        return imported.empty() ? json() : Reference(imported);
    };
    for (auto& entry : document["textures"]) {
        auto ref = sourceRef(entry["path"]);
        if (ref.is_null()) return false;
        textures[entry["id"].get<int>()] = ref;
        entry["source"] = ref; entry.erase("path");
    }
    const auto save = [&](json& entry, const char* kind, const char* folder, const char* ext) {
        const json id = entry.contains("id") ? entry["id"] : json();
        fs::path assetPath = FromUtf8(String(entry, "_assetPath"));
        if (assetPath.empty()) assetPath = UniquePath(m_root / folder, String(entry, "name"), ext);
        if (!SaveAsset(assetPath, kind, entry)) return false;
        entry = {{"id", id}, {"asset", Reference(assetPath)}};
        return !entry["asset"].is_null();
    };
    for (auto& entry : document["materials"]) {
        const int id = entry["id"].get<int>();
        MapTextures(entry, [&](const json& value) -> json {
            if (!value.is_number_integer()) return nullptr;
            const auto found = textures.find(value.get<int>());
            return found == textures.end() ? json() : found->second;
        });
        if (!save(entry, "material-asset", "Materials", ".tgmat")) return false;
        materials[id] = entry["asset"];
    }
    for (auto& entry : document["models"]) {
        entry["source"] = sourceRef(entry["path"]);
        if (entry["source"].is_null()) return false;
        entry.erase("path");
        for (auto& value : entry["materials"]) {
            if (value.is_number_integer()) {
                const auto found = materials.find(value.get<int>());
                value = found == materials.end() ? json() : found->second;
            }
        }
        if (!save(entry, "model-asset", "Models", ".tgmodel")) return false;
    }
    for (auto& entry : document["skies"]) {
        if (!entry["hdri"].is_null() && entry["hdri"] != "") {
            entry["hdri"] = sourceRef(entry["hdri"]);
            if (entry["hdri"].is_null()) return false;
        }
        if (!save(entry, "sky-asset", "Skies", ".tgsky")) return false;
    }
    // 同じ保存先のIDは維持し、名前を付けて保存では別のIDにする。
    json existing;
    const auto sceneUid = ReadJson(path, existing) ? String(existing, "sceneUid") : "";
    document["sceneUid"] = sceneUid.empty() ? NewUid() : sceneUid;
    if (String(document, "sceneUid").empty()) return false;
    document["format"] = "terrain-graph.scene";
    document["version"] = 1;
    if (document.contains("_components")) {
        if (!SaveSceneComponents(*this, path, document)) return false;
        if (document.value("_componentOnly", -1) >= 0) return true;
        return SetStartupScene(path);
    }
    if (!WriteJson(path, document)) return false;
    return SetStartupScene(path);
}

bool ProjectWorkspace::ReadScene(const fs::path& path, json& document) {
    if (!Contains(path) || !Scan() || !ReadJson(path, document) ||
        String(document, "format") != "terrain-graph.scene" || (document["version"] != 1 && document["version"] != 2)) return false;
    if (document["version"] == 2 && !ExpandSceneComponents(*this, document)) return false;
    return Expand(document);
}
bool ProjectWorkspace::Expand(json& document) {
    for (const char* key : {"textures", "materials", "models", "skies"}) {
        if (!document.contains(key)) document[key] = json::array();
        if (!document[key].is_array()) return false;
        for (const auto& entry : document[key]) if (!entry.is_object()) return false;
    }
    auto& textures = document["textures"];
    auto& materials = document["materials"];
    int nextTexture = 1, nextMaterial = 1;
    std::unordered_map<std::string, int> textureIds, materialIds;
    for (const auto& entry : textures) {
        if (!entry.contains("id") || !entry["id"].is_number_integer()) return false;
        const int id = entry["id"].get<int>();
        nextTexture = std::max(nextTexture, id + 1);
        textureIds[String(entry.value("source", json::object()), "uid")] = id;
    }
    for (const auto& entry : materials) {
        if (!entry.contains("id") || !entry["id"].is_number_integer()) return false;
        const int id = entry["id"].get<int>();
        nextMaterial = std::max(nextMaterial, id + 1);
        materialIds[String(entry.value("asset", json::object()), "uid")] = id;
    }
    const auto textureId = [&](const json& ref) -> json {
        if (ref.is_null()) return nullptr;
        const auto uid = String(ref, "uid");
        if (const auto found = textureIds.find(uid); found != textureIds.end()) return found->second;
        const int id = nextTexture++;
        textureIds[uid] = id;
        textures.push_back({{"id", id}, {"source", ref}});
        return id;
    };
    const auto materialId = [&](const json& ref) -> json {
        if (ref.is_null()) return nullptr;
        const auto uid = String(ref, "uid");
        if (const auto found = materialIds.find(uid); found != materialIds.end()) return found->second;
        const int id = nextMaterial++;
        materialIds[uid] = id;
        materials.push_back({{"id", id}, {"asset", ref}});
        return id;
    };
    const auto read = [&](json& entry, const char* kind) {
        const auto ref = entry.value("asset", json::object());
        const auto assetPath = Resolve(ref);
        json body;
        if (assetPath.empty() || !ReadAsset(assetPath, kind, body)) {
            TG_LOG_ERROR("アセットを読み込めません: %s", String(ref, "path").c_str());
            return false;
        }
        body["id"] = entry.value("id", json());
        body["_assetPath"] = ToUtf8Portable(assetPath);
        entry = std::move(body);
        return true;
    };
    for (auto& entry : document["models"]) {
        if (!read(entry, "model-asset")) return false;
        const auto source = Resolve(entry["source"]);
        if (source.empty()) return false;
        entry["path"] = ToUtf8Portable(source);
        if (!entry["materials"].is_array()) return false;
        for (auto& slot : entry["materials"]) slot = materialId(slot);
    }
    for (auto& entry : materials) {
        if (!read(entry, "material-asset")) return false;
        MapTextures(entry, textureId);
    }
    for (auto& entry : textures) {
        const auto source = Resolve(entry["source"]);
        if (source.empty()) return false;
        entry["path"] = ToUtf8Portable(source);
    }
    for (auto& entry : document["skies"]) {
        if (!read(entry, "sky-asset")) return false;
        if (entry["hdri"].is_object()) {
            const auto source = Resolve(entry["hdri"]);
            if (source.empty()) return false;
            entry["hdri"] = ToUtf8Portable(source);
        }
    }
    document["format"] = "terrain-graph.project";
    document["version"] = 4;
    return true;
}
}  // namespace tg::io
