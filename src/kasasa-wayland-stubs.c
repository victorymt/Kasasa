/* Minimal stubs for optional Wayland interfaces referenced by generated
 * hyprland-toplevel-export protocol code but unused by Kasasa. */

#include <wayland-client-core.h>

/* Referenced by capture_toplevel_with_wlr_toplevel_handle; we only use
 * capture_toplevel(handle) with hyprctl addresses. */
const struct wl_interface zwlr_foreign_toplevel_handle_v1_interface = {
  .name = "zwlr_foreign_toplevel_handle_v1",
  .version = 1,
  .method_count = 0,
  .methods = NULL,
  .event_count = 0,
  .events = NULL,
};
