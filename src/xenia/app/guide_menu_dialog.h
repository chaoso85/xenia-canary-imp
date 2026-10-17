/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the project root for more  *
 * information.                                                               *
 ******************************************************************************
 */

#ifndef XENIA_APP_GUIDE_MENU_DIALOG_H_
#define XENIA_APP_GUIDE_MENU_DIALOG_H_

#include <functional>

#include "xenia/ui/imgui_dialog.h"
#include "xenia/ui/imgui_drawer.h"

namespace xe {
namespace app {

class EmulatorWindow;

// In-game guide menu, opened with the Guide button (or Home on the keyboard)
// while a title is running. Works with controller (D-pad + A, B to go back)
// and keyboard (arrows + Enter, Esc). While it is open the title receives no
// input.
class GuideMenuDialog final : public ui::ImGuiDialog {
 public:
  GuideMenuDialog(ui::ImGuiDrawer* imgui_drawer,
                  EmulatorWindow& emulator_window, bool can_return_to_dashboard,
                  std::function<void()> on_closed);

 protected:
  void OnDraw(ImGuiIO& io) override;
  void OnClose() override;

 private:
  EmulatorWindow& emulator_window_;
  bool can_return_to_dashboard_;
  std::function<void()> on_closed_;
  bool has_opened_ = false;
};

}  // namespace app
}  // namespace xe

#endif  // XENIA_APP_GUIDE_MENU_DIALOG_H_
