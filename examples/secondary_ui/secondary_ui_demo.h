#pragma once
/**
 ******************************************************************************
 * ReXGlue runtime - examples                                                 *
 ******************************************************************************
 *
 * @author      Dual-screen framework support, 2026
 *
 * Desktop test harness that proves rex::ui::SecondaryUiSurface end to end: it
 * opens a SECOND real OS window and presents a trivial ImGui window to it via
 * an independent Presenter/Surface/swapchain, without any AYN hardware.
 *
 * This file is NOT part of the default build; enable -DREXGLUE_BUILD_SECONDARY_
 * UI_DEMO=ON. See examples/secondary_ui/README.md.
 */

#include <rex/ui/imgui_dialog.h>

namespace rex {
namespace ui {

class GraphicsProvider;
class WindowedAppContext;

// A trivial ImGui dialog (a single window with some text) used to prove the
// secondary surface lights up. Reusable by the menu/integration tasks as a
// "hello world" before wiring the real weapon menu.
class SecondaryDemoDialog final : public ImGuiDialog {
 public:
  explicit SecondaryDemoDialog(ImGuiDrawer* imgui_drawer) : ImGuiDialog(imgui_drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override;
};

#if defined(REX_PLATFORM_GNU_LINUX) && REX_PLATFORM_GNU_LINUX
// Opens a second X11/xcb window (default 1080x1240, matching the AYN Thor
// secondary panel), attaches a SecondaryUiSurface presenting SecondaryDemoDialog
// to it, and runs a small paint loop until the window is closed. `provider` must
// be the same already-initialized provider the app uses for the primary surface.
// Call from the UI thread. Returns false if the surface could not be created.
bool RunSecondaryUiDemoXcb(WindowedAppContext& app_context, GraphicsProvider& provider,
                           unsigned int width = 1080, unsigned int height = 1240);
#endif

}  // namespace ui
}  // namespace rex
