#include "io/SceneComponents.h"
#include "app/AssetSelectionContext.h"
#include "io/AssetRelations.h"
#include <fstream>
#include <iostream>

int main() {
    namespace fs = std::filesystem;
    using nlohmann::json;
    using namespace tg::io;
    ProjectWorkspace workspace;
    const auto root = fs::path(TG_TEST_DATA_DIR) / "component-test-data";
    int failures = 0;
    const auto check = [&](bool ok, const char* name) { if (!ok) { ++failures; std::cerr << name << '\n'; } };
    check(workspace.Open(root), "open root");
    // カタログは開いていない画像（meta未生成）と素材も列挙し、内部キャッシュは除く。
    tg::AssetSelectionContext selections;
    selections.root = fs::path(TG_TEST_DATA_DIR) / "selection-catalog-test-data";
    std::error_code catalogError;
    fs::create_directories(selections.root / "nested", catalogError);
    fs::create_directories(selections.root / ".cache", catalogError);
    for (const auto* name : {"nested/unopened.png", "nested/unopened.tgmat", "nested/layer.tglayer", ".cache/hidden.png", "nested/image.png.meta"})
        std::ofstream(selections.root / name).put('x');
    selections.Scan();
    check(selections.candidates.size() == 3, "catalog includes unopened nested files without parsing or GPU loading");
    uint32_t slot = 8;
    selections.owner = 12;
    selections.Queue(selections.root / "nested/unopened.png", 42, slot);
    selections.request.ready = true; selections.request.result = 9;
    check(!selections.Consume(43, slot) && slot == 8, "different widget cannot consume deferred assignment");
    selections.owner = 13;
    check(!selections.Consume(42, slot), "different owner cannot consume deferred assignment");
    selections.owner = 12;
    check(selections.Consume(42, slot) && slot == 9, "same field receives loaded ID");
    selections.Queue(selections.root / "missing.png", 42, slot); selections.request.ready = true;
    check(!selections.Consume(42, slot) && slot == 9, "failed load preserves assignment");
    selections.Queue(selections.root / "nested/unopened.png", 42, slot);
    selections.request.ready = true; selections.request.result = 10; slot = 11;
    check(!selections.Consume(42, slot) && slot == 11, "changed assignment rejects stale result");
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
        {"moonIlluminance", 0.3}, {"moonPhase", 0.5}, {"starIntensity", 2.0}, {"starRotation", 1.0}, {"starLatitude", 0.5},
        {"celestialMode", "dateTime"}, {"longitude", 2.4}, {"dateYear", 2026}, {"dateMonth", 6}, {"dateDay", 21}, {"localHour", 21.5}, {"utcOffset", 9.0}};
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
    // シーンを部品ごと複製する。部品とペイントは ID の違う独立したコピーになり、シーンはそれを参照する。
    {
        check(workspace.Scan() && workspace.ReadScene(migrated, expanded), "reopen before duplicate");
        json source;
        check(ProjectWorkspace::ReadJson(migrated, source), "read scene to duplicate");
        // テストのデータは前回の実行の分も残るので、毎回まだ無い名前を使う。
        const std::string name = workspace.UniquePath(root, "duplicated", "").filename().string();
        std::string reason;
        const auto copy = DuplicateScene(workspace, migrated, name, reason);
        check(!copy.empty() && copy.parent_path().filename().string() == name &&
              copy.filename().string() == name + ".tgscene", "duplicate scene into a folder with the new name");
        json copied, duplicated;
        check(ProjectWorkspace::ReadJson(copy, copied) && copied["sceneUid"].is_string() &&
              copied["sceneUid"] != source["sceneUid"], "duplicate gets a new scene ID");
        // この時点で地形と空は Moved へ移してあり、シーンと同じフォルダにあるのは雲だけ。
        // 同じフォルダの部品はコピー（新しい ID と名前）、別のフォルダの部品は共有のまま。
        const auto isLocal = [&](const json& reference) {
            const auto path = workspace.Resolve(reference);
            return !path.empty() && fs::equivalent(path.parent_path(), migrated.parent_path(), error);
        };
        std::vector<json> references;
        for (const auto& entry : source["components"]) references.push_back(entry["asset"]);
        references.push_back(source["atmosphere"]);
        std::vector<json> results;
        for (const auto& entry : copied["components"]) results.push_back(entry["asset"]);
        results.push_back(copied["atmosphere"]);
        bool localCopied = results.size() == references.size(), sharedKept = localCopied, anyLocal = false, anyShared = false;
        for (size_t i = 0; i < references.size() && i < results.size(); ++i) {
            const auto path = workspace.Resolve(results[i]);
            if (isLocal(references[i])) {
                anyLocal = true;
                localCopied = localCopied && results[i]["uid"] != references[i]["uid"] && !path.empty() &&
                              fs::equivalent(path.parent_path(), copy.parent_path(), error) &&
                              path.stem().string().starts_with(name + "_");
            } else {
                anyShared = true;
                sharedKept = sharedKept && results[i] == references[i];
            }
        }
        check(anyLocal && localCopied, "components in the scene folder are copied with new IDs and names");
        check(anyShared && sharedKept, "components in other folders stay shared");
        check(workspace.Scan() && workspace.ReadScene(copy, duplicated) &&
              duplicated["graph"]["nodes"].size() == expanded["graph"]["nodes"].size(), "duplicate opens");
        check(workspace.ReadScene(migrated, expanded), "original still opens after duplicate");
        check(DuplicateScene(workspace, migrated, name, reason).empty() && !reason.empty(), "existing folder is refused");
        check(DuplicateScene(workspace, migrated, "bad/name", reason).empty(), "invalid folder name is refused");
    }
    // 部品が全部シーンと同じフォルダにあるシーン。ペイントも複製先の部品の横へコピーする。
    {
        const auto localFolder = workspace.UniquePath(root, "duplicate-source", "");
        fs::create_directories(localFolder, error);
        const auto localTerrain = CreateGraphAsset(workspace, localFolder, false);
        const auto paint = workspace.UniquePath(localFolder, "paint", ".png");
        std::ofstream(paint).put('p');
        json terrainBody;
        check(!localTerrain.empty() && workspace.ReadAsset(localTerrain, "terrain-graph", terrainBody), "read local terrain");
        terrainBody["paintMasks"] = json::array({{{"id", 1}, {"source", workspace.Reference(paint)}, {"resolution", 1}}});
        auto localTerrainPath = localTerrain;
        check(workspace.SaveAsset(localTerrainPath, "terrain-graph", terrainBody), "save terrain with paint");
        auto localSkyPath = localFolder / L"local_sky.tgatmosphere";
        json localSkyBody = AtmosphereAssetBody(json::object(), "local_sky");
        check(workspace.SaveAsset(localSkyPath, "atmosphere-sky", localSkyBody), "save local sky");
        const auto localScene = localFolder / L"local.tgscene";
        json sceneBody = {{"format", "terrain-graph.scene"}, {"version", 3},
            {"components", json::array({{{"role", "terrain"}, {"asset", workspace.Reference(localTerrain)}}})},
            {"atmosphere", workspace.Reference(localSkyPath)}, {"preview", json::object()}, {"sceneUid", "{LOCAL}"}};
        check(ProjectWorkspace::WriteJson(localScene, sceneBody), "write local scene");
        const std::string name = workspace.UniquePath(root, "local-copy", "").filename().string();
        std::string reason;
        const auto copy = DuplicateScene(workspace, localScene, name, reason);
        json copied, copiedTerrain;
        check(!copy.empty() && ProjectWorkspace::ReadJson(copy, copied), "duplicate local scene");
        const auto copiedTerrainPath = workspace.Resolve(copied["components"][0]["asset"]);
        check(copiedTerrainPath.filename().string() == name + "_" + localTerrain.filename().string() &&
              workspace.Resolve(copied["atmosphere"]).filename().string() == name + "_sky.tgatmosphere",
              "names replace the scene name or get the new name as prefix");
        check(workspace.ReadAsset(copiedTerrainPath, "terrain-graph", copiedTerrain) &&
              copiedTerrain["uid"] != terrainBody["uid"], "terrain copy has a new ID");
        const auto copiedPaint = workspace.Resolve(copiedTerrain["paintMasks"][0]["source"]);
        check(!copiedPaint.empty() && copiedPaint != paint && fs::exists(paint) &&
              copiedTerrain["paintMasks"][0]["source"]["uid"] != terrainBody["paintMasks"][0]["source"]["uid"],
              "paint is copied, not shared");
        json localReopened;
        check(workspace.Scan() && workspace.ReadScene(copy, localReopened), "local duplicate opens");
    }
    std::cout << failures << " failures\n";
    return failures ? 1 : 0;
}
