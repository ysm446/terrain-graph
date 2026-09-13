#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>
#include <nlohmann/json.hpp>

namespace tg::io {

// GPUと独立したプロジェクトルート・永続ID・アセットの入出力。
class ProjectWorkspace {
public:
    bool Open(const std::filesystem::path& root);
    bool Scan();
    const std::filesystem::path& Root() const { return m_root; }
    std::filesystem::path StartupScene() const;
    bool SetStartupScene(const std::filesystem::path& scene);
    bool Contains(const std::filesystem::path& path) const;
    std::filesystem::path Import(const std::filesystem::path& source,
                                 const std::filesystem::path& directory);
    std::filesystem::path UniquePath(const std::filesystem::path& directory,
                                     const std::string& name, const char* extension) const;
    std::filesystem::path Resolve(const nlohmann::json& reference) const;
    nlohmann::json Reference(const std::filesystem::path& path);
    bool SaveAsset(std::filesystem::path& path, const char* kind, nlohmann::json& body);
    bool ReadAsset(const std::filesystem::path& path, const char* kind, nlohmann::json& body) const;
    bool SaveScene(const std::filesystem::path& path, nlohmann::json& document);
    bool ReadScene(const std::filesystem::path& path, nlohmann::json& document);
    // 単体アセットを既存の読み込み器が扱う文書へ展開する。
    bool Expand(nlohmann::json& document);
    static bool ReadJson(const std::filesystem::path& path, nlohmann::json& document);
    static bool WriteJson(const std::filesystem::path& path, const nlohmann::json& document);
    static std::string String(const nlohmann::json& value, const char* key);
private:
    std::filesystem::path m_root;
    nlohmann::json m_project;
    std::unordered_map<std::string, std::filesystem::path> m_paths;
    std::unordered_map<std::string, std::filesystem::path> m_imports;
    std::unordered_map<std::string, std::string> m_knownUids;
};

}  // namespace tg::io
