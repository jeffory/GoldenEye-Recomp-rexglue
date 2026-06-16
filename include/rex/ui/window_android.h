#pragma once
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

#include <atomic>
#include <memory>
#include <string>

#include <rex/platform.h>
#include <rex/ui/android_input_sink.h>
#include <rex/ui/menu_item.h>
#include <rex/ui/window.h>

struct ANativeWindow;

namespace rex {
namespace ui {

// Window backed by an Android ANativeWindow (one fullscreen surface per app).
// Mirrors GTKWindow / Win32Window.
//
// Phase 1 (current): compiles/links; CreateSurfaceImpl already produces a real
// AndroidNativeWindowSurface once the native window is attached.
// Phase 2 (TODO): SetNativeWindow() is called from the app-context loop on
// APP_CMD_INIT_WINDOW / TERM_WINDOW; OpenImpl/RequestPaintImpl/size+focus
// updates are wired to the activity lifecycle.
class AndroidWindow final : public Window {
  using super = Window;

 public:
  AndroidWindow(WindowedAppContext& app_context, const std::string_view title,
                uint32_t desired_logical_width, uint32_t desired_logical_height);
  ~AndroidWindow() override;

  void* GetNativeWindowHandle() const override {
    return reinterpret_cast<void*>(native_window_);
  }

  // Called by AndroidWindowedAppContext from the app-glue loop:
  // - SetNativeWindow attaches/detaches the surface as the activity's window is
  //   created (APP_CMD_INIT_WINDOW) / destroyed (APP_CMD_TERM_WINDOW). Passing
  //   nullptr tears the presentation surface down before the OS frees it.
  // - OnAndroidResized re-reads the native window size (APP_CMD_WINDOW_RESIZED).
  // - OnAndroidFocusChanged updates focus (APP_CMD_GAINED_/LOST_FOCUS).
  void SetNativeWindow(ANativeWindow* native_window);
  void OnAndroidResized();
  void OnAndroidFocusChanged(bool focused);

  // Registered by the Android input driver (rexinput) so the app-glue loop can
  // route gamepad/key/motion events to it. nullptr until a driver attaches.
  void SetAndroidInputSink(AndroidInputSink* sink) { input_sink_ = sink; }
  AndroidInputSink* android_input_sink() const { return input_sink_; }

  // Drive a paint/present from the Android UI loop. There is no OS repaint
  // callback (unlike GTK's draw signal), so the app-glue loop pumps this when a
  // paint has been requested; without it the presenter's PaintAndPresent is
  // never called and the guest output is never composited -> black screen.
  // Painting only on request (not every tick) avoids contending with the GPU
  // command processor when there is no new guest output to show. Public wrapper
  // for the protected Window::OnPaint. Call only on the UI (loop) thread.
  //
  // force_paint=true: once our own paint_pending_ gate has decided to paint
  // (the presenter requested it because the guest produced a new frame), the
  // present must actually happen. The Android swapchain does not retain the
  // previous frame's image across presents, so we must not let the present be
  // skipped by Presenter::PaintFromUIThread's separate ui_thread_paint_requested_
  // bookkeeping (which is a distinct atomic that may already have been consumed)
  // - that would leave a stale/black frame and trigger reconnect churn. The
  // paint_pending_ gate is the authoritative "a frame is waiting" signal here.
  void PaintFromUiThreadIfRequested() {
    if (paint_pending_.exchange(false, std::memory_order_acquire)) {
      OnPaint(true);
    }
  }

 protected:
  bool OpenImpl() override;
  void RequestCloseImpl() override;
  std::unique_ptr<Surface> CreateSurfaceImpl(Surface::TypeFlags allowed_types) override;
  void RequestPaintImpl() override;

 private:
  ANativeWindow* native_window_ = nullptr;
  AndroidInputSink* input_sink_ = nullptr;
  // Set by RequestPaintImpl (any thread), consumed by the UI loop. The presenter
  // requests a paint after the guest produces a new frame. Default false so we
  // do NOT paint before the first guest output exists - a premature paint
  // acquires the swapchain image and vsync-waits while holding the queue lock,
  // blocking the command processor's first swap (stall at present#1).
  std::atomic<bool> paint_pending_{false};
};

}  // namespace ui
}  // namespace rex
