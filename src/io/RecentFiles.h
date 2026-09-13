#pragma once
#include <filesystem>
#include <vector>

namespace tg::io {
// アプリ側に保存するルート履歴と、ルートごとのシーン履歴。
class RecentFiles {
public:
    static constexpr size_t kMaxEntries = 10;
    struct RootEntry {
        std::filesystem::path path;
        std::vector<std::filesystem::path> scenes;
    };
    void Load(const std::filesystem::path& storage = {});
    void AddRoot(const std::filesystem::path& root);
    void Add(const std::filesystem::path& root, const std::filesystem::path& scene);
    void Remove(const std::filesystem::path& root, const std::filesystem::path& scene);
    void Clear(const std::filesystem::path& root);
    void ClearRoots();
    const std::vector<RootEntry>& Roots() const { return m_roots; }
    const std::vector<std::filesystem::path>& Entries(const std::filesystem::path& root) const;
private:
    void Save() const;
    std::filesystem::path m_storage;
    std::vector<RootEntry> m_roots;
    std::vector<std::filesystem::path> m_legacy;
};
}  // namespace tg::io
