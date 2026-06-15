/**
 ******************************************************************************
 * ReXGlue runtime - Android audio backend                                    *
 ******************************************************************************
 */

#include <rex/audio/android/android_audio_driver.h>
#include <rex/audio/android/android_audio_system.h>

namespace rex::audio::android {

std::unique_ptr<AudioSystem> AndroidAudioSystem::Create(
    runtime::FunctionDispatcher* function_dispatcher) {
  return std::make_unique<AndroidAudioSystem>(function_dispatcher);
}

AndroidAudioSystem::AndroidAudioSystem(runtime::FunctionDispatcher* function_dispatcher)
    : AudioSystem(function_dispatcher) {}

AndroidAudioSystem::~AndroidAudioSystem() = default;

void AndroidAudioSystem::Initialize() { AudioSystem::Initialize(); }

X_STATUS AndroidAudioSystem::CreateDriver(size_t /*index*/, rex::thread::Semaphore* semaphore,
                                          AudioDriver** out_driver) {
  assert_not_null(out_driver);
  auto driver = new AndroidAudioDriver(memory_, semaphore);
  if (!driver->Initialize()) {
    driver->Shutdown();
    delete driver;
    return X_STATUS_UNSUCCESSFUL;
  }
  *out_driver = driver;
  return X_STATUS_SUCCESS;
}

void AndroidAudioSystem::DestroyDriver(AudioDriver* driver) {
  assert_not_null(driver);
  auto android_driver = dynamic_cast<AndroidAudioDriver*>(driver);
  assert_not_null(android_driver);
  android_driver->Shutdown();
  delete android_driver;
}

}  // namespace rex::audio::android
