/**
 ******************************************************************************
 * ReXGlue runtime - examples                                                 *
 ******************************************************************************
 *
 * @author      Dual-screen framework support, 2026
 */

#include "secondary_ui_demo.h"

#include <imgui.h>

#include <rex/logging.h>
#include <rex/ui/secondary_ui_surface.h>

namespace rex {
namespace ui {

void SecondaryDemoDialog::OnDraw(ImGuiIO& io) {
  ImGui::SetNextWindowPos(ImVec2(24.0f, 24.0f), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(320.0f, 200.0f), ImGuiCond_FirstUseEver);
  if (ImGui::Begin("Secondary surface", nullptr, ImGuiWindowFlags_NoCollapse)) {
    ImGui::TextUnformatted("Hello from the second display!");
    ImGui::Text("Display: %.0f x %.0f", io.DisplaySize.x, io.DisplaySize.y);
    ImGui::Text("%.1f FPS", double(io.Framerate));
    ImGui::Separator();
    ImGui::TextWrapped(
        "This ImGui window is composited by an independent "
        "Presenter / Surface / swapchain via rex::ui::SecondaryUiSurface, "
        "with its own ImGuiContext. The primary game surface is untouched.");
  }
  ImGui::End();
}

#if defined(REX_PLATFORM_GNU_LINUX) && REX_PLATFORM_GNU_LINUX

}  // namespace ui
}  // namespace rex

#include <xcb/xcb.h>

#include <chrono>
#include <cstdlib>
#include <thread>

namespace rex {
namespace ui {

bool RunSecondaryUiDemoXcb(WindowedAppContext& app_context, GraphicsProvider& provider,
                           unsigned int width, unsigned int height) {
  // 1. Create a real second OS window via xcb.
  int screen_number = 0;
  xcb_connection_t* connection = xcb_connect(nullptr, &screen_number);
  if (!connection || xcb_connection_has_error(connection)) {
    REXLOG_ERROR("secondary_ui_demo: failed to connect to the X server");
    return false;
  }

  const xcb_setup_t* setup = xcb_get_setup(connection);
  xcb_screen_iterator_t screen_iter = xcb_setup_roots_iterator(setup);
  for (int i = 0; i < screen_number; ++i) {
    xcb_screen_next(&screen_iter);
  }
  xcb_screen_t* screen = screen_iter.data;

  xcb_window_t window = xcb_generate_id(connection);
  const uint32_t value_mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
  const uint32_t value_list[] = {screen->black_pixel,
                                 XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_STRUCTURE_NOTIFY};
  xcb_create_window(connection, XCB_COPY_FROM_PARENT, window, screen->root, 0, 0,
                    uint16_t(width), uint16_t(height), 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                    screen->root_visual, value_mask, value_list);

  // Listen for the window-manager close button.
  xcb_intern_atom_cookie_t proto_cookie = xcb_intern_atom(connection, 1, 12, "WM_PROTOCOLS");
  xcb_intern_atom_reply_t* proto_reply = xcb_intern_atom_reply(connection, proto_cookie, nullptr);
  xcb_intern_atom_cookie_t del_cookie = xcb_intern_atom(connection, 0, 16, "WM_DELETE_WINDOW");
  xcb_intern_atom_reply_t* del_reply = xcb_intern_atom_reply(connection, del_cookie, nullptr);
  if (proto_reply && del_reply) {
    xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window, proto_reply->atom, 4, 32, 1,
                        &del_reply->atom);
  }

  xcb_map_window(connection, window);
  xcb_flush(connection);

  // 2. Build the secondary presenter chain from the supplied native handle.
  auto secondary = SecondaryUiSurface::Create(
      app_context, provider, ExternalWindowHandle::FromXcbWindow(connection, window), width,
      height, /*imgui_z_order=*/0, /*font_setup=*/nullptr, "Secondary (demo)");
  if (!secondary) {
    REXLOG_ERROR("secondary_ui_demo: SecondaryUiSurface::Create failed");
    if (del_reply) free(del_reply);
    if (proto_reply) free(proto_reply);
    xcb_destroy_window(connection, window);
    xcb_disconnect(connection);
    return false;
  }

  // 3. Attach the trivial demo dialog (registers itself with the drawer).
  new SecondaryDemoDialog(secondary->imgui_drawer());

  // 4. Paint loop: pump xcb events (resize / close), then present a frame.
  bool running = true;
  while (running) {
    xcb_generic_event_t* event;
    while ((event = xcb_poll_for_event(connection))) {
      switch (event->response_type & ~0x80) {
        case XCB_CONFIGURE_NOTIFY: {
          auto* cfg = reinterpret_cast<xcb_configure_notify_event_t*>(event);
          if (cfg->width && cfg->height) {
            secondary->NotifyResize(cfg->width, cfg->height);
          }
          break;
        }
        case XCB_CLIENT_MESSAGE: {
          auto* cm = reinterpret_cast<xcb_client_message_event_t*>(event);
          if (del_reply && cm->data.data32[0] == del_reply->atom) {
            running = false;
          }
          break;
        }
        default:
          break;
      }
      free(event);
    }

    secondary->Paint();
    // ~60 Hz; the real app would drive Paint() from its existing frame loop.
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
  }

  // 5. Tear down (SecondaryUiSurface detaches the presenter and destroys the
  // swapchain in its destructor before we drop the xcb window).
  secondary.reset();
  if (del_reply) free(del_reply);
  if (proto_reply) free(proto_reply);
  xcb_destroy_window(connection, window);
  xcb_disconnect(connection);
  return true;
}

#endif  // REX_PLATFORM_GNU_LINUX

}  // namespace ui
}  // namespace rex
