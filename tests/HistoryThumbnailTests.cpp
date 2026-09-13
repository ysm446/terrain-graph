#include "io/RecentFiles.h"
#include "io/ThumbnailStore.h"
#include "io/AssetRelations.h"
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
    const auto scene = a / "scene.tgscene";
    nlohmann::json sceneDocument = {{"textures", nlohmann::json::array()}, {"materials", nlohmann::json::array()},
        {"models", nlohmann::json::array()}, {"skies", nlohmann::json::array()}};
    check(workspace.SaveScene(scene, sceneDocument), "scene save with ID");
    const auto uid = sceneDocument["sceneUid"];
    const auto thumbnail = tg::io::SceneThumbnailPath(workspace, scene);
    check(thumbnail.parent_path() == a / ".terrain-graph" / "scene-thumbnails", "central scene thumbnail");
    fs::create_directories(a / "scene.assets", error);
    std::ofstream(a / "scene.assets/thumbnail.png").put('p');
    std::ofstream(a / "scene.assets/paint.png").put('m');
    tg::io::MigrateSceneThumbnails(workspace);
    check(fs::exists(thumbnail) && !fs::exists(a / "scene.assets/thumbnail.png") && fs::exists(a / "scene.assets/paint.png"), "migration preserves paint");
    check(workspace.SaveScene(scene, sceneDocument) && sceneDocument["sceneUid"] == uid, "overwrite preserves scene ID");
    const auto renamedScene = a / "renamed.tgscene";
    fs::rename(scene, renamedScene, error);
    check(tg::io::SceneThumbnailPath(workspace, renamedScene) == thumbnail, "rename preserves thumbnail");
    const auto otherScene = a / "copy.tgscene";
    check(workspace.SaveScene(otherScene, sceneDocument) && sceneDocument["sceneUid"] != uid, "save as gets separate scene ID");
    // 削除検査は別ルートで行い、上の欠落ファイルのテストと分離する。
    tg::io::ProjectWorkspace deletion;
    const auto deleteRoot = directory / "delete-root";
    check(deletion.Open(deleteRoot), "deletion root");
    const auto texture = deleteRoot / "image.png";
    std::ofstream(texture).put('t');
    const auto textureRef = deletion.Reference(texture);
    auto materialFile = deleteRoot / "shared.tgmat";
    nlohmann::json dependency = {{"maps", {{"baseColor", textureRef}}}};
    check(deletion.SaveAsset(materialFile, "material-asset", dependency), "deletion material");
    auto report = tg::io::InspectAssetRelations(deletion, texture);
    check(report.complete && report.referencers.size() == 1 && report.referencers[0] == materialFile && report.companions.size() == 1, "reference and metadata warning");
    std::ofstream(texture.wstring() + L".meta", std::ios::app).put(' ');
    check(!tg::io::RetireAsset(deletion, report) && fs::exists(texture), "changed metadata requires reconfirmation");
    report = tg::io::InspectAssetRelations(deletion, texture);
    const auto materialReport = tg::io::InspectAssetRelations(deletion, materialFile);
    check(materialReport.related.size() == 1 && materialReport.related[0] == texture, "outgoing dependency warning");
    auto secondMaterial = deleteRoot / "second.tgmat";
    dependency.erase("uid");
    check(deletion.SaveAsset(secondMaterial, "material-asset", dependency), "new reference after confirmation");
    check(!tg::io::RetireAsset(deletion, report) && fs::exists(texture), "changed references require reconfirmation");
    const auto updated = tg::io::InspectAssetRelations(deletion, texture);
    check(tg::io::RetireAsset(deletion, updated), "confirmed asset is retired");
    check(!fs::exists(texture) && !fs::exists(texture.wstring() + L".meta") && fs::exists(materialFile), "no cascading deletion");
    bool recoverable = false;
    for (const auto& entry : fs::recursive_directory_iterator(deleteRoot / ".terrain-graph/trash"))
        if (entry.path().filename() == "image.png") recoverable = true;
    check(recoverable, "retired source remains recoverable");
    std::ofstream(deleteRoot / "broken.tgmat") << "{broken";
    check(!tg::io::InspectAssetRelations(deletion, materialFile).complete, "incomplete scan blocks deletion");
    check(!tg::io::InspectAssetRelations(deletion, deleteRoot / "project.tgproj").complete, "workspace file protected");
    // 移動はIDで参照を保つ。付随する.metaとシーンのペイントデータも一緒に動く。
    tg::io::ProjectWorkspace moving;
    const auto moveRoot = directory / "move-root";
    check(moving.Open(moveRoot), "move root");
    const auto movedImage = moveRoot / "image.png";
    std::ofstream(movedImage).put('t');
    const auto movedRef = moving.Reference(movedImage);
    fs::create_directories(moveRoot / "Textures", error);
    const auto imageDestination = tg::io::MoveAsset(moving, movedImage, moveRoot / "Textures");
    check(imageDestination == moveRoot / "Textures" / "image.png" && fs::exists(imageDestination), "asset moved");
    check(!fs::exists(movedImage) && !fs::exists(movedImage.wstring() + L".meta") &&
          fs::exists(imageDestination.wstring() + L".meta"), "metadata moved with asset");
    check(moving.Resolve(movedRef) == imageDestination, "reference follows moved asset");
    check(tg::io::MoveAsset(moving, imageDestination, moveRoot / "Textures") == imageDestination, "same folder is a no-op");
    std::ofstream(moveRoot / "image.png").put('x');
    check(tg::io::MoveAsset(moving, imageDestination, moveRoot).empty() && fs::exists(imageDestination), "name clash refused");
    const auto movedScene = moveRoot / "scene.tgscene";
    nlohmann::json movedDocument = {{"textures", nlohmann::json::array()}, {"materials", nlohmann::json::array()},
        {"models", nlohmann::json::array()}, {"skies", nlohmann::json::array()}};
    check(moving.SaveScene(movedScene, movedDocument), "scene for move");
    fs::create_directories(moveRoot / "scene.assets", error);
    std::ofstream(moveRoot / "scene.assets/paint.png").put('m');
    fs::create_directories(moveRoot / "Scenes", error);
    const auto sceneDestination = tg::io::MoveAsset(moving, movedScene, moveRoot / "Scenes");
    check(sceneDestination == moveRoot / "Scenes" / "scene.tgscene" && fs::exists(moveRoot / "Scenes/scene.assets/paint.png") &&
          !fs::exists(moveRoot / "scene.assets"), "paint data moves with scene");
    check(tg::io::MoveAsset(moving, moveRoot / "project.tgproj", moveRoot / "Scenes").empty(), "workspace file cannot move");
    check(tg::io::MoveAsset(moving, sceneDestination, directory).empty() && fs::exists(sceneDestination), "outside root refused");
    std::cout << failures << " failures\n";
    return failures ? 1 : 0;
}
