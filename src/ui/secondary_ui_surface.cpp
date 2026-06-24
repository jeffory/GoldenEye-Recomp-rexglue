/**
 ******************************************************************************
 * ReXGlue runtime                                                            *
 ******************************************************************************
 *
 * @author      Dual-screen framework support, 2026
 */

#include <rex/ui/secondary_ui_surface.h>

#include <rex/logging.h>
#include <rex/ui/graphics_provider.h>
#include <rex/ui/immediate_drawer.h>
#include <rex/ui/presenter.h>

namespace rex {
namespace ui {

std::unique_ptr<SecondaryUiSurface> SecondaryUiSurface::Create(
    WindowedAppContext& app_context, GraphicsProvider& graphics_provider,
    const ExternalWindowHandle& handle, uint32_t physical_width, uint32_t physical_height,
    size_t imgui_z_order, ImGuiDrawer::FontSetupCallback font_setup,
    const std::string_view title) {
  auto surface = std::unique_ptr<SecondaryUiSurface>(new SecondaryUiSurface());

  surface->window_ =
      ExternalWindow::Create(app_context, handle, physical_width, physical_height, title);
  if (!surface->window_) {
    REXLOG_ERROR("SecondaryUiSurface: failed to open the external window");
    return nullptr;
  }

  // A dedicated presenter + immediate drawer on the same graphics device. This
  // is fully independent of the primary surface's presenter.
  surface->presenter_ = graphics_provider.CreatePresenter();
  if (!surface->presenter_) {
    REXLOG_ERROR("SecondaryUiSurface: failed to create the presenter");
    return nullptr;
  }
  surface->immediate_drawer_ = graphics_provider.CreateImmediateDrawer();
  if (!surface->immediate_drawer_) {
    REXLOG_ERROR("SecondaryUiSurface: failed to create the immediate drawer");
    return nullptr;
  }

  // Bind the presenter to the window's surface (creates the VkSurfaceKHR +
  // swapchain). The surface only exists for an open window.
  surface->window_->SetPresenter(surface->presenter_.get());
  if (!surface->window_->HasSurface()) {
    REXLOG_ERROR("SecondaryUiSurface: presenter could not connect to the external surface");
    return nullptr;
  }

  // An ImGui drawer with its own ImGuiContext, attached to this presenter only.
  surface->imgui_drawer_ = std::make_unique<ImGuiDrawer>(surface->window_.get(), imgui_z_order,
                                                         std::move(font_setup));
  surface->imgui_drawer_->SetPresenterAndImmediateDrawer(surface->presenter_.get(),
                                                         surface->immediate_drawer_.get());

  return surface;
}

SecondaryUiSurface::~SecondaryUiSurface() {
  // Tear down in reverse dependency order. The ImGui drawer references the
  // window (input listener) and presenter (UI drawer registration), so it goes
  // first while both are still alive.
  imgui_drawer_.reset();
  // Detach the presenter from the surface (destroys the swapchain + VkSurfaceKHR)
  // before the presenter and the external window are destroyed.
  if (window_) {
    window_->SetPresenter(nullptr);
  }
  presenter_.reset();
  immediate_drawer_.reset();
  window_.reset();
}

void SecondaryUiSurface::Paint(bool force_paint) { window_->Paint(force_paint); }

void SecondaryUiSurface::NotifyResize(uint32_t physical_width, uint32_t physical_height) {
  window_->NotifyResize(physical_width, physical_height);
}

}  // namespace ui
}  // namespace rex
