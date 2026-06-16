/**
 ******************************************************************************
 * ReXGlue runtime - Android audio backend                                    *
 ******************************************************************************
 *
 * Real low-latency AAudio output. AAudio runs DataCallback() on the platform
 * FAST/high-priority audio thread (off the game's cores). The callback drains
 * guest-submitted frames into the device buffer and releases the client
 * semaphore at the hardware consumption rate, which both paces the guest and
 * keeps it producing - no normal-priority `sleep_until` thread, and actual
 * sound instead of discarded output.
 */

#include <rex/audio/android/android_audio_driver.h>

#include <aaudio/AAudio.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>

#include <rex/audio/conversion.h>
#include <rex/cvar.h>
#include <rex/logging.h>

// Defined by the SDL driver (always compiled in); reused here so the Android
// path honours the same mute toggle.
REXCVAR_DECLARE(bool, audio_mute);

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

  {
    std::lock_guard<std::mutex> guard(lifecycle_mutex_);
    use_aaudio_ = OpenStream();
  }

  if (use_aaudio_) {
    REXAPU_INFO(
        "AndroidAudioDriver: AAudio LOW_LATENCY stream started ({} ch @ {} Hz)",
        device_channels_, kFrameFrequency);
    return true;
  }

  // No usable AAudio stream: keep the guest's audio init alive with a
  // high-priority, render-core-avoiding timer pacer (silent output) rather than
  // letting the semaphore go quiet and the guest tear its audio object down.
  REXAPU_WARN(
      "AndroidAudioDriver: AAudio unavailable; using fallback timer pacer (silent)");
  StartFallbackPacer();
  return true;
}

bool AndroidAudioDriver::OpenStream() {
  AAudioStreamBuilder* builder = nullptr;
  aaudio_result_t result = AAudio_createStreamBuilder(&builder);
  if (result != AAUDIO_OK || !builder) {
    REXAPU_ERROR("AAudio_createStreamBuilder failed: {}",
                 AAudio_convertResultToText(result));
    return false;
  }

  AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
  AAudioStreamBuilder_setSampleRate(builder, static_cast<int32_t>(kFrameFrequency));
  AAudioStreamBuilder_setChannelCount(builder, 2);
  AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
  AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
  AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_EXCLUSIVE);
  AAudioStreamBuilder_setDataCallback(builder, &AndroidAudioDriver::DataCallback, this);
  AAudioStreamBuilder_setErrorCallback(builder, &AndroidAudioDriver::ErrorCallback, this);

  result = AAudioStreamBuilder_openStream(builder, &stream_);
  if (result != AAUDIO_OK || !stream_) {
    // EXCLUSIVE is not always grantable; retry shared before giving up.
    if (stream_) {
      AAudioStream_close(stream_);
      stream_ = nullptr;
    }
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
    result = AAudioStreamBuilder_openStream(builder, &stream_);
  }
  AAudioStreamBuilder_delete(builder);

  if (result != AAUDIO_OK || !stream_) {
    REXAPU_ERROR("AAudioStreamBuilder_openStream failed: {}",
                 AAudio_convertResultToText(result));
    stream_ = nullptr;
    return false;
  }

  device_channels_ = static_cast<uint8_t>(AAudioStream_getChannelCount(stream_));
  if (device_channels_ != 2 && device_channels_ != 6) {
    REXAPU_WARN("AAudio opened with {} channels (only 2/6 supported); closing",
                device_channels_);
    AAudioStream_close(stream_);
    stream_ = nullptr;
    return false;
  }
  frame_samples_ = kChannelSamples * device_channels_;

  // Keep the device buffer to a couple of bursts for low latency.
  const int32_t burst = AAudioStream_getFramesPerBurst(stream_);
  if (burst > 0) {
    AAudioStream_setBufferSizeInFrames(stream_, burst * 2);
  }

  result = AAudioStream_requestStart(stream_);
  if (result != AAUDIO_OK) {
    REXAPU_ERROR("AAudioStream_requestStart failed: {}",
                 AAudio_convertResultToText(result));
    AAudioStream_close(stream_);
    stream_ = nullptr;
    return false;
  }

  return true;
}

void AndroidAudioDriver::CloseStream() {
  if (stream_) {
    AAudioStream_requestStop(stream_);
    AAudioStream_close(stream_);
    stream_ = nullptr;
  }
}

int32_t AndroidAudioDriver::DataCallback(AAudioStream* /*stream*/, void* user_data,
                                         void* audio_data, int32_t num_frames) {
  auto* self = static_cast<AndroidAudioDriver*>(user_data);
  float* out = static_cast<float*>(audio_data);
  const size_t need =
      static_cast<size_t>(num_frames) * static_cast<size_t>(self->device_channels_);

  size_t filled = 0;
  {
    std::unique_lock<std::mutex> guard(self->frames_mutex_);
    while (filled < need && !self->frames_queued_.empty()) {
      float* front = self->frames_queued_.front();
      const size_t avail = self->frame_samples_ - self->front_offset_;
      const size_t take = std::min(avail, need - filled);
      std::memcpy(out + filled, front + self->front_offset_, take * sizeof(float));
      filled += take;
      self->front_offset_ += take;
      if (self->front_offset_ >= self->frame_samples_) {
        self->frames_queued_.pop();
        self->frames_unused_.push(front);
        self->front_offset_ = 0;
      }
    }
  }

  // Underrun (or startup): fill the remainder with silence.
  if (filled < need) {
    std::memset(out + filled, 0, (need - filled) * sizeof(float));
  }

  // Credit the guest one frame per kChannelSamples actually consumed by the
  // device. Clocked off the hardware, so it both paces and primes the guest.
  self->PaceFromConsumed(num_frames);

  return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

void AndroidAudioDriver::PaceFromConsumed(int32_t frames_consumed) {
  pacing_accumulator_ += frames_consumed;
  while (pacing_accumulator_ >= static_cast<int64_t>(kChannelSamples)) {
    pacing_accumulator_ -= static_cast<int64_t>(kChannelSamples);
    // Release returns false when the buffer is already full (max reached);
    // that simply means "don't ask the guest for more right now" - ignore it.
    semaphore_->Release(1, nullptr);
  }
}

void AndroidAudioDriver::ErrorCallback(AAudioStream* stream, void* user_data,
                                       int32_t error) {
  auto* self = static_cast<AndroidAudioDriver*>(user_data);
  REXAPU_WARN("AndroidAudioDriver: AAudio error {} ({})", error,
              AAudio_convertResultToText(error));

  if (!self->running_.load()) {
    return;
  }
  // Recover on a separate thread - AAudio forbids reopening from the error
  // callback thread. Only one restart in flight at a time.
  bool expected = false;
  if (!self->restarting_.compare_exchange_strong(expected, true)) {
    return;
  }

  std::lock_guard<std::mutex> guard(self->lifecycle_mutex_);
  if (self->restart_thread_.joinable()) {
    self->restart_thread_.join();
  }
  self->restart_thread_ = std::thread([self, stream]() {
    rex::thread::set_current_thread_name("AAudio Restart");
    std::lock_guard<std::mutex> g(self->lifecycle_mutex_);
    if (self->stream_ == stream) {
      AAudioStream_close(stream);
      self->stream_ = nullptr;
    }
    if (self->running_.load() && !self->stream_) {
      if (self->OpenStream()) {
        REXAPU_INFO("AndroidAudioDriver: AAudio stream restarted");
      } else {
        REXAPU_ERROR("AndroidAudioDriver: AAudio restart failed; audio silent");
      }
    }
    self->restarting_.store(false);
  });
}

void AndroidAudioDriver::StartFallbackPacer() {
  pacing_thread_ = std::thread([this]() {
    rex::thread::set_current_thread_name("Android Audio Pacer");

    // Best-effort: lift above the normal run queue and keep off the render core
    // (CPU 0) so even a degraded silent pacer does not fight the frame thread.
    // Both calls can fail without privileges; failures are intentionally ignored.
    struct sched_param sp {};
    sp.sched_priority = sched_get_priority_min(SCHED_FIFO) + 1;
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);

    const long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpu > 1) {
      cpu_set_t cpus;
      CPU_ZERO(&cpus);
      for (long c = 1; c < ncpu; ++c) {
        CPU_SET(c, &cpus);
      }
      pthread_setaffinity_np(pthread_self(), sizeof(cpus), &cpus);
    }

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
}

void AndroidAudioDriver::SubmitFrame(uint32_t frame_ptr) {
  // Fallback pacer path discards output (no stream to feed).
  if (!use_aaudio_) {
    return;
  }

  const auto input_frame = memory_->TranslateVirtual<float*>(frame_ptr);

  float* output_frame;
  {
    std::unique_lock<std::mutex> guard(frames_mutex_);
    if (frames_unused_.empty()) {
      output_frame = new float[kMaxFrameSamples];
    } else {
      output_frame = frames_unused_.top();
      frames_unused_.pop();
    }
  }

  // Convert the guest's sequential big-endian 6-channel frame to the device's
  // interleaved little-endian layout off the realtime thread, so DataCallback
  // only has to memcpy.
  if (REXCVAR_GET(audio_mute)) {
    std::memset(output_frame, 0, frame_samples_ * sizeof(float));
  } else {
    switch (device_channels_) {
      case 2:
        conversion::sequential_6_BE_to_interleaved_2_LE(output_frame, input_frame,
                                                        kChannelSamples);
        break;
      case 6:
        conversion::sequential_6_BE_to_interleaved_6_LE(output_frame, input_frame,
                                                        kChannelSamples);
        break;
      default:
        std::memset(output_frame, 0, frame_samples_ * sizeof(float));
        break;
    }
  }

  static uint32_t submit_count = 0;
  if (submit_count < 10) {
    REXAPU_DEBUG("AndroidAudioDriver::SubmitFrame: frame_ptr={:08X} queued_count={}",
                 frame_ptr, frames_queued_.size() + 1);
    submit_count++;
  }

  {
    std::unique_lock<std::mutex> guard(frames_mutex_);
    frames_queued_.push(output_frame);
  }
}

void AndroidAudioDriver::Shutdown() {
  if (!running_.exchange(false)) {
    return;
  }

  // Tear the stream down. Holding lifecycle_mutex_ here blocks until any
  // in-flight restart releases it; the restart then observes running_==false.
  {
    std::lock_guard<std::mutex> guard(lifecycle_mutex_);
    CloseStream();
  }

  // Join outside the lock: a restart thread still finishing needs the lock.
  if (restart_thread_.joinable()) {
    restart_thread_.join();
  }
  if (pacing_thread_.joinable()) {
    pacing_thread_.join();
  }

  std::lock_guard<std::mutex> guard(frames_mutex_);
  while (!frames_unused_.empty()) {
    delete[] frames_unused_.top();
    frames_unused_.pop();
  }
  while (!frames_queued_.empty()) {
    delete[] frames_queued_.front();
    frames_queued_.pop();
  }
}

}  // namespace rex::audio::android
