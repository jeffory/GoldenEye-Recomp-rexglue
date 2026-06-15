/**
 ******************************************************************************
 * ReXGlue runtime - Android audio backend                                    *
 ******************************************************************************
 */

#include <rex/audio/android/android_audio_driver.h>

#include <chrono>

namespace rex::audio::android {

AndroidAudioDriver::AndroidAudioDriver(memory::Memory* memory,
                                       rex::thread::Semaphore* semaphore)
    : AudioDriver(memory), semaphore_(semaphore) {}

AndroidAudioDriver::~AndroidAudioDriver() { Shutdown(); }

bool AndroidAudioDriver::Initialize() {
  if (running_.load()) {
    return true;
  }
  running_.store(true);

  // Pace the guest at the real-time audio frame rate. Each guest frame carries
  // kChannelSamples (256) samples per channel at kFrameFrequency (48 kHz), i.e.
  // ~5.333 ms per frame. Releasing the client semaphore once per period lets
  // the audio worker invoke the guest render callback at the correct cadence,
  // exactly as the SDL backend does from its device callback.
  pacing_thread_ = std::thread([this]() {
    rex::thread::set_current_thread_name("Android Audio Pacer");
    const auto period = std::chrono::microseconds(
        (static_cast<int64_t>(kChannelSamples) * 1000000) / kFrameFrequency);
    auto next = std::chrono::steady_clock::now();
    while (running_.load()) {
      next += period;
      std::this_thread::sleep_until(next);
      if (!running_.load()) {
        break;
      }
      semaphore_->Release(1, nullptr);
    }
  });

  return true;
}

void AndroidAudioDriver::SubmitFrame(uint32_t /*frame_ptr*/) {
  // Output is discarded (silent). The pacing thread alone drives the guest.
}

void AndroidAudioDriver::Shutdown() {
  if (!running_.exchange(false)) {
    return;
  }
  if (pacing_thread_.joinable()) {
    pacing_thread_.join();
  }
}

}  // namespace rex::audio::android
