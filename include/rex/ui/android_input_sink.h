#pragma once
/**
 ******************************************************************************
 * ReXGlue runtime - Android input bridge
 ******************************************************************************
 * @author  Android backend, 2026
 *
 * Decouples the Android UI loop (rexui) from the input driver (rexinput): the
 * Android input driver implements this sink and registers it with the
 * AndroidWindow; AndroidWindowedAppContext forwards raw NDK input events to it.
 * rexui defines the interface so it has no dependency on rexinput.
 */

struct AInputEvent;

namespace rex {
namespace ui {

class AndroidInputSink {
 public:
  virtual ~AndroidInputSink() = default;
  // Returns 1 if the event was handled (consumed), 0 otherwise.
  virtual int32_t OnAndroidInputEvent(AInputEvent* event) = 0;
};

}  // namespace ui
}  // namespace rex
