#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 * @modified    Android backend, 2026
 */

#include <cstdint>

#include <rex/ui/windowed_app_context.h>

struct android_app;
struct AInputEvent;

namespace rex {
namespace ui {

class AndroidWindow;

// Android UI-loop context, driven by android_native_app_glue. Mirrors
// GTKWindowedAppContext / Win32 equivalent.
//
// The activity lifecycle is inverted relative to desktop: the OS hands us the
// ANativeWindow via APP_CMD_INIT_WINDOW rather than us creating it. The single
// active AndroidWindow (created by ReXApp::OnInitialize via Window::Create)
// registers itself here on construction; this context forwards the native
// window + lifecycle to it.
class AndroidWindowedAppContext final : public WindowedAppContext {
 public:
  explicit AndroidWindowedAppContext(android_app* app);
  ~AndroidWindowedAppContext() override;

  android_app* app() const { return app_; }

  // Called by AndroidWindow's ctor/dtor so the loop can route the native window
  // and input to it. Only one window is active at a time on Android.
  void RegisterWindow(AndroidWindow* window) { window_ = window; }
  void UnregisterWindow(AndroidWindow* window) {
    if (window_ == window) {
      window_ = nullptr;
    }
  }

  // Pumps the android_native_app_glue loop until quit / destroy. Call from the
  // UI thread (android_main) after the app's OnInitialize().
  void RunMainAndroidLoop();

 protected:
  void NotifyUILoopOfPendingFunctions() override;
  void PlatformQuitFromUIThread() override;

 private:
  // android_native_app_glue callback thunks (app_->userData == this).
  static void OnAppCmdThunk(android_app* app, int32_t cmd);
  static int32_t OnInputEventThunk(android_app* app, AInputEvent* event);

  void HandleAppCmd(int32_t cmd);
  int32_t HandleInputEvent(AInputEvent* event);

  android_app* app_ = nullptr;
  AndroidWindow* window_ = nullptr;
  bool quit_requested_ = false;
};

}  // namespace ui
}  // namespace rex
