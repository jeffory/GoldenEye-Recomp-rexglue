/**
 ******************************************************************************
 * ReXGlue runtime - Android audio backend                                    *
 ******************************************************************************
 *
 * Native Android audio driver. Unlike the SDL backend (which requires the
 * Java SDLActivity to register its audio subsystem and therefore cannot
 * initialize under our bare NativeActivity), this driver has no SDL or JNI
 * dependency.
 *
 * It opens a real AAudio output stream in LOW_LATENCY / EXCLUSIVE mode and
 * drives the guest from AAudio's data callback, which the platform runs on a
 * high-priority FAST audio thread off the game's cores. The callback both
 * fills the device buffer with submitted guest audio and releases the client
 * semaphore at the hardware consumption rate, replacing the old normal-priority
 * `sleep_until` pacing thread (which jittered under scheduler load, competed
 * with the render thread for cores, and produced no sound).
 *
 * If the AAudio stream cannot be opened (very old device / driver), the driver
 * falls back to a high-priority, render-core-avoiding timer pacer so the guest
 * still runs (silently) instead of stalling its audio init.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <queue>
#include <stack>
#include <thread>

#include <rex/audio/audio_driver.h>
#include <rex/thread.h>

// Mirror AAudio.h's opaque-handle typedef so this header needs no NDK include.
// Must match <aaudio/AAudio.h> exactly (it is `typedef struct ... AAudioStream`),
// otherwise the two declarations conflict when both are included.
struct AAudioStreamStruct;
typedef struct AAudioStreamStruct AAudioStream;

namespace rex::audio::android {

class AndroidAudioDriver : public AudioDriver {
 public:
  AndroidAudioDriver(memory::Memory* memory, rex::thread::Semaphore* semaphore);
  ~AndroidAudioDriver() override;

  bool Initialize();
  void SubmitFrame(uint32_t frame_ptr) override;
  void Shutdown();

 private:
  // Xbox audio frames are 256 samples per channel at 48 kHz. The guest submits
  // 6 sequential big-endian channels per frame.
  static constexpr uint32_t kChannelSamples = 256;
  static constexpr uint32_t kFrameFrequency = 48000;
  static constexpr uint32_t kGuestChannels = 6;
  // Frame buffers in the pool are always allocated at the 6-channel maximum so
  // a device channel-count change on an AAudio restart can never overflow a
  // buffer sized for the previous (smaller) layout.
  static constexpr uint32_t kMaxFrameSamples = kChannelSamples * kGuestChannels;

  bool OpenStream();
  void CloseStream();
  void StartFallbackPacer();

  // Releases the client semaphore once per kChannelSamples of output consumed
  // by the device, clocked off the AAudio callback instead of a sleep timer.
  // Bounded by the semaphore's max; surplus releases are silently dropped.
  void PaceFromConsumed(int32_t frames_consumed);

  // AAudio C callbacks.
  static int32_t DataCallback(AAudioStream* stream, void* user_data, void* audio_data,
                              int32_t num_frames);
  static void ErrorCallback(AAudioStream* stream, void* user_data, int32_t error);

  rex::thread::Semaphore* semaphore_ = nullptr;

  AAudioStream* stream_ = nullptr;
  // Channels the device actually opened with (2 for handhelds, occasionally 6).
  uint8_t device_channels_ = 2;
  // Interleaved float samples per output frame (kChannelSamples * device_channels_).
  uint32_t frame_samples_ = kChannelSamples * 2;

  // Frame FIFO converted to the device layout on the (non-realtime) submit
  // thread, drained by the realtime callback. front_offset_ tracks partial
  // consumption of the head frame when AAudio's burst size is not a multiple
  // of kChannelSamples. All three are guarded by frames_mutex_.
  std::mutex frames_mutex_;
  std::queue<float*> frames_queued_;
  std::stack<float*> frames_unused_;
  size_t front_offset_ = 0;

  // Per-channel frames consumed by the device but not yet credited to the
  // semaphore. Touched only from the realtime callback; no lock needed.
  int64_t pacing_accumulator_ = 0;

  // Fallback timer pacer (used only when AAudio fails to open).
  std::thread pacing_thread_;
  std::atomic<bool> running_{false};
  bool use_aaudio_ = false;

  // Serializes stream open/close/restart so the error-callback recovery path
  // never races Shutdown over stream_. The realtime DataCallback never touches
  // stream_, so it is unaffected by this lock.
  std::mutex lifecycle_mutex_;
  std::thread restart_thread_;
  // Guards a single in-flight restart kicked off from the error callback.
  std::atomic<bool> restarting_{false};
};

}  // namespace rex::audio::android
