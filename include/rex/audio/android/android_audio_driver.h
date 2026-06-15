/**
 ******************************************************************************
 * ReXGlue runtime - Android audio backend                                    *
 ******************************************************************************
 *
 * Native Android audio driver. Unlike the SDL backend (which requires the
 * Java SDLActivity to register its audio subsystem and therefore cannot
 * initialize under our bare NativeActivity), this driver has no SDL or JNI
 * dependency. It paces the guest audio worker at the real-time frame rate by
 * releasing the client semaphore once per frame period, which is all the guest
 * (and XAudioRegisterRenderDriverClient) needs to succeed and keep running.
 * Frames submitted by the guest are currently discarded (silent output); a
 * real AAudio output stream can be layered on top of this contract later.
 */

#pragma once

#include <atomic>
#include <thread>

#include <rex/audio/audio_driver.h>
#include <rex/thread.h>

namespace rex::audio::android {

class AndroidAudioDriver : public AudioDriver {
 public:
  AndroidAudioDriver(memory::Memory* memory, rex::thread::Semaphore* semaphore);
  ~AndroidAudioDriver() override;

  bool Initialize();
  void SubmitFrame(uint32_t frame_ptr) override;
  void Shutdown();

 private:
  // Xbox audio frames are 256 samples per channel at 48 kHz.
  static constexpr uint32_t kChannelSamples = 256;
  static constexpr uint32_t kFrameFrequency = 48000;

  rex::thread::Semaphore* semaphore_ = nullptr;
  std::thread pacing_thread_;
  std::atomic<bool> running_{false};
};

}  // namespace rex::audio::android
