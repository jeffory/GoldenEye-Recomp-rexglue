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

#include <rex/ui/windowed_app_context_android.h>

#include <android/looper.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>

#include <rex/logging.h>
#include <rex/ui/window_android.h>

#include <cstdio>  // [TEMP DEBUG] input-ANR diagnosis

namespace rex {
namespace ui {

namespace {
// Directly drain and finish any pending events on the app's input queue,
// mirroring android_native_app_glue's process_input. We call this every loop
// iteration rather than relying solely on the looper waking for
// LOOPER_ID_INPUT: on some devices/launchers (notably an external frontend that
// contends for window focus) the input-queue fd does not reliably wake the
// looper, which would leave key events undrained and trip the 5 s input-
// dispatch ANR. Runs on the UI loop thread only, so it never races the glue's
// own process_input; each event is finished exactly once.
void DrainAndroidInputQueue(android_app* app) {
  if (!app || !app->inputQueue) {
    return;
  }
  AInputEvent* event = nullptr;
  while (AInputQueue_getEvent(app->inputQueue, &event) >= 0) {
    if (AInputQueue_preDispatchEvent(app->inputQueue, event)) {
      continue;
    }
    int32_t handled = 0;
    if (app->onInputEvent) {
      handled = app->onInputEvent(app, event);
    }
    AInputQueue_finishEvent(app->inputQueue, event, handled);
  }
}
}  // namespace

// GE diagnostics: UI-loop iteration rate, to separate "loop runs slower than
// paint requests" from "paints are requested slower than frames are produced".
static std::atomic<uint64_t> ge_ui_loop_iteration_count{0};
extern "C" uint64_t rex_ge_ui_loop_iteration_count() {
  return ge_ui_loop_iteration_count.load(std::memory_order_relaxed);
}

// Defined in presenter.cpp: a delivered guest frame sits unconsumed in the
// mailbox (see RunMainAndroidLoop's poll-timeout selection).
extern "C" bool rex_ge_guest_frame_waiting();

AndroidWindowedAppContext::AndroidWindowedAppContext(android_app* app) : app_(app) {
  app_->userData = this;
  app_->onAppCmd = &AndroidWindowedAppContext::OnAppCmdThunk;
  app_->onInputEvent = &AndroidWindowedAppContext::OnInputEventThunk;
}

AndroidWindowedAppContext::~AndroidWindowedAppContext() {
  if (app_) {
    app_->onAppCmd = nullptr;
    app_->onInputEvent = nullptr;
    if (app_->userData == this) {
      app_->userData = nullptr;
    }
  }
}

void AndroidWindowedAppContext::WakeUILoop() {
  if (app_ && app_->looper) {
    ALooper_wake(app_->looper);
  }
}

void AndroidWindowedAppContext::NotifyUILoopOfPendingFunctions() {
  // Wake the looper so the next RunMainAndroidLoop iteration drains the queue.
  WakeUILoop();
}

void AndroidWindowedAppContext::PlatformQuitFromUIThread() {
  quit_requested_ = true;
  if (app_ && app_->activity) {
    ANativeActivity_finish(app_->activity);
  }
}

void AndroidWindowedAppContext::RunMainAndroidLoop() {
  // For safety, in case the quit request somehow happened before the loop.
  if (HasQuitFromUIThread()) {
    return;
  }
  while (!HasQuitFromUIThread()) {
    ge_ui_loop_iteration_count.fetch_add(1, std::memory_order_relaxed);
    int events;
    android_poll_source* source = nullptr;
    // Wake at least once per frame (~16 ms). The guest renders/presents on its
    // own GPU/CP threads, so this loop only pumps Android lifecycle + input and
    // must not busy-spin (timeout 0 burns a core). We also do NOT block forever:
    // the input-queue fd does not reliably wake this looper on every device, so
    // a frame-paced timeout plus the explicit DrainAndroidInputQueue() below
    // guarantees key events are dispatched and finished promptly (no ANR).
    // Frame-paced 16ms sleep, EXCEPT when a new guest frame is waiting to be
    // shown -- that must be serviced immediately: the ALooper_wake from
    // RequestPaintImpl can be eaten by the zero-timeout drain below (pollOnce
    // consumes the wake fd and returns ALOOPER_POLL_WAKE), and sleeping 16ms
    // on top of a pending frame serialized the loop to (16ms + paint) per
    // frame -- measured 49 shown/s vs 60 produced on the Ayn Thor (GESHOWN2
    // req/s=loop/s=49). Gating on the guest frame (not just HasPendingPaint)
    // keeps UI-only repaint requests paced at the poll timeout; skipping the
    // sleep for ANY pending paint span the loop at ~220 paints/s because
    // ImGui overlays re-arm a paint request after every paint.
    int poll_timeout_ms =
        (window_ && window_->HasPendingPaint() && rex_ge_guest_frame_waiting())
            ? 0
            : 16;
    int ident = ALooper_pollOnce(poll_timeout_ms, nullptr, &events,
                                 reinterpret_cast<void**>(&source));
    if (ident >= 0 && source) {
      source->process(app_, source);  // dispatches to OnAppCmd / input
    }
    DrainAndroidInputQueue(app_);
    if (app_->destroyRequested) {
      QuitFromUIThread();
      return;
    }
    ExecutePendingFunctionsFromUIThread();
    if (window_) {
      // Drive the actual paint/present from the UI loop. AndroidWindow's
      // RequestPaintImpl is a no-op (there's no OS-driven repaint callback like
      // GTK's draw signal), so without this the presenter's PaintAndPresent is
      // never called and the guest output is never composited to the swapchain
      // -> black screen. PaintFromUiThreadIfRequested() only paints when a
      // frame was actually requested, and forces the present through because
      // the swapchain image isn't retained between frames (see window_android.h).
      //
      // This present can BLOCK on swapchain acquire/present for up to a full
      // refresh (FIFO vsync wait). Ideally the present would run off this loop
      // thread entirely (the presenter's kGuestOutputThreadImmediately path),
      // but that path is gated by host_present_from_non_ui_thread AND disabled
      // whenever the surface has implicit vsync - which an Android FIFO
      // swapchain always does - so it falls back to UI-thread present here.
      // To keep a present stall from delaying input/lifecycle by a whole
      // iteration, re-service the looper non-blocking and re-drain input right
      // after the present returns, instead of waiting for the next frame-paced
      // ALooper_pollOnce(16ms) at the top of the loop.
      window_->PaintFromUiThreadIfRequested();

      int post_events;
      android_poll_source* post_source = nullptr;
      // timeout 0: drain only what is already ready, never block (a blocking
      // wait here would defeat the frame pacing and burn a core).
      while (ALooper_pollOnce(0, nullptr, &post_events,
                              reinterpret_cast<void**>(&post_source)) >= 0) {
        if (post_source) {
          post_source->process(app_, post_source);
        }
      }
      DrainAndroidInputQueue(app_);
      if (app_->destroyRequested) {
        QuitFromUIThread();
        return;
      }
    }
  }
}

void AndroidWindowedAppContext::OnAppCmdThunk(android_app* app, int32_t cmd) {
  static_cast<AndroidWindowedAppContext*>(app->userData)->HandleAppCmd(cmd);
}

int32_t AndroidWindowedAppContext::OnInputEventThunk(android_app* app, AInputEvent* event) {
  return static_cast<AndroidWindowedAppContext*>(app->userData)->HandleInputEvent(event);
}

void AndroidWindowedAppContext::HandleAppCmd(int32_t cmd) {
  switch (cmd) {
    case APP_CMD_INIT_WINDOW:
      // The OS created the surface window - hand it to the active window.
      if (window_ && app_->window) {
        window_->SetNativeWindow(app_->window);
      }
      break;
    case APP_CMD_TERM_WINDOW:
      // The surface is going away; detach before the OS destroys it.
      if (window_) {
        window_->SetNativeWindow(nullptr);
      }
      break;
    case APP_CMD_WINDOW_RESIZED:
    case APP_CMD_CONFIG_CHANGED:
      if (window_) {
        window_->OnAndroidResized();
      }
      break;
    case APP_CMD_GAINED_FOCUS:
      if (window_) {
        window_->OnAndroidFocusChanged(true);
      }
      break;
    case APP_CMD_LOST_FOCUS:
      if (window_) {
        window_->OnAndroidFocusChanged(false);
      }
      break;
    case APP_CMD_DESTROY:
      QuitFromUIThread();
      break;
    default:
      break;
  }
}

int32_t AndroidWindowedAppContext::HandleInputEvent(AInputEvent* event) {
  // Forward to the Android input driver (registered as the window's input sink)
  // which maps gamepad key/motion events into the Xbox 360 controller state.
  if (window_) {
    if (AndroidInputSink* sink = window_->android_input_sink()) {
      return sink->OnAndroidInputEvent(event);
    }
  }
  return 0;
}

}  // namespace ui
}  // namespace rex
