/**
 ******************************************************************************
 * ReXGlue runtime - Android gamepad input driver
 ******************************************************************************
 * @author  Android backend, 2026
 */

#include <rex/input/android/android_input_driver.h>

#include <android/input.h>
#include <android/keycodes.h>
#include <jni.h>

#include <cstdio>
#include <cstring>

#include <rex/input/flags.h>
#include <rex/input/input.h>
#include <rex/logging.h>
#include <rex/platform_android_jni.h>
#include <rex/ui/window_android.h>

namespace rex::input::android {

namespace {

// XInput thumbstick range is [-32768, 32767]; NDK stick axes are [-1, 1].
int16_t StickToAxis(float v) {
  if (v > 1.0f) {
    v = 1.0f;
  } else if (v < -1.0f) {
    v = -1.0f;
  }
  int32_t scaled = static_cast<int32_t>(v * 32767.0f);
  if (scaled > 32767) {
    scaled = 32767;
  } else if (scaled < -32768) {
    scaled = -32768;
  }
  return static_cast<int16_t>(scaled);
}

// XInput triggers are [0, 255]; NDK trigger axes are [0, 1].
uint8_t TriggerToByte(float v) {
  if (v > 1.0f) {
    v = 1.0f;
  } else if (v < 0.0f) {
    v = 0.0f;
  }
  return static_cast<uint8_t>(v * 255.0f + 0.5f);
}

uint16_t KeycodeToButton(int32_t keycode) {
  switch (keycode) {
    case AKEYCODE_BUTTON_A:
      return X_INPUT_GAMEPAD_A;
    case AKEYCODE_BUTTON_B:
      return X_INPUT_GAMEPAD_B;
    case AKEYCODE_BUTTON_X:
      return X_INPUT_GAMEPAD_X;
    case AKEYCODE_BUTTON_Y:
      return X_INPUT_GAMEPAD_Y;
    case AKEYCODE_BUTTON_L1:
      return X_INPUT_GAMEPAD_LEFT_SHOULDER;
    case AKEYCODE_BUTTON_R1:
      return X_INPUT_GAMEPAD_RIGHT_SHOULDER;
    case AKEYCODE_BUTTON_THUMBL:
      return X_INPUT_GAMEPAD_LEFT_THUMB;
    case AKEYCODE_BUTTON_THUMBR:
      return X_INPUT_GAMEPAD_RIGHT_THUMB;
    case AKEYCODE_BUTTON_START:
      return X_INPUT_GAMEPAD_START;
    case AKEYCODE_BUTTON_SELECT:
      return X_INPUT_GAMEPAD_BACK;
    case AKEYCODE_BUTTON_MODE:
      return X_INPUT_GAMEPAD_GUIDE;
    case AKEYCODE_DPAD_UP:
      return X_INPUT_GAMEPAD_DPAD_UP;
    case AKEYCODE_DPAD_DOWN:
      return X_INPUT_GAMEPAD_DPAD_DOWN;
    case AKEYCODE_DPAD_LEFT:
      return X_INPUT_GAMEPAD_DPAD_LEFT;
    case AKEYCODE_DPAD_RIGHT:
      return X_INPUT_GAMEPAD_DPAD_RIGHT;
    default:
      return 0;
  }
}

constexpr uint16_t kDpadMask = X_INPUT_GAMEPAD_DPAD_LEFT | X_INPUT_GAMEPAD_DPAD_RIGHT |
                               X_INPUT_GAMEPAD_DPAD_UP | X_INPUT_GAMEPAD_DPAD_DOWN;

}  // namespace

AndroidInputDriver::AndroidInputDriver(rex::ui::Window* window, size_t window_z_order)
    : InputDriver(window, window_z_order) {}

AndroidInputDriver::~AndroidInputDriver() = default;

X_STATUS AndroidInputDriver::Setup() {
  return X_STATUS_SUCCESS;
}

void AndroidInputDriver::OnWindowAvailable(rex::ui::Window* window) {
  // On Android the window is always an AndroidWindow; register so the app-glue
  // loop routes input events here.
  if (window) {
    static_cast<rex::ui::AndroidWindow*>(window)->SetAndroidInputSink(this);
  }
}

int32_t AndroidInputDriver::OnAndroidInputEvent(AInputEvent* event) {
  switch (AInputEvent_getType(event)) {
    case AINPUT_EVENT_TYPE_KEY:
      return HandleKey(event);
    case AINPUT_EVENT_TYPE_MOTION:
      return HandleMotion(event);
    default:
      return 0;
  }
}

int32_t AndroidInputDriver::HandleKey(AInputEvent* event) {
  int32_t source = AInputEvent_getSource(event);
  bool from_pad = (source & AINPUT_SOURCE_GAMEPAD) || (source & AINPUT_SOURCE_JOYSTICK) ||
                  (source & AINPUT_SOURCE_DPAD);
  if (!from_pad) {
    return 0;
  }
  int32_t action = AKeyEvent_getAction(event);
  if (action != AKEY_EVENT_ACTION_DOWN && action != AKEY_EVENT_ACTION_UP) {
    return 0;
  }
  bool down = (action == AKEY_EVENT_ACTION_DOWN);
  int32_t keycode = AKeyEvent_getKeyCode(event);

  // Some controllers deliver the analog triggers as L2/R2 button presses.
  if (keycode == AKEYCODE_BUTTON_L2 || keycode == AKEYCODE_BUTTON_R2) {
    std::lock_guard<std::mutex> lock(mutex_);
    uint8_t value = down ? 255 : 0;
    if (keycode == AKEYCODE_BUTTON_L2) {
      left_trigger_ = value;
    } else {
      right_trigger_ = value;
    }
    ++packet_number_;
    return 1;
  }

  uint16_t bit = KeycodeToButton(keycode);
  if (!bit) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (down) {
    buttons_ |= bit;
  } else {
    buttons_ &= ~bit;
  }
  ++packet_number_;
  return 1;
}

int32_t AndroidInputDriver::HandleMotion(AInputEvent* event) {
  int32_t source = AInputEvent_getSource(event);
  if (!(source & AINPUT_SOURCE_JOYSTICK)) {
    return 0;
  }

  float lx = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_X, 0);
  float ly = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_Y, 0);
  float rx = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_Z, 0);
  float ry = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_RZ, 0);
  float lt = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_LTRIGGER, 0);
  float rt = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_RTRIGGER, 0);
  // Fall back to BRAKE/GAS, which some controllers use for the triggers.
  if (lt == 0.0f) {
    lt = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_BRAKE, 0);
  }
  if (rt == 0.0f) {
    rt = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_GAS, 0);
  }
  float hat_x = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_HAT_X, 0);
  float hat_y = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_HAT_Y, 0);

  std::lock_guard<std::mutex> lock(mutex_);
  thumb_lx_ = StickToAxis(lx);
  thumb_ly_ = StickToAxis(-ly);  // Android Y is down-positive; XInput is up-positive.
  thumb_rx_ = StickToAxis(rx);
  thumb_ry_ = StickToAxis(-ry);
  left_trigger_ = TriggerToByte(lt);
  right_trigger_ = TriggerToByte(rt);
  // D-pad reported as a hat axis (controllers that send it as keys go through
  // HandleKey instead).
  buttons_ &= ~kDpadMask;
  if (hat_x < -0.5f) {
    buttons_ |= X_INPUT_GAMEPAD_DPAD_LEFT;
  } else if (hat_x > 0.5f) {
    buttons_ |= X_INPUT_GAMEPAD_DPAD_RIGHT;
  }
  if (hat_y < -0.5f) {
    buttons_ |= X_INPUT_GAMEPAD_DPAD_UP;
  } else if (hat_y > 0.5f) {
    buttons_ |= X_INPUT_GAMEPAD_DPAD_DOWN;
  }
  ++packet_number_;
  return 1;
}

X_RESULT AndroidInputDriver::GetCapabilities(uint32_t user_index, uint32_t flags,
                                             X_INPUT_CAPABILITIES* out_caps) {
  (void)flags;
  if (user_index != 0) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  if (out_caps) {
    std::memset(out_caps, 0, sizeof(*out_caps));
    out_caps->type = 0x01;      // XINPUT_DEVTYPE_GAMEPAD
    out_caps->sub_type = 0x01;  // XINPUT_DEVSUBTYPE_GAMEPAD
    out_caps->gamepad.buttons = 0xFFFF;
    out_caps->gamepad.left_trigger = 0xFF;
    out_caps->gamepad.right_trigger = 0xFF;
    out_caps->gamepad.thumb_lx = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_ly = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_rx = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_ry = static_cast<int16_t>(0x7FFF);
  }
  return X_ERROR_SUCCESS;
}

X_RESULT AndroidInputDriver::GetState(uint32_t user_index, X_INPUT_STATE* out_state) {
  // Report user 0 always connected (like the NOP driver) so the guest doesn't
  // stall waiting for a controller; state is idle until events arrive.
  if (user_index != 0) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  if (out_state) {
    std::lock_guard<std::mutex> lock(mutex_);
    out_state->packet_number = packet_number_;
    out_state->gamepad.buttons = buttons_;
    out_state->gamepad.left_trigger = left_trigger_;
    out_state->gamepad.right_trigger = right_trigger_;
    out_state->gamepad.thumb_lx = thumb_lx_;
    out_state->gamepad.thumb_ly = thumb_ly_;
    out_state->gamepad.thumb_rx = thumb_rx_;
    out_state->gamepad.thumb_ry = thumb_ry_;
  }
  return X_ERROR_SUCCESS;
}

void AndroidInputDriver::Vibrate(uint16_t left_motor, uint16_t right_motor) {
  JNIEnv* env = rex::GetAndroidJniEnv();
  jobject activity = rex::GetAndroidActivity();
  if (!env || !activity) {
    return;
  }
  if (env->PushLocalFrame(16) != 0) {
    return;
  }
  do {
    // Vibrator v = (Vibrator) activity.getSystemService("vibrator");
    jclass act_cls = env->GetObjectClass(activity);
    jmethodID m_get_service =
        env->GetMethodID(act_cls, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
    if (!m_get_service) {
      break;
    }
    jstring svc = env->NewStringUTF("vibrator");
    jobject vibrator = env->CallObjectMethod(activity, m_get_service, svc);
    if (!vibrator || env->ExceptionCheck()) {
      break;
    }
    jclass vib_cls = env->GetObjectClass(vibrator);

    uint16_t magnitude = left_motor > right_motor ? left_motor : right_motor;
    if (magnitude == 0) {
      jmethodID m_cancel = env->GetMethodID(vib_cls, "cancel", "()V");
      if (m_cancel) {
        env->CallVoidMethod(vibrator, m_cancel);
      }
      break;
    }

    // VibrationEffect e = VibrationEffect.createOneShot(120ms, amplitude 1..255);
    // v.vibrate(e). (minSdk 29, so VibrationEffect is always available.)
    jclass eff_cls = env->FindClass("android/os/VibrationEffect");
    jmethodID m_create =
        eff_cls ? env->GetStaticMethodID(eff_cls, "createOneShot",
                                         "(JI)Landroid/os/VibrationEffect;")
                : nullptr;
    if (!m_create) {
      break;
    }
    jint amplitude = 1 + (static_cast<int>(magnitude) * 254) / 65535;
    jobject effect = env->CallStaticObjectMethod(eff_cls, m_create, static_cast<jlong>(120),
                                                 amplitude);
    if (!effect || env->ExceptionCheck()) {
      break;
    }
    jmethodID m_vibrate =
        env->GetMethodID(vib_cls, "vibrate", "(Landroid/os/VibrationEffect;)V");
    if (m_vibrate) {
      env->CallVoidMethod(vibrator, m_vibrate, effect);
    }
  } while (false);

  if (env->ExceptionCheck()) {
    env->ExceptionClear();
  }
  env->PopLocalFrame(nullptr);
}

X_RESULT AndroidInputDriver::SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) {
  if (user_index != 0) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  // Map the XInput motor speeds onto a short device-vibrator pulse. Continuous
  // XInput rumble doesn't map perfectly to Android's one-shot effects; this is a
  // best-effort pulse whose amplitude tracks the stronger motor.
  if (vibration) {
    Vibrate(vibration->left_motor_speed, vibration->right_motor_speed);
  }
  return X_ERROR_SUCCESS;
}

X_RESULT AndroidInputDriver::GetKeystroke(uint32_t user_index, uint32_t flags,
                                          X_INPUT_KEYSTROKE* out_keystroke) {
  (void)flags;
  (void)out_keystroke;
  if (user_index != 0) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  return X_ERROR_EMPTY;
}

}  // namespace rex::input::android
