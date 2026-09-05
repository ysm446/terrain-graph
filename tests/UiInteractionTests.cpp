// UI の相互作用のテスト。GPU もウィンドウも要らない。
//
// ImGui へマウスイベントを直接注入し、1 フレームずつ進めて結果を見る。
// **スクリーンショットでは確認できない操作**（ドラッグ、ホバー）を押さえるためのもの。
//
// ここで守りたいのは次の 2 つ。どちらも壊れても画面には何も出ないので、
// 手で気づくのが難しい。
//
//   1. テクスチャのサムネイルからマテリアルのマップ欄へドラッグして割り当てられる
//   2. マップ欄のツールチップを足しても、1 が壊れない

#include "ui/UiStyle.h"

#include "TestSupport.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <cstdio>
#include <cstring>

namespace {

// アプリと同じペイロード種別（Application.cpp の kTextureDragDropType）。
const char* const kPayloadType = "TG_TEXTURE";

const ImVec2 kSourcePos(50.0f, 50.0f);
const ImVec2 kTargetPos(50.0f, 300.0f);
constexpr float kThumbnailSize = 72.0f;

using tg::tests::Check;
using tg::tests::Section;

void Frame(float x, float y, bool down) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(x, y);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
    ImGui::NewFrame();
}

// ツールチップのウィンドウが実際に出ているか。
bool TooltipVisible() {
    const ImGuiContext& g = *ImGui::GetCurrentContext();
    for (const ImGuiWindow* window : g.Windows) {
        if (window->Active && std::strstr(window->Name, "Tooltip") != nullptr) {
            return true;
        }
    }
    return false;
}

struct Result {
    bool sourceStarted = false;
    bool dropped = false;
    bool tooltipShown = false;
};

// ドラッグ元。アプリのテクスチャ一覧と同じ形。
//
// **「ImGui::Image を使うと動かない」ことはここでは試せない。**
// ID を持たないアイテムを掴むと ImGui が IM_ASSERT(0) で止まるようになっており
// （imgui.cpp の BeginDragDropSource）、Debug ビルドではテストごと落ちる。
// 裏を返せば、実装を元に戻してしまってもアプリの Debug ビルドが即座に知らせてくれる。
void SubmitSource(Result& result) {
    ImGui::SetCursorScreenPos(kSourcePos);
    ImGui::PushID(1234);
    tg::ui::ThumbnailButton("##thumbnail", ImTextureID{}, kThumbnailSize, true);
    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoHoldToOpenOthers)) {
        result.sourceStarted = true;
        const unsigned int textureId = 7;
        ImGui::SetDragDropPayload(kPayloadType, &textureId, sizeof(textureId));
        ImGui::TextUnformatted("dragging");
        ImGui::EndDragDropSource();
    }
    ImGui::PopID();
}

// 落とす先。アプリの DrawTextureCombo と同じ順序で積む。
// コンボ → ドロップの受け口 → ツールチップ。この順序が要点。
void SubmitTarget(Result& result, bool withTooltip) {
    ImGui::SetCursorScreenPos(kTargetPos);
    ImGui::SetNextItemWidth(190.0f);
    if (ImGui::BeginCombo("##value", "T_Dusty_Gravel_Ground_vd3odhr_2K_D.EXR")) {
        ImGui::EndCombo();
    }
    if (ImGui::BeginDragDropTarget()) {
        if (ImGui::AcceptDragDropPayload(kPayloadType) != nullptr) {
            result.dropped = true;
        }
        ImGui::EndDragDropTarget();
    }
    if (withTooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort) &&
        ImGui::GetDragDropPayload() == nullptr) {
        ImGui::SetTooltip("T_Dusty_Gravel_Ground_vd3odhr_2K_D.EXR\n2048 x 2048");
    }
}

void BeginPanel() {
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(600.0f, 600.0f));
    ImGui::Begin("test", nullptr, ImGuiWindowFlags_NoSavedSettings);
}

// サムネイルを掴んでコンボへ落とす。
Result RunDrag(bool withTooltip) {
    Result result;
    struct Step {
        float x;
        float y;
        bool down;
    };
    // 押した位置から動かさないとドラッグにならないので、途中の移動を挟む。
    const Step steps[] = {
        {kSourcePos.x + 10.0f, kSourcePos.y + 10.0f, false},
        {kSourcePos.x + 10.0f, kSourcePos.y + 10.0f, true},
        {kSourcePos.x + 40.0f, kSourcePos.y + 60.0f, true},
        {kTargetPos.x + 40.0f, kTargetPos.y + 10.0f, true},
        {kTargetPos.x + 40.0f, kTargetPos.y + 10.0f, false},
    };

    for (const Step& step : steps) {
        Frame(step.x, step.y, step.down);
        BeginPanel();
        SubmitSource(result);
        SubmitTarget(result, withTooltip);
        ImGui::End();
        ImGui::Render();
    }
    return result;
}

// コンボの上にカーソルを置いたまま進める（表示の遅延を越えるため）。
Result RunHover(bool withTooltip) {
    Result result;
    for (int i = 0; i < 40; ++i) {
        Frame(kTargetPos.x + 40.0f, kTargetPos.y + 10.0f, false);
        BeginPanel();
        SubmitSource(result);
        SubmitTarget(result, withTooltip);
        ImGui::End();
        ImGui::Render();
        if (TooltipVisible()) {
            result.tooltipShown = true;
        }
    }
    return result;
}

// --- 区画の境界（HorizontalSplitter） -----------------------------------
//
// 掴んで動かせること、範囲を越えないこと、離したときだけ true を返すことを見る。
// **どれもスクリーンショットには写らない。**

struct SplitterResult {
    float height = 0.0f;
    int releasedCount = 0;
};

// y へ向かって境界をドラッグする。steps は途中の移動を含む。
SplitterResult RunSplitterDrag(float startHeight, float dragToY, float minHeight,
                               float maxHeight) {
    SplitterResult result;
    result.height = startHeight;

    // 境界は上の区画の下に余白を挟んだ位置。掴む高さの中ほどを狙う。
    // **位置は定数から導く。** 直値で書くと、余白や掴み幅を変えたときに
    // 「掴めていないのに落ちる」テストになって原因が分かりにくい。
    const float grabY = kSourcePos.y + startHeight + tg::ui::kSplitterMargin +
                        tg::ui::kSplitterGrabWidth * 0.5f;
    const float grabX = kSourcePos.x + 100.0f;
    struct Step {
        float y;
        bool down;
    };
    const Step steps[] = {
        {grabY, false},    // ホバー
        {grabY, true},     // 掴む
        {dragToY, true},   // 動かす
        {dragToY, true},   // 動かし切った状態でもう 1 フレーム
        {dragToY, false},  // 離す
    };

    for (const Step& step : steps) {
        Frame(grabX, step.y, step.down);
        BeginPanel();
        ImGui::SetCursorScreenPos(kSourcePos);
        ImGui::BeginChild("top", ImVec2(300.0f, result.height));
        ImGui::EndChild();
        if (tg::ui::HorizontalSplitter("split", &result.height, minHeight, maxHeight, 300.0f)) {
            ++result.releasedCount;
        }
        ImGui::BeginChild("bottom", ImVec2(300.0f, 0.0f));
        ImGui::EndChild();
        ImGui::End();
        ImGui::Render();
    }
    return result;
}

// --- 数値の直接入力（Enter で確定） -----------------------------------------
//
// Ctrl + クリックで入る直接入力は、打っている途中で値を書き戻さないこと。
// 途中で書き戻すと、そのたびに合成が走って画面が切り替わる。

struct TypingResult {
    float value = 1.0f;
    int changedCount = 0;        // PropertyFloat が true を返した回数
    bool changedWhileTyping = false;
    float valueWhileTyping = 1.0f;  // 打っている最中に見えた値
};

TypingResult RunSliderTyping(bool confirmWithEnter) {
    TypingResult result;
    ImGuiIO& io = ImGui::GetIO();
    ImRect sliderRect;
    bool typing = false;

    // 1 フレーム描いて、スライダーの矩形を取る（テーブルの値列）。
    const auto draw = [&]() {
        BeginPanel();
        ImGui::SetCursorScreenPos(kSourcePos);
        if (tg::ui::BeginPropertyTable("rows")) {
            if (tg::ui::PropertyFloat("値", &result.value, 0.0f, 100.0f, 1.0f, "テスト", "%.1f")) {
                ++result.changedCount;
                if (typing) {
                    result.changedWhileTyping = true;
                }
            }
            sliderRect = ImGui::TableGetCellBgRect(ImGui::GetCurrentTable(), 1);
            tg::ui::EndPropertyTable();
        }
        ImGui::End();
        ImGui::Render();
    };

    Frame(0.0f, 0.0f, false);
    draw();
    const ImVec2 target(sliderRect.Min.x + 20.0f, (sliderRect.Min.y + sliderRect.Max.y) * 0.5f);

    // ホバー → Ctrl + クリック → 離す。これで直接入力に入る。
    Frame(target.x, target.y, false);
    draw();
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    Frame(target.x, target.y, true);
    draw();
    Frame(target.x, target.y, false);
    draw();
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    Frame(target.x, target.y, false);
    draw();

    // 「4」「2」と打つ。打っている最中は値が動かないこと。
    typing = true;
    io.AddInputCharacter('4');
    Frame(target.x, target.y, false);
    draw();
    io.AddInputCharacter('2');
    Frame(target.x, target.y, false);
    draw();
    result.valueWhileTyping = result.value;
    typing = false;

    // Enter で確定（または Esc で取り消し）。
    io.AddKeyEvent(confirmWithEnter ? ImGuiKey_Enter : ImGuiKey_Escape, true);
    Frame(target.x, target.y, false);
    draw();
    io.AddKeyEvent(confirmWithEnter ? ImGuiKey_Enter : ImGuiKey_Escape, false);
    Frame(target.x, target.y, false);
    draw();
    Frame(target.x, target.y, false);
    draw();
    return result;
}

}  // namespace

void RunUiInteractionTests() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    // 設定ファイルを書かせない。テストが作業ディレクトリを汚さないようにする。
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(600.0f, 600.0f);
    io.DeltaTime = 1.0f / 60.0f;
    io.Fonts->AddFontDefault();
    unsigned char* pixels = nullptr;
    int width = 0;
    int height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    io.Fonts->SetTexID(static_cast<ImTextureID>(1));

    std::printf("ImGui %s\n", IMGUI_VERSION);

    Section("ドラッグ&ドロップ");
    const Result helper = RunDrag(true);
    Check(helper.sourceStarted, "ui::ThumbnailButton はドラッグ元になる");
    Check(helper.dropped, "サムネイルからマップ欄へ割り当てられる");

    const Result withoutTooltip = RunDrag(false);
    Check(withoutTooltip.dropped, "ツールチップの有無で割り当ての成否が変わらない");

    Section("ツールチップ");
    const Result hoverOn = RunHover(true);
    Check(hoverOn.tooltipShown, "マップ欄をホバーするとツールチップが出る");
    const Result hoverOff = RunHover(false);
    Check(!hoverOff.tooltipShown, "ツールチップを積まなければ出ない（対照）");

    Section("数値の直接入力（Enter で確定）");
    const TypingResult typed = RunSliderTyping(true);
    Check(!typed.changedWhileTyping && typed.valueWhileTyping == 1.0f,
          "打っている最中は値を書き戻さない");
    Check(typed.changedCount == 1 && typed.value == 42.0f,
          "Enter で 1 回だけ確定し、打った値になる");
    const TypingResult cancelled = RunSliderTyping(false);
    Check(cancelled.changedCount == 0 && cancelled.value == 1.0f,
          "Esc で取り消すと元の値のまま");

    Section("区画の境界（ドラッグで高さを変える）");
    // 下へ 80 動かす。掴んだ位置から素直に広がること。
    const SplitterResult widened = RunSplitterDrag(200.0f, kSourcePos.y + 280.0f, 100.0f, 400.0f);
    Check(widened.height > 260.0f && widened.height < 300.0f,
          "境界を下へドラッグすると上の区画が広がる");
    Check(widened.releasedCount == 1, "離したフレームだけ true を返す（保存の合図）");

    // 上限を越えて引いても止まること。越えると下の区画が潰れる。
    const SplitterResult clamped = RunSplitterDrag(200.0f, kSourcePos.y + 900.0f, 100.0f, 260.0f);
    Check(clamped.height <= 260.0f, "上限を越えて広がらない");

    // 下限も同じ。上へ引き切っても一覧が消えない。
    const SplitterResult shrunk = RunSplitterDrag(200.0f, kSourcePos.y - 400.0f, 150.0f, 400.0f);
    Check(shrunk.height >= 150.0f, "下限を割って縮まない");

    ImGui::DestroyContext();
}
