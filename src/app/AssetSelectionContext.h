#pragma once
#include "io/AssetRelations.h"
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace tg {
// 選択欄はファイルを列挙するだけ。読み込み結果は次フレームの同じ欄へ返す。
// 一時コピーや vector 内のスロットへのポインタは保持しない。
struct AssetSelectionContext {
    struct Request {
        std::filesystem::path path;
        uint32_t widget = 0, previous = 0, result = 0;
        uint64_t owner = 0;
        bool ready = false;
    } request;
    std::filesystem::path root;
    std::vector<std::filesystem::path> candidates;
    uint64_t owner = 0;
    void Scan() {
        candidates.clear();
        std::error_code error;
        namespace fs = std::filesystem;
        fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, error), end;
        for (; it != end && !error; it.increment(error)) {
            if (it->is_symlink(error)) { it.disable_recursion_pending(); continue; }
            if (it->is_directory(error)) {
                if (it->path().filename().wstring().starts_with(L".")) it.disable_recursion_pending();
                continue;
            }
            if (!it->is_regular_file(error) || it->path().filename().wstring().starts_with(L".")) continue;
            const auto kind = io::KindOfAsset(it->path());
            if (kind == io::AssetKind::Image || kind == io::AssetKind::Material || kind == io::AssetKind::LayerMaterial)
                candidates.push_back(it->path());
        }
        std::sort(candidates.begin(), candidates.end());
    }
    void Queue(const std::filesystem::path& path, uint32_t widget, uint32_t previous) {
        request = {path, widget, previous, 0, owner, false};
    }
    bool Consume(uint32_t widget, uint32_t& value) {
        if (!request.ready || request.widget != widget || request.owner != owner || request.previous != value) return false;
        const auto result = request.result; request = {};
        if (!result) return false;
        value = result; return true;
    }
};
inline AssetSelectionContext* g_assetSelectionContext = nullptr;
struct AssetSelectionFrame {
    explicit AssetSelectionFrame(AssetSelectionContext& context) { g_assetSelectionContext = &context; }
    ~AssetSelectionFrame() {
        if (g_assetSelectionContext->request.ready) g_assetSelectionContext->request = {};
        g_assetSelectionContext = nullptr;
    }
};
} // namespace tg
