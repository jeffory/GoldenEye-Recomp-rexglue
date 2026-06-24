/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2015 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <thread>

#include <rex/cvar.h>
#include <rex/thread.h>

// big.LITTLE hot-thread pinning escape hatch. Defined here (always compiled) so
// the cvar is registered on every platform; the actual pinning lives in the
// platform threading backend (real on Android, no-op elsewhere) and reads this
// value via REXCVAR_DECLARE. Default on: the pin is a frame-throughput win and
// is self-gated by topology detection, but perturbing thread scheduling at boot
// can in principle shift the guest startup race (see docs/boot-startup-race.md),
// so it is kept as a one-flag A/B toggle and kill switch.
REXCVAR_DEFINE_BOOL(android_pin_hot_threads, true, "CPU",
                    "Android arm64 big.LITTLE: pin the hot guest threads (main game thread + "
                    "GPU command worker) to the big cluster with a small priority bump. No-op "
                    "off Android or when the host is not big.LITTLE / has too few cores.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace rex::thread {

// =============================================================================
// Common code
// =============================================================================

uint32_t logical_processor_count() {
  static uint32_t value = 0;
  if (!value) {
    value = std::thread::hardware_concurrency();
  }
  return value;
}

thread_local uint32_t current_thread_id_ = UINT_MAX;

uint32_t current_thread_id() {
  return current_thread_id_ == UINT_MAX ? current_thread_system_id() : current_thread_id_;
}

void set_current_thread_id(uint32_t id) {
  current_thread_id_ = id;
}

}  // namespace rex::thread
