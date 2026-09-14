#include "io/SceneComponents.h"
#include "io/AssetRelations.h"
#include <fstream>
#include <iostream>

int main() {
    namespace fs = std::filesystem;
    using nlohmann::json;
    using namespace tg::io;
    ProjectWorkspace workspace;
    const auto root = fs::current_path() / "component-test-data";
    int failures = 0;
    const auto check = [&](bool ok, const char* name) { if (!ok) { ++failures; std::cerr << name << '\n'; } };
    check(workspace.Open(root), "open root");
    const auto scene = workspace.UniquePath(root, "original", ".tgscene");
    json graph = {{"nodes", json::array({
        {{"id", 1}, {"kind", "heightmap"}, {"inputs", json::array()}, {"outputs", {2}}},
        {{"id", 3}, {"kind", "output"}, {"inputs", {4}}, {"outputs", json::array()}},
        {{"id", 6}, {"kind", "maskNoise"}, {"inputs", json::array()}, {"outputs", {7}}},
        {{"id", 8}, {"kind", "cloudWeatherLayer"}, {"inputs", {9}}, {"outputs", {10}}}
    })}, {"links", json::array({{{"id", 5}, {"start", 2}, {"end", 4}}, {{"id", 11}, {"start", 7}, {"end", 9}}})}};
    json original = {{"format", "terrain-graph.scene"}, {"version", 1}, {"graph", graph},
        {"textures", json::array()}, {"materials", json::array()}, {"models", json::array()}, {"skies", json::array()},
        {"paintMasks", json::array()}, {"preview", {{"exposure", 2.5}}}};
    const auto image = workspace.UniquePath(root, "height", ".png");
    std::ofstream(image).put('i');
    original["textures"].push_back({{"id", 22}, {"source", workspace.Reference(image)}});
    original["graph"]["nodes"][0]["layer"]["height"]["texture"] = {{"texture", 22}};
    original["graph"]["nodes"][2]["map"] = {{"texture", 22}};
    const auto paintDirectory = scene.parent_path() / (scene.stem().wstring() + L".assets");
    std::error_code paintError;
    fs::create_directories(paintDirectory, paintError);
    std::ofstream(paintDirectory / "paint.png").put('p');
    original["paintMasks"].push_back({{"id", 13}, {"file", "paint.png"}, {"resolution", 1}});
    original["graph"]["nodes"][0]["layer"]["mask"]["paint"] = 13;
    check(ProjectWorkspace::WriteJson(scene, original), "write original");
    const auto migrated = MigrateSceneComponents(workspace, scene);
    check(!migrated.empty() && migrated != scene, "migrate to new file");
    json unchanged, packed, expanded;
    check(ProjectWorkspace::ReadJson(scene, unchanged) && unchanged == original, "original unchanged");
    check(ProjectWorkspace::ReadJson(migrated, packed) && packed["version"] == 2 &&
          !packed.contains("graph") && packed["components"].size() == 2, "scene references two components");
    check(workspace.ReadScene(migrated, expanded) && expanded["graph"]["nodes"].size() == 4 &&
          expanded["graph"]["links"].size() == 2 && expanded["preview"] == original["preview"], "expand without loss");
    check(expanded["graph"]["nodes"][2]["component"] == 1, "cloud mask belongs to cloud graph");
    check(expanded["textures"].size() == 1 && expanded["graph"]["nodes"][0]["layer"]["height"]["texture"]["texture"] == 1 &&
          expanded["graph"]["nodes"][2]["map"]["texture"] == 1, "shared texture deduplicated and remapped");
    check(fs::exists(paintDirectory / "paint.png") && expanded["paintMasks"][0]["file"] != "paint.png", "paint copied without changing original");
    for (int i = 0; i < 3; ++i) {
        auto rewritten = expanded;
        rewritten["format"] = "terrain-graph.scene";
        check(SaveSceneComponents(workspace, migrated, rewritten) && workspace.ReadScene(migrated, expanded), "repeat component save and reopen");
        check(expanded["textures"].size() == 1 && expanded["paintMasks"].size() == 1 && expanded["graph"]["nodes"].size() == 4, "repeat save does not multiply dependencies");
    }
    ProjectWorkspace::ReadJson(migrated, packed);
    const auto originalTerrain = workspace.Resolve(packed["components"][0]["asset"]);
    json beforeFailure, afterFailure;
    ProjectWorkspace::ReadJson(originalTerrain, beforeFailure);
    auto failedSave = expanded; failedSave["format"] = "terrain-graph.scene";
    failedSave["graph"]["nodes"][0]["extra"] = "must roll back";
    const auto blockedPath = root / "blocked.tgscene";
    fs::create_directory(blockedPath, paintError);
    check(!SaveSceneComponents(workspace, blockedPath, failedSave), "scene write failure reported");
    check(ProjectWorkspace::ReadJson(originalTerrain, afterFailure) && beforeFailure == afterFailure, "component update rolled back on scene failure");
    // 不正な接続や地形から雲への依存を勝手に切らない。
    auto crossing = graph;
    crossing["links"].push_back({{"id", 12}, {"start", 2}, {"end", 9}});
    check(!AssignGraphComponents(crossing), "cross-component dependency refused");
    auto broken = graph; broken["links"][0]["start"] = 999;
    check(!AssignGraphComponents(broken), "dangling link refused");
    const auto terrainRef = packed["components"][0]["asset"];
    const auto terrain = workspace.Resolve(terrainRef);
    const auto folder = root / "Moved";
    std::error_code error; fs::create_directories(folder, error);
    const auto moved = MoveAsset(workspace, terrain, folder);
    check(!moved.empty() && workspace.ReadScene(migrated, expanded), "component move keeps scene reference");
    ProjectWorkspace reopened;
    check(reopened.Open(root) && reopened.ReadScene(migrated, expanded), "reopen migrated scene");
    // 名前が同じでも別の移行先へ書く。
    const auto second = MigrateSceneComponents(workspace, scene);
    check(!second.empty() && second != migrated, "repeat migration never overwrites");
    const auto backup = moved.wstring() + L".saved";
    fs::rename(moved, backup, error);
    check(!workspace.ReadScene(migrated, expanded), "missing component prevents scene load");
    fs::rename(backup, moved, error);
    std::cout << failures << " failures\n";
    return failures ? 1 : 0;
}
