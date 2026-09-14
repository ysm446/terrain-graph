#include "io/ProjectWorkspace.h"
#include "core/PathUtf8.h"
#include <algorithm>
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
    auto modelPath = workspace.UniquePath(root / "Nested", "catalog-model", ".tgmodel");
    json model = {{"name", "catalog-model"}, {"materials", json::array()}};
    check(workspace.SaveAsset(modelPath, "model-asset", model), "save unloaded model asset");
    check(workspace.Scan(), "scan project model catalog");
    const auto modelPaths = workspace.AssetsWithExtension(L".tgmodel");
    check(std::find(modelPaths.begin(), modelPaths.end(), modelPath) != modelPaths.end(), "catalog includes nested unloaded models");
    check(std::find(modelPaths.begin(), modelPaths.end(), materialPath) == modelPaths.end(), "model catalog excludes materials");
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
    // 同じ中身のアセットは増やさない。中身が一致するファイルを引き当て、そのIDを返す。
    std::string adoptedUid;
    json sameBody = {{"name", "sky"}, {"iblIntensity", 1.5}};
    auto skyPath = workspace.UniquePath(root, "sky", ".tgsky");
    check(workspace.SaveAsset(skyPath, "sky-asset", sameBody), "save sky for adoption");
    const auto savedUid = ProjectWorkspace::String(sameBody, "uid");
    check(workspace.Scan(), "scan after sky save");
    json fresh = {{"name", "sky"}, {"iblIntensity", 1.5}};
    check(workspace.FindIdenticalAsset("sky-asset", fresh, adoptedUid) == skyPath && adoptedUid == savedUid,
          "identical asset is adopted with its ID");
    json renamed = {{"name", "other"}, {"iblIntensity", 1.5}};
    check(workspace.FindIdenticalAsset("sky-asset", renamed, adoptedUid).empty(), "different name is a different asset");
    json edited = {{"name", "sky"}, {"iblIntensity", 2.0}};
    check(workspace.FindIdenticalAsset("sky-asset", edited, adoptedUid).empty(), "edited body is a different asset");
    check(workspace.FindIdenticalAsset("material-asset", fresh, adoptedUid).empty(), "other kind is never adopted");
    std::cout << failures << " failures\n";
    return failures ? 1 : 0;
}
