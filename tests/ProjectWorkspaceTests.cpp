#include "io/ProjectWorkspace.h"
#include "core/PathUtf8.h"
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;
using nlohmann::json;
using tg::io::ProjectWorkspace;
int main() {
    // テスト用の独立ルート。実データへ触れない。
    const auto root = fs::current_path() / "workspace-test-data";
    std::error_code error;
    fs::create_directories(root, error);
    ProjectWorkspace workspace;
    int failures = 0;
    const auto check = [&](bool ok, const char* name) {
        if (!ok) { ++failures; std::cerr << "FAIL: " << name << '\n'; }
    };
    check(workspace.Open(root), "open/create project");
    check(!workspace.Contains(root / ".." / "outside"), "root traversal");
    check(!workspace.Contains(root.string() + "-sibling/file"), "root prefix sibling");
    const auto image = workspace.UniquePath(root, "source", ".png");
    std::ofstream(image).put('x');
    const auto source = workspace.Reference(image);
    auto materialPath = workspace.UniquePath(root, "material", ".tgmat");
    json material = {{"name", "material"}, {"roughness", 0.25}, {"maps", {{"baseColor", source}}}};
    check(workspace.SaveAsset(materialPath, "material-asset", material), "save material");
    const auto materialRef = workspace.Reference(materialPath);
    json packed = {{"materials", json::array({{{"id", 7}, {"asset", materialRef}}})}};
    check(workspace.Expand(packed), "expand shared dependency");
    check(packed["textures"].size() == 1 && packed["materials"][0]["maps"]["baseColor"] == 1, "dependency remapping");
    const auto moved = workspace.UniquePath(root / "Moved", "renamed", ".tgmat");
    fs::create_directories(moved.parent_path(), error);
    fs::rename(materialPath, moved, error);
    check(!error && workspace.Scan() && workspace.Resolve(materialRef) == moved, "rename stable ID");
    const auto movedImage = workspace.UniquePath(root / "Moved", "source", ".png");
    fs::rename(image, movedImage, error);
    fs::rename(fs::path(image.wstring() + L".meta"), fs::path(movedImage.wstring() + L".meta"), error);
    check(!error && workspace.Scan() && workspace.Resolve(source) == movedImage, "source move with metadata");
    json changed;
    check(workspace.ReadAsset(moved, "material-asset", changed), "read renamed material");
    changed["roughness"] = 0.75;
    auto target = moved;
    check(workspace.SaveAsset(target, "material-asset", changed), "update shared material");
    json second = {{"materials", json::array({{{"id", 2}, {"asset", materialRef}}})}};
    check(workspace.Expand(second) && second["materials"][0]["roughness"] == 0.75, "other scene sees material edit");
    const auto scene = workspace.UniquePath(root / "Scenes", "scene", ".tgscene");
    json legacy = {{"textures", json::array({{{"id", 1}, {"path", tg::ToUtf8Portable(movedImage)}}})},
                   {"materials", json::array()}, {"models", json::array()}, {"skies", json::array()},
                   {"graph", {{"nodes", json::array()}}}};
    check(workspace.SaveScene(scene, legacy), "save scene manifest");
    json loaded;
    check(workspace.ReadScene(scene, loaded) && loaded["textures"][0]["path"] == tg::ToUtf8Portable(movedImage), "scene roundtrip");
    check(workspace.StartupScene() == scene, "startup scene");
    json broken = {{"materials", json::array({{{"id", 1}, {"asset", {{"uid", "missing"}, {"path", "existing.tgmat"}}}}})}};
    check(!workspace.Expand(broken), "missing ID fails instead of redirecting");
    const auto duplicate = workspace.UniquePath(root, "duplicate", ".tgmat");
    fs::copy_file(moved, duplicate, error);
    check(!workspace.Scan(), "duplicate ID detected");
    fs::remove(duplicate, error);
    check(workspace.Scan(), "recover after duplicate removed");
    json malformed = {{"materials", json::array({42})}};
    check(!workspace.Expand(malformed), "reject malformed asset table");
    fs::path relocated;
    for (unsigned i = 0; ; ++i) {
        relocated = root.parent_path() / ("workspace-relocated-" + std::to_string(i));
        if (!fs::exists(relocated, error)) break;
    }
    fs::rename(root, relocated, error);
    ProjectWorkspace movedWorkspace;
    check(!error && movedWorkspace.Open(relocated), "move entire project root");
    json movedScene;
    check(movedWorkspace.ReadScene(movedWorkspace.StartupScene(), movedScene), "open scene after project move");
    check(movedScene["textures"][0]["path"] == tg::ToUtf8Portable(relocated / movedImage.lexically_relative(root)),
          "source resolves within relocated root");
    std::cout << failures << " failures\n";
    return failures ? 1 : 0;
}
