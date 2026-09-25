#include "io/RecentFiles.h"
#include "core/PathUtf8.h"
#include "core/Log.h"
#include "io/AppSettings.h"
#include <nlohmann/json.hpp>
#include <Windows.h>
#include <algorithm>
#include <fstream>

namespace tg::io {
namespace fs = std::filesystem;
using nlohmann::json;
namespace {
fs::path Normalize(const fs::path& path) {
    std::error_code error;
    auto normalized = fs::weakly_canonical(path, error);
    return error ? path.lexically_normal() : normalized;
}
bool SamePath(const fs::path& a, const fs::path& b) {
    return _wcsicmp(Normalize(a).c_str(), Normalize(b).c_str()) == 0;
}
void Insert(std::vector<fs::path>& paths, const fs::path& path) {
    std::erase_if(paths, [&](const auto& existing) { return SamePath(existing, path); });
    paths.insert(paths.begin(), Normalize(path));
    if (paths.size() > RecentFiles::kMaxEntries) paths.resize(RecentFiles::kMaxEntries);
}
void ReadPaths(const json& values, std::vector<fs::path>& paths) {
    if (!values.is_array()) return;
    for (auto it = values.rbegin(); it != values.rend(); ++it)
        if (it->is_string() && !it->get_ref<const std::string&>().empty()) Insert(paths, FromUtf8(it->get<std::string>()));
}
json WritePaths(const std::vector<fs::path>& paths) {
    auto values = json::array();
    for (const auto& path : paths) values.push_back(ToUtf8Portable(path));
    return values;
}
}
void RecentFiles::Load(const fs::path& storage) {
    m_storage = storage.empty() ? AppDataDirectory() / L"recent.json" : storage;
    m_roots.clear(); m_legacy.clear();
    std::ifstream stream(m_storage, std::ios::binary);
    if (!stream) return;
    const auto document = json::parse(stream, nullptr, false);
    if (!document.is_object() || document.value("format", json()) != "terrain-graph.recent") return;
    ReadPaths(document.value("projects", json()), m_legacy);
    const auto roots = document.value("roots", json());
    if (!roots.is_array()) return;
    for (const auto& entry : roots) {
        if (!entry.is_object() || !entry.value("path", json()).is_string()) continue;
        const auto path = FromUtf8(entry["path"].get<std::string>());
        if (path.empty() || m_roots.size() >= kMaxEntries ||
            std::any_of(m_roots.begin(), m_roots.end(), [&](const auto& r) { return SamePath(r.path, path); })) continue;
        RootEntry root{Normalize(path), {}};
        ReadPaths(entry.value("scenes", json()), root.scenes);
        m_roots.push_back(std::move(root));
    }
}
void RecentFiles::InsertRoot(const fs::path& root) {
    RootEntry entry{Normalize(root), {}};
    const auto found = std::find_if(m_roots.begin(), m_roots.end(), [&](const auto& r) { return SamePath(r.path, root); });
    if (found != m_roots.end()) { entry = *found; m_roots.erase(found); }
    // 旧形式の履歴は所属ルートを開いたときに移行する。
    for (auto it = m_legacy.rbegin(); it != m_legacy.rend(); ++it) {
        auto parent = it->parent_path();
        while (!parent.empty()) {
            if (SamePath(parent, root)) { Insert(entry.scenes, *it); break; }
            // 入れ子の別プロジェクトに属する履歴を、親ルートへ取り込まない。
            std::ifstream marker(parent / L"project.tgproj", std::ios::binary);
            if (marker) {
                const auto project = json::parse(marker, nullptr, false);
                if (project.is_object() && project.value("format", json()) == "terrain-graph.workspace") break;
            }
            const auto next = parent.parent_path();
            if (next == parent) break;
            parent = next;
        }
    }
    std::erase_if(m_legacy, [&](const auto& scene) {
        return std::any_of(entry.scenes.begin(), entry.scenes.end(), [&](const auto& p) { return SamePath(p, scene); });
    });
    m_roots.insert(m_roots.begin(), std::move(entry));
    if (m_roots.size() > kMaxEntries) m_roots.resize(kMaxEntries);
}
void RecentFiles::AddRoot(const fs::path& root) {
    if (root.empty()) return;
    InsertRoot(root);
    Save();
}
void RecentFiles::Add(const fs::path& root, const fs::path& scene) {
    if (root.empty() || scene.empty()) return;
    InsertRoot(root); Insert(m_roots.front().scenes, scene); Save();
}
const std::vector<fs::path>& RecentFiles::Entries(const fs::path& root) const {
    static const std::vector<fs::path> empty;
    for (const auto& entry : m_roots) if (SamePath(entry.path, root)) return entry.scenes;
    return empty;
}
void RecentFiles::Remove(const fs::path& root, const fs::path& scene) {
    for (auto& entry : m_roots) if (SamePath(entry.path, root))
        std::erase_if(entry.scenes, [&](const auto& p) { return SamePath(p, scene); });
    Save();
}
void RecentFiles::Clear(const fs::path& root) {
    for (auto& entry : m_roots) if (SamePath(entry.path, root)) entry.scenes.clear();
    Save();
}
void RecentFiles::ClearRoots() { m_roots.clear(); m_legacy.clear(); Save(); }
void RecentFiles::Save() const {
    if (m_storage.empty()) return;
    std::error_code error;
    fs::create_directories(m_storage.parent_path(), error);
    json roots = json::array();
    for (const auto& entry : m_roots) roots.push_back({{"path", ToUtf8Portable(entry.path)}, {"scenes", WritePaths(entry.scenes)}});
    const json document = {{"format", "terrain-graph.recent"}, {"version", 2}, {"roots", roots}, {"projects", WritePaths(m_legacy)}};
    const fs::path temporary = m_storage.wstring() + L".tmp";
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    stream << document.dump(2, ' ', false, json::error_handler_t::replace) << '\n';
    stream.close();
    if (!stream || !MoveFileExW(temporary.c_str(), m_storage.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        TG_LOG_WARN("最近使ったルート・シーンの履歴を保存できませんでした");
}
}  // namespace tg::io
