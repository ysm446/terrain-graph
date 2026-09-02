#include "io/RecentFiles.h"

#include "core/PathUtf8.h"

#include "core/Log.h"
#include "io/AppSettings.h"

#include <nlohmann/json.hpp>

#include <Windows.h>

#include <fstream>
#include <string>
#include <system_error>

namespace tg::io {
namespace {

namespace fs = std::filesystem;
using nlohmann::json;

constexpr const char* kFormat = "terrain-graph.recent";
constexpr int kVersion = 1;

// 履歴の置き場所。設定と同じフォルダへ置く。
fs::path HistoryPath() {
    return AppDataDirectory() / L"recent.json";
}

// 同じファイルを別の書き方で指していても 1 つに寄せる。
bool SamePath(const fs::path& a, const fs::path& b) {
    std::error_code errorA;
    std::error_code errorB;
    const fs::path normalizedA = fs::weakly_canonical(a, errorA);
    const fs::path normalizedB = fs::weakly_canonical(b, errorB);
    const fs::path& keyA = errorA ? a : normalizedA;
    const fs::path& keyB = errorB ? b : normalizedB;
    return _wcsicmp(keyA.c_str(), keyB.c_str()) == 0;
}

}  // namespace

void RecentFiles::Load() {
    m_entries.clear();

    std::ifstream stream(HistoryPath(), std::ios::binary);
    if (!stream.is_open()) {
        return;  // まだ履歴が無いだけ。エラーにしない。
    }

    // 例外は使わない方針なので、パース失敗は discarded で受ける。
    const json document = json::parse(stream, nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
        TG_LOG_WARN("最近使ったプロジェクトの履歴を読めませんでした");
        return;
    }
    const auto format = document.find("format");
    if (format == document.end() || !format->is_string() ||
        format->get<std::string>() != kFormat) {
        return;
    }

    const auto projects = document.find("projects");
    if (projects == document.end() || !projects->is_array()) {
        return;
    }
    for (const json& entry : *projects) {
        if (!entry.is_string()) {
            continue;
        }
        const fs::path path = FromUtf8(entry.get<std::string>());
        if (path.empty() || m_entries.size() >= kMaxEntries) {
            continue;
        }
        m_entries.push_back(path);
    }
}

void RecentFiles::Add(const fs::path& path) {
    if (path.empty()) {
        return;
    }

    // 絶対パスで持つ。作業ディレクトリが変わっても指し先が変わらないようにする。
    std::error_code error;
    const fs::path absolute = fs::absolute(path, error);
    const fs::path entry = error ? path : absolute;

    std::erase_if(m_entries, [&entry](const fs::path& existing) {
        return SamePath(existing, entry);
    });
    m_entries.insert(m_entries.begin(), entry);
    if (m_entries.size() > kMaxEntries) {
        m_entries.resize(kMaxEntries);
    }
    Save();
}

void RecentFiles::Remove(const fs::path& path) {
    const size_t before = m_entries.size();
    std::erase_if(m_entries, [&path](const fs::path& existing) {
        return SamePath(existing, path);
    });
    if (m_entries.size() != before) {
        Save();
    }
}

void RecentFiles::Clear() {
    m_entries.clear();
    Save();
}

bool RecentFiles::Save() const {
    const fs::path path = HistoryPath();
    std::error_code error;
    if (const fs::path parent = path.parent_path(); !parent.empty()) {
        fs::create_directories(parent, error);
    }

    json document;
    document["format"] = kFormat;
    document["version"] = kVersion;
    json projects = json::array();
    for (const fs::path& entry : m_entries) {
        projects.push_back(ToUtf8Portable(entry));
    }
    document["projects"] = std::move(projects);

    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream.is_open()) {
        TG_LOG_WARN("最近使ったプロジェクトの履歴を保存できませんでした");
        return false;
    }
    // 壊れた文字列が混ざっていても例外を出さない（不正な UTF-8 は置換文字にする）。
    stream << document.dump(2, ' ', false, json::error_handler_t::replace) << '\n';
    return stream.good();
}

}  // namespace tg::io
