/**
 * @file        core/perf/counter.cpp
 * @brief       Performance counter registry implementation
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/perf/counter.h>

#include <rex/cvar.h>
#include <rex/logging.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

REXCVAR_DEFINE_STRING(perf_log_csv, "", "Perf",
                      "Path to write per-frame CSV log (empty = disabled)");

namespace rex::perf {

namespace {

constexpr size_t kNumCounters = static_cast<size_t>(CounterId::kCount);

std::array<std::atomic<int64_t>, kNumCounters> g_counters{};
std::array<std::atomic<int64_t>, kNumCounters> g_snapshot{};

constexpr const char* kCounterNames[] = {
    "frame_time_us",
    "fps",
    "draw_calls",
    "command_buffer_stalls",
    "vertices_processed",
    "xma_frames_decoded",
    "audio_frame_latency_us",
    "buffer_queue_depth",
    "functions_dispatched",
    "interrupt_dispatches",
    "active_threads",
    "apc_queue_depth",
    "critical_region_contentions",
    "texture_cache_hits",
    "texture_cache_misses",
    "pipeline_cache_hits",
    "pipeline_cache_misses",
    "cp_execute_us",
    "cp_idle_us",
    "cp_wait_reg_mem_us",
    "present_block_us",
    "guest_gpu_wait_us",
    "gpu_frame_us",
};
static_assert(std::size(kCounterNames) == kNumCounters, "kCounterNames must match CounterId enum");

// Gauge counters are snapshotted but NOT zeroed each frame.
// Accumulators (everything else) are zeroed after snapshot.
constexpr bool kIsGauge[] = {
    false,  // kFrameTimeUs       (set each frame)
    false,  // kFps               (set each frame)
    false,  // kDrawCalls
    false,  // kCommandBufferStalls
    false,  // kVerticesProcessed
    false,  // kXmaFramesDecoded
    false,  // kAudioFrameLatencyUs
    false,  // kBufferQueueDepth  (set each frame)
    false,  // kFunctionsDispatched
    false,  // kInterruptDispatches
    true,   // kActiveThreads     (inc/dec over lifetime)
    false,  // kApcQueueDepth
    true,   // kCriticalRegionContentions (running total)
    false,  // kTextureCacheHits
    false,  // kTextureCacheMisses
    false,  // kPipelineCacheHits
    false,  // kPipelineCacheMisses
    false,  // kCpExecuteUs       (accumulated per frame)
    false,  // kCpIdleUs          (accumulated per frame)
    false,  // kCpWaitRegMemUs    (accumulated per frame)
    false,  // kPresentBlockUs    (accumulated per frame)
    false,  // kGuestGpuWaitUs    (accumulated per frame)
    false,  // kGpuFrameUs        (set per frame)
};
static_assert(std::size(kIsGauge) == kNumCounters, "kIsGauge must match CounterId enum");

// CSV state. Guarded by g_csv_mutex: WriteCsvFrame runs on the CP worker while
// SetCsvLogPath can be called live from the UI thread (pause-menu toggle).
// Uncontended lock per frame write -- negligible next to the fprintf itself.
std::mutex g_csv_mutex;
std::FILE* g_csv_file = nullptr;
std::string g_csv_path;
int g_csv_frame_count = 0;

}  // anonymous namespace

const char* CounterName(CounterId id) {
  auto idx = static_cast<size_t>(id);
  if (idx < kNumCounters)
    return kCounterNames[idx];
  return "unknown";
}

void SetCounter(CounterId id, int64_t value) {
  g_counters[static_cast<size_t>(id)].store(value, std::memory_order_relaxed);
}

void IncrementCounter(CounterId id, int64_t delta) {
  g_counters[static_cast<size_t>(id)].fetch_add(delta, std::memory_order_relaxed);
}

int64_t GetCounter(CounterId id) {
  return g_counters[static_cast<size_t>(id)].load(std::memory_order_relaxed);
}

void ResetFrameCounters() {
  for (size_t i = 0; i < kNumCounters; ++i) {
    if (kIsGauge[i]) {
      // Gauges: snapshot the current value, don't zero
      g_snapshot[i].store(g_counters[i].load(std::memory_order_relaxed), std::memory_order_relaxed);
    } else {
      // Accumulators: snapshot and zero for next frame
      g_snapshot[i].store(g_counters[i].exchange(0, std::memory_order_relaxed),
                          std::memory_order_relaxed);
    }
  }
}

int64_t GetSnapshotCounter(CounterId id) {
  return g_snapshot[static_cast<size_t>(id)].load(std::memory_order_relaxed);
}

void Init() {
  for (auto& c : g_counters)
    c.store(0, std::memory_order_relaxed);
  for (auto& s : g_snapshot)
    s.store(0, std::memory_order_relaxed);
}

void SetCsvLogPath(const std::string& path) {
  std::lock_guard<std::mutex> lock(g_csv_mutex);
  if (g_csv_file) {
    std::fflush(g_csv_file);
    std::fclose(g_csv_file);
    g_csv_file = nullptr;
  }
  g_csv_path = path;
  g_csv_frame_count = 0;

  if (path.empty())
    return;

  g_csv_file = std::fopen(path.c_str(), "w");
  if (!g_csv_file) {
    REXLOG_WARN("perf: failed to open CSV log: {}", path);
    g_csv_path.clear();
    return;
  }

  // Write header
  for (size_t i = 0; i < kNumCounters; ++i) {
    if (i > 0)
      std::fputc(',', g_csv_file);
    std::fputs(kCounterNames[i], g_csv_file);
  }
  std::fputc('\n', g_csv_file);
}

void WriteCsvFrame() {
  std::lock_guard<std::mutex> lock(g_csv_mutex);
  if (!g_csv_file)
    return;

  for (size_t i = 0; i < kNumCounters; ++i) {
    if (i > 0)
      std::fputc(',', g_csv_file);
    std::fprintf(g_csv_file, "%lld",
                 static_cast<long long>(g_snapshot[i].load(std::memory_order_relaxed)));
  }
  std::fputc('\n', g_csv_file);

  if (++g_csv_frame_count % 60 == 0) {
    std::fflush(g_csv_file);
  }
}

void FlushCsv() {
  std::lock_guard<std::mutex> lock(g_csv_mutex);
  if (g_csv_file) {
    std::fflush(g_csv_file);
    std::fclose(g_csv_file);
    g_csv_file = nullptr;
  }
  g_csv_path.clear();
}

}  // namespace rex::perf
