/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 * @modified    Android backend, 2026
 */

#include <rex/ui/window_android.h>

#include <android/native_window.h>

#include <rex/ui/surface_android.h>
#include <rex/ui/windowed_app_context_android.h>

namespace rex {
namespace ui {

std::unique_ptr<Window> Window::Create(WindowedAppContext& app_context,
                                       const std::string_view title,
                                       uint32_t desired_logical_width,
                                       uint32_t desired_logical_height) {
  return std::make_unique<AndroidWindow>(app_context, title, desired_logical_width,
                                         desired_logical_height);
}

AndroidWindow::AndroidWindow(WindowedAppContext& app_context, const std::string_view title,
                             uint32_t desired_logical_width, uint32_t desired_logical_height)
    : super(app_context, title, desired_logical_width, desired_logical_height) {
  // On Android the context is always an AndroidWindowedAppContext (created by
  // the android_main entry point). Register so the app-glue loop can deliver the
  // native window + input here.
  static_cast<AndroidWindowedAppContext&>(app_context).RegisterWindow(this);
}

AndroidWindow::~AndroidWindow() {
  static_cast<AndroidWindowedAppContext&>(app_context()).UnregisterWindow(this);
  EnterDestructor();
}

void AndroidWindow::SetNativeWindow(ANativeWindow* native_window) {
  if (native_window == native_window_) {
    return;
  }
  WindowDestructionReceiver receiver(this);
  if (!native_window) {
    // Detach: drop the presenter surface before the OS frees the ANativeWindow.
    OnSurfaceChanged(false);
    native_window_ = nullptr;
    return;
  }
  native_window_ = native_window;
  // Report the physical size first so the swapchain is created at the right
  // extent, then connect the surface (CreateSurfaceImpl uses native_window_),
  // then mark focused.
  uint32_t width = static_cast<uint32_t>(ANativeWindow_getWidth(native_window_));
  uint32_t height = static_cast<uint32_t>(ANativeWindow_getHeight(native_window_));
  OnActualSizeUpdate(width, height, receiver);
  if (receiver.IsWindowDestroyed()) {
    return;
  }
  OnSurfaceChanged(true);
  if (receiver.IsWindowDestroyed()) {
    return;
  }
  OnFocusUpdate(true, receiver);
}

void AndroidWindow::OnAndroidResized() {
  if (!native_window_) {
    return;
  }
  WindowDestructionReceiver receiver(this);
  uint32_t width = static_cast<uint32_t>(ANativeWindow_getWidth(native_window_));
  uint32_t height = static_cast<uint32_t>(ANativeWindow_getHeight(native_window_));
  OnActualSizeUpdate(width, height, receiver);
}

void AndroidWindow::OnAndroidFocusChanged(bool focused) {
  WindowDestructionReceiver receiver(this);
  OnFocusUpdate(focused, receiver);
}

bool AndroidWindow::OpenImpl() {
  // The native window is owned by the activity and arrives asynchronously via
  // APP_CMD_INIT_WINDOW (SetNativeWindow). Opening just marks us ready; the
  // surface is connected once the ANativeWindow is delivered.
  return true;
}

void AndroidWindow::RequestCloseImpl() {
  // Phase 2: route through the activity lifecycle (ANativeActivity_finish).
}

std::unique_ptr<Surface> AndroidWindow::CreateSurfaceImpl(Surface::TypeFlags allowed_types) {
  if (!native_window_ ||
      !(allowed_types & Surface::kTypeFlag_AndroidNativeWindow)) {
    return nullptr;
  }
  return std::make_unique<AndroidNativeWindowSurface>(native_window_);
}

void AndroidWindow::RequestPaintImpl() {
  // Called from any thread (the presenter requests a paint after the guest
  // produces a frame). Flag it; the UI loop drives the actual OnPaint/present
  // via PaintFromUiThreadIfRequested(). The loop's frame-paced wake picks it up.
  paint_pending_.store(true, std::memory_order_release);
}

// Android has no native menu bar; the in-app ImGui menus handle UI. Provide a
// trivial concrete MenuItem so the SDK links (mirrors GTKMenuItem / Win32MenuItem).
namespace {
class AndroidMenuItem final : public MenuItem {
 public:
  AndroidMenuItem(Type type, const std::string& text, const std::string& hotkey,
                  std::function<void()> callback)
      : MenuItem(type, text, hotkey, std::move(callback)) {}
};
}  // namespace

std::unique_ptr<MenuItem> MenuItem::Create(Type type, const std::string& text,
                                           const std::string& hotkey,
                                           std::function<void()> callback) {
  return std::make_unique<AndroidMenuItem>(type, text, hotkey, std::move(callback));
}

}  // namespace ui
}  // namespace rex
