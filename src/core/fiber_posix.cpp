/**
 * @file        rex/core/fiber_posix.cpp
 * @brief       POSIX backend for rex::thread::Fiber (makecontext/swapcontext)
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/platform.h>
#if REX_PLATFORM_LINUX || REX_PLATFORM_MAC

#include <rex/thread/fiber.h>

#include <cassert>
#include <cstdlib>
#include <ucontext.h>

namespace rex::thread {

thread_local Fiber* Fiber::tls_current_ = nullptr;

#if REX_PLATFORM_ANDROID
// bionic exposes the ucontext_t *type* but not getcontext/makecontext/
// swapcontext/setcontext. Implement fibers with a custom AArch64 context switch
// (fiber_android_arm64.S) - same semantics as the ucontext path: a suspended
// fiber is a saved callee-saved register frame on its own heap stack, and
// Destroy just frees that stack.
#include <cstring>

extern "C" void rex_fiber_swap(void** from_sp_out, void* to_sp);

namespace {
// Bytes the .S frame occupies (10 register pairs). x30 (lr) sits at offset 88,
// i.e. uint64_t index 11.
constexpr size_t kFiberFrameBytes = 160;
constexpr size_t kFiberLrIndex = 11;
constexpr size_t kFiberMinStack = 64 * 1024;
}  // namespace

Fiber* Fiber::ConvertCurrentThread() {
  auto* f = new Fiber();
  f->is_thread_fiber_ = true;
  // sp_ is written by rex_fiber_swap on the first SwitchTo away from this fiber.
  tls_current_ = f;
  return f;
}

Fiber* Fiber::Create(size_t stack_size, void (*entry)(void*), void* arg) {
  auto* f = new Fiber();
  f->entry_ = entry;
  f->arg_ = arg;
  if (stack_size < kFiberMinStack) {
    stack_size = kFiberMinStack;
  }
  f->stack_.resize(stack_size);

  // Build an initial frame at the (16-byte aligned) top of the stack so the
  // first rex_fiber_swap restores lr = Trampoline and returns onto a clean stack.
  uintptr_t top = reinterpret_cast<uintptr_t>(f->stack_.data()) + f->stack_.size();
  top &= ~static_cast<uintptr_t>(15);
  uintptr_t frame_sp = top - kFiberFrameBytes;
  auto* frame = reinterpret_cast<uint64_t*>(frame_sp);
  std::memset(frame, 0, kFiberFrameBytes);
  frame[kFiberLrIndex] = reinterpret_cast<uint64_t>(&Fiber::Trampoline);
  f->sp_ = reinterpret_cast<void*>(frame_sp);
  return f;
}

/*static*/ void Fiber::Trampoline() {
  // tls_current_ was set to this fiber by SwitchTo before the swap landed here.
  Fiber* f = tls_current_;
  f->entry_(f->arg_);
  // Per the XDK contract a fiber entry should not return (the guest's
  // FiberEntryPoint switches back first). There is no valid continuation if it
  // does, so fail loudly rather than execute garbage.
  std::abort();
}

void Fiber::SwitchTo(Fiber* target) {
  Fiber* from = tls_current_;
  tls_current_ = target;
  rex_fiber_swap(&from->sp_, target->sp_);
}

void Fiber::Destroy() {
  if (is_thread_fiber_) {
    tls_current_ = nullptr;
  }
  delete this;  // frees stack_ via the vector destructor
}
#else  // !REX_PLATFORM_ANDROID

Fiber* Fiber::ConvertCurrentThread() {
  auto* f = new Fiber();
  if (getcontext(&f->context_) == -1) {
    delete f;
    return nullptr;
  }
  f->is_thread_fiber_ = true;
  tls_current_ = f;
  return f;
}

Fiber* Fiber::Create(size_t stack_size, void (*entry)(void*), void* arg) {
  auto* f = new Fiber();
  f->entry_ = entry;
  f->arg_ = arg;
  f->stack_.resize(stack_size);

  if (getcontext(&f->context_) == -1) {
    delete f;
    return nullptr;
  }
  f->context_.uc_stack.ss_sp = f->stack_.data();
  f->context_.uc_stack.ss_size = f->stack_.size();
  f->context_.uc_link = nullptr;
  // Trampoline reads entry_/arg_ from tls_current_ — no pointer splitting needed.
  makecontext(&f->context_, &Fiber::Trampoline, 0);
  return f;
}

/*static*/ void Fiber::Trampoline() {
  // tls_current_ was updated by SwitchTo before swapcontext returned here.
  Fiber* f = tls_current_;
  f->entry_(f->arg_);
}

void Fiber::SwitchTo(Fiber* target) {
  Fiber* from = tls_current_;
  tls_current_ = target;
  swapcontext(&from->context_, &target->context_);
}

void Fiber::Destroy() {
  // Thread fibers are destroyed from the owning thread itself.
  if (is_thread_fiber_) {
    tls_current_ = nullptr;
  } else {
    assert(this != tls_current_ && "Destroy called on the currently running fiber");
  }
  // No POSIX equivalent of ConvertFiberToThread; stack_ is freed by the vector destructor.
  delete this;
}
#endif  // REX_PLATFORM_ANDROID

}  // namespace rex::thread

#endif  // REX_PLATFORM_LINUX || REX_PLATFORM_MAC
