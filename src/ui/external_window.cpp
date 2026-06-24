/**
 ******************************************************************************
 * ReXGlue runtime                                                            *
 ******************************************************************************
 *
 * @author      Dual-screen framework support, 2026
 */

#include <rex/ui/external_window.h>

#include <rex/assert.h>
#include <rex/logging.h>
#include <rex/platform.h>

// Pull in only the Surface subtype that this build platform can construct. The
// descriptor's `type` is still validated at runtime so a mismatched handle is
// rejected cleanly rather than silently producing the wrong surface.
#if REX_PLATFORM_ANDROID
#include <rex/ui/surface_android.h>
#elif REX_PLATFORM_WIN32
#include <rex/ui/surface_win.h>
#elif REX_PLATFORM_GNU_LINUX
#include <rex/ui/surface_gnulinux.h>
#endif

namespace rex {
namespace ui {

std::unique_ptr<ExternalWindow> ExternalWindow::Create(WindowedAppContext& app_context,
                                                       const ExternalWindowHandle& handle,
                                                       uint32_t physical_width,
                                                       uint32_t physical_height,
                                                       const std::string_view title) {
  auto window = std::unique_ptr<ExternalWindow>(
      new ExternalWindow(app_context, handle, physical_width, physical_height, title));
  if (!window->Open()) {
    return nullptr;
  }
  return window;
}

ExternalWindow::ExternalWindow(WindowedAppContext& app_context, const ExternalWindowHandle& handle,
                               uint32_t physical_width, uint32_t physical_height,
                               const std::string_view title)
    // DPI defaults to medium (96) for this window, so logical == physical; pass
    // the physical size as the desired logical size for a 1:1 mapping.
    : Window(app_context, title, physical_width, physical_height),
      handle_(handle),
      physical_width_(physical_width),
      physical_height_(physical_height) {}

ExternalWindow::~ExternalWindow() {
  // Detach from the surface (and thus the presenter) before the externally-owned
  // native handle can disappear. We do not own the native window, so there is
  // nothing else to destroy here.
  EnterDestructor();
}

bool ExternalWindow::OpenImpl() {
  // There is no native window to create - it was supplied externally. Just
  // report the initial actual state so the common Window (and, once attached,
  // the Presenter) sizes correctly. Events to listeners are deferred until the
  // window reaches kOpen, so this only stores the size/focus here.
  WindowDestructionReceiver destruction_receiver(this);
  OnActualSizeUpdate(physical_width_, physical_height_, destruction_receiver);
  if (destruction_receiver.IsWindowDestroyedOrClosed()) {
    return true;
  }
  // The secondary surface is always considered focused for input routing - it
  // has no OS focus concept of its own (e.g. an Android Presentation).
  OnFocusUpdate(true, destruction_receiver);
  return true;
}

void ExternalWindow::RequestCloseImpl() {
  // No native event loop will deliver a close; drive the close transition
  // directly. This disconnects the surface from the presenter as part of
  // OnBeforeClose.
  WindowDestructionReceiver destruction_receiver(this);
  OnBeforeClose(destruction_receiver);
  if (destruction_receiver.IsWindowDestroyed()) {
    return;
  }
  OnAfterClose();
}

std::unique_ptr<Surface> ExternalWindow::CreateSurfaceImpl(Surface::TypeFlags allowed_types) {
#if REX_PLATFORM_ANDROID
  if ((allowed_types & Surface::kTypeFlag_AndroidNativeWindow) &&
      handle_.type == Surface::kTypeIndex_AndroidNativeWindow && handle_.native_window) {
    return std::make_unique<AndroidNativeWindowSurface>(
        static_cast<ANativeWindow*>(handle_.native_window));
  }
#elif REX_PLATFORM_WIN32
  if ((allowed_types & Surface::kTypeFlag_Win32Hwnd) &&
      handle_.type == Surface::kTypeIndex_Win32Hwnd && handle_.native_window) {
    return std::make_unique<Win32HwndSurface>(static_cast<HINSTANCE>(handle_.native_instance),
                                              static_cast<HWND>(handle_.native_window));
  }
#elif REX_PLATFORM_GNU_LINUX
  if ((allowed_types & Surface::kTypeFlag_XcbWindow) &&
      handle_.type == Surface::kTypeIndex_XcbWindow && handle_.native_instance) {
    return std::make_unique<XcbWindowSurface>(
        static_cast<xcb_connection_t*>(handle_.native_instance),
        static_cast<xcb_window_t>(handle_.native_id));
  }
#endif
  REXLOG_ERROR(
      "ExternalWindow: cannot create a surface - the supplied native handle "
      "type ({}) is not supported by the presenter on this platform",
      int(handle_.type));
  return nullptr;
}

void ExternalWindow::RequestPaintImpl() {
  // No event loop to schedule on; record the request for the host to poll.
  paint_pending_.store(true, std::memory_order_release);
}

void ExternalWindow::NotifyResize(uint32_t physical_width, uint32_t physical_height) {
  assert_true(app_context().IsInUIThread());
  physical_width_ = physical_width;
  physical_height_ = physical_height;
  WindowDestructionReceiver destruction_receiver(this);
  OnActualSizeUpdate(physical_width, physical_height, destruction_receiver);
}

void ExternalWindow::Paint(bool force_paint) {
  assert_true(app_context().IsInUIThread());
  paint_pending_.store(false, std::memory_order_release);
  OnPaint(force_paint);
}

void ExternalWindow::InjectKeyDown(KeyEvent& e) {
  WindowDestructionReceiver destruction_receiver(this);
  OnKeyDown(e, destruction_receiver);
}

void ExternalWindow::InjectKeyUp(KeyEvent& e) {
  WindowDestructionReceiver destruction_receiver(this);
  OnKeyUp(e, destruction_receiver);
}

void ExternalWindow::InjectKeyChar(KeyEvent& e) {
  WindowDestructionReceiver destruction_receiver(this);
  OnKeyChar(e, destruction_receiver);
}

void ExternalWindow::InjectMouseDown(MouseEvent& e) {
  WindowDestructionReceiver destruction_receiver(this);
  OnMouseDown(e, destruction_receiver);
}

void ExternalWindow::InjectMouseMove(MouseEvent& e) {
  WindowDestructionReceiver destruction_receiver(this);
  OnMouseMove(e, destruction_receiver);
}

void ExternalWindow::InjectMouseUp(MouseEvent& e) {
  WindowDestructionReceiver destruction_receiver(this);
  OnMouseUp(e, destruction_receiver);
}

void ExternalWindow::InjectMouseWheel(MouseEvent& e) {
  WindowDestructionReceiver destruction_receiver(this);
  OnMouseWheel(e, destruction_receiver);
}

void ExternalWindow::InjectTouchEvent(TouchEvent& e) {
  WindowDestructionReceiver destruction_receiver(this);
  OnTouchEvent(e, destruction_receiver);
}

}  // namespace ui
}  // namespace rex
