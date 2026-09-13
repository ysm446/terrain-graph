// モデルアセットの一覧エリア。読み込み・編集・配置は後続の実装で追加する。
#include "app/Application.h"
#include "ui/UiStyle.h"

#include <imgui.h>

namespace tg {

void Application::DrawModelLibraryPanel() {
    if (!ImGui::Begin("モデル")) {
        ImGui::End();
        return;
    }
    if (ImGui::BeginChild("modelGrid", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders)) {
        ui::HintText("モデルアセットはまだありません。");
        ui::HintText("岩や低木などの3Dモデルを管理するエリアです。読み込み機能は準備中です。");
    }
    ImGui::EndChild();
    ImGui::End();
}

} // namespace tg
