#pragma once
/**
 ******************************************************************************
 * ReXGlue runtime                                                            *
 ******************************************************************************
 *
 * @author      Dual-screen framework support, 2026
 *
 * A rex::ui::Window that ADOPTS an externally-supplied native window handle
 * instead of creating its own native window and pumping a platform event loop.
 *
 * This is the enabling foundation for a secondary render surface (e.g. the AYN
 * Thor's 3.92" touch panel, whose ANativeWindow* comes from an Android
 * Presentation, not from a NativeActivity). Because it derives from the common
 * Window base, the existing Presenter / Surface / ImGuiDrawer machinery works
 * unchanged - no regression to the primary single-window -> presenter -> surface
 * path.
 *
 * The host does not own a native event loop for this window, so painting is
 * driven explicitly from the UI thread via Paint(); RequestPaintImpl() merely
 * raises a pending flag the host can poll.
 */

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include <rex/ui/surface.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

namespace rex {
namespace ui {

// Platform-agnostic descriptor of an already-existing native window handle.
// No platform headers are pulled into this header on purpose; the .cpp
// reinterprets the opaque fields into the concrete Surface subclass under the
// appropriate REX_PLATFORM_* guard. Today only the surface type matching the
// build platform is creatable, but the descriptor is shaped so Win32/Xcb can be
// produced on desktop for testing the abstraction without AYN hardware.
struct ExternalWindowHandle {
  // Which Surface subtype to build from this handle.
  Surface::TypeIndex type = Surface::kTypeIndex_AndroidNativeWindow;

  // ANativeWindow* (Android) or HWND (Win32).
  void* native_window = nullptr;
  // xcb_connection_t* (Xcb) or HINSTANCE (Win32). Unused on Android.
  void* native_instance = nullptr;
  // xcb_window_t / XID (Xcb only). Unused otherwise.
  uint32_t native_id = 0;

  // ANativeWindow* from an Android Presentation / SurfaceView.
  static ExternalWindowHandle FromAndroidNativeWindow(void* a_native_window) {
    ExternalWindowHandle h;
    h.type = Surface::kTypeIndex_AndroidNativeWindow;
    h.native_window = a_native_window;
    return h;
  }

  // An xcb connection + window id (used for the desktop GNU/Linux test harness).
  static ExternalWindowHandle FromXcbWindow(void* xcb_connection, uint32_t xcb_window) {
    ExternalWindowHandle h;
    h.type = Surface::kTypeIndex_XcbWindow;
    h.native_instance = xcb_connection;
    h.native_id = xcb_window;
    return h;
  }

  // A Win32 HINSTANCE + HWND (used for the desktop Windows test harness).
  static ExternalWindowHandle FromWin32Hwnd(void* hinstance, void* hwnd) {
    ExternalWindowHandle h;
    h.type = Surface::kTypeIndex_Win32Hwnd;
    h.native_instance = hinstance;
    h.native_window = hwnd;
    return h;
  }
};

class ExternalWindow final : public Window {
 public:
  // physical_width / physical_height are the surface dimensions in physical
  // pixels (e.g. 1080x1240 for the AYN Thor secondary panel). They are reported
  // to the common Window as the actual size so the Presenter sizes its swapchain
  // correctly even though no platform resize events will arrive.
  static std::unique_ptr<ExternalWindow> Create(WindowedAppContext& app_context,
                                                 const ExternalWindowHandle& handle,
                                                 uint32_t physical_width,
                                                 uint32_t physical_height,
                                                 const std::string_view title = "Secondary");

  ~ExternalWindow() override;

  const ExternalWindowHandle& handle() const { return handle_; }
  void* GetNativeWindowHandle() const override { return handle_.native_window; }

  // Expose the base's surface-existence check publicly (it's protected on
  // Window) so the owning SecondaryUiSurface can confirm the presenter connected.
  using Window::HasSurface;

  // Update the cached physical size after the external surface is resized.
  // Must be called from the UI thread. Drives Presenter::OnSurfaceResizeFromUIThread.
  void NotifyResize(uint32_t physical_width, uint32_t physical_height);

  // Whether something (a UI drawer animating, a resize, the presenter's
  // connection recovery) has asked for a repaint since the last Paint(). The
  // host UI loop may poll this, but driving Paint() every frame is also fine.
  bool IsPaintPending() const { return paint_pending_.load(std::memory_order_acquire); }

  // Paint and present once, from the UI thread. Clears the pending flag.
  void Paint(bool force_paint = false);

  // Inject input from the platform layer. The AYN Thor secondary screen is
  // touch; on desktop these let the harness forward mouse input. Each forwards
  // to the window's input listeners (e.g. the attached ImGuiDrawer). The event's
  // target should be this window.
  void InjectKeyDown(KeyEvent& e);
  void InjectKeyUp(KeyEvent& e);
  void InjectKeyChar(KeyEvent& e);
  void InjectMouseDown(MouseEvent& e);
  void InjectMouseMove(MouseEvent& e);
  void InjectMouseUp(MouseEvent& e);
  void InjectMouseWheel(MouseEvent& e);
  void InjectTouchEvent(TouchEvent& e);

 protected:
  bool OpenImpl() override;
  void RequestCloseImpl() override;
  std::unique_ptr<Surface> CreateSurfaceImpl(Surface::TypeFlags allowed_types) override;
  void RequestPaintImpl() override;

 private:
  ExternalWindow(WindowedAppContext& app_context, const ExternalWindowHandle& handle,
                 uint32_t physical_width, uint32_t physical_height,
                 const std::string_view title);

  ExternalWindowHandle handle_;
  uint32_t physical_width_;
  uint32_t physical_height_;
  std::atomic<bool> paint_pending_{false};
};

}  // namespace ui
}  // namespace rex
