/**
 ******************************************************************************
 * ReXGlue runtime - Android audio backend                                    *
 ******************************************************************************
 */

#pragma once

#include <rex/audio/audio_system.h>

namespace rex::audio::android {

class AndroidAudioSystem : public AudioSystem {
 public:
  explicit AndroidAudioSystem(runtime::FunctionDispatcher* function_dispatcher);
  ~AndroidAudioSystem() override;

  static bool IsAvailable() { return true; }

  static std::unique_ptr<AudioSystem> Create(runtime::FunctionDispatcher* function_dispatcher);

  X_STATUS CreateDriver(size_t index, rex::thread::Semaphore* semaphore,
                        AudioDriver** out_driver) override;
  void DestroyDriver(AudioDriver* driver) override;

 protected:
  void Initialize() override;
};

}  // namespace rex::audio::android
