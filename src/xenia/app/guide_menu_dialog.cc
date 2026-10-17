/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the project root for more  *
 * information.                                                               *
 ******************************************************************************
 */

#include "xenia/app/guide_menu_dialog.h"

#include "third_party/imgui/imgui.h"
#include "xenia/app/emulator_window.h"
#include "xenia/base/logging.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xam/xam.h"
#include "xenia/kernel/xam/xam_state.h"

namespace xe {
namespace app {

GuideMenuDialog::GuideMenuDialog(ui::ImGuiDrawer* imgui_drawer,
                                 EmulatorWindow& emulator_window,
                                 bool can_return_to_dashboard,
                                 std::function<void()> on_closed)
    : ui::ImGuiDialog(imgui_drawer),
      emulator_window_(emulator_window),
      can_return_to_dashboard_(can_return_to_dashboard),
      on_closed_(std::move(on_closed)) {
  // Same mechanism the profile dialogs use: the title sees the system UI as
  // active and gets no controller input.
  auto kernel_state = emulator_window_.emulator()->kernel_state();
  kernel_state->BroadcastNotification(kXNotificationSystemUI, true);
  kernel_state->xam_state()->is_xam_dialog_present_.store(true);
}

void GuideMenuDialog::OnClose() {
  auto kernel_state = emulator_window_.emulator()->kernel_state();
  kernel_state->BroadcastNotification(kXNotificationSystemUI, false);
  kernel_state->xam_state()->is_xam_dialog_present_.store(false);
  if (on_closed_) {
    on_closed_();
  }
}

void GuideMenuDialog::OnDraw(ImGuiIO& io) {
  constexpr const char* kTitle = "Xenia Guide";
  if (!has_opened_) {
    ImGui::OpenPopup(kTitle);
    has_opened_ = true;
  }

  ImGui::SetNextWindowPos(
      ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
      ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  if (!ImGui::BeginPopupModal(kTitle, nullptr,
                              ImGuiWindowFlags_AlwaysAutoResize |
                                  ImGuiWindowFlags_NoMove |
                                  ImGuiWindowFlags_NoSavedSettings)) {
    // Closed with B / Esc.
    Close();
    return;
  }

  const ImVec2 button_size(340.0f, 46.0f);
  enum class Choice {
    kNone,
    kResume,
    kDashboard,
    kQuit
  } choice = Choice::kNone;

  ImGui::TextDisabled("Use the D-pad and A, or the arrow keys and Enter.");
  ImGui::Dummy(ImVec2(0.0f, 6.0f));

  if (ImGui::Button("Resume game", button_size)) {
    choice = Choice::kResume;
  }
  ImGui::SetItemDefaultFocus();
  if (can_return_to_dashboard_) {
    if (ImGui::Button("Return to Dashboard", button_size)) {
      choice = Choice::kDashboard;
    }
  }
  if (ImGui::Button("Quit Xenia", button_size)) {
    choice = Choice::kQuit;
  }
  ImGui::Dummy(ImVec2(0.0f, 4.0f));
  ImGui::TextDisabled("B / Esc: back to the game");

  if (choice != Choice::kNone) {
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();

  switch (choice) {
    case Choice::kResume:
      Close();
      break;
    case Choice::kDashboard:
      Close();
      emulator_window_.ReturnToDashboard();
      break;
    case Choice::kQuit:
      Close();
      emulator_window_.RequestQuit();
      break;
    default:
      break;
  }
}

}  // namespace app
}  // namespace xe
