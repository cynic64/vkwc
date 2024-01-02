#ifndef vkwc_h_INCLUDED
#define vkwc_h_INCLUDED

#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_matrix.h>
#include <wlr/render/interface.h>
#include <wlr/util/log.h>
#include <wlr/util/region.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/xwayland.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_subcompositor.h>

enum CursorMode	{
	VKWC_CURSOR_PASSTHROUGH,
	VKWC_CURSOR_XY_ROTATE,
	VKWC_CURSOR_Z_ROTATE,
	VKWC_CURSOR_X_ROTATE_SPEED,
	VKWC_CURSOR_Y_ROTATE_SPEED,
	VKWC_CURSOR_Z_ROTATE_SPEED,
	VKWC_CURSOR_X_MOVE,
	VKWC_CURSOR_Y_MOVE,
	VKWC_CURSOR_Z_MOVE,
};

struct Server {
	struct wl_display *wl_display;
	struct wlr_backend *backend;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;

	struct wlr_xdg_shell *xdg_shell;
	struct wl_listener new_xdg_surface;
	struct wl_listener handle_new_subsurface;
	struct wl_listener handle_subsurface_map;

	struct wlr_cursor *cursor;
	struct wlr_xcursor_manager *cursor_mgr;
	struct wl_listener cursor_motion;
	struct wl_listener cursor_motion_absolute;
	struct wl_listener cursor_button;
	struct wl_listener cursor_axis;
	struct wl_listener cursor_frame;

        struct wl_listener xdg_new_toplevel_decoration;

	struct wlr_seat	*seat;
	struct wl_listener new_input;
	struct wl_listener request_cursor;
	struct wl_listener request_set_selection;
	struct wl_list keyboards;
	enum CursorMode	cursor_mode;
	double grab_x, grab_y;
	struct wlr_box grab_geobox;
	uint32_t resize_edges;
	struct Surface *grabbed_surface;

	struct wlr_output *output;
	struct wlr_output_layout *output_layout;	// Even though we only support one output,
                                                        // the screencopy API requires this
	struct wl_listener output_frame;
	struct wl_listener new_output;

	struct wl_listener new_xwayland_surface;

	struct wl_list windows;

	struct wl_listener new_surface;
	struct wl_list surfaces;
};

#endif // vkwc_h_INCLUDED
