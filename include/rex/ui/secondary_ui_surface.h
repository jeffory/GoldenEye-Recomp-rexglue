#pragma once
/**
 ******************************************************************************
 * ReXGlue runtime                                                            *
 ******************************************************************************
 *
 * @author      Dual-screen framework support, 2026
 *
 * SecondaryUiSurface bundles everything needed to present an ImGui-only UI to a
 * SECOND display, independent of the primary game surface:
 *
 *   ExternalWindow (adopts a native handle)
 *      -> Presenter + Surface + swapchain   (from the GraphicsProvider)
 *      -> ImmediateDrawer                    (from the GraphicsProvider)
 *      -> ImGuiDrawer + its own ImGuiContext
 *
 * It is the enabling foundation for the on-screen weapon menu on the AYN Thor's
 * secondary touch panel. The primary game surface is untouched: this creates a
 * fully independent Presenter on the same graphics device (multiple
 * VkSurfaceKHR/swapchains on one Vulkan device are supported), with its own
 * paint-mode mutex, mailbox and ImGui context. There is no guest framebuffer on
 * this surface - it only composites ImGui draw lists.
 *
 * Threading: like all Presenter/Surface/Window methods, every method here must
 * be called from the UI thread. Drive Paint() from the same UI thread that
 * drives the primary window's paint, so the two presenters never contend.
 */

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

#include <rex/ui/external_window.h>
#include <rex/ui/imgui_drawer.h>

namespace rex {
namespace ui {

class GraphicsProvider;
class ImmediateDrawer;
class Presenter;

class SecondaryUiSurface {
 public:
  // Creates the secondary presenter chain from an already-existing native
  // window handle. Returns nullptr if the presenter, immediate drawer or
  // surface could not be created (e.g. the handle type isn't supported by the
  // presenter on this platform). Must be called from the UI thread.
  //
  //  - graphics_provider: the SAME provider used for the primary surface, so
  //    resources are shared on one device.
  //  - handle / physical_width / physical_height: the external surface.
  //  - imgui_z_order: z-order for the ImGui drawer (only one drawer here, so the
  //    value is mostly cosmetic; kept for parity with the primary path).
  //  - font_setup: optional ImFontAtlas setup callback (e.g. to load a font).
  static std::unique_ptr<SecondaryUiSurface> Create(
      WindowedAppContext& app_context, GraphicsProvider& graphics_provider,
      const ExternalWindowHandle& handle, uint32_t physical_width, uint32_t physical_height,
      size_t imgui_z_order = 0, ImGuiDrawer::FontSetupCallback font_setup = nullptr,
      const std::string_view title = "Secondary");

  SecondaryUiSurface(const SecondaryUiSurface&) = delete;
  SecondaryUiSurface& operator=(const SecondaryUiSurface&) = delete;
  ~SecondaryUiSurface();

  // Attach ImGuiDialogs to this drawer (an ImGuiDialog registers itself with the
  // drawer on construction). The drawer owns its own ImGuiContext, isolated from
  // the primary drawer's context.
  ImGuiDrawer* imgui_drawer() const { return imgui_drawer_.get(); }
  Presenter* presenter() const { return presenter_.get(); }
  ExternalWindow* window() const { return window_.get(); }

  // Paint and present the secondary surface once. Call every UI-thread frame
  // (or whenever window()->IsPaintPending()).
  void Paint(bool force_paint = false);

  // Forward a resize of the external surface (physical pixels).
  void NotifyResize(uint32_t physical_width, uint32_t physical_height);

 private:
  SecondaryUiSurface() = default;

  std::unique_ptr<ExternalWindow> window_;
  std::unique_ptr<Presenter> presenter_;
  std::unique_ptr<ImmediateDrawer> immediate_drawer_;
  std::unique_ptr<ImGuiDrawer> imgui_drawer_;
};

}  // namespace ui
}  // namespace rex
