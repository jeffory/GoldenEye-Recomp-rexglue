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
 * guest polls a snapshot lock-free via GetState (acquire load; UI thread writes
 * via release store after each event).
 */

#include <atomic>
#include <cstdint>
#include <jni.h>

#include <rex/input/input_driver.h>
#include <rex/ui/android_input_sink.h>

namespace rex::input::android {

// Snapshot of XInput controller state published atomically by the UI thread.
// Packed to exactly 16 bytes so std::atomic<InputSnapshot> is lock-free on
// AArch64 (uses LDXP/STXP; verified at runtime in Setup()).
struct InputSnapshot {
  uint32_t packet_number = 0;
  uint16_t buttons = 0;
  uint8_t left_trigger = 0;
  uint8_t right_trigger = 0;
  int16_t thumb_lx = 0;
  int16_t thumb_ly = 0;
  int16_t thumb_rx = 0;
  int16_t thumb_ry = 0;
};
static_assert(sizeof(InputSnapshot) == 16);

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

  // Writer-side state: mutated only on the UI thread (HandleKey/HandleMotion).
  // After every mutation it is release-stored into snapshot_ below.
  InputSnapshot w_state_{};

  // Published snapshot: stored (release) by the UI thread after every write;
  // loaded (acquire) lock-free by the guest poll thread in GetState.
  // On AArch64 std::atomic<InputSnapshot> is lock-free via LDXP/STXP.
  std::atomic<InputSnapshot> snapshot_{InputSnapshot{}};

  // JNI handles cached on the first Vibrate() call (guest thread, single
  // caller).  Global refs kept for process lifetime; released in destructor.
  bool jni_cached_ = false;
  jobject vibrator_global_ = nullptr;    // NewGlobalRef
  jmethodID m_cancel_ = nullptr;
  jclass eff_cls_global_ = nullptr;      // NewGlobalRef
  jmethodID m_create_oneshot_ = nullptr;
  jmethodID m_vibrate_ = nullptr;
};

}  // namespace rex::input::android
