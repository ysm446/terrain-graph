// アプリ本体のコア。初期化と終了、フレームループ、ドックレイアウト、
// ステータスバー、設定ウィンドウ、情報ウィンドウ。
// パネルの描画と入出力は Application*.cpp に分かれている。

#include "app/Application.h"

#include "app/ApplicationUiHelpers.h"
#include "core/FileDialog.h"
#include "core/Log.h"
#include "io/ProjectIo.h"
#include "ui/UiStyle.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <DirectXMath.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

namespace tg {
namespace {

// クライアント領域（描画される中身）のサイズ。ウィンドウ枠は含まない。
// DPI では拡大しない。スクリーンショットや録画の解像度を固定するため。
constexpr uint32_t kInitialWidth = 1920;
constexpr uint32_t kInitialHeight = 1080;

// ホットリロードの走査間隔（フレーム数）。毎フレーム走査するほどの頻度は要らない。
constexpr uint32_t kHotReloadIntervalFrames = 30;

#if defined(TG_DEBUG)
constexpr bool kEnableDebugLayer = true;
#else
constexpr bool kEnableDebugLayer = false;
#endif

// シェーダの探索先。環境変数 TG_SHADER_DIR で差し替えられるようにしておく。
std::filesystem::path ResolveShaderRoot() {
    const DWORD needed = ::GetEnvironmentVariableW(L"TG_SHADER_DIR", nullptr, 0);
    if (needed > 0) {
        std::wstring value;
        value.resize(needed);
        const DWORD written = ::GetEnvironmentVariableW(L"TG_SHADER_DIR", value.data(), needed);
        if (written > 0) {
            value.resize(written);
            return std::filesystem::path(value);
        }
    }
    return std::filesystem::path(TG_SHADER_DIR);
}

// スクリーンショットの置き場所。環境変数 TG_DATA_DIR で差し替えられる。
// data/ は .gitignore で外してあるので、撮ったものがリポジトリに混ざらない。
std::filesystem::path ResolveScreenshotDirectory() {
    std::filesystem::path dataDir(TG_DATA_DIR);
    const DWORD needed = ::GetEnvironmentVariableW(L"TG_DATA_DIR", nullptr, 0);
    if (needed > 0) {
        std::wstring value;
        value.resize(needed);
        const DWORD written = ::GetEnvironmentVariableW(L"TG_DATA_DIR", value.data(), needed);
        if (written > 0) {
            value.resize(written);
            dataDir = std::filesystem::path(value);
        }
    }
    return dataDir / L"screenshots";
}

// 撮った時刻をそのままファイル名にする。連番だと前回の続きが分からない。
std::string ScreenshotFileName() {
    const std::time_t now = std::time(nullptr);
    std::tm local = {};
    ::localtime_s(&local, &now);
    char buffer[64] = {};
    std::strftime(buffer, sizeof(buffer), "terrain_graph_%Y%m%d_%H%M%S.png", &local);
    return buffer;
}

}  // namespace

bool Application::Initialize(const StartupOptions& options) {
    m_options = options;

    // ファイル選択ダイアログ（IFileDialog）が COM を使う。
    m_comInitialized =
        SUCCEEDED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE));
    if (!m_comInitialized) {
        TG_LOG_WARN("COM を初期化できませんでした。ファイル選択ダイアログは使えません");
    }

    // ウィンドウ生成より前に済ませる必要がある。
    ImGuiLayer::EnableDpiAwareness();

    // クライアント領域を実ピクセルで 1920x1080 にする。DPI では拡大しない。
    // UI の大きさは ImGui 側の DPI スケールで合わせる。
    // モニタからはみ出す場合は Window::Create 側で作業領域に収める。
    if (!m_window.Create(L"Terrain Graph", kInitialWidth, kInitialHeight)) {
        return false;
    }

    if (!m_device.Initialize(m_window.Handle(), m_window.Width(), m_window.Height(),
                             kEnableDebugLayer)) {
        return false;
    }

    if (!m_shaderCompiler.Create(ResolveShaderRoot())) {
        return false;
    }
    if (!m_pipelineCache.Create(m_device.GetDevice(), &m_shaderCompiler)) {
        return false;
    }
    if (!m_renderer.Initialize(m_device, m_pipelineCache)) {
        return false;
    }
    if (!m_renderer.Resize(m_device, m_requestedViewportWidth, m_requestedViewportHeight)) {
        return false;
    }

    if (!m_imgui.Initialize(m_window, m_device)) {
        return false;
    }

    m_window.SetMessageHook([this](HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
        return m_imgui.HandleMessage(hwnd, msg, wparam, lparam);
    });
    m_window.SetResizeCallback([this](uint32_t width, uint32_t height) {
        m_device.Resize(width, height);
    });
    // エクスプローラからのドロップ。拡張子で行き先を振り分ける。
    m_window.SetDropCallback([this](const std::vector<std::filesystem::path>& paths) {
        HandleDroppedFiles(paths);
    });

    m_pendingTexturePaths = options.texturePaths;

    // 天球は必ず 1 つある状態にする。--hdri が来ていれば、その既定の天球へ入れる。
    m_skyLibrary.EnsureDefault();
    if (!options.hdriPath.empty()) {
        if (renderer::SkyAsset* sky = m_skyLibrary.ActiveMutable(); sky != nullptr) {
            sky->name = ToUtf8Display(options.hdriPath.stem());
            sky->sky.source = renderer::SkySource::Hdri;
            sky->sky.hdriPath = options.hdriPath;
        }
    }
    // 読み込みは GPU 待機を伴うので、ここでは要求だけ積む。
    // 最初のフレームの前に ProcessPendingFileWork が処理する。
    if (!options.projectPath.empty()) {
        m_pendingProjectOpen = options.projectPath;
    }
    UpdateWindowTitle();

    m_settings.Load();
    m_recentProjects.Load();
    // 表示設定は写し取らない。使うところで m_settings.Display() を直接読む。
    // 設定に拡大率が残っていれば、ウィンドウの大きさもそれに合わせる。
    ApplyUiScale();

    // ログをステータスバーへ流す。以降の警告やエラーは画面上でも見える。
    SetLogSink([this](LogLevel level, const char* text) { PushStatus(level, text); });

    // アンドゥの起点。ここを取り忘れると、最初の 1 回が空の文書へ戻ってしまう。
    m_committed = CaptureDocument();

    TG_LOG_INFO("terrain-graph %s を起動しました", TG_APP_VERSION);
    return true;
}

void Application::Shutdown() {
    // シンクは this を掴んでいる。破棄より先に必ず外す。
    SetLogSink({});

    m_device.WaitForGpu();
    // ImGui のコンテキストより先に破棄する（エディタが ImGui に依存している）。
    DestroyGraphEditor();
    m_paintMasks.Destroy(m_device);
    m_materialLibrary.Destroy(m_device);
    m_skyLibrary.Destroy(m_device);
    m_textureLibrary.Destroy(m_device);
    m_renderer.Shutdown(m_device);
    m_imgui.Shutdown();
    m_pipelineCache.Destroy();
    m_shaderCompiler.Destroy();
    m_device.Shutdown();
    m_window.Destroy();
    // 初期化に失敗していたのに解除すると、他所の COM 初期化を巻き戻してしまう。
    if (m_comInitialized) {
        ::CoUninitialize();
    }
}

// F12 の撮影。バックバッファをそのまま読み戻すので、
// 手前に別のウィンドウが重なっていても、画面外へはみ出していても欠けない。
void Application::RequestScreenshot() {
    const std::filesystem::path directory = ResolveScreenshotDirectory();
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        TG_LOG_ERROR("スクリーンショットの保存先を作れませんでした: %s",
                     ToUtf8Display(directory).c_str());
        m_toasts.Push("スクリーンショットを保存できませんでした", "保存先を作れません");
        return;
    }

    const std::filesystem::path path = directory / FromUtf8(ScreenshotFileName());
    m_device.RequestBackBufferCapture(
        path, [this](bool success, const std::filesystem::path& saved, uint32_t width,
                     uint32_t height) {
            if (!success) {
                m_toasts.Push("スクリーンショットを保存できませんでした",
                              ToUtf8Display(saved.filename()));
                return;
            }
            char detail[160] = {};
            std::snprintf(detail, sizeof(detail), "%u x %u  %s", width, height,
                          ToUtf8Display(saved.filename()).c_str());
            m_toasts.Push("スクリーンショットを保存しました", detail, saved);
            TG_LOG_INFO("スクリーンショットを保存しました: %s", ToUtf8Display(saved).c_str());
        });
}

void Application::PollShaderHotReload() {
    if (!m_settings.Display().hotReload) {
        return;
    }
    if ((m_frameCounter % kHotReloadIntervalFrames) != 0) {
        return;
    }
    if (!m_shaderCompiler.PollChanges()) {
        return;
    }

    TG_LOG_INFO("シェーダの更新を検出しました。PSO を作り直します");
    // PSO は GPU が参照中の可能性があるため、破棄前に必ず待つ。
    m_device.WaitForGpu();
    m_pipelineCache.InvalidateAll();
}

int Application::Run() {
    while (m_window.PumpMessages()) {
        if (m_window.IsMinimized()) {
            ::WaitMessage();
            continue;
        }

        // --- フレームレートの上限 ------------------------------------------
        // **描くのを間引くだけで、長くはブロックしない。** フレームの間ずっと
        // 眠るとメッセージを汲めず、OS からは無反応なウィンドウに見える
        // （クリックしても固まったように感じる）。
        //
        // 背面（非アクティブ）のときは別の低い上限を使う。見えていない絵に
        // GPU を回し続ける理由がない。完全に止めないのは、シェーダの
        // ホットリロードや進行中の処理を生かしておくため。
        //
        // 開発用のオプションが動いている間は落とさない。起動直後はウィンドウが
        // 背面のことがあり、待つと書き出しやスクリーンショットが遅くなる。
        if (!Headless()) {
            const bool foreground = m_window.IsForeground();
            if (foreground != m_wasForeground) {
                // 前面へ戻った直後に、積み上げた締め切りで 1 フレーム待たされないように。
                m_wasForeground = foreground;
                m_frameLimiter.Reset();
            }
            const io::DisplaySettings& display = m_settings.Display();
            if (!m_frameLimiter.ShouldRender(foreground ? display.frameRateLimit
                                                        : display.inactiveFrameRateLimit)) {
                continue;
            }
        }

        PollShaderHotReload();



        // UI の拡大率はスタイル・フォントとウィンドウの大きさに効く。フレームの外で。
        ApplyUiScale();

        // プロジェクトとマテリアルの読み書きも GPU 待機を伴うため、フレームの外で。
        // 他の保留処理より先に行う（読み込みが中身を丸ごと入れ替えるため）。
        ProcessPendingFileWork();

        // 開発用: 数フレーム描いてから合成結果を書き出して終了する。
        if (!m_options.exportDirectory.empty() && m_frameCounter >= m_options.screenshotFrame) {
            m_exportSettings.directory = m_options.exportDirectory;
            SyncGraphStack();
            // プレビューと同じくグラフのコンパイル結果を書き出す。
            const io::ExportRefs refs{m_graphStack, m_textureLibrary, m_materialLibrary,
                                      m_paintMasks};
            io::ExportMaterialTextures(m_device, m_pipelineCache, refs, m_exportSettings);
            break;
        }

        // 開発用: 数フレーム描いてからプロジェクトを保存して終了する。
        // 対話せずに保存と読み込みを確かめるために使う。
        if (!m_options.saveProjectPath.empty() && m_frameCounter >= m_options.screenshotFrame) {
            const io::ProjectRefs refs{m_textureLibrary, m_materialLibrary, m_paintMasks,
                                       m_skyLibrary,     m_renderer,       m_graph};
            io::SaveProject(m_options.saveProjectPath, m_device, refs);
            break;
        }

        // 選択中の天球をレンダラへ渡す。**毎フレーム渡してよい。**
        // 中身が変わっていれば、必要な作り直しだけが予約される。
        m_skyLibrary.EnsureDefault();
        if (const renderer::SkyAsset* sky = m_skyLibrary.Active(); sky != nullptr) {
            m_renderer.SetActiveSky(sky->sky);
        }

        // 環境マップやマテリアル解像度の作り直しは GPU 待機を伴うため、
        // フレームの外で処理する。
        m_renderer.ProcessPendingWork(m_device, m_pipelineCache);
        // ペイントマスクの解像度変更も作り直しを伴うため、フレームの外で処理する。
        m_paintMasks.ProcessPendingWork(m_device, m_pipelineCache);

        // テクスチャ読み込みも GPU 待機を伴うため、フレームの外で処理する。
        if (!m_pendingTexturePaths.empty()) {
            std::vector<std::filesystem::path> paths;
            paths.swap(m_pendingTexturePaths);
            bool loaded = false;
            for (const std::filesystem::path& path : paths) {
                const compositor::TextureId id =
                    m_textureLibrary.Load(m_device, m_pipelineCache, path);
                if (id == compositor::kNoTexture) {
                    continue;
                }
                loaded = true;
                // 読み込んだものを選択して一覧に見せる。
                m_selectedTexture =
                    static_cast<int>(m_textureLibrary.Entries().size()) - 1;
                m_scrollToSelectedTexture = true;
            }
            if (loaded) {
                // 読み込んだ画像を参照しているサムネイルを作り直す。
                for (const compositor::MaterialAsset& asset : m_materialLibrary.Entries()) {
                    m_materialLibrary.MarkThumbnailDirty(asset.id);
                }
                m_graphStack.MarkDirty();
            }
        }

        // サムネイルの生成も GPU 待機を伴う。
        m_materialLibrary.ProcessPendingWork(m_device, m_pipelineCache, m_textureLibrary);
        // 天球のサムネイルは HDR ファイルの読み込みを伴うので、1 フレームに 1 枚だけ作る。
        m_skyLibrary.ProcessPendingWork(m_device, m_pipelineCache);

        // ビューポートの作り直しは GPU 待機を伴うため、フレームの外で行う。
        if (m_requestedViewportWidth != m_renderer.Width() ||
            m_requestedViewportHeight != m_renderer.Height()) {
            m_renderer.Resize(m_device, m_requestedViewportWidth, m_requestedViewportHeight);
        }

        m_imgui.BeginFrame();
        DrawUi();

        ID3D12GraphicsCommandList* commandList =
            m_device.BeginFrame(m_settings.Display().clearColor);
        if (commandList == nullptr) {
            // フレームを開始できなかった場合は ImGui の状態を捨てて次へ進む。
            ImGui::EndFrame();
            continue;
        }

        // ブラシは前フレームの UV バッファを読むため、合成の評価より前に流す。
        const compositor::PaintContext paintContext =
            m_renderer.PrepareUvBufferForRead(commandList);
        if (m_paintMasks.Process(m_device, m_pipelineCache, commandList, paintContext)) {
            // マスクの中身が変わったので合成をやり直す。
            m_graphStack.MarkDirty();
        }

        // ハイトの範囲は深度テストのためレンダラが描く。設定の写しは持たない方針
        // だが、レンダラは AppSettings を知らないので、描く直前に毎フレーム渡す。
        m_renderer.ShowHeightGuide() = m_settings.Display().showHeightGuide;

        // グラフをレイヤー列へコンパイルした結果で評価する。
        SyncGraphStack();
        m_renderer.Render(m_device, m_pipelineCache, commandList, m_graphStack,
                          m_textureLibrary, m_materialLibrary, m_paintMasks);

        // レンダラがターゲットを差し替えているので、ImGui を描く前に戻す。
        m_device.BindBackBuffer(commandList);
        m_imgui.EndFrame(commandList);

        // UI 込みの書き出しは、バックバッファが描き終わったこのフレームで写す。
        // **合成の評価は非同期なので、走っている最中は撮らない**（前回の絵が写る）。
        const bool evaluationIdle = !m_renderer.Evaluator().IsEvaluating();
        const bool captureUi = !m_options.uiScreenshotPath.empty() &&
                               (m_frameCounter + 1) >= m_options.screenshotFrame && evaluationIdle;
        if (captureUi) {
            m_device.RequestBackBufferCapture(m_options.uiScreenshotPath);
        } else if (m_screenshotPending) {
            m_screenshotPending = false;
            RequestScreenshot();
        }

        m_device.EndFrame(m_settings.Display().vsync);

        // デバッグレイヤーが溜めた検証エラーをログ（とステータスバー）へ流す。
        // 汲まないとデバッガを繋がない限り誰の目にも触れない。
        m_device.DrainDebugMessages();
        ++m_frameCounter;

        // 開発用のスクリーンショット。書き出したら終了する。
        if (captureUi) {
            break;
        }
        if (!m_options.screenshotPath.empty() && m_frameCounter >= m_options.screenshotFrame &&
            evaluationIdle) {
            m_device.WaitForGpu();
            m_renderer.SaveOutputToPng(m_device, m_options.screenshotPath);
            break;
        }

    }
    return 0;
}

// 開発用オプションで動いているか。対話せずに書き出して終わる経路。
bool Application::Headless() const {
    return !m_options.screenshotPath.empty() || !m_options.uiScreenshotPath.empty() ||
           !m_options.exportDirectory.empty() || !m_options.saveProjectPath.empty();
}

void Application::DrawUi() {
    // ショートカットはメニューを開いていなくても効かせたいので、先に見る。
    HandleShortcuts();

    // メニューバーを先に作ることで、メインビューポートの作業領域が
    // メニューバー分を差し引いた状態になる。既定のパネル配置がこれに依存する。
    if (ImGui::BeginMainMenuBar()) {
        DrawFileMenu();
        if (ImGui::BeginMenu("編集")) {
            if (ImGui::MenuItem("元に戻す", "Ctrl+Z", false, m_undoHistory.CanUndo())) {
                m_pendingHistoryStep = -1;
            }
            if (ImGui::MenuItem("やり直す", "Ctrl+Y", false, m_undoHistory.CanRedo())) {
                m_pendingHistoryStep = 1;
            }
            ImGui::Separator();
            ImGui::TextDisabled("対象はグラフとマテリアル");
            ImGui::EndMenu();
        }
        // ビューポートの表示に関わる切り替え（FPS / 統計 / ハイトの範囲）は
        // メニューではなくビューポート左上の「表示」ボタンに置く。
        if (ImGui::BeginMenu("ウィンドウ")) {
            if (ImGui::MenuItem("レイアウトをリセット")) {
                m_rebuildLayout = true;
            }
            ImGui::Separator();
            ImGui::MenuItem("情報", nullptr, &m_showInfo);
            ImGui::MenuItem("設定", nullptr, &m_showSettings);
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    // パネルはすべてドックへ収める。絶対座標で置くと、ウィンドウの大きさが
    // 変わったときや、別の大きさで保存された ini を読んだときに画面外へはみ出す。
    // ドックスペースの ID には版を付ける。**パネルを増減したら版を上げること。**
    // ID が変われば ini に配置が無い状態になり、既定レイアウトが組み直される。
    // 上げないと、新しいパネルがどこにも入らず浮いたままになる。
    const ImGuiID dockspaceId = ImGui::GetID("TerrainGraphDockSpace_v16");

    // ステータスバーもメニューバーと同じく、先に作って作業領域を狭めておく。
    DrawStatusBar();

    // ini にドックの配置が無ければ既定レイアウトを組む。
    // DockSpaceOverViewport がノードを作る前に判定すること。
    if (!m_layoutChecked) {
        m_layoutChecked = true;
        m_rebuildLayout = (ImGui::DockBuilderGetNode(dockspaceId) == nullptr);
    }
    if (m_rebuildLayout) {
        m_rebuildLayout = false;
        BuildDefaultLayout(dockspaceId);
    }

    ImGui::DockSpaceOverViewport(dockspaceId, ImGui::GetMainViewport());

    DrawViewportPanel();
    // タブが重なる枠では、**最初に submit したパネルが前面のタブになり、
    // タブは submit した順に並ぶ**（ini に配置が無いとき）。
    // 作業の起点はグラフなので、右カラムの他のパネルより先に描く。
    DrawGraphPanel();
    DrawMaterialLibraryPanel();
    DrawSkyLibraryPanel();
    DrawTextureLibraryPanel();
    DrawMaterialPanel();
    DrawLightingPanel();

    DrawInfoWindow();
    DrawSettingsWindow();
    DrawExportWindow();

    if (m_focusDefaultTabs > 0) {
        --m_focusDefaultTabs;
    }

    // --- アンドゥの段を畳む -------------------------------------------------
    // パネルは変更を見つけると m_documentDirty を立てるだけにしておき、
    // ここで 1 フレームぶんをまとめて 1 段にする。
    //
    // 変更後に気づく作りなので、積むのは「1 つ前に確定した状態」。
    // 掴んでいるウィジェットの ID を渡すことで、スライダーのドラッグが
    // 1 段に収まる（毎フレーム変更が来ても ID は変わらない）。
    if (m_documentDirty) {
        m_documentDirty = false;
        m_undoHistory.Push(m_committed, static_cast<uint32_t>(ImGui::GetActiveID()));
        m_committed = CaptureDocument();
        // 古い段が押し出されると、そこでしか参照されていなかったマスクが浮く。
        m_pendingPaintSweep = true;
    }
    // 掴んでいたものが離れたら、次の編集は別の段にする。
    if (ImGui::GetActiveID() == 0) {
        m_undoHistory.EndEdit();
    }

    // **撮影するフレームには通知を描かない。** 直前の通知が写り込んでしまう。
    // 他のウィンドウより後に描くことで最前面に出す。
    if (!m_screenshotPending) {
        m_toasts.Draw();
    }
}

// 既定のドックレイアウト。
//
//   +--------------------------------+------------------+
//   | ビューポート                    | グラフ            |
//   |                                | プレビュー設定     |
//   |                                | ライティング         |
//   +---------------+----------------+                  |
//   | テクスチャ     | マテリアル / 天球 |                  |
//   +---------------+----------------+------------------+
//
// 比率で組むので、ウィンドウの大きさが変わってもパネルははみ出さない。
void Application::BuildDefaultLayout(ImGuiID dockspaceId) {
    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    // 分割する前に大きさを入れておかないと、分割比が当てにならない。
    ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->WorkSize);

    ImGuiID center = dockspaceId;
    ImGuiID right = 0;
    ImGuiID bottom = 0;
    ImGuiID bottomRight = 0;
    // **カラムは右の 1 本だけにする。** 左右に分けると 1 本あたりが狭くなる。
    // 幅は「ラベル：値」の行が窮屈にならない範囲で**できるだけ細く**取る。
    // パネルは中を上下に割って使うので、横幅を広く取る理由がない。
    // 余った幅はビューポートへ回す（素材の見え方を確かめるのが主目的）。
    ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.26f, &right, &center);
    // アセットの帯。**右カラムを切り出した後の center を割る**ので、
    // 帯はビューポートの真下だけに伸び、右カラムの下へは回り込まない。
    ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.28f, &bottom, &center);
    // 帯をさらに左右へ割る。**素材の一覧はどれもサムネイルの格子**なので、
    // 「ラベル：値」の行を並べる右カラムより、横に広い帯のほうが枡が多く入る。
    ImGui::DockBuilderSplitNode(bottom, ImGuiDir_Right, 0.45f, &bottomRight, &bottom);

    ImGui::DockBuilderDockWindow("ビューポート", center);
    // テクスチャは帯の左、マテリアルは帯の右。**テクスチャのサムネイルを
    // マテリアルのマップ欄へドラッグして割り当てる**ので、左右に並べて
    // 掴む側と落とす側が同時に見えるようにする。
    ImGui::DockBuilderDockWindow("テクスチャ", bottom);
    // **前面にしたい「マテリアル」を後にドックする。** 同じ枠では最後に
    // ドックしたものが選ばれる。タブの並びは submit した順（マテリアル → 天球）。
    ImGui::DockBuilderDockWindow("天球", bottomRight);
    ImGui::DockBuilderDockWindow("マテリアル", bottomRight);
    // 右カラムへタブで重ねる。縦に積むと 1 枚あたりが短くなり、
    // どれもスクロールしないと全体が見えなくなる。
    // **グラフは右カラムに置く。** 中央のタブにするとビューポートと排他になり、
    // ノードを選んだ結果をプレビューで確かめられない。盤面は狭くなるが、
    // 「繋ぎ替えて見た目を確かめる」の往復を優先する。
    ImGui::DockBuilderDockWindow("グラフ", right);
    ImGui::DockBuilderDockWindow("プレビュー設定", right);
    ImGui::DockBuilderDockWindow("ライティング", right);

    ImGui::DockBuilderFinish(dockspaceId);

    // 前面のタブは右カラムが「グラフ」、帯の右が「マテリアル」。
    // この時点ではまだウィンドウが無いので、実際の指定は各パネルの Begin 直前で行う。
    m_focusDefaultTabs = 3;
}

void Application::PushStatus(LogLevel level, const char* text) {
    if (text == nullptr) {
        return;
    }
    m_status.text = text;
    m_status.level = level;
    m_status.time = std::chrono::steady_clock::now();
    m_status.valid = true;
}

// 画面下端のステータスバー。左に直近の通知、右にいま何を持っているか。
//
// メニューバーと同じ仕組み（BeginViewportSideBar）で作業領域を狭めるので、
// **ドックスペースより前に呼ぶこと。** 後だとドックがバーの下へはみ出す。
void Application::DrawStatusBar() {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoScrollbar |
                                        ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_MenuBar;

    if (ImGui::BeginViewportSideBar("##statusBar", viewport, ImGuiDir_Down,
                                    ImGui::GetFrameHeight(), kFlags)) {
        if (ImGui::BeginMenuBar()) {
            // --- 左: いまのモードと直近の通知 -------------------------------
            // モードでビューポートの操作が変わるので、常に見える場所へ出す。
            if (const compositor::MaterialLayer* paintLayer = CurrentPaintLayer();
                paintLayer != nullptr) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetColorU32(ImGuiCol_CheckMark));
                ImGui::Text("ペイント中: %s", paintLayer->name.c_str());
                ImGui::PopStyleColor();
                ImGui::TextDisabled("|");
            }
            // 合成の評価はコンピュートキューで走る。見えている絵が古い間はここで分かる。
            if (m_renderer.Evaluator().IsEvaluating()) {
                ImGui::TextDisabled("合成を評価中…");
                ImGui::TextDisabled("|");
            }

            if (m_status.valid) {
                const auto age = std::chrono::duration<float>(
                                     std::chrono::steady_clock::now() - m_status.time)
                                     .count();
                // 情報は流れて消える。警告とエラーは次の通知まで残す。
                const bool keep = (m_status.level != LogLevel::Info) || (age < kStatusHoldSeconds);
                if (keep) {
                    ImU32 color = ImGui::GetColorU32(ImGuiCol_TextDisabled);
                    if (m_status.level == LogLevel::Warn) {
                        color = ui::WarnColor();
                    } else if (m_status.level == LogLevel::Error) {
                        color = ui::ErrorColor();
                    }
                    ImGui::PushStyleColor(ImGuiCol_Text, color);
                    ImGui::TextUnformatted(m_status.text.c_str());
                    ImGui::PopStyleColor();
                    if (ImGui::IsItemHovered()) {
                        // 長い通知（パスなど）は切れるので、全文はここで読む。
                        ImGui::SetTooltip("%s", m_status.text.c_str());
                    }
                }
            }

            // --- 右: いま何を持っているか -----------------------------------
            const std::string project =
                m_projectPath.empty() ? std::string("未保存のプロジェクト")
                                      : ToUtf8Display(m_projectPath.filename());
            char summary[320] = {};
            std::snprintf(summary, sizeof(summary),
                          "%s   ノード %zu / マテリアル %zu / テクスチャ %zu   合成 %u^2   "
                          "%.0f FPS",
                          project.c_str(), m_graph.Nodes().size(),
                          m_materialLibrary.Entries().size(), m_textureLibrary.Entries().size(),
                          m_renderer.MaterialResolution(), ImGui::GetIO().Framerate);

            const float summaryWidth = ImGui::CalcTextSize(summary).x;
            const float right = ImGui::GetWindowWidth() - summaryWidth -
                                ImGui::GetStyle().ItemSpacing.x * 2.0f;
            // 通知が長いときは重ねない。右寄せできる余白があるときだけ出す。
            if (right > ImGui::GetCursorPosX()) {
                ImGui::SetCursorPosX(right);
                ImGui::TextDisabled("%s", summary);
            }

            ImGui::EndMenuBar();
        }
    }
    ImGui::End();
}

float Application::DesiredUiScale() const {
    const io::UiSettings& ui = m_settings.Ui();
    return ui.followSystemScale ? m_imgui.MonitorScale() : ui.manualScale;
}

namespace {

// 拡大率を掛けた大きさ。動画やテクスチャの都合で偶数に丸める。
uint32_t ScaledClientSize(uint32_t base, float scale) {
    const auto scaled = static_cast<uint32_t>(std::lround(static_cast<float>(base) * scale));
    return (scaled + 1u) & ~1u;
}

}  // namespace

uint32_t Application::DefaultClientWidth() const {
    return ScaledClientSize(kInitialWidth, m_imgui.UiScale());
}

uint32_t Application::DefaultClientHeight() const {
    return ScaledClientSize(kInitialHeight, m_imgui.UiScale());
}

// UI を拡大したぶんウィンドウも大きくする。こうすると**作業面積（論理サイズ）が
// 1920x1080 のまま**で、文字と部品だけが大きくなる。
// 拡大率だけ上げるとパネルが窮屈になるので、既定では大きさを揃える。
void Application::ApplyUiScale() {
    const float desired = DesiredUiScale();
    if (std::abs(desired - m_imgui.UiScale()) < 0.001f) {
        return;
    }

    m_imgui.SetUiScale(desired);
    m_window.ResizeClient(DefaultClientWidth(), DefaultClientHeight());
}

// アプリの設定。プロジェクトには保存しない（`%LOCALAPPDATA%` の settings.json）。
//
// ドックへは収めない。常設パネルは「何を作るか」に関わるものだけにして、
// たまにしか触らない設定で作業面積を食わない。
void Application::DrawSettingsWindow() {
    if (!m_showSettings) {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(ui::Scaled(460.0f), ui::Scaled(500.0f)),
                             ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("設定", &m_showSettings)) {
        ImGui::End();
        return;
    }

    io::UiSettings& ui = m_settings.Ui();
    bool changed = false;

    ui::SectionHeader("UI");
    if (ui::BeginPropertyTable("settingsUiRows")) {
        ui::PropertyValue("表示スケール", "%.0f%%  (Windows)", m_imgui.MonitorScale() * 100.0f);

        const io::UiSettings defaults;
        changed |= ui::PropertyBool("スケール追従", &ui.followSystemScale,
                                    defaults.followSystemScale,
                                    "Windows の表示スケール（DPI）に UI の大きさを合わせる。"
                                    "切ると常に 100% で描く");

        if (!ui.followSystemScale) {
            static const char* const kScaleLabels[] = {"100%", "125%", "150%", "200%"};
            constexpr float kScaleValues[] = {1.0f, 1.25f, 1.5f, 2.0f};
            int selected = 0;
            for (int i = 0; i < IM_ARRAYSIZE(kScaleValues); ++i) {
                if (std::abs(kScaleValues[i] - ui.manualScale) < 0.01f) {
                    selected = i;
                }
            }
            if (ui::PropertyCombo("拡大率", &selected, kScaleLabels, IM_ARRAYSIZE(kScaleLabels), 0,
                                  "UI の大きさ。ウィンドウの大きさは変わらない")) {
                ui.manualScale = kScaleValues[selected];
                changed = true;
            }
        }
        ui::EndPropertyTable();
    }
    ui::HintText("拡大するとウィンドウも同じ倍率で大きくなる（作業面積は変わらない）");
    ui::HintText("パネルの幅は ini にピクセルで残る。ずれたら ウィンドウ > レイアウトをリセット");

    ui::SectionHeader("ウィンドウ");
    if (ui::BeginPropertyTable("settingsWindowRows")) {
        ui::PropertyValue("描画サイズ", "%u x %u", m_device.Width(), m_device.Height());
        ui::PropertyLabelEmpty("windowReset");
        // 録画やスクリーンショットの解像度を揃えるために、既定へ戻す手段を残す。
        if (ui::Button("既定の大きさに戻す", ui::kWideButtonWidth)) {
            m_window.ResizeClient(DefaultClientWidth(), DefaultClientHeight());
        }
        ui::PropertyEnd();
        ui::EndPropertyTable();
    }
    ui::HintText("既定は %u x %u（%u x %u の %.0f%%）", DefaultClientWidth(),
                 DefaultClientHeight(), kInitialWidth, kInitialHeight,
                 m_imgui.UiScale() * 100.0f);

    ui::SectionHeader("表示");
    if (ui::BeginPropertyTable("settingsDisplayRows")) {
        // **設定を直接編集する。** 写しを経由すると、書き戻しを 1 つ忘れただけで
        // 「画面では変わったのに次回の起動で戻る」という壊れ方をする。
        io::DisplaySettings& display = m_settings.Display();
        const io::DisplaySettings defaults;
        bool displayChanged = false;
        displayChanged |= ui::PropertyBool("垂直同期", &display.vsync, defaults.vsync);

        // FPS の上限。**値が少数の離散値なので、スライダーではなく選択にする。**
        // 「60 に合わせたつもりが 59」のような外れ方をしない。
        static const char* const kLimitLabels[] = {"制限なし", "144", "120", "60", "30"};
        constexpr int kLimitValues[] = {0, 144, 120, 60, 30};
        const auto findLimit = [&kLimitValues](int fps) {
            for (int i = 0; i < IM_ARRAYSIZE(kLimitValues); ++i) {
                if (kLimitValues[i] == fps) {
                    return i;
                }
            }
            return 0;
        };

        int limit = findLimit(display.frameRateLimit);
        if (ui::PropertyCombo("FPS 上限", &limit, kLimitLabels, IM_ARRAYSIZE(kLimitLabels), 0,
                              "前面にあるときの上限。垂直同期が入っていれば、"
                              "モニタのリフレッシュレートとの低いほうが効く")) {
            display.frameRateLimit = kLimitValues[limit];
            displayChanged = true;
        }

        static const char* const kInactiveLabels[] = {"制限なし", "30", "10", "5"};
        constexpr int kInactiveValues[] = {0, 30, 10, 5};
        int inactive = 0;
        for (int i = 0; i < IM_ARRAYSIZE(kInactiveValues); ++i) {
            if (kInactiveValues[i] == display.inactiveFrameRateLimit) {
                inactive = i;
            }
        }
        if (ui::PropertyCombo("背面のとき", &inactive, kInactiveLabels,
                              IM_ARRAYSIZE(kInactiveLabels), 2,
                              "他のウィンドウの後ろにあるときの上限。"
                              "見えていない絵に GPU を回し続けないための設定。"
                              "完全には止めないので、シェーダのホットリロードは効いたまま")) {
            display.inactiveFrameRateLimit = kInactiveValues[inactive];
            displayChanged = true;
        }
        displayChanged |= ui::PropertyBool("ホットリロード", &display.hotReload,
                                           defaults.hotReload,
                                           "shaders/ の更新を検出して PSO を作り直す");
        // 背景色はバックバッファ（非 sRGB）へそのまま書く表示色なので、
        // リニア変換は挟まない。
        displayChanged |= ui::PropertyColor("背景色", display.clearColor, kDefaultClearColor);
        if (displayChanged) {
            // design-guide の「設定ウィンドウ」に従い、変えたらその場で書く。
            // 直接編集しているので、ここで写し戻す必要はない。
            changed = true;
        }
        ui::EndPropertyTable();
    }

    if (changed) {
        // 次のフレームの頭で拡大率を反映し、設定を書き出す。
        m_settings.Save();
    }

    ImGui::End();
}

// 実行状況の情報。常設ドックの面積を使わず、必要なときだけメニューから開く。
void Application::DrawInfoWindow() {
    if (!m_showInfo) {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(ui::Scaled(460.0f), ui::Scaled(360.0f)),
                             ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("情報", &m_showInfo)) {
        ImGui::End();
        return;
    }

    const ImGuiIO& io = ImGui::GetIO();

    if (ui::BeginPropertyTable("infoRows")) {
        ui::PropertyValue("バージョン", "%s", TG_APP_VERSION);
        ui::PropertyValue("フレーム", "%.1f FPS (%.3f ms)", io.Framerate,
                          1000.0f / io.Framerate);
        ui::PropertyValue("バックバッファ", "%u x %u", m_device.Width(), m_device.Height());
        ui::PropertyValue("ビューポート", "%u x %u", m_renderer.Width(), m_renderer.Height());
        ui::PropertyValue("合成", "%u^2 / %u レイヤー / %u タイル",
                          m_renderer.MaterialResolution(),
                          m_renderer.Evaluator().EvaluatedLayerCount(),
                          m_renderer.Evaluator().EvaluatedTileCount());
        ui::PropertyValue("ペイント", "%zu 枚 / %u^2 / 履歴 %zu 段", m_paintMasks.Count(),
                          m_paintMasks.Resolution(), m_paintMasks.UndoCount());
        ui::PropertyValue("アンドゥ", "%zu 段 / やり直し %zu 段", m_undoHistory.UndoCount(),
                          m_undoHistory.RedoCount());
        ui::PropertyValue("PSO", "%zu 件", m_pipelineCache.PipelineCount());
        ui::PropertyValue("解放待ち", "%zu 件", m_device.PendingDeletionCount());
        ui::PropertyValue("アップロード", "%llu / %llu KB",
                          static_cast<unsigned long long>(m_device.Upload().PeakBytes() / 1024),
                          static_cast<unsigned long long>(m_device.Upload().BytesPerFrame() / 1024));
        ui::EndPropertyTable();
    }

    ImGui::End();
}

}  // namespace tg
