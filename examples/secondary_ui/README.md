# Secondary render surface (dual-screen foundation)

This example proves `rex::ui::SecondaryUiSurface` — the framework support for
presenting an **ImGui-only UI to a second display**, independent of the primary
game surface. It is the enabling foundation for the on-screen weapon menu on the
AYN Thor's 3.92" secondary touch panel (1080×1240 @ 60Hz).

## The framework API

Two new always-built files in `rex::ui` provide the abstraction:

| Header | Purpose |
| --- | --- |
| `rex/ui/external_window.h` | `ExternalWindow` — a `rex::ui::Window` that **adopts** an externally-supplied native handle (no native window creation, no event loop; paint driven manually). |
| `rex/ui/secondary_ui_surface.h` | `SecondaryUiSurface` — bundles `ExternalWindow` + a dedicated `Presenter` + `Surface` + swapchain + `ImmediateDrawer` + `ImGuiDrawer` (its own `ImGuiContext`). |

### Why this shape

The framework's `Presenter` and `ImGuiDrawer` are tightly coupled to
`rex::ui::Window` (the presenter calls `window_->RequestPaint()` and asserts a
non-null window whenever a surface is attached; the ImGui drawer pulls its
display size/DPI and input from the window). Rather than decouple them — which
would risk regressing the primary single-surface path — `ExternalWindow` reuses
the entire common `Window` base while skipping only the two things a second
display doesn't have: native window creation and a platform event loop. The
result needs **zero changes** to `Presenter`, `Surface`, `ImGuiDrawer`, or the
primary path.

`ExternalWindowHandle` is platform-agnostic (opaque pointers + a `Surface::
TypeIndex` tag); only the surface type matching the build platform is creatable
today, but the descriptor and `CreateSurfaceImpl` switch are shaped so Win32/Xcb
can be produced on desktop and `ANativeWindow*` on Android.

### Usage

```cpp
#include <rex/ui/secondary_ui_surface.h>

// app_context + provider are the SAME ones used for the primary surface.
auto handle = rex::ui::ExternalWindowHandle::FromAndroidNativeWindow(a_native_window);
//   ...or FromXcbWindow(conn, win) / FromWin32Hwnd(hinstance, hwnd) on desktop.

auto secondary = rex::ui::SecondaryUiSurface::Create(
    app_context, provider, handle, /*width=*/1080, /*height=*/1240);

// Attach dialogs (each ImGuiDialog registers itself with the drawer):
new MyWeaponMenuDialog(secondary->imgui_drawer());

// Every UI-thread frame:
secondary->Paint();

// On external surface resize:
secondary->NotifyResize(new_w, new_h);

// Touch input from the secondary display (AYN Thor panel is touch):
rex::ui::TouchEvent e(secondary->window(), pointer_id, action, x, y);
secondary->window()->InjectTouchEvent(e);

// Destroying the SecondaryUiSurface detaches the presenter and destroys the
// swapchain/VkSurfaceKHR in the correct order before the native handle is gone.
```

### Threading / no-deadlock

Every method must be called from the **UI thread**, like all
`Presenter`/`Surface`/`Window` methods. Each `SecondaryUiSurface` has its own
`Presenter` (own paint-mode mutex + mailbox) and its own `ImGuiContext`, so it
never contends with the primary present as long as both are driven from the same
UI thread. The surface composites **only ImGui draw lists** — there is no guest
framebuffer/mailbox traffic on it. Multiple `VkSurfaceKHR`/swapchains on one
Vulkan device are supported; the secondary presenter is created from the same
`GraphicsProvider` so device/queue resources are shared safely.

## The desktop harness (this example)

`RunSecondaryUiDemoXcb()` opens a real second X11/xcb window (default
1080×1240), attaches a `SecondaryUiSurface` presenting a trivial `SecondaryDemo
Dialog`, and runs a paint loop until the window is closed — proving the
abstraction with no AYN hardware.

Build it (it is **off** by default):

```sh
cmake -S . -B build -DREXGLUE_BUILD_SECONDARY_UI_DEMO=ON   # plus your usual presets
```

This produces a static lib `rexui_secondary_demo`. Call `rex::ui::RunSecondary
UiDemoXcb(app_context, provider)` from a host that already has an initialized
`GraphicsProvider` (e.g. just after the primary window is up). The harness reuses
that provider rather than bootstrapping a second Vulkan device.
