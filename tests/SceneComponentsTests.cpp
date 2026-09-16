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
    // 新規グラフは独立IDと有効な出力ノードを持ち、同名でも上書きしない。
    for (const bool cloud : {false, true}) {
        const auto asset = CreateGraphAsset(workspace, root, cloud);
        const auto another = CreateGraphAsset(workspace, root, cloud);
        json body, other;
        check(!asset.empty() && asset != another &&
              workspace.ReadAsset(asset, cloud ? "cloud-graph" : "terrain-graph", body) &&
              ProjectWorkspace::ReadJson(another, other), "create independent graph assets");
        check(body["uid"].is_string() && body["uid"] != other["uid"], "new graphs have distinct IDs");
        check(body["graph"]["nodes"].size() == 1 && body["graph"]["links"].empty() &&
              body["graph"]["nodes"][0]["kind"] == (cloud ? "cloudOutput" : "output"), "new graph output node");
        json document = {{"components", json::array({{{"role", cloud ? "cloud" : "terrain"},
            {"asset", workspace.Reference(asset)}}})}};
        check(ExpandSceneComponents(workspace, document) && document["graph"]["nodes"].size() == 1,
              "new graph expands as scene component");
    }
    check(CreateGraphAsset(workspace, root.parent_path(), false).empty(), "create outside root refused");
    std::error_code folderError;
    const auto emptyFolder = workspace.UniquePath(root, "empty-folder", "");
    fs::create_directory(emptyFolder, folderError);
    check(RemoveEmptyAssetFolder(workspace, emptyFolder) && !fs::exists(emptyFolder), "delete empty folder");
    const auto occupiedFolder = workspace.UniquePath(root, "occupied-folder", "");
    fs::create_directory(occupiedFolder, folderError);
    const auto hiddenFile = occupiedFolder / ".hidden.meta";
    std::ofstream(hiddenFile).put('x');
    check(!RemoveEmptyAssetFolder(workspace, occupiedFolder) && fs::exists(hiddenFile), "hidden file prevents folder deletion");
    check(!RemoveEmptyAssetFolder(workspace, hiddenFile), "regular file cannot be deleted as folder");
    const auto parentFolder = workspace.UniquePath(root, "parent-folder", "");
    fs::create_directories(parentFolder / "child", folderError);
    check(!RemoveEmptyAssetFolder(workspace, parentFolder), "child folder prevents deletion");
    check(!RemoveEmptyAssetFolder(workspace, root) && !RemoveEmptyAssetFolder(workspace, root.parent_path()),
          "root and outside folder protected");
    const auto protectedFolder = root / ".terrain-graph" / "protected-empty";
    fs::create_directories(protectedFolder, folderError);
    check(!RemoveEmptyAssetFolder(workspace, protectedFolder), "internal folder protected");
    const auto scene = workspace.UniquePath(root, "original", ".tgscene");
    json graph = {{"nodes", json::array({
        {{"id", 1}, {"kind", "heightmap"}, {"inputs", json::array()}, {"outputs", {2}}},
        {{"id", 3}, {"kind", "output"}, {"inputs", {4}}, {"outputs", json::array()}},
        {{"id", 6}, {"kind", "maskNoise"}, {"inputs", json::array()}, {"outputs", {7}}},
        {{"id", 8}, {"kind", "cloudWeatherLayer"}, {"inputs", {9}}, {"outputs", {10}}}
    })}, {"links", json::array({{{"id", 5}, {"start", 2}, {"end", 4}}, {{"id", 11}, {"start", 7}, {"end", 9}}})}};
    json original = {{"format", "terrain-graph.scene"}, {"version", 1}, {"graph", graph},
        {"textures", json::array()}, {"materials", json::array()}, {"models", json::array()}, {"skies", json::array()},
        {"paintMasks", json::array()}, {"preview", {{"exposure", 2.5}, {"lightingMode", "ibl"}, {"light", {{"azimuth", 0.4}}},
            {"atmosphere", {{"azimuth", 1.2}, {"elevation", 0.3}, {"illuminance", 85000}, {"mie", 0.7}, {"coverage", 0.4}}}}}};
    const json night = {{"nightEnabled", true}, {"moonAzimuth", -1.1}, {"moonElevation", 0.7},
        {"moonIlluminance", 0.3}, {"moonPhase", 0.5}, {"starIntensity", 2.0}, {"starRotation", 1.0}, {"starLatitude", 0.5}};
    original["preview"]["atmosphere"].update(night);
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
    check(ProjectWorkspace::ReadJson(migrated, packed) && packed["version"] == 3 &&
          !packed.contains("graph") && packed["components"].size() == 2, "scene references two components");
    check(workspace.ReadScene(migrated, expanded) && expanded["graph"]["nodes"].size() == 4 &&
          expanded["graph"]["links"].size() == 2 && expanded["preview"]["exposure"] == original["preview"]["exposure"] &&
          expanded["preview"]["atmosphere"] == original["preview"]["atmosphere"], "expand without loss");
    json skyBody;
    const auto skyPath = workspace.Resolve(packed["atmosphere"]);
    check(!skyPath.empty() && workspace.ReadAsset(skyPath, "atmosphere-sky", skyBody), "scene references independent atmosphere asset");
    for(const auto& [key,value] : night.items())
        check(skyBody["settings"].contains(key) && skyBody["settings"][key]==value,
              "night settings belong to the atmosphere asset and survive scene expansion");
    check(skyBody["settings"]["azimuth"] == 1.2 && skyBody["settings"]["illuminance"] == 85000 &&
          !skyBody["settings"].contains("coverage") && !skyBody["settings"].contains("exposure"), "sky owns sun but not clouds or exposure");
    check(!packed.contains("skies") && !packed["preview"].contains("lightingMode") && !packed["preview"].contains("light"), "work environment removed from scene");
    check(workspace.WorkEnvironment()["lightingMode"] == "ibl" && workspace.WorkEnvironment()["light"]["azimuth"] == 0.4,
          "legacy working lighting preserved in project");
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
    // 変更の無い部品は書き直さない。ファイルの中身とバックアップの数が変わらず、参照は残る。
    {
        ProjectWorkspace::ReadJson(migrated, packed);
        const auto terrainPath = workspace.Resolve(packed["components"][0]["asset"]);
        const auto cloudPath = workspace.Resolve(packed["components"][1]["asset"]);
        const auto atmospherePath = workspace.Resolve(packed["atmosphere"]);
        const auto backups = root / ".terrain-graph/component-backups";
        const auto countBackups = [&]() {
            size_t count = 0;
            std::error_code error;
            for (const auto& entry : fs::directory_iterator(backups, error)) if (entry.is_regular_file()) ++count;
            return count;
        };
        json terrainBefore, cloudBefore, skyBefore;
        ProjectWorkspace::ReadJson(terrainPath, terrainBefore);
        ProjectWorkspace::ReadJson(cloudPath, cloudBefore);
        ProjectWorkspace::ReadJson(atmospherePath, skyBefore);
        const auto backupsBefore = countBackups();
        // 保存は渡した文書を参照形式へ書き換えるので、毎回展開済みの文書から作り直す。
        const auto edited = [&](int write) {
            auto document = expanded;
            document["format"] = "terrain-graph.scene";
            document["_componentWrite"] = write;
            document["preview"]["exposure"] = 3.5;
            document["graph"]["nodes"][0]["extra"] = "not written";
            document["preview"]["atmosphere"]["azimuth"] = 9.0;
            return document;
        };
        auto partial = edited(0);
        json packedPartial, terrainAfter, cloudAfter, skyAfter;
        check(SaveSceneComponents(workspace, migrated, partial) && ProjectWorkspace::ReadJson(migrated, packedPartial),
              "scene-only save succeeds");
        check(!packedPartial.contains("_componentWrite") && packedPartial["preview"]["exposure"] == 3.5 &&
              packedPartial["components"].size() == 2 && packedPartial["atmosphere"] == packed["atmosphere"] &&
              packedPartial["components"][0]["asset"] == packed["components"][0]["asset"], "scene-only save keeps component references");
        check(ProjectWorkspace::ReadJson(terrainPath, terrainAfter) && terrainAfter == terrainBefore &&
              ProjectWorkspace::ReadJson(cloudPath, cloudAfter) && cloudAfter == cloudBefore &&
              ProjectWorkspace::ReadJson(atmospherePath, skyAfter) && skyAfter == skyBefore, "scene-only save leaves components untouched");
        check(countBackups() == backupsBefore, "scene-only save makes no component backups");
        // 地形だけを書き直す指定では、地形だけが変わる。
        auto terrainOnly = edited(1);
        check(SaveSceneComponents(workspace, migrated, terrainOnly) && ProjectWorkspace::ReadJson(terrainPath, terrainAfter) &&
              terrainAfter["graph"]["nodes"][0]["extra"] == "not written" && ProjectWorkspace::ReadJson(cloudPath, cloudAfter) &&
              cloudAfter == cloudBefore && ProjectWorkspace::ReadJson(atmospherePath, skyAfter) && skyAfter == skyBefore,
              "write mask rewrites only the terrain");
        json packedMasked;
        check(ProjectWorkspace::ReadJson(migrated, packedMasked) && packedMasked["components"].size() == 2 &&
              packedMasked["components"][0]["role"] == "terrain" && packedMasked["components"][1]["role"] == "cloud",
              "partial writes keep the component order");
        // 大気散乱スカイだけの保存。シーン本体と地形・雲は触らない。
        json sceneBefore; ProjectWorkspace::ReadJson(migrated, sceneBefore);
        ProjectWorkspace::ReadJson(terrainPath, terrainBefore);
        auto restoreSky = expanded;
        restoreSky["format"] = "terrain-graph.scene";
        restoreSky["_componentOnly"] = 2;
        auto skyOnly = restoreSky;
        skyOnly["preview"]["atmosphere"]["azimuth"] = 2.5;
        skyOnly["preview"]["exposure"] = 7.0;
        json sceneAfter;
        check(SaveSceneComponents(workspace, migrated, skyOnly) && ProjectWorkspace::ReadJson(atmospherePath, skyAfter) &&
              skyAfter["settings"]["azimuth"] == 2.5 && skyAfter["uid"] == skyBefore["uid"], "atmosphere-only save updates the sky asset");
        check(ProjectWorkspace::ReadJson(terrainPath, terrainAfter) && terrainAfter == terrainBefore, "atmosphere-only save leaves graphs untouched");
        // 呼び出し側（SaveScene）が部品だけの保存ではシーン本体を書かない。ここでは書き込みの対象を確認する。
        check(ProjectWorkspace::ReadJson(migrated, sceneAfter) && sceneAfter == sceneBefore, "atmosphere-only save does not touch the scene file in this call");
        // 後続の確認は元の太陽の向きを前提にするので戻しておく。
        check(SaveSceneComponents(workspace, migrated, restoreSky) && ProjectWorkspace::ReadJson(atmospherePath, skyAfter) &&
              skyAfter["settings"]["azimuth"] == 1.2, "atmosphere-only save restores the sun");
        check(workspace.ReadScene(migrated, expanded) && expanded["preview"]["atmosphere"]["azimuth"] == 1.2, "reopen after partial saves");
    }
    ProjectWorkspace::ReadJson(migrated, packed);
    const auto originalTerrain = workspace.Resolve(packed["components"][0]["asset"]);
    json beforeFailure, afterFailure;
    ProjectWorkspace::ReadJson(originalTerrain, beforeFailure);
    auto failedSave = expanded; failedSave["format"] = "terrain-graph.scene";
    failedSave["graph"]["nodes"][0]["extra"] = "must roll back";
    const auto blockedPath = root / "blocked.tgscene";
    fs::create_directory(blockedPath, paintError);
    const auto workBeforeFailure = workspace.WorkEnvironment();
    json skyBeforeFailure; ProjectWorkspace::ReadJson(skyPath, skyBeforeFailure);
    failedSave["preview"]["atmosphere"]["azimuth"] = 2.0;
    failedSave["preview"]["lightingMode"] = "ibl";
    check(!SaveSceneComponents(workspace, blockedPath, failedSave), "scene write failure reported");
    check(ProjectWorkspace::ReadJson(originalTerrain, afterFailure) && beforeFailure == afterFailure, "component update rolled back on scene failure");
    json skyAfterFailure; ProjectWorkspace::ReadJson(skyPath, skyAfterFailure);
    check(skyBeforeFailure == skyAfterFailure && workspace.WorkEnvironment() == workBeforeFailure,
          "sky and work environment rolled back on scene failure");
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
    const auto movedSky = MoveAsset(workspace, skyPath, folder);
    check(!movedSky.empty() && workspace.ReadScene(migrated, expanded) && expanded["preview"]["atmosphere"]["azimuth"] == 1.2,
          "sky move retains sun and scene reference");
    const auto relations = InspectAssetRelations(workspace, movedSky);
    check(relations.complete && !relations.referencers.empty(), "sky deletion finds scene references");
    const auto skyBackup = movedSky.wstring() + L".saved";
    fs::rename(movedSky, skyBackup, error);
    check(!workspace.ReadScene(migrated, expanded), "missing sky prevents scene load");
    fs::rename(skyBackup, movedSky, error);
    json standalone = {{"atmosphere", packed["atmosphere"]},
        {"preview", {{"atmosphere", {{"density", 8}, {"coverage", 0.8}}}, {"exposure", 4}}}};
    check(workspace.Scan() && ExpandSceneAtmosphere(workspace, standalone) &&
          !standalone["preview"]["atmosphere"].contains("density") && standalone["preview"]["atmosphere"]["coverage"] == 0.8 &&
          standalone["preview"]["exposure"] == 4, "sky replacement resets unspecified sky values but preserves clouds and exposure");
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
