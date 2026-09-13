#include "io/RecentFiles.h"
#include "io/ThumbnailStore.h"
#include "core/PathUtf8.h"
#include <chrono>
#include <fstream>
#include <iostream>
namespace fs = std::filesystem;
namespace tg::io { fs::path AppDataDirectory() { return {}; } }
int main() {
    const auto directory = fs::current_path() / ("history-thumbnail-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code error;
    fs::create_directories(directory, error);
    int failures = 0;
    const auto check = [&](bool value, const char* name) { if (!value) { ++failures; std::cerr << "FAIL: " << name << '\n'; } };
    tg::io::RecentFiles history;
    const auto storage = directory / "recent.json", a = directory / "A", b = directory / "B";
    history.Load(storage);
    history.Add(a, a / "same.tgscene"); history.Add(b, b / "same.tgscene");
    check(history.Entries(a).size() == 1 && history.Entries(b).size() == 1, "root isolation");
    history.Add(a, a / "SAME.tgscene");
    check(history.Entries(a).size() == 1 && history.Roots().front().path == a, "case insensitive dedup and root order");
    for (int i = 0; i < 12; ++i) history.Add(a, a / (std::to_string(i) + ".tgscene"));
    check(history.Entries(a).size() == 10 && history.Entries(a).front().filename() == "11.tgscene", "scene limit and order");
    tg::io::RecentFiles loaded; loaded.Load(storage);
    check(loaded.Entries(a) == history.Entries(a) && loaded.Entries(b).size() == 1, "persistence");
    loaded.Clear(a); check(loaded.Entries(a).empty() && loaded.Entries(b).size() == 1, "scoped clear");
    for (int i = 0; i < 12; ++i) loaded.AddRoot(directory / std::to_string(i));
    check(loaded.Roots().size() == 10, "root limit");
    tg::io::ProjectWorkspace::WriteJson(storage, {{"format", "terrain-graph.recent"}, {"version", 1},
        {"projects", {tg::ToUtf8Portable(a / "legacy.tgscene"), tg::ToUtf8Portable(b / "legacy.tgscene")}}});
    loaded.Load(storage); loaded.AddRoot(a);
    check(loaded.Entries(a).size() == 1 && loaded.Entries(b).empty(), "legacy migration scope");
    loaded.Load(storage); loaded.AddRoot(b);
    check(loaded.Entries(a).size() == 1 && loaded.Entries(b).size() == 1, "unassigned legacy retained");
    loaded.ClearRoots(); loaded.Load(storage); check(loaded.Roots().empty(), "clear all persists");
    tg::io::ProjectWorkspace workspace;
    check(workspace.Open(a), "workspace open");
    tg::io::ProjectWorkspace::WriteJson(storage, {{"format", "terrain-graph.recent"},
        {"projects", {tg::ToUtf8Portable(a / "nested.tgscene")}}});
    loaded.Load(storage); loaded.AddRoot(directory);
    check(loaded.Entries(directory).empty(), "nested workspace is not assigned to parent");
    loaded.AddRoot(a); check(loaded.Entries(a).size() == 1, "nested workspace migration");
    const auto image = a / "source.png";
    std::ofstream(image).put('a');
    const auto source = workspace.Reference(image);
    auto material = a / "material.tgmat";
    nlohmann::json materialBody = {{"maps", {{"baseColor", source}}}};
    check(workspace.SaveAsset(material, "material-asset", materialBody), "material fixture");
    const auto original = tg::io::AssetThumbnailRecord(workspace, material);
    fs::create_directories(original.image.parent_path(), error); std::ofstream(original.image).put('x');
    check(tg::io::CommitThumbnail(original) && tg::io::ThumbnailIsCurrent(original), "persistent cache record");
    std::ofstream(a / "unrelated.png").put('z');
    check(tg::io::ThumbnailIsCurrent(tg::io::AssetThumbnailRecord(workspace, material)), "unrelated file preserves cache");
    std::ofstream(image, std::ios::app).put('b');
    const auto changed = tg::io::AssetThumbnailRecord(workspace, material);
    check(original.image == changed.image && !tg::io::ThumbnailIsCurrent(changed), "dependency change invalidates same cache slot");
    fs::remove(image, error);
    check(changed.stamp != tg::io::AssetThumbnailRecord(workspace, material).stamp, "missing dependency invalidates");
    check(tg::io::SceneThumbnailPath(a / "scene.tgscene") == a / "scene.assets" / "thumbnail.png", "scene sidecar");
    std::cout << failures << " failures\n";
    return failures ? 1 : 0;
}
