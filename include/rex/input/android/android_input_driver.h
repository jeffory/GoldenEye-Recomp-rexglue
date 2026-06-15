#pragma once
/**
 ******************************************************************************
 * ReXGlue runtime - Android gamepad input driver
 ******************************************************************************
 * @author  Android backend, 2026
 *
 * Maps NDK gamepad events (delivered via the NativeActivity input queue and
 * forwarded by AndroidWindowedAppContext) into the Xbox 360 controller state the
 * guest polls through XInput. Mirrors the SDL/MnK drivers but is event-driven:
 * the app-glue loop pushes events through the AndroidInputSink interface; the
 * guest polls a snapshot via GetState (cross-thread, mutex-guarded).
 */

#include <cstdint>
#include <mutex>

#include <rex/input/input_driver.h>
#include <rex/ui/android_input_sink.h>

namespace rex::input::android {

class AndroidInputDriver final : public InputDriver, public rex::ui::AndroidInputSink {
 public:
  explicit AndroidInputDriver(rex::ui::Window* window, size_t window_z_order);
  ~AndroidInputDriver() override;

  X_STATUS Setup() override;

  X_RESULT GetCapabilities(uint32_t user_index, uint32_t flags,
                           X_INPUT_CAPABILITIES* out_caps) override;
  X_RESULT GetState(uint32_t user_index, X_INPUT_STATE* out_state) override;
  X_RESULT SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) override;
  X_RESULT GetKeystroke(uint32_t user_index, uint32_t flags,
                        X_INPUT_KEYSTROKE* out_keystroke) override;

  void OnWindowAvailable(rex::ui::Window* window) override;

  // AndroidInputSink - called on the UI thread from the app-glue loop.
  int32_t OnAndroidInputEvent(AInputEvent* event) override;

 private:
  int32_t HandleKey(AInputEvent* event);
  int32_t HandleMotion(AInputEvent* event);
  // Drives the device vibrator via JNI from XInput motor speeds (best-effort).
  void Vibrate(uint16_t left_motor, uint16_t right_motor);

  // Single player (user 0). Aggregated XInput state, written from input events
  // and read by GetState on the guest thread.
  std::mutex mutex_;
  uint16_t buttons_ = 0;
  uint8_t left_trigger_ = 0;
  uint8_t right_trigger_ = 0;
  int16_t thumb_lx_ = 0;
  int16_t thumb_ly_ = 0;
  int16_t thumb_rx_ = 0;
  int16_t thumb_ry_ = 0;
  uint32_t packet_number_ = 0;
};

}  // namespace rex::input::android
