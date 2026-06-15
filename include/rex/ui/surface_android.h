#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 * @modified    Android backend, 2026
 */

#include <rex/ui/surface.h>

struct ANativeWindow;

namespace rex {
namespace ui {

// Presentation surface backed by an Android ANativeWindow. Mirrors
// XcbWindowSurface (GNU/Linux) and Win32HwndSurface (Windows). The Vulkan
// presenter consumes window() to build a VK_KHR_android_surface (see
// vulkan_presenter.cpp, Surface::kTypeIndex_AndroidNativeWindow).
class AndroidNativeWindowSurface final : public Surface {
 public:
  explicit AndroidNativeWindowSurface(ANativeWindow* window) : window_(window) {}
  TypeIndex GetType() const override { return kTypeIndex_AndroidNativeWindow; }
  ANativeWindow* window() const { return window_; }

 protected:
  bool GetSizeImpl(uint32_t& width_out, uint32_t& height_out) const override;

 private:
  ANativeWindow* window_;
};

}  // namespace ui
}  // namespace rex
