#include "ui/UiStyle.h"

#include "core/ColorSpace.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <array>
#include <cstring>
#include <string>
#include <vector>

namespace tg::ui {
namespace {

float g_dpiScale = 1.0f;

// グレー基調のパレット。彩度はほぼ持たせず、明度だけで階層を作る。
// 値は sRGB の 8bit を 255 で割ったもの。design-guide.md の表と一致させること。
constexpr ImVec4 Gray(float value, float alpha = 1.0f) {
    return ImVec4(value, value, value, alpha);
}

constexpr ImVec4 kWindowBg = Gray(0.118f);      // #1E1E1E
constexpr ImVec4 kPanelBg = Gray(0.137f);       // #232323
constexpr ImVec4 kTitleBg = Gray(0.094f);       // #181818
constexpr ImVec4 kFrameBg = Gray(0.169f);       // #2B2B2B
constexpr ImVec4 kFrameBgHovered = Gray(0.212f);// #363636
constexpr ImVec4 kFrameBgActive = Gray(0.255f); // #414141
constexpr ImVec4 kHeaderBg = Gray(0.196f);      // #323232
constexpr ImVec4 kHeaderHovered = Gray(0.235f); // #3C3C3C
constexpr ImVec4 kHeaderActive = Gray(0.290f);  // #4A4A4A
constexpr ImVec4 kBorder = Gray(0.204f);        // #343434
// タブ。**矩形で、明度の 3 段（帯 < 非選択 < 選択）だけで状態を表す。**
// 帯（タブを並べる下地）を一番暗くすることで、タブが板として浮いて見える。
constexpr ImVec4 kTab = Gray(0.169f);           // #2B2B2B 非選択
constexpr ImVec4 kTabHovered = Gray(0.212f);    // #363636
constexpr ImVec4 kTabSelected = Gray(0.243f);   // #3E3E3E 選択
// 選択中のタブの上辺に引く線。**色は付けない。**
// 明度だけで階層を作る方針に合わせ、中身との繋がりを示す控えめな線に留める。
constexpr ImVec4 kTabOverline = Gray(0.400f);   // #666666
constexpr ImVec4 kSeparator = Gray(0.220f);     // #383838
constexpr ImVec4 kGrab = Gray(0.376f);          // #606060
constexpr ImVec4 kGrabActive = Gray(0.510f);    // #828282
constexpr ImVec4 kText = Gray(0.804f);          // #CDCDCD
constexpr ImVec4 kTextDisabled = Gray(0.455f);  // #747474

// 既定値マーカーの「既定値のまま」の色。専用に持つ。
//
// もとは kTextDisabled (#747474) を使っていたが、アクセント (#96A3AD) と明度が近く、
// **変えてあるかどうかがひと目で分からなかった。** はっきり暗く落として差をつける。
// kTextDisabled 自体は補助テキストでも使うので、そちらは動かさない。
constexpr ImVec4 kResetDot = Gray(0.271f);         // #454545
// 既定値のままでもホバーで少し持ち上げる。押せることが分かるようにするため。
// アクセントには寄せない（変更済みと紛らわしくなる）。
constexpr ImVec4 kResetDotHovered = Gray(0.455f);  // #747474

// 唯一のアクセント。彩度を持たせすぎるとグレー基調が崩れるので、
// わずかに青へ寄せた明るい灰にとどめる。選択・チェック・つまみの強調にだけ使う。
constexpr ImVec4 kAccent = ImVec4(0.588f, 0.639f, 0.678f, 1.0f);  // #96A3AD

// 通知の意味色。座標軸ギズモの X/Y/Z と同じく「意味を持つ色」で、
// テーマのグレーとは別枠。彩度を上げるとグレー基調が崩れるので低めに抑える。
constexpr ImVec4 kWarn = ImVec4(0.749f, 0.627f, 0.416f, 1.0f);   // #BFA06A
constexpr ImVec4 kError = ImVec4(0.784f, 0.482f, 0.447f, 1.0f);  // #C87B72

// ツールチップの折り返し幅（フォントサイズ比）。
constexpr float kTooltipWrapRatio = 22.0f;

void DrawTooltip(const char* tooltip) {
    if (tooltip == nullptr || tooltip[0] == '\0') {
        return;
    }
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        return;
    }
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * kTooltipWrapRatio);
    ImGui::TextUnformatted(tooltip);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

// 数値行のツールチップ。**直接入力できることは気づきにくい**ので必ず添える。
// 戻り値は次に呼ぶまでの一時領域を指す（そのまま PropertyLabel へ渡す前提）。
const char* NumericTooltip(const char* tooltip) {
    static char buffer[512];
    constexpr const char* kInputHelp = "Ctrl + クリックで直接入力";
    if (tooltip == nullptr || tooltip[0] == '\0') {
        return kInputHelp;
    }
    std::snprintf(buffer, sizeof(buffer), "%s\n%s", tooltip, kInputHelp);
    return buffer;
}

// 既定値マーカー。既定値と違うときだけ明るく塗り、押すと既定値へ戻す。
//
// 「既定値から変えてあるか」の視覚表現はこの点だけに集約する。
// ラベルの色を変えるといった二重の表現は入れない。
bool ResetDot(bool isDefault, const std::string& defaultText) {
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);

    const float size = ImGui::GetFrameHeight();
    const bool pressed = ImGui::InvisibleButton("##reset", ImVec2(size, size));

    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    const ImVec2 center((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
    const bool hovered = ImGui::IsItemHovered();

    ImU32 color = ImGui::GetColorU32(hovered ? kResetDotHovered : kResetDot);
    if (!isDefault) {
        color = ImGui::GetColorU32(hovered ? ImGuiCol_Text : ImGuiCol_CheckMark);
    }
    const float radius = std::max(2.0f, size * 0.16f);
    ImGui::GetWindowDrawList()->AddCircleFilled(center, radius, color);

    if (hovered) {
        ImGui::SetTooltip("既定値に戻す\n既定値: %s", defaultText.c_str());
    }
    return pressed;
}

std::string FormatFloat(float value, const char* format) {
    char buffer[64] = {};
    std::snprintf(buffer, sizeof(buffer), format, value);
    return buffer;
}

bool NearlyEqual(float a, float b) {
    return std::fabs(a - b) <= 1e-5f * std::max(1.0f, std::max(std::fabs(a), std::fabs(b)));
}

// スライダーやコンボの幅。値列の残り幅から既定値マーカーぶんを引いて丸める。
float ValueWidth(float minWidth, float maxWidth) {
    const float reserved = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x;
    return std::clamp(ImGui::GetContentRegionAvail().x - reserved, Scaled(minWidth),
                      Scaled(maxWidth));
}

}  // namespace

// サムネイルの選択枠。画像の上に重ねて描く。
//
// 背景を敷く方式では表せない。サムネイルが不透明だと下が完全に隠れるため。
// 選択はアクセント、ホバーは押せることが分かる程度の弱い枠にとどめる。
void ThumbnailFrame(const ImVec2& min, const ImVec2& max, bool selected, bool hovered) {
    if (!selected && !hovered) {
        return;
    }

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const float rounding = ImGui::GetStyle().FrameRounding;

    if (selected) {
        // 枠は矩形の内側へ寄せて描く。ちょうど境界へ置くと、
        // 隣のサムネイルとの間で線の太さが揃わない。
        const float thickness = Scaled(2.0f);
        const float inset = thickness * 0.5f;
        drawList->AddRect(ImVec2(min.x + inset, min.y + inset),
                          ImVec2(max.x - inset, max.y - inset),
                          ImGui::GetColorU32(ImGuiCol_CheckMark), rounding, 0, thickness);
        return;
    }

    drawList->AddRect(min, max, ImGui::GetColorU32(ImGuiCol_HeaderHovered), rounding, 0,
                      Scaled(1.0f));
}

Thumbnail ThumbnailButton(const char* id, ImTextureID texture, float size, bool selected) {
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 max(min.x + size, min.y + size);

    // ID を持つアイテムを先に置く。これが無いとドラッグ元にできない。
    ImGui::InvisibleButton(id, ImVec2(size, size));

    Thumbnail state;
    state.hovered = ImGui::IsItemHovered();
    state.clicked = ImGui::IsItemClicked();

    // **まだ絵が無いときは枠だけ描く。** ImTextureID の 0 は ImTextureID_Invalid で、
    // そのまま AddImage へ渡すとデバッグビルドの ImGui がアサートで落ちる。
    // 天球のサムネイルは HDR の読み込みを伴うので 1 フレームに 1 枚ずつ作られ、
    // 未生成の枠が必ず一覧に並ぶ。
    if (texture != ImTextureID_Invalid) {
        ImGui::GetWindowDrawList()->AddImage(texture, min, max);
    }
    ThumbnailFrame(min, max, selected, state.hovered);
    return state;
}

void ThumbnailImage(ImTextureID texture, float size) {
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 max(min.x + size, min.y + size);

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const float rounding = ImGui::GetStyle().FrameRounding;
    if (texture != 0) {
        drawList->AddImage(texture, min, max, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f),
                           IM_COL32_WHITE);
    }
    // まだ中身が無いときも場所は空けておく。行ごとに幅が変わると読みにくい。
    // 枠は中身の有無に関わらず引く。行に並ぶ箱の大きさを揃えて見せるため。
    drawList->AddRect(min, max, ImGui::GetColorU32(ImGuiCol_Border), rounding, 0, Scaled(1.0f));

    ImGui::Dummy(ImVec2(size, size));
}

void ColorSwatch(const ImVec4& color, float size) {
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 max(min.x + size, min.y + size);

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const float rounding = ImGui::GetStyle().FrameRounding;
    // 色そのものが中身なので塗る。枠だけはテーマの色を使う。
    drawList->AddRectFilled(min, max, ImGui::GetColorU32(color), rounding);
    drawList->AddRect(min, max, ImGui::GetColorU32(ImGuiCol_Border), rounding, 0, Scaled(1.0f));

    ImGui::Dummy(ImVec2(size, size));
}

// レイヤー種類のアイコン。字形を持たないので図形で描く。
//
// ThumbnailImage / ColorSwatch と同じ大きさ・同じ枠のタイルに、
// 本文の色（テーマ）で線画を載せる。塗りは持たせない。
// 一覧の行では素材のサムネイルと同じ場所に並ぶので、箱の見た目を揃える。
namespace {

void IconTileFrame(ImDrawList* drawList, const ImVec2& min, const ImVec2& max) {
    drawList->AddRect(min, max, ImGui::GetColorU32(ImGuiCol_Border),
                      ImGui::GetStyle().FrameRounding, 0, Scaled(1.0f));
}

}  // namespace

void MountainIcon(float size) {
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 max(min.x + size, min.y + size);
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    // 高い峰と低い峰の 2 つ。地面の線は引かない（稜線だけで山と読める）。
    const auto at = [&](float x, float y) {
        return ImVec2(min.x + x * size, min.y + y * size);
    };
    drawList->PathClear();
    drawList->PathLineTo(at(0.14f, 0.74f));
    drawList->PathLineTo(at(0.40f, 0.30f));
    drawList->PathLineTo(at(0.54f, 0.52f));
    drawList->PathLineTo(at(0.68f, 0.40f));
    drawList->PathLineTo(at(0.86f, 0.74f));
    drawList->PathStroke(ImGui::GetColorU32(ImGuiCol_Text), ImDrawFlags_None, Scaled(1.3f));

    IconTileFrame(drawList, min, max);
    ImGui::Dummy(ImVec2(size, size));
}

void WavesIcon(float size) {
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 max(min.x + size, min.y + size);
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    // 横に走る波を 2 本。1 周期の正弦をずらして重ねる。
    constexpr int kSegments = 16;
    constexpr float kPi = 3.14159265358979323846f;
    const float left = min.x + size * 0.16f;
    const float width = size * 0.68f;
    const float amplitude = size * 0.07f;
    const ImU32 color = ImGui::GetColorU32(ImGuiCol_Text);
    for (int wave = 0; wave < 2; ++wave) {
        const float baseY = min.y + size * (0.40f + 0.24f * static_cast<float>(wave));
        drawList->PathClear();
        for (int i = 0; i <= kSegments; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(kSegments);
            drawList->PathLineTo(
                ImVec2(left + t * width, baseY + std::sin(t * 2.0f * kPi) * amplitude));
        }
        drawList->PathStroke(color, ImDrawFlags_None, Scaled(1.3f));
    }

    IconTileFrame(drawList, min, max);
    ImGui::Dummy(ImVec2(size, size));
}

// 目のアイコン。字形を持たないので図形で描く。
//
// 開いた目は上下 2 本の放物線で作った紡錘形と瞳、閉じた目は下向きの弧 1 本。
// 「見えている / 見えていない」が一目で分かればよいので、これ以上作り込まない。
namespace {

void DrawEye(ImDrawList* drawList, const ImVec2& center, float size, bool open, ImU32 color) {
    constexpr int kSegments = 14;
    const float halfWidth = size * 0.46f;
    const float halfHeight = size * 0.30f;
    const float thickness = Scaled(1.3f);

    if (!open) {
        // 閉じたまぶた。下へふくらむ弧。
        drawList->PathClear();
        for (int i = 0; i <= kSegments; ++i) {
            const float t = -1.0f + 2.0f * static_cast<float>(i) / static_cast<float>(kSegments);
            drawList->PathLineTo(
                ImVec2(center.x + t * halfWidth, center.y + halfHeight * (1.0f - t * t)));
        }
        drawList->PathStroke(color, ImDrawFlags_None, thickness);
        return;
    }

    drawList->PathClear();
    for (int i = 0; i <= kSegments; ++i) {
        const float t = -1.0f + 2.0f * static_cast<float>(i) / static_cast<float>(kSegments);
        drawList->PathLineTo(
            ImVec2(center.x + t * halfWidth, center.y - halfHeight * (1.0f - t * t)));
    }
    for (int i = kSegments; i >= 0; --i) {
        const float t = -1.0f + 2.0f * static_cast<float>(i) / static_cast<float>(kSegments);
        drawList->PathLineTo(
            ImVec2(center.x + t * halfWidth, center.y + halfHeight * (1.0f - t * t)));
    }
    drawList->PathStroke(color, ImDrawFlags_Closed, thickness);
    drawList->AddCircleFilled(center, size * 0.15f, color, 12);
}

}  // namespace

bool EyeToggle(const char* id, bool* value, float size) {
    const ImVec2 min = ImGui::GetCursorScreenPos();

    ImGui::InvisibleButton(id, ImVec2(size, size));
    const bool hovered = ImGui::IsItemHovered();

    bool changed = false;
    if (ImGui::IsItemClicked()) {
        *value = !*value;
        changed = true;
    }

    // 表示中は本文の色、非表示は補助文字の色。ホバー中だけ本文の色へ持ち上げて、
    // 押せることを示す。色はテーマから引く（直書きしない）。
    const ImU32 color = (*value || hovered) ? ImGui::GetColorU32(ImGuiCol_Text)
                                            : ImGui::GetColorU32(ImGuiCol_TextDisabled);
    DrawEye(ImGui::GetWindowDrawList(), ImVec2(min.x + size * 0.5f, min.y + size * 0.5f), size,
            *value, color);

    return changed;
}

ImU32 WarnColor() {
    return ImGui::GetColorU32(kWarn);
}

ImU32 ErrorColor() {
    return ImGui::GetColorU32(kError);
}

float Scaled(float value) {
    return value * g_dpiScale;
}

void ApplyTheme(float dpiScale) {
    g_dpiScale = (dpiScale > 0.0f) ? dpiScale : 1.0f;

    // 拡大率を変えて呼び直しても累積しないよう、毎回既定値から作り直す。
    // ScaleAllSizes は「現在の値に掛ける」ので、リセット無しだと 125% → 150% の
    // ような変更で、下で上書きしていない寸法だけが二重に拡大されてしまう。
    ImGui::GetStyle() = ImGuiStyle{};
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();

    // 角丸は控えめに。面の階層は明度で作り、形では作らない。
    style.WindowRounding = 0.0f;
    style.ChildRounding = 2.0f;
    style.FrameRounding = 2.0f;
    style.PopupRounding = 3.0f;
    style.GrabRounding = 2.0f;
    // **タブだけは角を丸めない。** 帯の上に板が並んでいるように見せたいので、
    // 隣とのあいだに丸みの隙間を作らない。
    style.TabRounding = 0.0f;
    style.ScrollbarRounding = 2.0f;

    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;

    style.WindowPadding = ImVec2(10.0f, 8.0f);
    style.FramePadding = ImVec2(7.0f, 4.0f);
    style.CellPadding = ImVec2(4.0f, 3.0f);
    style.ItemSpacing = ImVec2(8.0f, 5.0f);
    style.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
    style.ScrollbarSize = 12.0f;
    style.GrabMinSize = 9.0f;

    style.WindowTitleAlign = ImVec2(0.0f, 0.5f);
    style.SeparatorTextBorderSize = 1.0f;
    style.SeparatorTextAlign = ImVec2(0.0f, 0.5f);
    style.SeparatorTextPadding = ImVec2(14.0f, 4.0f);

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text] = kText;
    colors[ImGuiCol_TextDisabled] = kTextDisabled;
    colors[ImGuiCol_WindowBg] = kWindowBg;
    colors[ImGuiCol_ChildBg] = kPanelBg;
    colors[ImGuiCol_PopupBg] = kPanelBg;
    colors[ImGuiCol_Border] = kBorder;
    colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);

    colors[ImGuiCol_FrameBg] = kFrameBg;
    colors[ImGuiCol_FrameBgHovered] = kFrameBgHovered;
    colors[ImGuiCol_FrameBgActive] = kFrameBgActive;

    // **ドックのタブを並べる帯もここで決まる**（ImGui はタブバーの下地に
    // TitleBg / TitleBgActive を使う）。フォーカスで明度を変えない。
    // 変えると、タブそのものの明度差（非選択 / 選択）が読み取りにくくなる。
    colors[ImGuiCol_TitleBg] = kTitleBg;
    colors[ImGuiCol_TitleBgActive] = kTitleBg;
    colors[ImGuiCol_TitleBgCollapsed] = kTitleBg;
    colors[ImGuiCol_MenuBarBg] = kTitleBg;

    colors[ImGuiCol_ScrollbarBg] = kWindowBg;
    colors[ImGuiCol_ScrollbarGrab] = Gray(0.290f);
    colors[ImGuiCol_ScrollbarGrabHovered] = Gray(0.353f);
    colors[ImGuiCol_ScrollbarGrabActive] = Gray(0.424f);

    colors[ImGuiCol_CheckMark] = kAccent;
    colors[ImGuiCol_SliderGrab] = kGrab;
    colors[ImGuiCol_SliderGrabActive] = kGrabActive;

    colors[ImGuiCol_Button] = kFrameBg;
    colors[ImGuiCol_ButtonHovered] = kFrameBgHovered;
    colors[ImGuiCol_ButtonActive] = kFrameBgActive;

    colors[ImGuiCol_Header] = kHeaderBg;
    colors[ImGuiCol_HeaderHovered] = kHeaderHovered;
    colors[ImGuiCol_HeaderActive] = kHeaderActive;

    colors[ImGuiCol_Separator] = kSeparator;
    colors[ImGuiCol_SeparatorHovered] = Gray(0.318f);
    colors[ImGuiCol_SeparatorActive] = kAccent;

    colors[ImGuiCol_ResizeGrip] = Gray(0.255f, 0.6f);
    colors[ImGuiCol_ResizeGripHovered] = Gray(0.353f, 0.8f);
    colors[ImGuiCol_ResizeGripActive] = kAccent;

    // **フォーカスの有無でタブの色を変えない**（Dimmed も同じ値にする）。
    // 見たいのは「どのタブを開いているか」で、枠がアクティブかどうかではない。
    colors[ImGuiCol_Tab] = kTab;
    colors[ImGuiCol_TabHovered] = kTabHovered;
    colors[ImGuiCol_TabSelected] = kTabSelected;
    colors[ImGuiCol_TabSelectedOverline] = kTabOverline;
    colors[ImGuiCol_TabDimmed] = kTab;
    colors[ImGuiCol_TabDimmedSelected] = kTabSelected;
    colors[ImGuiCol_TabDimmedSelectedOverline] = kTabOverline;

    colors[ImGuiCol_DockingPreview] = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.35f);
    colors[ImGuiCol_DockingEmptyBg] = kWindowBg;

    colors[ImGuiCol_TableHeaderBg] = kHeaderBg;
    colors[ImGuiCol_TableBorderStrong] = kBorder;
    colors[ImGuiCol_TableBorderLight] = kSeparator;
    colors[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    colors[ImGuiCol_TableRowBgAlt] = Gray(1.0f, 0.02f);

    colors[ImGuiCol_TextSelectedBg] = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.35f);
    colors[ImGuiCol_NavCursor] = kAccent;
    colors[ImGuiCol_DragDropTarget] = kAccent;
    colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.45f);

    colors[ImGuiCol_PlotLines] = kGrab;
    colors[ImGuiCol_PlotLinesHovered] = kAccent;
    colors[ImGuiCol_PlotHistogram] = kGrab;
    colors[ImGuiCol_PlotHistogramHovered] = kAccent;

    // 余白と枠は最後にまとめて DPI へ合わせる。色はスケールの影響を受けない。
    // 既定値から作り直しているので、100% へ戻す方向でも常に掛けてよい。
    style.ScaleAllSizes(g_dpiScale);
}

bool BeginPropertyTable(const char* id) {
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp)) {
        return false;
    }
    ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, Scaled(kLabelColumnWidth));
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
    return true;
}

void EndPropertyTable() {
    ImGui::EndTable();
}

void SectionHeader(const char* label) {
    ImGui::SeparatorText(label);
}

void PropertyLabel(const char* label, const char* tooltip) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    // 「パラメータ名：値」と読めるよう、ラベルの末尾にコロンを付ける。
    ImGui::Text("%s：", label);
    DrawTooltip(tooltip);
    ImGui::TableSetColumnIndex(1);
    ImGui::PushID(label);
}

void PropertyLabelEmpty(const char* id) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TableSetColumnIndex(1);
    ImGui::PushID(id);
}

void PropertyEnd() {
    ImGui::PopID();
}

bool PropertyFloat(const char* label, float* value, float minValue, float maxValue,
                   float defaultValue, const char* tooltip, const char* format,
                   ImGuiSliderFlags flags, float snapStep) {
    // 読み込み直後の範囲外値を UI 側で吸収する。
    *value = std::clamp(*value, minValue, maxValue);

    PropertyLabel(label, NumericTooltip(tooltip));
    ImGui::SetNextItemWidth(ValueWidth(kSliderMinWidth, kSliderMaxWidth));
    bool changed = ImGui::SliderFloat("##value", value, minValue, maxValue, format, flags);

    // ドラッグ中だけ刻みへ吸着させる。マウスを押していないときの変更
    // （Ctrl + クリックの直接入力、キーボード）は丸めない。
    if (changed && snapStep > 0.0f && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        *value = std::clamp(std::round(*value / snapStep) * snapStep, minValue, maxValue);
    }

    if (ResetDot(NearlyEqual(*value, defaultValue), FormatFloat(defaultValue, format))) {
        *value = std::clamp(defaultValue, minValue, maxValue);
        changed = true;
    }
    PropertyEnd();
    return changed;
}

bool PropertyInt(const char* label, int* value, int minValue, int maxValue, int defaultValue,
                 const char* tooltip) {
    *value = std::clamp(*value, minValue, maxValue);

    PropertyLabel(label, NumericTooltip(tooltip));
    ImGui::SetNextItemWidth(ValueWidth(kSliderMinWidth, kSliderMaxWidth));
    bool changed = ImGui::SliderInt("##value", value, minValue, maxValue);

    if (ResetDot(*value == defaultValue, std::to_string(defaultValue))) {
        *value = std::clamp(defaultValue, minValue, maxValue);
        changed = true;
    }
    PropertyEnd();
    return changed;
}

bool PropertyBool(const char* label, bool* value, bool defaultValue, const char* tooltip) {
    PropertyLabel(label, tooltip);
    bool changed = ImGui::Checkbox("##value", value);

    if (ResetDot(*value == defaultValue, defaultValue ? "オン" : "オフ")) {
        *value = defaultValue;
        changed = true;
    }
    PropertyEnd();
    return changed;
}

namespace {

// 色の行の実体。linear が真なら、値はリニアで持ちピッカーへは sRGB で見せる。
//
// **編集されたときだけ書き戻す。** 毎フレーム sRGB へ出して戻すと、
// 変換の丸め誤差が少しずつ積もって値が動いてしまう。
bool PropertyColorImpl(const char* label, float* rgb, const float* defaultRgb,
                       const char* tooltip, bool linear) {
    PropertyLabel(label, tooltip);

    float shown[3];
    for (int i = 0; i < 3; ++i) {
        shown[i] = linear ? LinearToSrgb(rgb[i]) : rgb[i];
    }

    ImGui::SetNextItemWidth(ValueWidth(kSliderMinWidth, kSliderMaxWidth));
    bool changed = ImGui::ColorEdit3("##value", shown,
                                     ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);
    if (changed) {
        for (int i = 0; i < 3; ++i) {
            rgb[i] = linear ? SrgbToLinear(shown[i]) : shown[i];
        }
    }

    // 既定かどうかは格納している値どうしで見る（変換を挟むと丸めで揺れる）。
    const bool isDefault = NearlyEqual(rgb[0], defaultRgb[0]) &&
                           NearlyEqual(rgb[1], defaultRgb[1]) &&
                           NearlyEqual(rgb[2], defaultRgb[2]);
    // 出す数字はピッカーに合わせる。リニア値を出しても、
    // ピッカーに見えている数字と違うので手掛かりにならない。
    char defaultText[64] = {};
    std::snprintf(defaultText, sizeof(defaultText), "%.2f, %.2f, %.2f",
                  linear ? LinearToSrgb(defaultRgb[0]) : defaultRgb[0],
                  linear ? LinearToSrgb(defaultRgb[1]) : defaultRgb[1],
                  linear ? LinearToSrgb(defaultRgb[2]) : defaultRgb[2]);
    if (ResetDot(isDefault, defaultText)) {
        rgb[0] = defaultRgb[0];
        rgb[1] = defaultRgb[1];
        rgb[2] = defaultRgb[2];
        changed = true;
    }
    PropertyEnd();
    return changed;
}

}  // namespace

bool PropertyColor(const char* label, float* rgb, const float* defaultRgb, const char* tooltip) {
    return PropertyColorImpl(label, rgb, defaultRgb, tooltip, false);
}

bool PropertyColorLinear(const char* label, float* linearRgb, const float* defaultLinearRgb,
                         const char* tooltip) {
    return PropertyColorImpl(label, linearRgb, defaultLinearRgb, tooltip, true);
}

bool PropertyCombo(const char* label, int* value, const char* const items[], int itemCount,
                   int defaultValue, const char* tooltip) {
    if (itemCount <= 0) {
        return false;
    }
    *value = std::clamp(*value, 0, itemCount - 1);

    PropertyLabel(label, tooltip);
    ImGui::SetNextItemWidth(ValueWidth(kSliderMinWidth, kComboMaxWidth));
    bool changed = ImGui::Combo("##value", value, items, itemCount);

    if (ResetDot(*value == defaultValue, items[std::clamp(defaultValue, 0, itemCount - 1)])) {
        *value = std::clamp(defaultValue, 0, itemCount - 1);
        changed = true;
    }
    PropertyEnd();
    return changed;
}

bool PropertyTextInput(const char* label, char* buffer, size_t bufferSize, const char* tooltip) {
    PropertyLabel(label, tooltip);
    ImGui::SetNextItemWidth(
        std::min(Scaled(kTextInputWidth), ImGui::GetContentRegionAvail().x));
    const bool changed = ImGui::InputText("##value", buffer, bufferSize);
    PropertyEnd();
    return changed;
}

void PropertyValue(const char* label, const char* format, ...) {
    PropertyLabel(label, nullptr);
    va_list args;
    va_start(args, format);
    ImGui::TextV(format, args);
    va_end(args);
    PropertyEnd();
}

bool Button(const char* label, float width) {
    return ImGui::Button(label, ImVec2(Scaled(width), 0.0f));
}

namespace {

// 名前を width に収まる 2 行へ割る。
//
// **行数は常に 2 で固定する。** 名前の長さで行数が変わると、
// 一覧の升目の高さが揃わずに段がガタつく。
// 収まらないぶんは中央を省略し、**先頭と末尾の両方を残す**。
// `T_Rocky_Soil_..._BaseColor` と `..._Normal` のように、
// 素材名は先頭、マップの種別は末尾に入るため、
// 片側だけ残すと同じ表示になって見分けられない。
std::array<std::string, 2> SplitCaptionLines(const char* text, float width) {
    std::array<std::string, 2> lines;

    // UTF-8 の途中で切らないよう、文字の先頭バイト位置を集めておく。
    std::vector<size_t> starts;
    for (size_t i = 0; text[i] != 0; ++i) {
        if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) {
            starts.push_back(i);
        }
    }
    starts.push_back(std::strlen(text));
    const size_t charCount = starts.size() - 1;

    // 1 行目: 先頭から入るだけ。
    size_t head = 0;
    while (head < charCount) {
        const std::string candidate(text, starts[head + 1]);
        if (ImGui::CalcTextSize(candidate.c_str()).x > width) {
            break;
        }
        ++head;
    }
    lines[0].assign(text, starts[head]);
    if (head == charCount) {
        return lines;  // 1 行で収まった。
    }

    // 2 行目: 残りが入るならそのまま。入らなければ末尾を残して中央を省略する。
    const std::string rest(text + starts[head]);
    if (ImGui::CalcTextSize(rest.c_str()).x <= width) {
        lines[1] = rest;
        return lines;
    }

    static const char* const kEllipsis = "…";
    size_t tail = 0;
    while (head + tail < charCount) {
        const std::string candidate =
            std::string(kEllipsis) + std::string(text + starts[charCount - tail - 1]);
        if (ImGui::CalcTextSize(candidate.c_str()).x > width) {
            break;
        }
        ++tail;
    }
    lines[1] = std::string(kEllipsis) + std::string(text + starts[charCount - tail]);
    return lines;
}

}  // namespace

bool HorizontalSplitter(const char* id, float* height, float minHeight, float maxHeight,
                        float width) {
    if (height == nullptr || width <= 0.0f) {
        return false;
    }

    // 掴む高さは線より広く取る。線と同じ太さだと狙って掴めない。
    // さらに上下へ余白を空ける。**詰めると隣の区画の枠が線に貼り付いて窮屈に見える。**
    const float grabHeight = Scaled(kSplitterGrabWidth);
    const float margin = Scaled(kSplitterMargin);
    // **余白は Dummy ではなくカーソルの直接指定で空ける。**
    // Dummy を挟むと ItemSpacing がもう 1 つ余分に入り、境界の位置が
    // 呼び出し側の想定（区画の下 + margin）からずれる。
    // 直前のアイテムの後でカーソルには既に ItemSpacing.y が乗っているので、その差だけ足す。
    const float spacing = ImGui::GetStyle().ItemSpacing.y;
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + margin - spacing);
    ImGui::InvisibleButton(id, ImVec2(width, grabHeight));

    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    // **離したかどうかはここで拾う。** 後ろの Dummy もアイテムなので、
    // 最後まで待つと IsItemDeactivated がそちらを指してしまう。
    const bool released = ImGui::IsItemDeactivated();
    if (hovered || active) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    }
    if (active) {
        *height = std::clamp(*height + ImGui::GetIO().MouseDelta.y, minHeight, maxHeight);
    }

    // 線は掴める高さの中央へ 1 本。掴めることが分かる程度に留める。
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    const float centerY = (min.y + max.y) * 0.5f;
    const ImU32 color = ImGui::GetColorU32(active      ? ImGuiCol_SeparatorActive
                                           : hovered   ? ImGuiCol_SeparatorHovered
                                                       : ImGuiCol_Separator);
    ImGui::GetWindowDrawList()->AddLine(ImVec2(min.x, centerY), ImVec2(max.x, centerY), color,
                                        Scaled(1.0f));

    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + margin - spacing);
    return released;
}

void GridCaption(const char* text, float width) {
    if (text == nullptr) {
        return;
    }
    // ImGui 1.92 は同じフォントを別サイズで積める。第 2 フォントは読み込まない。
    ImGui::PushFont(nullptr, kCaptionFontSize);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));

    // 2 行目が空でも行は描く。**空行を飛ばすと升目の高さが揃わない。**
    for (const std::string& line : SplitCaptionLines(text, width)) {
        // サムネイルの幅の中で中央へ寄せる。左詰めだと升目の並びが揺れて見える。
        const float textWidth = ImGui::CalcTextSize(line.c_str()).x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (width - textWidth) * 0.5f));
        ImGui::TextUnformatted(line.empty() ? " " : line.c_str());
    }

    ImGui::PopStyleColor();
    ImGui::PopFont();
}

void HintText(const char* format, ...) {
    va_list args;
    va_start(args, format);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextV(format, args);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    va_end(args);
}

}  // namespace tg::ui
