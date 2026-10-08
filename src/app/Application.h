#pragma once
#include "io/ProjectWorkspace.h"
#include "app/AssetThumbnailCache.h"
#include "app/AssetSelectionContext.h"
#include "io/AssetRelations.h"

#include "compositor/MaterialLibrary.h"
#include "compositor/MaterialStack.h"
#include "compositor/PaintMask.h"
#include "compositor/TextureLibrary.h"
#include "core/Log.h"
#include "core/FrameLimiter.h"
#include "core/Window.h"
#include "graph/NodeGraph.h"
#include "app/UndoHistory.h"
#include "io/AppSettings.h"
#include "io/MaterialExport.h"
#include "io/ProjectIo.h"
#include "io/RecentFiles.h"
#include "renderer/MaterialSphere.h"
#include "renderer/GeneratedMeshes.h"
#include "renderer/ModelPreview.h"
#include "renderer/PreviewRenderer.h"
#include "renderer/SkyLibrary.h"
#include "renderer/SkySphere.h"
#include "rhi/Device.h"
#include "rhi/PipelineCache.h"
#include "rhi/ShaderCompiler.h"
#include "ui/ImGuiLayer.h"
#include "ui/UiStyle.h"
#include "ui/Toast.h"
#include "ui/AxisTranslationDrag.h"

#include <imgui.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// imgui-node-editor のコンテキスト。ヘッダを丸ごと引き込まないための前方宣言。
namespace ax::NodeEditor {
struct EditorContext;
}

namespace tg {

// コマンドラインから渡せる起動オプション。
struct StartupOptions {
    bool migrateComponents = false;
    std::filesystem::path openGraph;
    // 起動時に読み込む HDRI。空なら手続き的な空を使う。
    std::filesystem::path hdriPath;
    // 起動時にテクスチャライブラリへ読み込む画像。--texture を繰り返し指定できる。
    std::vector<std::filesystem::path> texturePaths;
    // 起動時に開くプロジェクト (.tgproj)。空なら既定のスタックで始める。
    std::filesystem::path projectPath;
    std::filesystem::path projectRoot;
    // 削除確認画面のスクリーンショット検証用。削除そのものは実行しない。
    std::filesystem::path inspectAssetDelete;
    // 参照ビューアのスクリーンショット検証用。指定したアセットを中心に開く。
    std::filesystem::path inspectAssetRelations;
    // 指定すると、数フレーム描いてから合成結果を画像へ書き出して終了する。
    // 対話せずに書き出しを確かめるための開発用オプション。
    std::filesystem::path exportDirectory;
    // 指定すると、数フレーム描いてからプロジェクトを保存して終了する。
    // 保存と読み込みを対話なしで確かめるための開発用オプション。
    std::filesystem::path saveProjectPath;
    // 指定すると、数フレーム描いてからビューポートを PNG に書き出して終了する。
    // 画面キャプチャに頼らず描画結果を確認するための開発用オプション。
    std::filesystem::path screenshotPath;
    // 指定すると、ウィンドウ全体（UI 込み）を PNG に書き出して終了する。
    // 画面キャプチャは他ウィンドウを掴むことがあるため、確認にはこちらを使う。
    std::filesystem::path uiScreenshotPath;
    // 指定すると、読み込んだグラフの評価が落ち着くまで待ってから、エラー・寸法・メッシュ数・
    // 配置数などを JSON に書いて終了する（--evaluate-report）。スクリプトや LLM が UI を見ずに
    // グラフを確かめるための経路。reportThumbnailPath を指定するとビューポートの縮小画像も書く。
    std::filesystem::path reportPath;
    std::filesystem::path reportThumbnailPath;
    // 指定すると、ノードカタログ（io::NodeCatalog）を JSON に書いて終了する。ウィンドウも GPU も使わない。
    std::filesystem::path catalogPath;
    uint32_t benchmarkFrames = 0;
    bool referenceCloudLighting = false;
    // --gpu-validation。Debug のデバッグレイヤーに加えて GPU ベースバリデーションを有効にする。
    bool gpuValidation = false;
    bool fullResolutionClouds = false;
    bool disableTemporalClouds = false; // 比較用。時間方向の再投影を無効にする。
    uint32_t screenshotFrame = 8;
    uint32_t screenshotCount = 1; // ビューポート連番の枚数。
    uint32_t screenshotInterval = 1;
    int previewModel = -1;
    int previewModelLod = 0;
    std::filesystem::path importModel; // 開発用。読み込みとプレビューの確認。
    // 開発用。指定したモデル（.tgmodel のパス、または "all" でシーンの全モデル）の
    // インポスターを焼き直し、.tgmodel を保存して終了する。--project と一緒に使う。
    std::vector<std::filesystem::path> bakeImpostors;
    std::filesystem::path revealAsset; // 開発用。参照元への移動を画面確認する。
    graph::GraphId selectNode = 0; // 開発用。読み込んだグラフのプロパティを画像で確認する。
    // 開発用。この番号のフレームで「ノードの重なりを解消」を 1 回実行する（0 なら何もしない）。
    // `--screenshot-ui` や `--save-project` と組み合わせて、結果を画像やファイルで確かめる。
    int resolveGraphOverlapsFrame = 0;
    // 開発用。「チャンネル」パネルを前面にして、この番号のチャンネルを出す（--screenshot-ui で確かめる）。負なら何もしない。
    int channelPreview = -1;
    // 開発用。読み込んだシーンの全項目を未保存扱いにし、階層の印と保存ボタンを画像で確認する。
    bool showUnsaved = false;
};

// アプリ本体。ウィンドウ、デバイス、UI の生存期間とフレームループを持つ。
class Application {
public:
    bool Initialize(const StartupOptions& options);
    void Shutdown();
    int Run();

private:
    void PollShaderHotReload();
    // F12 で撮ったスクリーンショットの書き出しを要求する。撮れたら通知を出す。
    void RequestScreenshot();
    // F9 で撮ったビューポート（UI なし）を書き出す。撮れたら通知を出す。
    void SaveViewportScreenshot();
    void DrawUi();
    // 既定のドックレイアウトを組む。ini に配置が無いときと、明示的な要求で呼ぶ。
    void BuildDefaultLayout(ImGuiID dockspaceId);
    // 下のパネルを畳んでいる間、この名前のウィンドウも隠すか（「アセット」と同じ枠のタブ）。
    bool HiddenWithAssetBand(const char* windowName);
    void DrawViewportPanel();
    void DrawMaterialPanel();
    void DrawLightingPanel();
    // 実行状況の情報ウィンドウ（ウィンドウ > 情報）。常設ドックには置かない。
    void DrawInfoWindow();
    // ノードグラフパネル。サーフェス / シェイプ / 水面をノードとして繋ぎ、
    // 出力ノードへ届いたチェーンをレイヤー列へコンパイルしてプレビューに使う。
    void DrawGraphPanel();
    void DrawSceneHierarchy();
    void OpenComponentEditor(int component);
    // シーンの部品（グラフ・スカイ）を新しく作るときの置き場所。シーンと同じフォルダ、
    // まだ保存していないシーンならルートの Scenes/。
    std::filesystem::path SceneAssetDirectory();
    void FinishComponentPreview(bool place);
    // エディタのコンテキストを破棄する。Shutdown から呼ぶ。
    void DestroyGraphEditor();
    // グラフのノード位置をエディタへ流し込み直す（読み込み・リセット・アンドゥの後）。
    // navigate が真なら、流し込み後に全体を画面へ収める。
    // アンドゥでは偽にする（戻すたびに視点が飛ぶと編集にならない）。
    void RequestGraphNodePlacement(bool navigate = true);
    // グラフのエディタ部（imgui-node-editor）。パネルの中で呼ぶ。
    void DrawGraphEditor();
    void DrawGraphNodeNotes();
    // 引いて見たときに、ノードの名前をノードの上へ画面上で一定の大きさで出す。
    void DrawGraphNodeTitles();
    // グラフのノード 1 枚。カード・ピン・リンクの当たり判定を描く。
    void DrawGraphNode(const graph::Node& node);
    // ノードに出すマスクのサムネイル（そのノードの outputIndex 番目の Mask 出力）。
    // 表側の評価結果に無ければ ptr が 0。
    D3D12_GPU_DESCRIPTOR_HANDLE GraphMaskThumbnail(graph::GraphId nodeId,
                                                   size_t outputIndex) const;
    // ノードに出す合成結果のサムネイル（そのノードのレイヤーまで合成した見た目）。
    D3D12_GPU_DESCRIPTOR_HANDLE GraphLayerThumbnail(graph::GraphId nodeId) const;
    // グラフノードのレイヤー設定のプロパティ行。
    // 変更があれば true。isBase はマスクが効かない一番下のレイヤーのとき。
    // isSource は入力を持たないノード（ハイトマップ）。マスクの節を出さない。
    // maskFromNode が真のとき、マスクの出どころは Mask 入力に繋いだノード。
    // ソースと画像の行は出さない（同じ値を 2 か所から編集させない）。
    // maskResolves が偽なら「Mask 入力に繋いだのに効いていない」注意書きを出す
    // （堆積 / 崩落の Mask を、そのチェーンの外から繋いだとき）。
    // pathUv が真なら Surface の UV Path に Path が繋がっている（帯の座標で貼る）。
    // UV スケールの代わりにパス UV の行を出す。
    bool DrawLayerSettings(compositor::MaterialLayer& layer, bool isBase, bool isSource = false,
                           bool maskFromNode = false,
                           bool maskResolves = true, bool pathUv = false);
    // グラフの変更をコンパイル結果（m_graphStack）へ反映する。フレームの頭で呼ぶ。
    void SyncGraphStack();
    // 選択中のノードを控える / 貼り付ける（Ctrl+C / Ctrl+V）。
    void CopySelectedGraphNodes();
    // 控えたノードを貼る。viewCenter は今のキャンバスの中央（キャンバス座標）で、
    // 貼った集合の中心をそこへ置く。相対の配置は保つ。
    void PasteGraphNodes(const ImVec2& viewCenter);
    // 実際に貼る。foreign なら別の文書から来たノードとして参照を引き直す（フレームの外で呼ぶ）。
    void PlaceGraphClipboard(const ImVec2& viewCenter, bool foreign);
    // 選んだノードの揃え方（ノードの右クリックメニュー）。Stack は縦 / 横に詰めて並べる
    // （離れた列 / 行は別々に並べるので、複数列のまま整えられる）。
    enum class GraphAlign { Left, Right, Top, Bottom, CenterX, CenterY, DistributeX, DistributeY, StackX, StackY };
    // 選んだノードを揃える。2 個未満（等間隔は 3 個未満）なら何もしない。アンドゥ 1 段になる。
    // ノードエディタを current にした状態で呼ぶ。
    void AlignSelectedGraphNodes(GraphAlign mode);
    // 編集中のグラフのノードを、どの接続も左から右へ向くように並べ直す（graph::ArrangeLeftToRight）。
    // ノードと背景の右クリックの「グラフ全体を左から右へ並べ直す」。選択には依らない。アンドゥ 1 段になる。
    // ノードエディタを current にした状態で呼ぶ。
    void ArrangeGraphLeftToRight(bool overlapsOnly = false);
    // 「ノードの重なりを解消」の要求（右クリックのメニュー）。メニューはエディタを止めたポップアップの
    // 中にあるので、次のフレームで ArrangeGraphLeftToRight(true) を実行する。
    bool m_pendingGraphSpread = false;
    // グループ（枠）。選んだノードを囲む枠を作る（G キー、右クリックのメニュー）。選択が無ければ at に空の枠。
    void CreateGraphGroup(const ImVec2& at);
    void DrawGraphGroups();
    // 畳んだグループ。中のノードは描かず（m_graphHiddenNodes）、外と繋がるピンだけを持つノードを 1 個描く。
    void UpdateGraphHiddenNodes();
    void DrawCollapsedGraphGroup(graph::NodeGroup& group);
    void ToggleGraphGroupCollapsed(int groupId);
    // 畳んだグループを動かす。中のノードと、中にある枠も同じだけ動かす。
    void MoveCollapsedGraphGroup(graph::NodeGroup& group, const ImVec2& position);
    // 枠の中にあるノード（モデルの位置で判定する。隠れているノードはエディタに位置を聞けないため）。
    std::vector<graph::GraphId> GraphGroupMembers(const graph::NodeGroup& group) const;
    std::unordered_map<graph::GraphId, int> m_graphHiddenNodes;    // 隠しているノード → 畳んだグループの ID
    std::unordered_set<int> m_graphHiddenGroups;                    // 畳んだグループの中にあって隠している枠
    std::unordered_map<graph::GraphId, ImVec2> m_graphNodeSizes;   // 最後に描いたときのノードの大きさ
    std::unordered_map<int, std::array<float, 2>> m_graphCollapsedSynced;  // エディタへ伝えた畳んだノードの位置
    // 整列や並べ直しでノードを動かす直前に呼ぶ。枠の中にあるノードが動くなら、動いた後のノードを
    // 囲むように枠を合わせ直す（moved はノードの ID → 動かした後の位置）。中身が動かない枠はそのまま。
    void RefitGraphGroups(const std::unordered_map<graph::GraphId, ImVec2>& moved);
    // 「ノードの重なりを解消」の、グループ（枠）があるときの版。枠の中のノードの重なりを解いてから、
    // 枠どうし・枠と外のノードが重ならないように、枠を中身ごと下へずらす。動かしたノードと枠の数を返す。
    size_t ResolveGraphOverlapsWithGroups();
    // エディタへ伝えてある枠の位置と大きさ（グループの ID → x, y, 幅, 高さ）。モデルの値がこれと違えば、
    // アンドゥや読み込みで変わったとみなしてエディタへ流し込む。
    std::unordered_map<int, std::array<float, 4>> m_graphGroupSynced;
    int m_selectedGraphGroup = 0;        // プロパティに出している枠（0 なら無し）
    bool m_graphGroupMoved = false;      // 枠を動かした / 大きさを変えた（マウスを離したら未保存にする）
    bool m_pendingGraphGroup = false;    // 右クリックのメニューからの「グループにまとめる」
    ImVec2 m_pendingGraphGroupAt{};
    // メニューはエディタを止めたポップアップの中にあるので、要求だけ置いて次のフレームで実行する。
    bool m_pendingGraphArrange = false;
    // 背景の右クリックで「ノードを追加」を開いた位置（キャンバス座標）。追加したノードをここへ置く。
    ImVec2 m_graphAddNodeAt{};
    // ビューポートに出すノードを決める。出力ノードや無効な ID は
    // 「出力ノードのチェーン」（0）に落とす。
    // outputPin は**どの出力を見るか**。0 なら最初の出力（レイヤーなら Result）。
    void SetPreviewGraphNode(graph::GraphId nodeId, graph::GraphId outputPin = 0);
    // 出口ノードの表示フラグ。Ctrl を押しながらだと、その出口だけを出す / 全部へ戻す。
    void ToggleOutputDisplay(graph::GraphId nodeId, bool solo);
    bool OutputHidden(graph::GraphId nodeId) const { return m_hiddenOutputs.contains(nodeId); }
    // 出口ノードのリファレンス表示（Houdini のテンプレートフラグ）。灰色のワイヤーフレームで描く。
    void ToggleOutputReference(graph::GraphId nodeId);
    bool OutputReference(graph::GraphId nodeId) const { return m_referenceOutputs.contains(nodeId); }
    // 今のグラフにある出口のうち隠しているものの数。消えたノードの ID は捨てる。
    size_t HiddenOutputCount();
    // 地形の面と雲を隠すか、地形の面をリファレンス表示にするかをレンダラへ渡す（Output / Cloud Output のフラグ）。
    void SyncOutputDisplay();
    void DrawModelPreviewWindow();
    void ProcessModelWork();
    void PrepareModelScatters();
    // Model Place の配置の点を組んで上げる（PrepareModelScatters から呼ぶ。フレームの外）。
    // 使う配置用メッシュのキーを meshKeys へ足す。
    void PrepareModelPlaces(std::vector<std::string>& meshKeys);
    // Model Place の敷地（Pad）を、読み手の Mask Mesh / Grading の足跡として置く（PrepareRoadMeshes から呼ぶ）。
    void UpdateModelPlacePads(std::vector<graph::GraphId>& aliveFootprints);
    // 地形の最低標高（m）。標高 = ハイト（0〜1）× 標高差 + これ。
    float TerrainBaseElevation() const;
    // --- Road Mesh / Mesh Output（ApplicationRoadMesh.cpp） ---
    // Mesh Output に繋がった Road Mesh の路面を作り、GPU へ上げる。フレームの外で呼ぶ
    // （形が変わったときだけ上げ直す。ExecuteImmediate を伴う）。
    void PrepareRoadMeshes();
    // 影の段と本描画で呼ぶ（モデルの配置と同じ所）。
    void DrawGeneratedMeshes(ID3D12GraphicsCommandList* commandList, const DirectX::XMFLOAT4X4& viewProjection,
                             bool shadow);
    // 帯（路面・路肩）の材質を GPU の定数へ。通常の Material は 1 層の Layered Material として
    // 繰り返し長で組む。材質が無ければ偽（error は空）。組めなければ偽で error に理由。
    bool SurfaceMaterialGpu(compositor::MaterialAssetId material, float uvRepeatMeters,
                            compositor::LayerMaterialGpu& out, std::string* error) const;
    // Road Mesh / Shoulder のプロパティ。変更があれば true。
    bool DrawRoadMeshSettings(graph::Node& node);
    bool DrawShoulderSettings(graph::Node& node);
    bool DrawLaneMarkingSettings(graph::Node& node);
    // 材質の不透明度での切り抜き（Masked / Translucent の通常の Material）。区画線の帯で使う。
    renderer::GeneratedMeshItem::Cutout MaterialCutout(compositor::MaterialAssetId material, float uvRepeatMeters) const;
    bool DrawShoulderSection(graph::RoadShoulderSettings& shoulder);
    // Road Mesh / Shoulder のプロパティの「状態」節（作れなかった理由、延長、メッシュの量）。
    void DrawRoadNodeStatus(graph::GraphId nodeId, const char* disconnectedHint);
    // 境界マテリアル（.tgboundary）。読み込んだものを ID（無ければパス）で持つ。
    // マスクとハイトの画像はテクスチャライブラリへ読み込む（GPU 待機を伴うのでフレームの外で呼ぶ）。
    struct BoundaryAsset {
        std::filesystem::path path;  // 絶対パス
        std::string uid, name;
        compositor::TextureId mask = compositor::kNoTexture, height = compositor::kNoTexture;
        float widthMeters = 0.5f, repeatMeters = 2.0f, depthMeters = 0.03f, heightCenter = 0.5f;
        bool alongU = false, invertMask = false;
        bool dirty = false;   // 編集して保存していない
        std::string error;    // 読めなかった理由
    };
    BoundaryAsset* AcquireBoundary(const std::string& path, const std::string& uid);
    bool SaveBoundary(BoundaryAsset& asset);
    // 境界マテリアルの設定の行（プロパティ表の中で呼ぶ。保存のボタンまで）。
    void DrawBoundaryAssetRows(BoundaryAsset& boundary);
    // 境界マテリアルを選ぶ行（路肩の最初の区間と、区間の切り替えで共通）。変えたら true。
    bool DrawBoundaryCombo(const char* label, std::string& path, std::string& uid);
    // 選んだ境界マテリアルの要約（読むだけ。マスクのサムネイル・幅と、窓で開くボタン）。
    void DrawBoundarySummary(const char* id, const std::string& path, const std::string& uid);
    // 境界マテリアルの窓（アセットブラウザのダブルクリックで開く）。
    void OpenBoundaryPreview(const std::filesystem::path& file);
    void DrawBoundaryPreviewWindow();
    bool m_showBoundaryPreview = false;
    std::string m_boundaryPreviewPath, m_boundaryPreviewUid;
    // 配置の点の元（散布 / 崩落）の、いま使える点の組。本体の評価器が作っていれば
    // そちらを、無ければ元ごとの評価器を見る。Ready で *out が nullptr なら点は 0。
    enum class PlacementPointsState { Missing, Evaluating, Ready };
    PlacementPointsState PlacementPointsOf(graph::GraphId source,
                                           const compositor::PlacementPointSet** out) const;
    // 配置用メッシュの描画量を読み戻す。フレームの記録を始めた後、描画より前に呼ぶ。
    void CollectModelScatterStats();
    // インポスターの作成・削除と画像の読み込み。フレームの外で呼ぶ。
    void ProcessImpostorWork();
    // --bake-impostors の処理。焼いて保存できたら true。
    bool BakeRequestedImpostors();
    void DrawImpostorSection(renderer::ModelAsset& asset);
    // Snow Plume ノードを集め、Source のマスクを評価するスロットを揃える。フレームの外で呼ぶ。
    void PrepareSnowPlumes();
    // Liquid の波の設定をレンダラへ写す（最初の有効な Liquid）。
    void PrepareWater();
    // 評価済みのマスクと設定をレンダラへ渡す。描く直前に毎フレーム呼ぶ。
    void SubmitSnowPlumes();
    void DrawModelScatters(ID3D12GraphicsCommandList* commandList, const DirectX::XMFLOAT4X4& viewProjection, bool shadow);
    void RenderModelPreviews(ID3D12GraphicsCommandList* commandList);

    // マテリアル 1 つのプロパティ（基本 + マップ）。変更があれば真を返す。
    // **置き場所はプレビューの窓だけ**（一覧はサムネイルだけを出す）。
    bool DrawLayerMaterialProperties(compositor::MaterialAsset& asset);
    bool DrawMaterialProperties(compositor::MaterialAsset& asset);
    void CommitMaterialEdit();
    // マテリアルプレビューの窓（回せる球 + プロパティ）。
    // 一覧のサムネイルをダブルクリックするか、ウィンドウメニューから開く。
    void DrawMaterialSphereWindow();
    // 素材プレビューに重ねるライトのギズモ（ビューポートと同じ絵）。
    void DrawMaterialSphereLightGizmo(const ImVec2& previewMin, const ImVec2& previewMax);
    // 天球パネル。一覧で選んだものがそのままビューポートの環境になる。
    // 天球一覧の右クリックメニュー（追加 / 複製 / 削除）。
    // 天球プレビューの窓（大きい絵 + 設定）。
    // 一覧のサムネイルをダブルクリックするか、ウィンドウメニューから開く。
    void DrawSkyPreviewWindow();
    void DrawAssetBrowser();
    void RefreshAssetBrowser();
    void ProcessAssetWork();
    void ProcessAssetSelections();
    AssetSelectionContext m_assetSelections;
    void DrawSceneSwitchDialog();
    void DrawAssetDeleteDialog();
    // 参照ビューア（UE5 の Reference Viewer に倣う）。中心のアセットから左へ参照元、右へ参照先を段ごとに並べる。
    // 右クリックの「関連を表示…」と「ウィンドウ」メニューから開く。ApplicationReferenceViewer.cpp。
    void OpenReferenceViewer(const std::filesystem::path& center);
    void DrawReferenceViewer();
    void LayoutReferenceViewer();
    void DestroyReferenceViewer();
    // 窓の中のアセット 1 行（サムネイル + パス）。ダブルクリックされたら true。
    bool DrawAssetRow(const std::filesystem::path& path, float size, bool fullPath);
    // 未保存のアセットを、保存されている内容へ戻す確認。
    void DrawAssetRevertDialog();
    void DrawSceneDuplicateDialog();
    // アセット 1 つを、ファイルの内容へ戻す。**読み込みを伴うのでフレームの外で呼ぶ。**
    void RevertAsset(const std::filesystem::path& path);
    bool IsAssetLoaded(const std::filesystem::path& path) const;
    // 一覧のサムネイルを受け取るドロップ先。フォルダ階層と一覧のフォルダに置く。
    void SyncLoadedMaterialThumbnails();
    void AssetFolderDropTarget(const std::filesystem::path& directory);
    // 同じ種類のアセットを選ぶピッカー。削除確認の「代わり」と、シーンの天球の差し替えで使う。
    void DrawAssetPicker();
    void CollectAssetPickerCandidates();
    void OpenAssetPicker(io::AssetKind kind, const std::filesystem::path& folder,
                         const std::filesystem::path& exclude);
    // シーンの天球を別の .tgsky に差し替えるピッカーを開く。
    void OpenSkyPicker();
    // 一覧と確認ダイアログで使うサムネイル。読み込み済みならその絵、無ければ一覧用の生成物。
    ImTextureID AssetThumbnailHandle(const std::filesystem::path& path);
    bool IsAssetSelected(const std::filesystem::path& path) const;
    // クリックで選ぶ。Ctrl で追加 / 除外、Shift で起点からの範囲。
    void SelectAsset(const std::filesystem::path& path, bool toggle, bool range);
    // 選択中のファイルを順に削除確認へ回す。
    void QueueAssetDelete();
    void OpenAssetRename(const std::filesystem::path& path);
    // その場の名前入力を終える。commit なら入力した名前で改名を予約する（拡張子は元のまま）。
    void FinishAssetRename(bool commit);
    // 移動・改名したアセットの、読み込み済みの絶対パスを付け替える（フォルダなら配下も）。
    void RelinkAssetPaths(const std::filesystem::path& from, const std::filesystem::path& to);
    // 日時モードなら観測地と日時から太陽・月・星空の回転を計算して設定へ書き込む。
    void ApplyCelestialSettings();
    void ResumeSceneSwitch();
    // --- リンク切れの解消 ---------------------------------------------------
    // ファイルを選ぶダイアログを出し、選ばれたら再リンクを予約する
    // （読み込みは GPU 待機を伴うのでフレームの外で行う）。
    void RequestTextureRelink(compositor::TextureId id);
    // フォルダを選び、そこにあるリンク切れのファイル名をまとめて繋ぎ直す予約をする。
    // 素材のフォルダごと移した（別の PC で開いた）ときの入口。
    void RequestTextureRelinkFolder();
    // 予約した再リンクを処理する。繋ぎ直せたら、参照しているサムネイルと合成を作り直す。
    void ProcessPendingTextureRelinks();
    // マテリアルが参照しているテクスチャのどれかがリンク切れか。一覧の目印に使う。
    bool MaterialHasMissingTexture(const compositor::MaterialAsset& asset) const;
    // テクスチャプレビューの窓（拡大表示 + 詳細）。
    // 一覧のサムネイルをダブルクリックするか、ウィンドウメニューから開く。
    void DrawTexturePreviewWindow();
    // 「チャンネル」パネル（合成結果のチャンネルを 1 枚の絵で見る）。ApplicationChannelPreview.cpp。
    void DrawChannelPreviewPanel();
    void PrepareChannelPreview(ID3D12GraphicsCommandList* commandList);
    void DestroyChannelPreview();
    // アプリの設定ウィンドウ（ウィンドウ > 設定）。プロジェクトに保存しない設定を置く。
    void DrawSettingsWindow();
    // 合成結果を画像へ書き出すウィンドウ（ファイル > テクスチャを書き出す…）。
    void DrawExportWindow();
    // 開発用オプション（スクリーンショット / 書き出し / 保存）で動いているか。
    // 真のときはフレームレートを落とさない。
    bool Headless() const;
    // 選択したノードの設定の行（グラフのパネルの下半分）。selected が nullptr なら案内だけ。
    void DrawNodeProperties(graph::Node* selected);
    // --evaluate-report: 評価が落ち着いたか（本体の評価・雲や雪煙のマスク・配置の点の数まで）。
    bool EvaluationSettled(bool evaluationIdle);
    // --evaluate-report: レポートを書く。書けなければ偽。reportOk は問題が無かったか。
    bool WriteEvaluationReport(bool timedOut, bool& reportOk);
    // 読み込みから溜めた警告とエラー（--evaluate-report のときだけ）。
    std::vector<std::pair<LogLevel, std::string>> m_reportLog;
    // 読み込みで読まなかった / 直した設定（io::SetGraphReadIssueSink の記録先）。
    std::vector<io::GraphReadIssue> m_reportReadIssues;

    // --- 設定の行の範囲（ノードカタログと評価のレポート。ApplicationProbe.cpp） ---
    // 見えないウィンドウでノードの複製の設定を描き、行の範囲・既定値・書式を記録する。フレームの中で呼ぶ。
    std::vector<ui::PropertyRecord> RecordNodeProperties(graph::Node& node);
    bool CanTouchProperty(const graph::Node& node, const ui::PropertyRecord& record) const;
    // 記録した行が編集する値の保存のキー（"." 区切り）。値を動かして書き出しを比べて求める。
    // 求まらなければ空。
    std::string FindPropertyPath(graph::Node& node, const ui::PropertyRecord& record,
                                 const nlohmann::json& baseFlat);
    nlohmann::json DescribeProperty(graph::Node& node, const ui::PropertyRecord& record,
                                    const std::string& path);
    // 種類の設定の行を、選択肢やオン・オフを切り替えて辿りながら集める（ノードカタログ用）。
    nlohmann::json ProbeNodeParameters(const graph::Node& node);
    // --dump-catalog の行の収集と、--evaluate-report の範囲の検査。DrawUi の最後で 1 度だけ行う。
    void RunPropertyProbes();
    // io::NodeCatalog に、集めた設定の行（parameters）を合わせたノードカタログ。
    nlohmann::json BuildNodeCatalog();
    nlohmann::json m_catalogParameters = nlohmann::json::object();  // kind → parameters
    std::vector<nlohmann::json> m_reportRangeIssues;
    bool m_propertyProbeRequested = false, m_propertyProbeDone = false;
    int m_probeId = 0;
    uint32_t m_reportSettledFrames = 0;
    std::chrono::steady_clock::time_point m_reportStart = std::chrono::steady_clock::now();
    // 設定から決まる UI の拡大率。追従なら Windows の表示スケール。
    float DesiredUiScale() const;
    // 拡大率を掛けた既定のクライアント領域。1920x1080 を拡大率倍したもの。
    // 追従を入れたときに作業面積（論理サイズ）が変わらないようにするため。
    uint32_t DefaultClientWidth() const;
    uint32_t DefaultClientHeight() const;
    // 設定に合わせて拡大率とウィンドウの大きさを反映する。フレームの外で呼ぶこと。
    void ApplyUiScale();
    // ファイルメニュー。要求を積むだけで、読み書きはフレームの外で行う。
    void DrawFileMenu();
    // キーボードショートカット（Ctrl+N / O / S / Shift+S）。メニューと同じ入口を通す。
    void HandleShortcuts();
    void RequestOpenProject();
    // 「最近使ったプロジェクト」。開く要求を積むだけ。
    void DrawRecentMenu();
    // saveAs が偽でも、まだ保存先が決まっていなければダイアログを出す。
    void RequestSaveProject(bool saveAs);
    void RequestSaveSelection();
    void ProcessSelectedAssetSave();
    bool IsAssetDirty(const std::filesystem::path& path) const;
    void RememberAssetStates(bool saved = false);
    std::map<std::filesystem::path, size_t> m_savedAssetStates;
    std::map<std::filesystem::path, size_t> m_assetStates;
    std::vector<std::filesystem::path> m_pendingSelectedAssetSave;
    // シーン階層の項目 1 つだけを保存する要求（0 地形 / 1 雲 / 2 大気散乱スカイ / 3 シーン本体）。
    // シーン本体は変更の無い部品を書き直さず、まだ無い部品だけ作る。
    // まだ保存先の無いシーンでは、通常の保存（保存先の問い合わせ）へ回す。
    void RequestComponentSave(int item);
    // 現在の内容を最後に保存 / 読み込みした内容と比べ、未保存の項目を m_sceneDirty へ入れる。
    // グラフの書き出しを通すので毎フレームは呼ばず、編集の確定・ドラッグの終わり・保存の後で呼ぶ。
    void RefreshSceneDirty();
    // mask（kDirty* のビット）の項目を「いま保存した内容」として覚える。
    void MarkSceneSaved(unsigned mask);
    // 未保存の項目名を「、」で繋いだ文字列。確認ダイアログに出す。
    std::string UnsavedItemNames() const;
    void SaveSceneThumbnail(const std::filesystem::path& path);
    // 画面下端のステータスバー。操作モード・評価中の状態と直近の通知を出す。
    // ドックスペースより前に呼ぶこと（作業領域をバーのぶん狭める）。
    void DrawStatusBar();
    // ログをステータスバーへ流す。Initialize で SetLogSink に登録する。
    void PushStatus(LogLevel level, const char* text);
    // エクスプローラから落とされたファイルを、拡張子で行き先へ振り分ける。
    void HandleDroppedFiles(const std::vector<std::filesystem::path>& paths);
    // プロジェクトとマテリアルの読み書き、テクスチャの追加と削除。
    // どれも GPU 待機を伴うため、フレームの外（Run のフレーム前）で呼ぶ。
    void ProcessPendingFileWork();
    // 中身を空にして作り直す。プロジェクトを開く前と「新規」で使う。
    void ResetProject();
    // ウィンドウタイトルを「プロジェクト名 - Terrain Graph」に揃える。
    void UpdateWindowTitle();
    // 参照している箇所の数だけを数える。毎フレーム呼ぶので文字列は作らない。
    size_t CountTextureUsers(compositor::TextureId id) const;
    // --- アンドゥ -----------------------------------------------------------
    // 対象はグラフ（ノード / リンク / 設定 / 位置）とマテリアル。
    // テクスチャの読み込みと削除、ペイントの筆致、プレビュー設定は含めない
    // （前者 2 つは GPU リソースそのもの、ペイントは PaintMaskStore が別の履歴を持つ）。
    // ノードの移動だけでは段を積まない（位置は他の変更の段に相乗りする）。
    //
    // いまの文書を写し取る。
    DocumentSnapshot CaptureDocument() const;
    // 写し取った文書を書き戻す。**マテリアルの破棄を伴うのでフレームの外で呼ぶ。**
    void ApplyDocument(const DocumentSnapshot& snapshot);
    // レイヤーかマテリアルを変えたときに呼ぶ。フレームの終わりに 1 段積まれる。
    void MarkDocumentChanged(bool terrainChanged = true);
    // 文書からも履歴からも参照されなくなったペイントマスクを破棄する。
    // レイヤーを消してもすぐには捨てないため、ここで回収する。
    void SweepPaintMasks();
    // 存在しないテクスチャ ID を「なし」に落とす。
    // テクスチャは履歴の外で消えるため、書き戻した参照が宙に浮くことがある。
    compositor::TextureId ValidTexture(compositor::TextureId id) const;
    // ペイントの対象になるレイヤー。ペイントモードで、選択中のレイヤーが
    // ペイントマスクを持つときだけ返す。
    compositor::MaterialLayer* CurrentPaintLayer();
    // レイヤーパネルのマスク欄に出すペイント関連の UI。
    bool DrawPaintSection(compositor::MaterialLayer& layer);
    // ビューポートに重ねる操作（表示モードと、重ねる情報の切り替え）。
    // 画像の描画より後に呼ぶ。右上には FPS を出すので、右端の座標も渡す。
    void DrawViewportOverlay(const ImVec2& viewportMin, const ImVec2& viewportMax);
    // ビューポート上の L + 左ドラッグでライトの向きを変える。
    // 掴んでいる間は true を返す（軌道やブラシへ渡さない）。
    bool HandleLightDrag(bool itemActive);
    // ビューポート上の F / A キーで視点をメッシュへ戻す。
    void HandleCameraShortcuts(bool itemHovered);
    // ライトの向きを示すギズモ。動かしている間と、その直後だけ出す。
    bool HandleCloudTransformGizmo(bool itemActive, bool itemHovered, const ImVec2& viewportMin, const ImVec2& viewportMax);
    ui::AxisTranslationDrag m_cloudTransformDrag;
    void DrawCloudShapeGizmo(const ImVec2& viewportMin, const ImVec2& viewportMax);
    void DrawLightGizmo(const ImVec2& viewportMin, const ImVec2& viewportMax);
    // ハイトの範囲。height 0 / 0.5 / 1 がワールドのどこに来るかを枠で示す
    // （ビューポート左上の `表示 > ハイトの範囲`。平面のときだけ描く）。
    void DrawHeightGuide(const ImVec2& viewportMin, const ImVec2& viewportMax);
    // ビューポート上のドラッグをブラシへ渡す。ペイントモードのときだけ呼ぶ。
    void HandlePaintInput(compositor::MaterialLayer& layer, bool itemActive,
                          const ImVec2& imageOrigin, const ImVec2& imageSize);

    // --- パスの編集（ApplicationPathEdit.cpp） --------------------------------
    // 編集の対象になる Path ノード。グラフで Path ノードを選んでいるときだけ返す
    // （ペイントと同じく、選択がビューポートの操作モードを決める）。
    graph::Node* CurrentPathNode();
    // ビューポート上の入力をパスの編集へ渡す。Path ノードが選ばれているときだけ呼ぶ。
    // 変更があれば文書の変更を記録する。
    void HandlePathInput(graph::Node& node, bool itemActive, bool itemHovered,
                         const ImVec2& viewportMin, const ImVec2& viewportMax);
    // パスの点と線をビューポートへ重ねて描く（ImGui。深度テストはしない）。
    void DrawPathOverlay(const graph::Node& node, const ImVec2& viewportMin,
                         const ImVec2& viewportMax);
    // カーソル位置を地形へ投影する。CPU 側のハイトへレイを飛ばして最初の交点を返す。
    // 地形に当たらなければ偽。
    bool PickTerrainUv(const ImVec2& mouse, const ImVec2& viewportMin, const ImVec2& viewportMax,
                       float& outU, float& outV) const;
    // ホイールのズームの中心にする、カーソルの下のワールドの点。地形に当たればその点、当たらなければ
    // 高さ 0 の水平面との交点。どちらにも当たらなければ偽（空を見上げているとき）。
    bool PickZoomPoint(const ImVec2& mouse, const ImVec2& viewportMin, const ImVec2& viewportMax,
                       DirectX::XMFLOAT3& outPoint) const;
    // --- 経路探索（ApplicationPathEdit.cpp） ---
    // Path ノードの Base に繋いだチェーンを 512² で焼いて、経路探索用の地形の写しにする。
    // 上流が前回と同じ（Height に効く状態のハッシュが同じ）なら焼かない。
    // GPU 待機を伴うので**フレームの外で呼ぶ**。Base が繋がっていなければ偽。
    bool BakePathRouteTerrain(const graph::Node& node);
    // 編集中の Path ノードの地形の写しを上流に追従させ、先送りにした再計算を片付ける。
    // フレームの外で呼ぶ。
    void ProcessPendingPathRoutes();
    // 経路探索が有効なエッジの経路を計算し直す。既定は経路が古い（両端が動いた / 未計算）
    // エッジだけ。force で全部、edges で対象を絞る。地形の写しがまだ無ければ先送りにし、
    // 次のフレームの前に焼いてから計算する。変更があれば文書の変更を記録する。
    void RecomputePathRoutes(graph::Node& node, bool force,
                             const std::vector<graph::PathElementId>* edges);
    // 正規化 UV をワールド座標へ。高さは CPU 側のハイトから引く。
    DirectX::XMFLOAT3 PathWorldPosition(float u, float v, float heightOffsetMeters) const;
    // 編集中のパスで選んでいる点（鎖なら両端と内側の点）の重心（ワールド）。選択が無ければ偽。
    // F キーで視点をそこへ寄せるのに使う。
    bool SelectedPathWorldCenter(DirectX::XMFLOAT3& outCenter) const;
    // Path ノードのプロパティ（グラフパネルのプロパティ欄から呼ぶ）。変更があれば true。
    bool DrawPathSettings(graph::Node& node);
    // --- Road Path（ApplicationRoadPath.cpp） ---
    // 平面の点とエッジの編集は Path と共通（上の関数が Road Path も扱う）。ここは縦断・バンク角。
    // 中心線（base: 地形 + ずれ、centerline: 縦断を反映）。高さは **Road Path の Base に繋いだ地形**
    // （RoadBaseHeightfield）から引く。最終出力ではないので、地形の均しなど下流の結果に左右されない。
    // Road Path でない / 線が 1 本でないときは偽で、error に理由を入れる。Base の地形がまだ
    // 評価できていないときも偽で、pending があれば真を入れる（前の形を保つ判断に使う）。
    bool BuildRoadCenterline(const graph::Node& node, graph::RoadProfileCurve& base,
                             graph::RoadProfileCurve& centerline, std::string* error,
                             bool* pending = nullptr) const;
    // Road Path ごとに、Base に繋いだチェーンを別の評価器（512²）で評価して高さを CPU へ写す。
    // フレームの外で呼ぶ（Road Path の無くなった評価器を捨てる）。評価そのものはフレームの中で
    // 配置の点の評価器と同じ順番で進める。
    void PrepareRoadBaseTerrain();
    // Road Path の Base の高さ（CPU 側の写し）。まだ評価できていなければ nullptr。
    const compositor::CpuHeightfield* RoadBaseHeightfield(graph::GraphId roadPathId) const;
    // Road Path の Base に線が繋がっているか。繋がっていなければ変位 0 の平面に沿う。
    bool RoadBaseConnected(const graph::Node& node) const;
    // 縦断曲線とバンク角のプロパティ（縦断図を含む）。変更があれば true。
    bool DrawRoadPathSettings(graph::Node& node);
    // 縦断を反映した中心線、切土・盛土の目安、縦断・バンクのポイントをビューポートへ重ねる。
    // ビューポートで縦断ポイントの菱形を掴む（HandlePathInput より先に呼ぶ）。カーソルが菱形の上か
    // 掴んでいる間は真を返し、その間はパスの編集にクリックを渡さない。
    bool HandleRoadProfileInput(graph::Node& node, bool itemHovered, const ImVec2& viewportMin,
                                const ImVec2& viewportMax);
    void DrawRoadPathOverlay(const graph::Node& node, const ImVec2& viewportMin,
                             const ImVec2& viewportMax);

    Window m_window;
    bool m_deviceLostNotified = false;  // GPU のデバイスロストを知らせたか
    rhi::Device m_device;
    rhi::ShaderCompiler m_shaderCompiler;
    rhi::PipelineCache m_pipelineCache;
    renderer::PreviewRenderer m_renderer;
    // マテリアルプレビューの球。窓を開いている間だけ描く。
    renderer::MaterialSphere m_materialSphere;
    // 天球プレビューの球。同じく窓を開いている間だけ描く。
    renderer::SkySphere m_skySphere;
    // --- ノードグラフ -------------------------------------------------------
    // 合成はグラフが唯一の入口。グラフをレイヤー列へコンパイルして
    // 既存の GPU 評価器で評価する。m_graphStack はそのコンパイル結果。
    graph::NodeGraph m_graph = graph::NodeGraph::CreateDefault();
    compositor::MaterialStack m_graphStack;
    // 雲層の分布・天候層の雲量と雲種のマスク。地形プレビューと独立した 512×512 の評価。
    struct CloudMaskSlot {
        compositor::MaterialStack stack;
        std::vector<graph::CompiledGraph::MaskOpSource> sources;
        compositor::MaterialEvaluator evaluator;
        uint64_t graphRevision = 0, paintRevision = 0;
        graph::GraphId pin = 0;
        bool Ready() const { return !pin || evaluator.EvaluatedRevision() == stack.Revision(); }
        bool Idle() const { return !pin || (!evaluator.IsEvaluating() && Ready()); }
        uint64_t Revision() const { return pin ? evaluator.EvaluatedRevision() : 0; }
        uint32_t Srv() const {
            if (!pin) return UINT32_MAX;
            return Ready() ? evaluator.Textures().baseColor.SrvIndex() : UINT32_MAX-1;
        }
    };
    struct ModelPointSlot {
        compositor::MaterialStack stack;
        compositor::MaterialEvaluator evaluator;
        uint64_t graphRevision = 0, documentRevision = 0, paintRevision = 0, footprintRevision = 0;
    };
    std::unordered_map<graph::GraphId, std::unique_ptr<ModelPointSlot>> m_modelPoints;
    // Road Path ごとの Base の地形（ApplicationRoadPath.cpp）。Base に繋いだチェーンを 512² で
    // 評価し、評価器の CPU 側のハイトを中心線が読む。
    struct RoadBaseSlot {
        compositor::MaterialStack stack;
        compositor::MaterialEvaluator evaluator;
        uint64_t graphRevision = 0, paintRevision = 0;
        // 組んだレイヤー列のハッシュ。点を動かしただけ（Base は同じ）では評価し直さない。
        uint64_t stackHash = 0;
    };
    std::unordered_map<graph::GraphId, std::unique_ptr<RoadBaseSlot>> m_roadBaseSlots;
    // メッシュの足跡（Mask Mesh が読む）。鎖ごとの路面と路肩の三角形を Mask Mesh の ID で置く。
    // 中身が変わったら本体のスタックを改版して焼き直す（ペイントマスクと同じ扱い）。
    compositor::MeshFootprintStore m_meshFootprints;
    uint64_t m_meshFootprintRevisionSeen = 0;
    // 本体の評価器が点まで作っている元（出力のチェーンをプレビューしているときだけ）。
    // ここに無い元は m_modelPoints の評価器で作る。
    std::vector<graph::GraphId> m_mainPointSources;
    std::vector<graph::CompiledModelScatter> m_modelScatters;
    // Model Place（ユニークなモデルの配置）。配置の点を CPU で組み、Model Scatter の点と同じ並びの
    // 小さなテクスチャ（RGBA32_FLOAT、1024 x 2）へ上げて、同じインスタンス描画に乗せる。
    struct ModelPlaceSlot {
        rhi::GpuTexture points;
        uint64_t hash = 0;
        uint32_t count = 0;
    };
    std::vector<graph::CompiledModelPlace> m_modelPlaces;
    std::unordered_map<graph::GraphId, ModelPlaceSlot> m_modelPlaceSlots;
    std::unordered_map<std::string, std::unique_ptr<renderer::ModelPreview>> m_instanceMeshes;
    // Mesh Output が描くユニークなメッシュ（Road Mesh の路面）。
    renderer::GeneratedMeshes m_generatedMeshes;
    // 作った帯（路面か路肩）。key は形に効く値のハッシュで、変われば作り直す。stride は 1 行の頂点数。
    struct RoadStrip {
        uint64_t key = 0;
        renderer::MeshData mesh;
        uint32_t stride = 0;
        std::string error;  // 形を作れなかった理由
    };
    // Mesh Output ごと（鎖ごと）の作った形。路肩は作った順（内側から、左右それぞれ）。
    // 区画線（Lane Marking ごと）。種類ごと（中央線・外側線・車線境界線）のメッシュ。
    struct RoadMarkingCache {
        uint64_t key = 0;
        std::array<renderer::MeshData, graph::kRoadMarkingKindCount> meshes;
        std::string error;
    };
    struct RoadChainCache {
        RoadStrip road;
        std::vector<RoadStrip> shoulders;
        std::vector<RoadMarkingCache> markings;
        float lengthMeters = 0;
    };
    std::unordered_map<graph::GraphId, RoadChainCache> m_roadMeshCache;
    // ノードごとのプロパティに出す状態（毎フレーム作り直す）。
    struct RoadNodeStatus {
        std::string error, materialError;
        float lengthMeters = 0;
        size_t vertices = 0, triangles = 0;
    };
    std::unordered_map<graph::GraphId, RoadNodeStatus> m_roadNodeStatus;
    std::unordered_map<std::string, BoundaryAsset> m_boundaries;
    CloudMaskSlot m_cloudMasks[2]; // 0: 分布／雲量、1: 雲種。
    // Snow Plume ノードごとの Source マスク（512²）。雲のマスクと同じ評価の仕方。
    struct SnowPlumeSlot {
        CloudMaskSlot mask;
        uint64_t documentRevision = 0;
    };
    std::unordered_map<graph::GraphId, std::unique_ptr<SnowPlumeSlot>> m_snowPlumeMasks;
    std::vector<graph::CompiledSnowPlume> m_snowPlumes;
    // 雲と雪煙のコンパイル結果は、グラフの版（Revision）が変わったときだけ求め直す。
    // グラフを丸ごと入れ替えたら（読み込み・リセット・アンドゥ）版が偶然一致し得るので 0 へ戻す。
    graph::CompiledCloud m_compiledCloud;
    uint64_t m_compiledCloudRevision = 0;
    uint64_t m_snowPlumesRevision = 0;
    // マスクの再コンパイルと評価器の作成。作成に失敗したら偽。
    bool PrepareCloudMask(CloudMaskSlot& slot, graph::GraphId maskNode, graph::GraphId maskPin);
    uint64_t m_compiledGraphRevision = 0;
    // 循環に入っているノード（グラフの版ごとに求め直す）。グラフでエラー色の枠を付ける。
    const std::vector<graph::GraphId>& GraphCycleNodes();
    std::vector<graph::GraphId> m_graphCycleNodes;
    uint64_t m_graphCycleRevision = 0;
    // 前回コンパイルしたプレビュー対象。選択が変わっても再コンパイルするために持つ。
    graph::GraphId m_compiledGraphTarget = 0;
    graph::GraphId m_compiledGraphTargetPin = 0;
    // コンパイルした op の出どころを、スタックの版ごとに控える。評価は非同期なので、
    // 表側にある結果はいまのコンパイルより古いことがある。ノードのサムネイルは
    // 「表側の結果の版」に合う対応で引かないと、別のノードの模様が出る。
    struct GraphMaskOpSources {
        uint64_t revision = 0;
        std::vector<graph::CompiledGraph::MaskOpSource> ops;
        // レイヤーごとの元ノード（添字はレイヤーの添字）。結果サムネイル用。
        std::vector<graph::GraphId> layers;
    };
    std::vector<GraphMaskOpSources> m_graphMaskOpSources;
    graph::GraphId m_selectedGraphNode = 0;
    // 次にエディタを描くときに選ぶノード（--select-node）。エディタ側の選択も合わせないと、
    // 毎フレームの選択同期（未選択 → 0）に消されてしまう。
    graph::GraphId m_pendingSelectGraphNode = 0;
    // 真なら、今の選択を外してから選ぶ（シーン階層の出口の行から）。偽なら、選択があれば譲る。
    bool m_pendingSelectReplace = false;
    // フライ（UE5 と同じ）: ビューポートで右ボタンを押している間、マウスで見回し WASD / QE で動く。
    // 動かさずに放したら右クリック（パスのメニューなど）として扱う。
    struct FlyCamera {
        bool held = false;     // ビューポートで右ボタンを押している
        bool active = false;   // この押下でフライになった（見回したか、キーで動いた）
        bool flew = false;     // 直前の押下がフライだった（放したときの右クリックを出さない）
        float dragPixels = 0;  // 押してから動いた量（フライになるまで）
        int anchorX = 0, anchorY = 0;  // 押した位置（画面座標）。見回す間はここへカーソルを戻す
        float speedScale = 1;          // ホイールで変える速さの倍率
        double speedShownUntil = 0;    // 速さを表示する時刻（ImGui::GetTime）
    } m_fly;
    // フライの入力。ビューポートの不可視ボタンの直後に呼ぶ。フライ中なら true。
    bool HandleFlyCamera(bool itemActive, bool enabled);
    // フライの速さ（m/s、Shift を含まない）。地形の大きさから決める基準 × 倍率。
    float FlySpeed() const;
    // エディタで選ばれているノード全部。コピーはこれを見る
    // （プロパティに出すのは先頭の 1 つ = m_selectedGraphNode）。
    std::vector<graph::GraphId> m_selectedGraphNodes;

    // ノードのコピー元。**OS のクリップボードは使わない**（アプリ内だけ）。
    // 位置と設定に加えて、**入力ピンごとの接続元**を覚える。
    // コピーした集合の中を指していれば貼った側どうしで繋ぎ直し、
    // 外を指していれば**元の親へ繋いだまま**にする（別の文書へ貼るときは繋がない）。
    struct GraphClipboardNode {
        graph::NodeKind kind = graph::NodeKind::Surface;
        graph::NodeSettings settings;
        float posX = 0.0f;
        float posY = 0.0f;
        // コピーした時点のノードの大きさ。貼るときに集合の中心を出すのに使う
        // （位置だけだと左上しか分からず、画面中央に寄せると右下へずれる）。
        float sizeX = 0.0f;
        float sizeY = 0.0f;
        std::string note;
        bool bypass = false;
        struct Source {
            int copiedIndex = -1;              // コピーした集合の中の添字
            graph::GraphId externalPin = 0;    // 集合の外なら、その出力ピン
        };
        std::vector<Source> inputs;
    };
    std::vector<GraphClipboardNode> m_graphClipboard;
    // コピー元の文書の印（NodeGraph::Identity）。貼る先と違えば別のシーンから来たノードで、
    // ID で持つ参照（テクスチャ・マテリアル・モデル）を下の控えから引き直す。
    uint64_t m_graphClipboardIdentity = 0;
    // コピーした時点の参照の中身。キーはコピー元での ID。
    struct GraphClipboardAsset {
        std::filesystem::path path;  // テクスチャは画像、マテリアルとモデルはアセットのファイル
        std::string uid;
        std::string name;
    };
    std::unordered_map<compositor::TextureId, GraphClipboardAsset> m_graphClipboardTextures;
    std::unordered_map<compositor::MaterialAssetId, GraphClipboardAsset> m_graphClipboardMaterials;
    std::unordered_map<uint64_t, GraphClipboardAsset> m_graphClipboardModels;
    // 別の文書へ貼るのは、参照の読み込み（GPU 待機を伴う）があるのでフレームの外で行う。
    // 値は貼る先のキャンバスの中央。
    std::optional<ImVec2> m_pendingGraphPaste;
    // 貼るたびに位置をずらす回数。コピーし直すと 0 に戻す。
    int m_graphPasteCount = 0;
    // メモの印（か省略したメモ）にカーソルが載っているノード。ed::End の後でツールチップを出す。
    graph::GraphId m_graphNoteHover = 0;
    // カーソルを載せているピンと、載せ始めた時刻（少し待ってから説明を出す）。
    graph::GraphId m_graphPinHover = 0;
    double m_graphPinHoverTime = 0.0;
    // ビューポートに出しているノード。**選択とは別に持つ。**
    // 結果を見ながら別のノードのプロパティをいじれるようにするため
    // （terrain-editor と同じ作法）。0 は出力ノードのチェーン。
    graph::GraphId m_previewGraphNode = 0;
    // 表示フラグを下ろした出口ノード（Houdini の表示フラグの逆）。作業中だけの切り替えで、
    // シーンにもアプリの設定にも保存しない。シーンを開き直すと全部表示へ戻る。
    std::unordered_set<graph::GraphId> m_hiddenOutputs;
    // リファレンス表示にした出口ノード（Output / Mesh Output）。表示フラグと同じく保存しない。
    // 表示フラグで隠していれば描かない（隠すほうが勝つ）。
    std::unordered_set<graph::GraphId> m_referenceOutputs;
    // シーン階層で、グラフの行の下に出口ノードの行を開いているか（0 地形 / 1 雲）。保存しない。
    bool m_hierarchyOutputsOpen[2] = {true, true};
    // プレビューしている出力ピン。0 なら最初の出力。
    // **堆積は Result と Mask を出す**ので、ノードだけでは決まらない。
    graph::GraphId m_previewGraphPin = 0;
    // 出力ピンの「クリック」を拾うための押した位置。ドラッグ（リンク作成）と
    // 区別するために、押した / 離したが同じピンで、ほとんど動いていないときだけ
    // クリックとみなす。
    graph::GraphId m_graphPressedPin = 0;
    ImVec2 m_graphPressedPinPos{};
    ax::NodeEditor::EditorContext* m_nodeEditor = nullptr;
    uint32_t m_presetEditorId = 0;
    int m_selectedPresetLayer = 0;
    std::string m_surfacePresetError;
    // グラフパネル内の「エディタ / プロパティ」境界の高さ（96 DPI 基準）。
    float m_graphEditorHeight = 380.0f;
    // 位置をエディタへ流し込むべきノード。作成・読み込みのときに積む。
    std::vector<graph::GraphId> m_graphNodesToPlace;
    // 位置を流し込んだ後に全体を画面へ収めるまでの残りフレーム数。
    // **キャンバスの大きさが安定しているフレームだけ数える。** エディタは
    // サイズ変化のたびに前の表示領域を復元するので、ドックの確定前に寄せると
    // 上書きされて効かない。
    int m_graphNavigateCountdown = 0;
    // グラフのタブ（0 = 地形、1 = 雲）ごとの、キャンバスの表示位置とズーム（エディタの設定 JSON の
    // "view" だけ）。タブを切り替えるとエディタを作り直すので、戻ったときに同じ所を出すために持つ。
    // 空なら全体を画面へ収める。グラフが丸ごと入れ替わったら捨てる。保存はしない。
    std::array<std::string, 2> m_graphViewStates;
    // タブバーが前のフレームに出していたタブ（外から編集対象が変わったときに選び直すため）。
    int m_graphTabShown = -2;
    ImVec2 m_graphCanvasSize = ImVec2(0.0f, 0.0f);
    compositor::TextureLibrary m_textureLibrary;
    compositor::MaterialLibrary m_materialLibrary;
    // モデルの編集データと、履歴に含めないGPUプレビュー。
    std::vector<renderer::ModelAsset> m_models;
    uint64_t m_nextModelId = 1;
    uint64_t m_selectedModel = 0;
    int m_modelLod = 0;
    // インポスター。作成・削除と画像の読み込みはフレームの外（ProcessModelWork）で行う。
    renderer::ImpostorLibrary m_impostors;
    uint64_t m_pendingImpostorBake = 0, m_pendingImpostorDelete = 0;
    bool m_modelShowImpostor = false;  // モデルプレビューを焼いた画像で描く（確認用、保存しない）
    bool m_showModelPreview = false;
    bool m_modelPreviewVisible = false;
    std::vector<std::filesystem::path> m_pendingModels;
    std::unordered_map<uint64_t, std::unique_ptr<renderer::ModelPreview>> m_modelPreviews;
    std::unordered_set<uint64_t> m_renderedModelThumbnails;
    // 天球アセット。マテリアルと並ぶアセットだが、**アンドゥの対象には入れない。**
    // 環境は作っているマテリアルそのものではなく、見え方の設定に近い
    // （プレビュー設定を履歴に載せないのと同じ理由）。
    renderer::SkyLibrary m_skyLibrary;
    compositor::PaintMaskStore m_paintMasks;
    int m_selectedMaterial = 0;
    compositor::MaterialAsset m_materialEditDraft;
    bool m_materialEditPending = false;
    bool m_materialEditAppearanceChanged = false;
    // ORD をまとめて割り当てるときに選ぶテクスチャ（UI の一時状態）。
    compositor::TextureId m_ordTexture = compositor::kNoTexture;
    compositor::BrushSettings m_brush;
    // ペイントモード中はビューポートの左ドラッグがブラシになる。
    bool m_paintMode = false;
    // ライトの向きを掴んでいる間。ギズモは離してからも少しの間だけ残す。
    bool m_lightDragActive = false;
    // 日時モードの L + ドラッグで時刻を変えたか。離したときに 1 段だけアンドゥへ積む。
    bool m_lightDragChangedTime = false;
    double m_lightGizmoUntil = 0.0;
    // 素材プレビューの L＋ドラッグでも同じギズモを出す。ビューポートとは別の窓なので
    // 消えるまでの時刻を別に持つ。
    double m_materialLightGizmoUntil = 0.0;
    // ストローク中の状態。前フレームのカーソル位置から線分としてブラシを積む。
    bool m_strokeActive = false;
    float m_strokeLastX = 0.0f;
    float m_strokeLastY = 0.0f;

    // パスの編集の状態。ノードが変わったら捨てる。
    // 点 / エッジの ID はそのパスの中でしか意味を持たないので、毎フレーム実在を確かめる。
    struct PathEditState {
        graph::GraphId nodeId = 0;
        // 選択している点。プロパティの編集と Delete の対象で、**伸ばす起点**でもある
        // （Ctrl + クリックは先頭の点から伸びる）。
        std::vector<graph::PathElementId> selected;
        // 選択しているエッジ。1 本（クリック）か鎖（ダブルクリック）。
        // 点の選択とは排他（Delete の意味を曖昧にしないため）。
        std::vector<graph::PathElementId> selectedEdges;
        // 鎖を選んだときの、その内側の点（両端を除く）。Delete で一緒に消す。
        std::vector<graph::PathElementId> selectedStrandInterior;
        // ホバー中の点 / エッジ（エッジは最寄りの位置 t も）。
        graph::PathElementId hoverPoint = 0;
        graph::PathElementId hoverEdge = 0;
        float hoverEdgeT = 0.0f;
        // ドラッグ中の点。押した位置から動いたら移動、動かなければクリック。
        graph::PathElementId dragPoint = 0;
        bool dragging = false;
        bool dragMoved = false;
        ImVec2 pressPos{};
        // ドラッグ中の吸着先（点が優先、無ければエッジ）。
        graph::PathElementId snapPoint = 0;
        graph::PathElementId snapEdge = 0;
        float snapEdgeT = 0.0f;
        // 移動ギズモ。選択（点の集合 / 鎖）の重心に置き、X（u）/ Z（v）の軸と中央の
        // 平面ハンドルで選択をまとめて動かす。gizmoAxis は 0 = X、1 = Z、2 = 平面、-1 = 無し。
        int gizmoHover = -1;
        int gizmoAxis = -1;
        bool gizmoDragging = false;
        ImVec2 gizmoPressPos{};
        // 平面ハンドルで掴んだときの地形上の UV。動かす量はここからの差。
        float gizmoPressU = 0.0f;
        float gizmoPressV = 0.0f;
        // 掴んだときの各点の位置。差分を足すのではなく、ここから置き直す（丸め誤差を溜めない）。
        struct GizmoStart {
            graph::PathElementId id = 0;
            float u = 0.0f;
            float v = 0.0f;
        };
        std::vector<GizmoStart> gizmoStart;
        // 右クリックしたときの対象（メニューを描くフレームでは状況が変わっているため控える）。
        graph::PathElementId menuPoint = 0;
        graph::PathElementId menuEdge = 0;
        float menuEdgeT = 0.0f;
        bool menuOnTerrain = false;
        float menuU = 0.0f;
        float menuV = 0.0f;
    };
    PathEditState m_pathEdit;
    // Road Path の縦断図での縦断ポイントの編集。ノードが変わったら捨てる。
    // ポイントの ID はそのノードの中でしか意味を持たないので、毎フレーム実在を確かめる。
    struct RoadProfileEditState {
        graph::GraphId nodeId = 0;
        // 選んでいる縦断ポイント。プロパティに出すのはこれだけ。
        graph::PathElementId selected = 0;
        // ドラッグ中のポイント。掴んだ点と取っ手（交点）のずれを保ち、押した所へ跳ばないようにする。
        graph::PathElementId dragging = 0;
        float grabDistance = 0.0f;
        float grabHeight = 0.0f;
        // 図の縦の範囲（ワールドの高さ）。ドラッグ中は固定する（動かすたびに範囲が広がって点が逃げないように）。
        float plotLow = 0.0f;
        float plotHigh = 0.0f;
        // ビューポートの菱形。カーソルの下のポイントと、掴んでいるポイント（縦断図のドラッグとは別に持つ。
        // 図は自分の InvisibleButton が非アクティブなら dragging を 0 へ戻すため）。
        // 掴み方は押したときに決める: そのままなら道なり（u）、Shift なら高さ（offsetMeters）。
        graph::PathElementId viewportHover = 0;
        graph::PathElementId viewportDrag = 0;
        bool viewportDragHeight = false;
        float viewportGrabDistance = 0.0f;  // 掴んだ点の道のり − カーソルの道のり（跳ばないように保つ）
        float viewportGrabHeight = 0.0f;    // 掴んだ点の高さ − カーソルの高さ
        // V を押している間は挿入の構え（離すと挿入、Esc でやめる）。入る位置（中心線上の道のり）を控え、
        // 重ね描きが同じフレームの番号のときだけ印を出す（入力が呼ばれないフレームに残さない）。
        bool viewportInsertArmed = false;
        int viewportInsertFrame = -1;
        float viewportInsertDistance = 0.0f;
        // 縦断ポイントの自動作成の設定（保存しない。ノードを替えても持ち越す）と、直前の結果の報告。
        graph::RoadVerticalAutoParams autoParams;
        std::string autoReport;
    };
    RoadProfileEditState m_roadProfileEdit;
    // パスのクリップボード（アプリ内）。鎖や点の集合をコピーして、カーソルの所へ貼る。
    // 別の Path ノードへも貼れる。
    graph::PathClip m_pathClipboard;
    // 鎖の設定（曲線 / 幅 / 蛇行 / 経路探索）のクリップボード。選んだ鎖へ上書きで貼る。
    std::optional<graph::PathEdgeStyle> m_pathStyleClipboard;
    // 鎖をクロソイドの制御点へ置き換えるときの許容誤差（m）。作業中の値で、保存しない。
    float m_pathFitToleranceMeters = 2.0f;
    // 経路探索が読む地形。**Path ノードの Base に繋いだチェーン**を、プレビューとは別に
    // 焼いた Height の写し（プレビューが別の地形を見ていても Base を使う）。
    // 上流を変えても経路は勝手に作り直さないが、写し自体は編集中に追従させておく
    // （次の編集や再計算のボタンが今の地形を使えるように）。
    struct PathRouteTerrainCache {
        graph::GraphId nodeId = 0;
        uint64_t stackHash = 0;        // 焼いたときの上流の Height に効く状態
        uint64_t checkedRevision = 0;  // この改版で上流を確かめた（改版ごとに 1 回）
        bool valid = false;
        compositor::CpuHeightfield heightfield;
        // Avoid に繋いだマスク（登山道が避ける所）。繋いでいなければ空。
        // 地形と同じ解像度に揃えてある。
        compositor::CpuHeightfield avoid;
        float sizeMeters = 1024.0f;
        float heightMeters = 200.0f;
    };
    PathRouteTerrainCache m_pathRouteTerrain;
    // 経路探索用の評価器（512²、同期）。最初に使うときに作る。
    compositor::MaterialEvaluator m_pathRouteEvaluator;
    // フレームの中で要求されたが、地形の写しが無くて先送りにした再計算。
    struct PathRouteRequest {
        bool pending = false;
        graph::GraphId nodeId = 0;
        bool force = false;
        std::vector<graph::PathElementId> edges;  // 空なら全部
    };
    PathRouteRequest m_pathRouteRequest;
    int m_selectedTexture = 0;
    // 拡大プレビューで出すチャンネル。0 = RGB、1..4 = R / G / B / A。
    // ORD のように 1 枚へ複数のマップを詰めたテクスチャの中身を確かめるためのもの。
    int m_previewChannel = 0;

    // ステータスバーに出す直近の通知。ログから受け取る。
    // 時刻は ImGui に依存させない（ログはコンテキストが無い時期にも来る）。
    struct StatusMessage {
        std::string text;
        LogLevel level = LogLevel::Info;
        std::chrono::steady_clock::time_point time{};
        bool valid = false;
    };
    StatusMessage m_status;
    // 読み込みは GPU 待機を伴うため、フレームの外で処理する。
    std::vector<std::filesystem::path> m_pendingTexturePaths;

    // --- ファイル操作の保留 -------------------------------------------------
    // ダイアログはフレームの中で出すが、読み書きは GPU 待機を伴うので、
    // 選ばれたパスをここへ積んでおき、次のフレームの頭で処理する。
    io::ProjectWorkspace m_workspace;
    AssetThumbnailCache m_assetThumbnails;
    // 保存済みのサムネイルを書き直す候補（読み込み済みのマテリアル）。保存・移動・削除のあとに積み、
    // 1 フレームに 1 つずつ確かめる（SyncLoadedMaterialThumbnails）。
    std::vector<compositor::MaterialAssetId> m_materialThumbnailSync;
    std::filesystem::path m_assetDirectory;
    std::filesystem::path m_pendingAssetReveal, m_assetRevealTarget;
    std::vector<std::filesystem::directory_entry> m_assetEntries;
    // 一覧で選んでいるもの（複数）。Shift の範囲選択は m_assetSelectionAnchor を起点にする。
    std::vector<std::filesystem::path> m_selectedAssets;
    std::filesystem::path m_assetSelectionAnchor;
    // DEL で複数を削除するときの残り。確認ダイアログを 1 件ずつ出す。
    std::vector<std::filesystem::path> m_assetDeleteQueue;
    // 名前の変更。一覧のサムネイルの下でその場で入力し、確定分をフレームの外で処理する。
    // m_assetRenameTarget が空でなければ編集中。m_assetRenameFocus は入力欄が掴むまで立てておく。
    std::filesystem::path m_assetRenameTarget;
    bool m_assetRenameFocus = false;
    // 左のフォルダ階層の行で編集しているとき true（一覧の同じフォルダには欄を出さない）。
    bool m_assetRenameInTree = false;
    // シーン階層の行で編集しているとき true（アセットブラウザには欄を出さず、取り消しもしない）。
    bool m_assetRenameInHierarchy = false;
    char m_assetRenameBuffer[256] = {};
    // 選択済みの項目の名前をもう一度クリックしたときの改名待ち（エクスプローラと同じ）。
    // ダブルクリックの猶予が過ぎても他の操作が無ければ改名に入る。
    std::filesystem::path m_assetRenameArmed;
    double m_assetRenameArmedTime = 0.0;
    std::filesystem::path m_pendingAssetRename;
    std::string m_pendingAssetRenameName;
    // マテリアル・天球・モデルの名前はファイル名（拡張子なし）と同じにする。
    // 名前欄の編集はファイルの改名として扱い、ファイルを持つアセットの名前は毎フレーム
    // ファイル名から引き直す。ファイルがまだ無いものは名前をそのまま持つ。
    void RequestAssetRename(const std::filesystem::path& assetPath, const std::string& newStem);
    void SyncAssetNamesToFiles();
    std::filesystem::path m_pendingAssetDeleteInspect;
    io::AssetRelations m_assetDeleteRelations;
    bool m_assetDeleteDialog = false;
    // 参照ビューア。対応表はワークスペース全体を読むので、描画の外（削除の確認と同じ所）で作る。
    bool m_showReferenceViewer = false;
    ax::NodeEditor::EditorContext* m_referenceEditor = nullptr;
    io::AssetReferenceIndex m_referenceIndex;
    bool m_referenceIndexDirty = true;
    bool m_referenceLayoutDirty = true;
    std::filesystem::path m_referenceCenter;
    int m_referenceDepthIn = 2;   // 参照元（左）の段数
    int m_referenceDepthOut = 2;  // 参照先（右）の段数
    bool m_referenceFollowSelection = false;
    bool m_referenceFocus = false;  // 次に描くとき窓を前へ出す
    int m_referenceNavigateFrames = 0;  // 並べ直したあと、全体へ視点を合わせるまでのフレーム数
    ImVec2 m_referenceCanvasSize{};
    std::filesystem::path m_referenceFollowed;  // 追従で最後に中心へ据えた選択
    struct ReferenceViewerNode {
        std::filesystem::path path;
        int column = 0;  // 0 が中心、負が参照元、正が参照先
        ImVec2 position{};
    };
    std::vector<ReferenceViewerNode> m_referenceNodes;
    std::vector<std::pair<size_t, size_t>> m_referenceLinks;  // 参照する側 → 参照される側（m_referenceNodes の添字）
    size_t m_referenceHidden = 0;  // 箱の上限で出さなかった数
    bool m_pendingAssetDelete = false;
    // 「変更前に戻す」。確認の対象と、確定した戻す要求（フレームの外で処理する）。
    std::filesystem::path m_assetRevertTarget;
    bool m_assetRevertDialog = false;
    std::filesystem::path m_pendingAssetRevert;
    // 「シーンを複製…」。複製元のシーン、新しい名前の入力、失敗したときの理由。
    std::filesystem::path m_sceneDuplicateSource;
    bool m_sceneDuplicateDialog = false;
    char m_sceneDuplicateName[128] = {};
    std::string m_sceneDuplicateError;
    // 削除対象の代わりに参照元へ割り当てるアセット。空なら参照切れのまま削除する。
    std::filesystem::path m_assetReplacement;
    // 代わりを選ぶピッカー。候補は開いたときに集め、絞り込みの条件が変わったら集め直す。
    enum class AssetPickerPurpose { Replacement, SceneSky };
    AssetPickerPurpose m_assetPickerPurpose = AssetPickerPurpose::Replacement;
    io::AssetKind m_assetPickerKind = io::AssetKind::Other;
    std::filesystem::path m_assetPickerFolder;   // 「同じフォルダだけ」の基準
    std::filesystem::path m_assetPickerExclude;  // 候補から外すもの（対象自身）
    bool m_assetPickerOpen = false;
    bool m_assetPickerSameFolder = true;
    bool m_assetPickerRefresh = false;
    char m_assetPickerFilter[128] = {};
    std::vector<std::filesystem::path> m_assetPickerCandidates;
    std::filesystem::path m_assetPickerSelection;
    // ドラッグ＆ドロップで要求されたアセットの移動（移動元と移動先フォルダ）。
    // 読み込み済みアセットのパス差し替えを伴うのでフレームの外で処理する。
    std::vector<std::filesystem::path> m_pendingAssetMoves;
    std::filesystem::path m_pendingAssetMoveTarget;
    std::filesystem::path m_pendingRoot;
    nlohmann::json m_sceneAtmosphere;
    bool m_pendingWorkEnvironmentSave = false;
    bool m_pendingWorkSkySave = false;
    bool m_pendingAtmosphereSave = false;
    bool m_focusLighting = false;
    std::filesystem::path m_pendingAssetOpen;
    // プルダウンに出す、ルート内のファイルの一覧（拡張子で探す。ドットで始まるフォルダは見ない）。
    // プルダウンを開いた瞬間に探し直すので、アプリの外で足したファイルも「更新」を押さずに出る。
    const std::vector<std::filesystem::path>& DropdownFiles(const wchar_t* extension);
    // モデルを選ぶ行（Model Scatter / Model Place）。割り当ててあるモデルのサムネイルと、サムネイル付きの
    // プルダウン。未読み込みのモデルを選ぶと、読み込みを頼んで（m_pendingScatterModel）偽を返す。
    bool DrawModelSlotRow(const char* label, uint64_t& model, graph::GraphId nodeId, size_t choice);
    std::vector<std::filesystem::path> m_dropdownFiles;
    std::filesystem::path m_pendingScatterModel;
    graph::GraphId m_pendingScatterNode = 0;
    size_t m_pendingScatterChoice = 0;
    bool m_pendingAssetsSave = false;
    // 「天球を作成」で足した天球だけを残す要求。破棄は GPU 待機を伴うのでフレームの外で。
    bool m_pendingSkyKeepOnly = false;
    bool m_assetRefresh = true;
    std::filesystem::path m_deferredRoot;
    std::filesystem::path m_deferredScene;
    bool m_deferredNew = false;
    bool m_sceneSwitchDialog = false;
    bool m_allowSceneSwitch = false;
    bool m_saveThenSwitch = false;
    // 切り替えではなく終了の確認として同じダイアログを出している印。
    bool m_deferredExit = false;
    // 確認を済ませたので、次の閉じる要求はそのまま通す。
    bool m_allowClose = false;

    // --- 未保存の判定 -------------------------------------------------------
    // 項目のビット。地形 / 雲 / 大気散乱スカイは部品のファイル、シーン本体は .tgscene、
    // 共有はマテリアルとモデル（Ctrl+S でまとめて書く。階層には出さない）。
    static constexpr unsigned kDirtyTerrain = 1;
    static constexpr unsigned kDirtyCloud = 2;
    static constexpr unsigned kDirtyAtmosphere = 4;
    static constexpr unsigned kDirtyScene = 8;
    static constexpr unsigned kDirtyShared = 16;
    // 最後に保存 / 読み込みした内容の指紋。現在の内容と比べて未保存の印を出す。
    io::SceneFingerprint m_savedFingerprint;
    io::SceneFingerprint m_previewSavedFingerprint;
    io::SceneFingerprint m_currentFingerprint;
    // 未保存の項目（kDirty* のビット）。
    unsigned m_sceneDirty = 0;
    // ペイントの筆跡は指紋に映らないので、塗ったグラフを別に覚える（bit0 地形 / bit1 雲）。
    unsigned m_paintDirty = 0;
    // 項目単位の保存要求。ファイル入出力を伴うのでフレームの外で処理する。
    int m_pendingComponentSave = -1;
    // 前のフレームで掴んでいたウィジェット。離れたフレームに未保存の判定を更新する
    // （ノードの移動のように、アンドゥの段を積まない編集も拾う）。
    uint32_t m_lastActiveWidget = 0;
    nlohmann::json m_sceneComponents = nlohmann::json::array();
    int m_editComponent = -1;
    bool m_pendingComponentMigration = false;
    int m_componentPreview = -1;
    int m_pendingPreviewFinish = 0;
    graph::NodeGraph m_previewOriginalGraph;
    nlohmann::json m_previewOriginalComponents;
    std::filesystem::path m_componentPreviewPath;
    // 地形 / 雲グラフか大気散乱スカイのアセットを、現在のシーンへ配置する要求。
    // 差し替える部品に未保存の編集が無ければ一時プレビューを挟まずに差し替える。
    std::filesystem::path m_pendingComponentPlace;
    // 「シーンを作成」の保存先。新規シーンへ切り替えた直後にここへ保存して部品も作る。
    std::filesystem::path m_pendingSceneCreate;
    std::filesystem::path m_projectPath;  // 現在のプロジェクト。未保存なら空
    io::RecentFiles m_recentProjects;
    io::AppSettings m_settings;
    // 設定ウィンドウを出しているか。ドックへは収めない補助ウィンドウ。
    bool m_showSettings = false;
    // 情報ウィンドウ。必要なときだけウィンドウメニューから開く。
    bool m_showInfo = false;
    // マテリアルプレビューの窓。ドックへは収めない補助ウィンドウ。
    bool m_showMaterialSphere = false;
    // テクスチャプレビューの窓。同じくドックへは収めない。
    bool m_showTexturePreview = false;
    // 「チャンネル」パネルの状態。UI が要求（どのチャンネルを、どの範囲で）を置き、
    // 同じフレームの描画（PrepareChannelPreview）が表示用のテクスチャを焼く。保存しない。
    struct ChannelPreviewState {
        int mode = 0;
        // 表示する範囲。中心の UV と拡大率（1 で全体）。
        float centerU = 0.5f;
        float centerV = 0.5f;
        float zoom = 1.0f;
        // このフレームに焼くか（パネルが見えているときだけ真）。
        bool requested = false;
        bool imageReady = false;
        // カーソルの位置（UV）と、読み戻した値（表示用に直す前の 4 成分）。
        bool probeHovered = false;
        float probeU = 0.0f;
        float probeV = 0.0f;
        float probeValue[4] = {};
        bool probeValueValid = false;
        // 「水際からの距離」の等値線の間隔（m）。見えている範囲から毎フレーム決める。
        float contourMeters = 25.0f;
        rhi::GpuTexture image;
        rhi::GpuBuffer result;
        rhi::GpuBuffer readback;
        bool pending[rhi::kFrameCount] = {};
        bool pendingProbe[rhi::kFrameCount] = {};
    };
    ChannelPreviewState m_channelPreview;
    // ビューポートが入っているドックの ID（「チャンネル」を同じ枠へタブで入れるため）。
    ImGuiID m_viewportDockId = 0;
    // 天球プレビューの窓。同じくドックへは収めない。
    bool m_showSkyPreview = false;
    // その窓の中身をこのフレームに描いたか（折りたたまれていれば球も描かない）。
    bool m_skyPreviewVisible = false;
    // その窓の中身をこのフレームに描いたか。**折りたたまれていれば球も描かない。**
    // UI（DrawUi）はフレームの記録より前に走るので、その結果をここへ残して使う。
    bool m_materialSphereVisible = false;
    // 書き出しウィンドウ。設定ウィンドウと同じくドックへは収めない。
    bool m_showExport = false;
    io::ExportSettings m_exportSettings;
    // 書き出しの実行要求。**GPU 待機とファイル入出力を伴うのでフレームの外で処理する。**
    bool m_pendingExport = false;
    std::filesystem::path m_pendingProjectSave;
    std::filesystem::path m_pendingProjectOpen;
    // シーン読み込みは同期で、その間は画面が止まる。読み込む前に 1 フレームだけ描いて
    // ステータスバーに「読み込み中」を見せるための印。読み終えたら経過時間を通知する。
    bool m_sceneLoadAnnounced = false;
    // 直近のシーン読み込みに掛かった秒数。負なら未読み込み。通知は後続のログで
    // 流れて消えるので、ステータスバーには別枠で残す。
    float m_sceneLoadSeconds = -1.0f;
    std::filesystem::path m_pendingMaterialExport;
    std::filesystem::path m_pendingMaterialImport;
    compositor::MaterialAssetId m_pendingExportMaterial = compositor::kNoMaterialAsset;
    // 繋ぎ直しの予約（対象のテクスチャと新しいパス）。ダイアログで選んだものと、
    // フォルダ指定で見つけたものの両方がここへ積まれる。
    struct TextureRelink {
        compositor::TextureId id = compositor::kNoTexture;
        std::filesystem::path path;
    };
    std::vector<TextureRelink> m_pendingTextureRelinks;
    // 削除要求のあったマテリアル。一覧の描画中に消すと、描画側が erase 済みの
    // 要素を読んでしまうため、フレームの外で処理する。
    bool m_pendingProjectNew = false;

    // --- アンドゥの状態 -----------------------------------------------------
    UndoHistory m_undoHistory;
    // 直近に確定した文書。変更を見つけたとき、これを「変更前」として積む。
    DocumentSnapshot m_committed;
    // このフレームでレイヤーかマテリアルが変わったか。フレームの終わりに畳む。
    bool m_documentDirty = false;
    // このフレームの変更が「直前の編集の続き」であることの印。アンドゥの段を直前の
    // 段に畳む（ドラッグを離した時点で走る経路の計算し直しが、別の段にならないように）。
    bool m_documentJoinsEdit = false;
    // -1 でアンドゥ、+1 でリドゥ。マテリアルの破棄を伴うのでフレームの外で処理する。
    int m_pendingHistoryStep = 0;
    // 参照が切れたペイントマスクの回収を予約する。破棄は GPU 待機を伴う。
    bool m_pendingPaintSweep = false;

    ImGuiLayer m_imgui;
    // 右下に出す通知。保存の完了などを知らせる。
    ui::ToastQueue m_toasts;
    // F12 が押されたフレームに立つ。EndFrame で撮ってから下ろす。
    bool m_screenshotPending = false;
    // F9 が押されたフレームに立つ。フレームを送り終えてからビューポートだけを撮る。
    bool m_viewportScreenshotPending = false;

    // ビューポートの表示サイズ。UI 側で決まり、次のフレーム頭で反映する。
    uint32_t m_requestedViewportWidth = 512;
    uint32_t m_requestedViewportHeight = 512;

    StartupOptions m_options;

    // CoInitializeEx が成功したときだけ CoUninitialize する。
    bool m_comInitialized = false;
    // ドックレイアウトの初期化。ini に配置が無ければ既定レイアウトを組む。
    bool m_layoutChecked = false;
    bool m_rebuildLayout = false;
    // 前面へ出したいタブ（右カラムは「グラフ」）を押さえるための残りフレーム数。
    //
    // **起動のたびに効かせる。** ini には前回選んでいたタブが残っているので、
    // それに任せると「前回ライティングを見ていた」だけで次の起動もそこから始まる。
    // 作業の起点はグラフなので、起動時は必ずグラフを前面にする。
    int m_focusDefaultTabs = 3;
    // **表示設定（垂直同期・FPS 上限・ホットリロード・背景色・オーバーレイ）は
    // ここに写しを持たない。** `m_settings.Display()` を直接読み書きする。
    // 写しを持つと「UI では変わったのに設定へ書き戻し忘れて次回起動で戻る」
    // という壊れ方をする（実際に FPS 上限でそうなった）。
    FrameLimiter m_frameLimiter;
    // 前フレームで前面だったか。切り替わった時点で締め切りを捨てる。
    bool m_wasForeground = true;
    uint32_t m_frameCounter = 0;
};

}  // namespace tg


