#include "ui/Toast.h"

#include "core/Shell.h"
#include "ui/UiStyle.h"

#include <imgui.h>

#include <algorithm>
#include <optional>

namespace tg::ui {
namespace {

// 同時に出す上限。積み上がりすぎると画面を覆ってしまう。
constexpr size_t kMaxToasts = 4;
// 消える直前にこの秒数だけかけて薄くする。
constexpr float kFadeSeconds = 0.45f;
// ホバー中は寿命をこの値まで延ばし続ける。読んでいる途中で消えないようにする。
constexpr float kHoverHoldSeconds = 1.5f;

constexpr float kMargin = 18.0f;
constexpr float kSpacing = 8.0f;

}  // namespace

void ToastQueue::Push(std::string title, std::string detail,
                      std::filesystem::path revealPath, float seconds) {
    Toast toast;
    toast.title = std::move(title);
    toast.detail = std::move(detail);
    toast.revealPath = std::move(revealPath);
    toast.expiresAt = std::chrono::steady_clock::now() +
                      std::chrono::milliseconds(static_cast<int>(seconds * 1000.0f));
    m_toasts.push_back(std::move(toast));

    // 古いものから捨てる。新しい通知のほうが今の操作に対応している。
    if (m_toasts.size() > kMaxToasts) {
        m_toasts.erase(m_toasts.begin(),
                       m_toasts.begin() +
                           static_cast<std::ptrdiff_t>(m_toasts.size() - kMaxToasts));
    }
}

void ToastQueue::Draw() {
    if (m_toasts.empty()) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();

    // クリックされた場所は、走査を終えてから開く。
    // 途中で開くと、そのフレームの残りの当たり判定が入力を取り合う。
    std::optional<std::filesystem::path> pendingReveal;

    float offsetY = 0.0f;
    for (size_t i = 0; i < m_toasts.size(); ++i) {
        Toast& toast = m_toasts[i];
        const float remaining = std::chrono::duration<float>(toast.expiresAt - now).count();
        const float fade = std::clamp(remaining / kFadeSeconds, 0.0f, 1.0f);

        // 右下を原点にして、下から上へ積む。
        ImGui::SetNextWindowPos(
            ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - Scaled(kMargin),
                   viewport->WorkPos.y + viewport->WorkSize.y - Scaled(kMargin) - offsetY),
            ImGuiCond_Always, ImVec2(1.0f, 1.0f));
        ImGui::SetNextWindowBgAlpha(fade);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, fade);

        constexpr ImGuiWindowFlags kFlags =
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking;

        // ID は積み順で作る。中身が変わっても位置が入れ替わらないようにするため。
        char windowId[32] = {};
        std::snprintf(windowId, sizeof(windowId), "##toast%zu", i);
        if (ImGui::Begin(windowId, nullptr, kFlags)) {
            ImGui::TextUnformatted(toast.title.c_str());
            if (!toast.detail.empty()) {
                ImGui::TextDisabled("%s", toast.detail.c_str());
            }
            if (!toast.revealPath.empty()) {
                ImGui::TextDisabled("クリックで保存先を開く");
            }

            if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
                if (!toast.revealPath.empty()) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                }
                // 読んでいる間は消さない。
                toast.expiresAt =
                    std::max(toast.expiresAt,
                             now + std::chrono::milliseconds(
                                       static_cast<int>(kHoverHoldSeconds * 1000.0f)));
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    if (!toast.revealPath.empty()) {
                        pendingReveal = toast.revealPath;
                    }
                    toast.expiresAt = now;
                } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                    // 開かずに閉じる。
                    toast.expiresAt = now;
                }
            }
            offsetY += ImGui::GetWindowSize().y + Scaled(kSpacing);
        }
        ImGui::End();
        ImGui::PopStyleVar();
    }

    m_toasts.erase(std::remove_if(m_toasts.begin(), m_toasts.end(),
                                  [&now](const Toast& toast) {
                                      return toast.expiresAt <= now;
                                  }),
                   m_toasts.end());

    if (pendingReveal) {
        RevealFileInExplorer(*pendingReveal);
    }
}

}  // namespace tg::ui
