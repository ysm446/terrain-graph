#pragma once

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
#include "io/RecentFiles.h"
#include "renderer/PreviewRenderer.h"
#include "renderer/SkyLibrary.h"
#include "rhi/Device.h"
#include "rhi/PipelineCache.h"
#include "rhi/ShaderCompiler.h"
#include "ui/ImGuiLayer.h"
#include "ui/Toast.h"

#include <imgui.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

// imgui-node-editor のコンテキスト。ヘッダを丸ごと引き込まないための前方宣言。
namespace ax::NodeEditor {
struct EditorContext;
}

namespace tg {

// コマンドラインから渡せる起動オプション。
struct StartupOptions {
    // 起動時に読み込む HDRI。空なら手続き的な空を使う。
    std::filesystem::path hdriPath;
    // 起動時にテクスチャライブラリへ読み込む画像。--texture を繰り返し指定できる。
    std::vector<std::filesystem::path> texturePaths;
    // 起動時に開くプロジェクト (.tgproj)。空なら既定のスタックで始める。
    std::filesystem::path projectPath;
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
    uint32_t screenshotFrame = 8;
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
    void DrawUi();
    // 既定のドックレイアウトを組む。ini に配置が無いときと、明示的な要求で呼ぶ。
    void BuildDefaultLayout(ImGuiID dockspaceId);
    void DrawViewportPanel();
    void DrawMaterialPanel();
    void DrawLightingPanel();
    // 実行状況の情報ウィンドウ（ウィンドウ > 情報）。常設ドックには置かない。
    void DrawInfoWindow();
    // ノードグラフパネル。サーフェス / シェイプ / 水面をノードとして繋ぎ、
    // 出力ノードへ届いたチェーンをレイヤー列へコンパイルしてプレビューに使う。
    void DrawGraphPanel();
    // エディタのコンテキストを破棄する。Shutdown から呼ぶ。
    void DestroyGraphEditor();
    // グラフのノード位置をエディタへ流し込み直す（読み込み・リセット・アンドゥの後）。
    // navigate が真なら、流し込み後に全体を画面へ収める。
    // アンドゥでは偽にする（戻すたびに視点が飛ぶと編集にならない）。
    void RequestGraphNodePlacement(bool navigate = true);
    // グラフのエディタ部（imgui-node-editor）。パネルの中で呼ぶ。
    void DrawGraphEditor();
    // グラフのノード 1 枚。カード・ピン・リンクの当たり判定を描く。
    void DrawGraphNode(const graph::Node& node);
    // グラフノードのレイヤー設定のプロパティ行。
    // 変更があれば true。isBase はマスクが効かない一番下のレイヤーのとき。
    // isSource は入力を持たないノード（ハイトマップ）。マスクの節を出さない。
    // maskFromNode が真のとき、マスクの出どころは Mask 入力に繋いだノード。
    // ソースと画像の行は出さない（同じ値を 2 か所から編集させない）。
    bool DrawLayerSettings(compositor::MaterialLayer& layer, bool isBase, bool isSource = false,
                           bool maskFromNode = false);
    // グラフの変更をコンパイル結果（m_graphStack）へ反映する。フレームの頭で呼ぶ。
    void SyncGraphStack();
    // 選択中のノードを控える / 貼り付ける（Ctrl+C / Ctrl+V）。
    void CopySelectedGraphNodes();
    void PasteGraphNodes();
    // ビューポートに出すノードを決める。出力ノードや無効な ID は
    // 「出力ノードのチェーン」（0）に落とす。
    void SetPreviewGraphNode(graph::GraphId nodeId);
    void DrawMaterialLibraryPanel();
    // 天球パネル。一覧で選んだものがそのままビューポートの環境になる。
    void DrawSkyLibraryPanel();
    void DrawTextureLibraryPanel();
    // アプリの設定ウィンドウ（ウィンドウ > 設定）。プロジェクトに保存しない設定を置く。
    void DrawSettingsWindow();
    // 合成結果を画像へ書き出すウィンドウ（ファイル > テクスチャを書き出す…）。
    void DrawExportWindow();
    // 開発用オプション（スクリーンショット / 書き出し / 保存）で動いているか。
    // 真のときはフレームレートを落とさない。
    bool Headless() const;
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
    // 画面下端のステータスバー。直近の通知と、いま何を持っているかを出す。
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
    // このテクスチャを使っている場所の一覧（削除の確認に出す）。
    std::vector<std::string> CollectTextureUsers(compositor::TextureId id) const;
    // 参照している箇所の数だけを数える。毎フレーム呼ぶので文字列は作らない。
    size_t CountTextureUsers(compositor::TextureId id) const;
    // 参照が残っているテクスチャを消そうとしたときの確認。
    void DrawTextureRemoveModal();
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
    void MarkDocumentChanged();
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
    void DrawLightGizmo(const ImVec2& viewportMin, const ImVec2& viewportMax);
    // ハイトの範囲。height 0 / 0.5 / 1 がワールドのどこに来るかを枠で示す
    // （ビューポート左上の `表示 > ハイトの範囲`。平面のときだけ描く）。
    void DrawHeightGuide(const ImVec2& viewportMin, const ImVec2& viewportMax);
    // ビューポート上のドラッグをブラシへ渡す。ペイントモードのときだけ呼ぶ。
    void HandlePaintInput(compositor::MaterialLayer& layer, bool itemActive,
                          const ImVec2& imageOrigin, const ImVec2& imageSize);

    Window m_window;
    rhi::Device m_device;
    rhi::ShaderCompiler m_shaderCompiler;
    rhi::PipelineCache m_pipelineCache;
    renderer::PreviewRenderer m_renderer;
    // --- ノードグラフ -------------------------------------------------------
    // 合成はグラフが唯一の入口。グラフをレイヤー列へコンパイルして
    // 既存の GPU 評価器で評価する。m_graphStack はそのコンパイル結果。
    graph::NodeGraph m_graph = graph::NodeGraph::CreateDefault();
    compositor::MaterialStack m_graphStack;
    uint64_t m_compiledGraphRevision = 0;
    // 前回コンパイルしたプレビュー対象。選択が変わっても再コンパイルするために持つ。
    graph::GraphId m_compiledGraphTarget = 0;
    graph::GraphId m_selectedGraphNode = 0;
    // エディタで選ばれているノード全部。コピーはこれを見る
    // （プロパティに出すのは先頭の 1 つ = m_selectedGraphNode）。
    std::vector<graph::GraphId> m_selectedGraphNodes;

    // ノードのコピー元。**OS のクリップボードは使わない**（アプリ内だけ）。
    // 位置と設定に加えて、**入力ピンごとの接続元**を覚える。
    // コピーした集合の中を指していれば貼った側どうしで繋ぎ直し、
    // 外を指していれば**元の親へ繋いだまま**にする。
    struct GraphClipboardNode {
        graph::NodeKind kind = graph::NodeKind::Surface;
        graph::NodeSettings settings;
        float posX = 0.0f;
        float posY = 0.0f;
        struct Source {
            int copiedIndex = -1;              // コピーした集合の中の添字
            graph::GraphId externalPin = 0;    // 集合の外なら、その出力ピン
        };
        std::vector<Source> inputs;
    };
    std::vector<GraphClipboardNode> m_graphClipboard;
    // 貼るたびに位置をずらす回数。コピーし直すと 0 に戻す。
    int m_graphPasteCount = 0;
    // ビューポートに出しているノード。**選択とは別に持つ。**
    // 結果を見ながら別のノードのプロパティをいじれるようにするため
    // （terrain-editor と同じ作法）。0 は出力ノードのチェーン。
    graph::GraphId m_previewGraphNode = 0;
    // 出力ピンの「クリック」を拾うための押した位置。ドラッグ（リンク作成）と
    // 区別するために、押した / 離したが同じピンで、ほとんど動いていないときだけ
    // クリックとみなす。
    graph::GraphId m_graphPressedPin = 0;
    ImVec2 m_graphPressedPinPos{};
    ax::NodeEditor::EditorContext* m_nodeEditor = nullptr;
    // グラフパネル内の「エディタ / プロパティ」境界の高さ（96 DPI 基準）。
    float m_graphEditorHeight = 380.0f;
    // 位置をエディタへ流し込むべきノード。作成・読み込みのときに積む。
    std::vector<graph::GraphId> m_graphNodesToPlace;
    // 位置を流し込んだ後に全体を画面へ収めるまでの残りフレーム数。
    // **キャンバスの大きさが安定しているフレームだけ数える。** エディタは
    // サイズ変化のたびに前の表示領域を復元するので、ドックの確定前に寄せると
    // 上書きされて効かない。
    int m_graphNavigateCountdown = 0;
    ImVec2 m_graphCanvasSize = ImVec2(0.0f, 0.0f);
    compositor::TextureLibrary m_textureLibrary;
    compositor::MaterialLibrary m_materialLibrary;
    // 天球アセット。マテリアルと並ぶアセットだが、**アンドゥの対象には入れない。**
    // 環境は作っているマテリアルそのものではなく、見え方の設定に近い
    // （プレビュー設定を履歴に載せないのと同じ理由）。
    renderer::SkyLibrary m_skyLibrary;
    compositor::PaintMaskStore m_paintMasks;
    int m_selectedMaterial = 0;
    // ORD をまとめて割り当てるときに選ぶテクスチャ（UI の一時状態）。
    compositor::TextureId m_ordTexture = compositor::kNoTexture;
    compositor::BrushSettings m_brush;
    // ペイントモード中はビューポートの左ドラッグがブラシになる。
    bool m_paintMode = false;
    // ライトの向きを掴んでいる間。ギズモは離してからも少しの間だけ残す。
    bool m_lightDragActive = false;
    double m_lightGizmoUntil = 0.0;
    // ストローク中の状態。前フレームのカーソル位置から線分としてブラシを積む。
    bool m_strokeActive = false;
    float m_strokeLastX = 0.0f;
    float m_strokeLastY = 0.0f;
    int m_selectedTexture = 0;
    // 拡大プレビューで出すチャンネル。0 = RGB、1..4 = R / G / B / A。
    // ORD のように 1 枚へ複数のマップを詰めたテクスチャの中身を確かめるためのもの。
    int m_previewChannel = 0;
    // 読み込んだ直後のテクスチャを一覧に見せるための要求。
    // 一覧はスクロールするので、追加しただけでは枠外に入って気づけない。
    bool m_scrollToSelectedTexture = false;
    // 追加・複製した直後のマテリアルを一覧の枠内へ送る要求。上と同じ理由。
    bool m_scrollToSelectedMaterial = false;
    // 追加・複製した直後の天球を一覧の枠内へ送る要求。上と同じ理由。
    bool m_scrollToSelectedSky = false;

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
    std::filesystem::path m_projectPath;  // 現在のプロジェクト。未保存なら空
    io::RecentFiles m_recentProjects;
    io::AppSettings m_settings;
    // 設定ウィンドウを出しているか。ドックへは収めない補助ウィンドウ。
    bool m_showSettings = false;
    // 情報ウィンドウ。必要なときだけウィンドウメニューから開く。
    bool m_showInfo = false;
    // 書き出しウィンドウ。設定ウィンドウと同じくドックへは収めない。
    bool m_showExport = false;
    io::ExportSettings m_exportSettings;
    // 書き出しの実行要求。**GPU 待機とファイル入出力を伴うのでフレームの外で処理する。**
    bool m_pendingExport = false;
    std::filesystem::path m_pendingProjectSave;
    std::filesystem::path m_pendingProjectOpen;
    std::filesystem::path m_pendingMaterialExport;
    std::filesystem::path m_pendingMaterialImport;
    compositor::MaterialAssetId m_pendingExportMaterial = compositor::kNoMaterialAsset;
    compositor::TextureId m_pendingTextureRemove = compositor::kNoTexture;
    // 削除要求のあったマテリアル。一覧の描画中に消すと、描画側が erase 済みの
    // 要素を読んでしまうため、フレームの外で処理する。
    compositor::MaterialAssetId m_pendingMaterialRemove = compositor::kNoMaterialAsset;
    // 削除要求のあった天球。マテリアルと同じ理由でフレームの外で処理する。
    renderer::SkyAssetId m_pendingSkyRemove = renderer::kNoSkyAsset;
    // 確認待ちのテクスチャ。参照が残っているときだけ入る。
    compositor::TextureId m_textureRemoveCandidate = compositor::kNoTexture;
    std::vector<std::string> m_textureRemoveUsers;
    bool m_pendingProjectNew = false;

    // --- アンドゥの状態 -----------------------------------------------------
    UndoHistory m_undoHistory;
    // 直近に確定した文書。変更を見つけたとき、これを「変更前」として積む。
    DocumentSnapshot m_committed;
    // このフレームでレイヤーかマテリアルが変わったか。フレームの終わりに畳む。
    bool m_documentDirty = false;
    // -1 でアンドゥ、+1 でリドゥ。マテリアルの破棄を伴うのでフレームの外で処理する。
    int m_pendingHistoryStep = 0;
    // 参照が切れたペイントマスクの回収を予約する。破棄は GPU 待機を伴う。
    bool m_pendingPaintSweep = false;

    ImGuiLayer m_imgui;
    // 右下に出す通知。保存の完了などを知らせる。
    ui::ToastQueue m_toasts;
    // F12 が押されたフレームに立つ。EndFrame で撮ってから下ろす。
    bool m_screenshotPending = false;

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
