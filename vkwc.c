#define	_POSIX_C_SOURCE	200112L
#include <assert.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <xkbcommon/xkbcommon.h>
#include <pixman-1/pixman.h>
#include <vulkan/vulkan.h>
#include <math.h>
#include <drm_fourcc.h>

#include "wlroots/include/wlr/backend.h"
#include "wlroots/include/wlr/render/allocator.h"
#include "wlroots/include/wlr/render/wlr_renderer.h"
#include "wlroots/include/wlr/types/wlr_cursor.h"
#include "wlroots/include/wlr/types/wlr_compositor.h"
#include "wlroots/include/wlr/types/wlr_data_device.h"
#include "wlroots/include/wlr/types/wlr_input_device.h"
#include "wlroots/include/wlr/types/wlr_keyboard.h"
#include "wlroots/include/wlr/types/wlr_output.h"
#include "wlroots/include/wlr/types/wlr_output_layout.h"
#include "wlroots/include/wlr/types/wlr_pointer.h"
#include "wlroots/include/wlr/types/wlr_scene.h"
#include "wlroots/include/wlr/types/wlr_seat.h"
#include "wlroots/include/wlr/types/wlr_xcursor_manager.h"
#include "wlroots/include/wlr/types/wlr_xdg_shell.h"
#include "wlroots/include/wlr/types/wlr_presentation_time.h"
#include "wlroots/include/wlr/types/wlr_output_damage.h"
#include "wlroots/include/wlr/types/wlr_matrix.h"
#include "wlroots/include/wlr/render/interface.h"
#include "wlroots/include/wlr/util/log.h"
#include "wlroots/include/wlr/util/region.h"
#include "wlroots/include/wlr/render/vulkan.h"
#include "wlroots/include/wlr/types/wlr_xcursor_manager.h"
#include "wlroots/include/wlr/xwayland.h"
#include "wlroots/include/wlr/types/wlr_screencopy_v1.h"
#include "wlroots/include/wlr/types/wlr_xdg_output_v1.h"

// Stuff I had to clone	wlroots	for (not in include/wlr)
#include "wlroots/include/render/vulkan.h"

#define	M_PI 3.14159265358979323846

// I want to count how many surfaces I render each frame
int rendered_surface_count = 0;

// If we took a	screenshot the instant Alt+F3 was pressed, no render buffer would be bound.
// We have to do it after output commit	instead
bool must_take_screenshot = false;

// To print scene node types as	text
const char* const SCENE_NODE_TYPE_LOOKUP[] = {"ROOT", "TREE", "SURFACE", "RECT", "BUFFER", "INVALID"};

struct VertPcrData {
	float mat4[4][4];
	float uv_off[2];
	float uv_size[2];
};

/* For brevity's sake, struct members are annotated where they are used. */
enum CursorMode	{
	VKWC_CURSOR_PASSTHROUGH,
	VKWC_CURSOR_MOVE,
	VKWC_CURSOR_RESIZE,
};

enum ViewType {
	XDG_SHELL_VIEW,
	XWAYLAND_VIEW,
};

struct Server {
	struct wl_display *wl_display;
	struct wlr_backend *backend;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	struct wlr_scene *scene;

	struct wlr_xdg_shell *xdg_shell;
	struct wl_listener new_xdg_surface;
	struct wl_list views;

	struct wlr_cursor *cursor;
	struct wlr_xcursor_manager *cursor_mgr;
	struct wl_listener cursor_motion;
	struct wl_listener cursor_motion_absolute;
	struct wl_listener cursor_button;
	struct wl_listener cursor_axis;
	struct wl_listener cursor_frame;

	struct wlr_seat	*seat;
	struct wl_listener new_input;
	struct wl_listener request_cursor;
	struct wl_listener request_set_selection;
	struct wl_list keyboards;
	enum CursorMode	cursor_mode;
	struct View *grabbed_view;
	double grab_x, grab_y;
	struct wlr_box grab_geobox;
	uint32_t resize_edges;

	struct wlr_output_layout *output_layout;
	struct wl_list outputs;
	struct wl_listener new_output;

	struct wl_listener new_xwayland_surface;
};

struct Output {
	struct wl_list link;
	struct Server *server;
	struct wlr_output *wlr_output;
	struct wl_listener frame;
};

struct View {
	struct wl_list link;
	struct Server *server;
	struct wlr_xdg_surface *xdg_surface;
	struct wlr_scene_node *scene_node;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener destroy;
	struct wl_listener request_move;
	struct wl_listener request_resize;
	int x, y;

	enum ViewType type;
};

struct XWaylandView {
	struct View view;
	struct wlr_xwayland_surface *xwayland_surface;
	struct wl_listener destroy;
	struct wl_listener unmap;
	struct wl_listener map;
	struct wl_listener request_fullscreen;
};

struct Keyboard	{
	struct wl_list link;
	struct Server *server;
	struct wlr_input_device	*device;

	struct wl_listener modifiers;
	struct wl_listener key;
};

void write_bmp(FILE *fp, uint32_t width, uint32_t height, const	void *data) {
	// data	should be width	* height * 3 bytes long
	uint32_t byte_count = width * height * 3 + 0x36;

	const char header[] = {0x42, 0x4d,	// "BM"
		// Total file size, little-endian
		byte_count & 0xff, (byte_count & 0xff00) >> 8,
		(byte_count & 0xff0000)	>> 16, (byte_count & 0xff000000) >> 24,

		0x00, 0x00, 0x00, 0x00,		// Optional stuff
		0x36, 0x00, 0x00, 0x00,		// Will	be address of pixel array
		0x28, 0x00, 0x00, 0x00,		// DIB header is 0x28 bytes long
		// Width in pixels, little-endian
		width &	0xff, (width & 0xff00) >> 8, (width & 0xff0000)	>> 16, (width &	0xff000000) >> 24,

		// Height in pixels, little-endian
		height & 0xff, (height & 0xff00) >> 8, (height & 0xff0000) >> 16, (height & 0xff000000)	>> 24,

		0x01, 0x00, 0x18, 0x00,	0x00, 0x00,	// Copied from GIMP, no	idea
		0x00, 0x00, 0x00, 0x03,	0x00, 0x00, 0x23, 0x2e,
		0x00, 0x00, 0x23, 0x2e,	0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00,	0x00, 0x00
	};

	if (fwrite(header, sizeof(header), 1, fp) != 1)	{
		fprintf(stderr,	"Couldn't write	header\n");
		exit(1);
	}

	if (fwrite(data, width * height	* 3, 1,	fp) != 1) {
		fprintf(stderr,	"Couldn't write	pixels\n");
		exit(1);
	}
}

void render_rect_simple(struct wlr_renderer *renderer, const float color[4], int x, int	y, int width, int height) {
	struct wlr_box box = { .x = x, .y = y, .width =	width, .height = height	};
	float identity_matrix[9] = { 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0,	0.0, 1.0 };
	wlr_render_rect(renderer, &box,	color, identity_matrix);;
}

static void focus_view(struct View *view, struct wlr_surface *surface) {
	/* Note: this function only deals with keyboard	focus. */
	if (view == NULL) {
		return;
	}
	struct Server *server =	view->server;
	struct wlr_seat	*seat =	server->seat;
	struct wlr_surface *prev_surface = seat->keyboard_state.focused_surface;
	if (prev_surface == surface) {
		/* Don't re-focus an already focused surface. */
		return;
	}
	if (prev_surface) {
		/*
		 * Deactivate the previously focused surface. This lets	the client know
		 * it no longer	has focus and the client will repaint accordingly, e.g.
		 * stop	displaying a caret.
		 */
		struct wlr_xdg_surface *previous = wlr_xdg_surface_from_wlr_surface(
					seat->keyboard_state.focused_surface);
		wlr_xdg_toplevel_set_activated(previous, false);
	}
	struct wlr_keyboard *keyboard =	wlr_seat_get_keyboard(seat);
	/* Move	the view to the	front */
	wlr_scene_node_raise_to_top(view->scene_node);
	wl_list_remove(&view->link);
	wl_list_insert(&server->views, &view->link);
	/* Activate the	new surface */
	if (view->type == XDG_SHELL_VIEW) {
		wlr_xdg_toplevel_set_activated(view->xdg_surface, true);

		/*
		 * Tell	the seat to have the keyboard enter this surface. wlroots will keep
		 * track of this and automatically send	key events to the appropriate
		 * clients without additional work on your part.
		 */
		wlr_seat_keyboard_notify_enter(seat, view->xdg_surface->surface,
			keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
	}
}

static void handle_keyboard_modifiers(
		struct wl_listener *listener, void *data) {
	/* This	event is raised	when a modifier	key, such as shift or alt, is
	 * pressed. We simply communicate this to the client. */
	struct Keyboard	*keyboard =
		wl_container_of(listener, keyboard, modifiers);
	/*
	 * A seat can only have	one keyboard, but this is a limitation of the
	 * Wayland protocol - not wlroots. We assign all connected keyboards to	the
	 * same	seat. You can swap out the underlying wlr_keyboard like	this and
	 * wlr_seat handles this transparently.
	 */
	wlr_seat_set_keyboard(keyboard->server->seat, keyboard->device);
	/* Send	modifiers to the client. */
	wlr_seat_keyboard_notify_modifiers(keyboard->server->seat,
		&keyboard->device->keyboard->modifiers);
}

static bool handle_keybinding(struct Server *server, xkb_keysym_t sym) {
	/*
	 * Here	we handle compositor keybindings. This is when the compositor is
	 * processing keys, rather than	passing	them on	to the client for its own
	 * processing.
	 *
	 * This	function assumes Alt is	held down.
	 */
	switch (sym) {
	case XKB_KEY_Escape:
		wl_display_terminate(server->wl_display);
		break;
	case XKB_KEY_F1:
		/* Cycle to the	next view */
		if (wl_list_length(&server->views) < 2)	{
			break;
		}
		struct View *next_view = wl_container_of(
			server->views.prev, next_view, link);
		focus_view(next_view, next_view->xdg_surface->surface);
		break;
	case XKB_KEY_F2:
		if (fork() == 0) {
			const char *arg[] = {"foot", NULL };
			setsid();
			execvp(((char **)arg)[0], (char	**)arg);
			fprintf(stderr,	"dwm: execvp %s", ((char **)arg)[0]);
			perror(" failed");
			exit(EXIT_SUCCESS);
		}
		break;
	case XKB_KEY_F3:
	{
		must_take_screenshot = true;
		break;
	}
	case XKB_KEY_F4:
	{

	}
	default:
		return false;
	}
	return true;
}

static void handle_keyboard_key(struct wl_listener *listener, void *data) {
	/* This	event is raised	when a key is pressed or released. */
	struct Keyboard	*keyboard =
		wl_container_of(listener, keyboard, key);
	struct Server *server =	keyboard->server;
	struct wlr_event_keyboard_key *event = data;
	struct wlr_seat	*seat =	server->seat;

	/* Translate libinput keycode -> xkbcommon */
	uint32_t keycode = event->keycode + 8;
	/* Get a list of keysyms based on the keymap for this keyboard */
	const xkb_keysym_t *syms;
	int nsyms = xkb_state_key_get_syms(
			keyboard->device->keyboard->xkb_state, keycode,	&syms);

	bool handled = false;
	uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard->device->keyboard);
	if ((modifiers & WLR_MODIFIER_ALT) &&
			event->state ==	WL_KEYBOARD_KEY_STATE_PRESSED) {
		/* If alt is held down and this	button was _pressed_, we attempt to
		 * process it as a compositor keybinding. */
		for (int i = 0;	i < nsyms; i++)	{
			handled	= handle_keybinding(server, syms[i]);
		}
	}

	if (!handled) {
		/* Otherwise, we pass it along to the client. */
		wlr_seat_set_keyboard(seat, keyboard->device);
		wlr_seat_keyboard_notify_key(seat, event->time_msec,
			event->keycode,	event->state);
	}
}

static void server_new_keyboard(struct Server *server, struct wlr_input_device *device)	{
	struct Keyboard	*keyboard =
		calloc(1, sizeof(struct	Keyboard));
	keyboard->server = server;
	keyboard->device = device;

	/* We need to prepare an XKB keymap and	assign it to the keyboard. This
	 * assumes the defaults	(e.g. layout = "us"). */
	struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, NULL,
		XKB_KEYMAP_COMPILE_NO_FLAGS);

	wlr_keyboard_set_keymap(device->keyboard, keymap);
	xkb_keymap_unref(keymap);
	xkb_context_unref(context);
	wlr_keyboard_set_repeat_info(device->keyboard, 25, 600);

	/* Here	we set up listeners for	keyboard events. */
	keyboard->modifiers.notify = handle_keyboard_modifiers;
	wl_signal_add(&device->keyboard->events.modifiers, &keyboard->modifiers);
	keyboard->key.notify = handle_keyboard_key;
	wl_signal_add(&device->keyboard->events.key, &keyboard->key);

	wlr_seat_set_keyboard(server->seat, device);

	/* And add the keyboard	to our list of keyboards */
	wl_list_insert(&server->keyboards, &keyboard->link);
}

static void server_new_pointer(struct Server *server,
		struct wlr_input_device	*device) {
	/* We don't do anything	special	with pointers. All of our pointer handling
	 * is proxied through wlr_cursor. On another compositor, you might take	this
	 * opportunity to do libinput configuration on the device to set
	 * acceleration, etc. */
	wlr_cursor_attach_input_device(server->cursor, device);
}

static void handle_new_input(struct wl_listener	*listener, void	*data) {
	/* This	event is raised	by the backend when a new input	device becomes
	 * available. */
	struct Server *server =
		wl_container_of(listener, server, new_input);
	struct wlr_input_device	*device	= data;
	switch (device->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		server_new_keyboard(server, device);
		break;
	case WLR_INPUT_DEVICE_POINTER:
		server_new_pointer(server, device);
		break;
	default:
		break;
	}
	/* We need to let the wlr_seat know what our capabilities are, which is
	 * communiciated to the	client.	In TinyWL we always have a cursor, even	if
	 * there are no	pointer	devices, so we always include that capability. */
	uint32_t caps =	WL_SEAT_CAPABILITY_POINTER;
	if (!wl_list_empty(&server->keyboards))	{
		caps |=	WL_SEAT_CAPABILITY_KEYBOARD;
	}
	wlr_seat_set_capabilities(server->seat,	caps);
}

static void handle_new_cursor_image(struct wl_listener *listener, void *data) {
	struct Server *server =	wl_container_of(
			listener, server, request_cursor);
	/* This	event is raised	by the seat when a client provides a cursor image */
	struct wlr_seat_pointer_request_set_cursor_event *event	= data;
	struct wlr_seat_client *focused_client =
		server->seat->pointer_state.focused_client;
	/* This	can be sent by any client, so we check to make sure this one is
	 * actually has	pointer	focus first. */
	if (focused_client == event->seat_client) {
		/* Once	we've vetted the client, we can	tell the cursor	to use the
		 * provided surface as the cursor image. It will set the hardware cursor
		 * on the output that it's currently on	and continue to	do so as the
		 * cursor moves	between	outputs. */
		wlr_cursor_set_surface(server->cursor, event->surface,
				event->hotspot_x, event->hotspot_y);
	}
}

static void handle_selection_request(struct wl_listener	*listener, void	*data) {
	/* This	event is raised	by the seat when a client wants	to set the selection,
	 * usually when	the user copies	something. wlroots allows compositors to
	 * ignore such requests	if they	so choose, but in vkwc we always honor
	 */
	struct Server *server =	wl_container_of(
			listener, server, request_set_selection);
	struct wlr_seat_request_set_selection_event *event = data;
	wlr_seat_set_selection(server->seat, event->source, event->serial);
}

static struct View *desktop_view_at(
		struct Server *server, double lx, double ly,
		struct wlr_surface **surface, double *sx, double *sy) {
	/* This	returns	the topmost node in the	scene at the given layout coords.
	 * we only care	about surface nodes as we are specifically looking for a
	 * surface in the surface tree of a View. */
	struct wlr_scene_node *node = wlr_scene_node_at(
		&server->scene->node, lx, ly, sx, sy);
	if (node == NULL || node->type != WLR_SCENE_NODE_SURFACE) {
		return NULL;
	}
	*surface = wlr_scene_surface_from_node(node)->surface;
	/* Find	the node corresponding to the View at the root of this
	 * surface tree, it is the only	one for	which we set the data field. */
	while (node != NULL && node->data == NULL) {
		node = node->parent;
	}

	void *data = node->data;

	return data;
}

static void process_cursor_move(struct Server *server, uint32_t	time) {
	/* Move	the grabbed view to the	new position. */
	struct View *view = server->grabbed_view;
	view->x	= server->cursor->x - server->grab_x;
	view->y	= server->cursor->y - server->grab_y;
	wlr_scene_node_set_position(view->scene_node, view->x, view->y);
}

static void process_cursor_resize(struct Server	*server, uint32_t time)	{
	/*
	 * Resizing the	grabbed	view can be a little bit complicated, because we
	 * could be resizing from any corner or	edge. This not only resizes the	view
	 * on one or two axes, but can also move the view if you resize	from the top
	 * or left edges (or top-left corner).
	 *
	 * Note	that I took some shortcuts here. In a more fleshed-out compositor,
	 * you'd wait for the client to	prepare	a buffer at the	new size, then
	 * commit any movement that was	prepared.
	 */
	struct View *view = server->grabbed_view;
	double border_x	= server->cursor->x - server->grab_x;
	double border_y	= server->cursor->y - server->grab_y;
	int new_left = server->grab_geobox.x;
	int new_right =	server->grab_geobox.x +	server->grab_geobox.width;
	int new_top = server->grab_geobox.y;
	int new_bottom = server->grab_geobox.y + server->grab_geobox.height;

	if (server->resize_edges & WLR_EDGE_TOP) {
		new_top	= border_y;
		if (new_top >= new_bottom) {
			new_top	= new_bottom - 1;
		}
	} else if (server->resize_edges	& WLR_EDGE_BOTTOM) {
		new_bottom = border_y;
		if (new_bottom <= new_top) {
			new_bottom = new_top + 1;
		}
	}
	if (server->resize_edges & WLR_EDGE_LEFT) {
		new_left = border_x;
		if (new_left >=	new_right) {
			new_left = new_right - 1;
		}
	} else if (server->resize_edges	& WLR_EDGE_RIGHT) {
		new_right = border_x;
		if (new_right <= new_left) {
			new_right = new_left + 1;
		}
	}

	struct wlr_box geo_box;
	wlr_xdg_surface_get_geometry(view->xdg_surface,	&geo_box);
	view->x	= new_left - geo_box.x;
	view->y	= new_top - geo_box.y;
	wlr_scene_node_set_position(view->scene_node, view->x, view->y);

	int new_width =	new_right - new_left;
	int new_height = new_bottom - new_top;
	wlr_xdg_toplevel_set_size(view->xdg_surface, new_width,	new_height);
}

static void process_cursor_motion(struct Server	*server, uint32_t time)	{
	/* If the mode is non-passthrough, delegate to those functions.	*/
	if (server->cursor_mode	== VKWC_CURSOR_MOVE) {
		process_cursor_move(server, time);
		return;
	} else if (server->cursor_mode == VKWC_CURSOR_RESIZE) {
		process_cursor_resize(server, time);
		return;
	}

	/* Otherwise, find the view under the pointer and send the event along.	*/
	double sx, sy;
	struct wlr_seat	*seat =	server->seat;
	struct wlr_surface *surface = NULL;
	struct View *view = desktop_view_at(server,
			server->cursor->x, server->cursor->y, &surface,	&sx, &sy);

	if (!view) {
		/* If there's no view under the	cursor,	set the	cursor image to	a
		 * default. This is what makes the cursor image	appear when you	move it
		 * around the screen, not over any views. */
		wlr_xcursor_manager_set_cursor_image(
				server->cursor_mgr, "left_ptr",	server->cursor);
	}
	if (surface) {
		/*
		 * Send	pointer	enter and motion events.
		 *
		 * The enter event gives the surface "pointer focus", which is distinct
		 * from	keyboard focus.	You get	pointer	focus by moving	the pointer over
		 * a window.
		 *
		 * Note	that wlroots will avoid	sending	duplicate enter/motion events if
		 * the surface has already has pointer focus or	if the client is already
		 * aware of the	coordinates passed.
		 */
		wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
		wlr_seat_pointer_notify_motion(seat, time, sx, sy);
	} else {
		/* Clear pointer focus so future button	events and such	are not	sent to
		 * the last client to have the cursor over it. */
		wlr_seat_pointer_clear_focus(seat);
	}
}

static void handle_cursor_motion_relative(struct wl_listener *listener,	void *data) {
	/* This	event is forwarded by the cursor when a	pointer	emits a	_relative_
	 * pointer motion event	(i.e. a	delta) */
	struct Server *server =
		wl_container_of(listener, server, cursor_motion);
	struct wlr_event_pointer_motion	*event = data;
	/* The cursor doesn't move unless we tell it to. The cursor automatically
	 * handles constraining	the motion to the output layout, as well as any
	 * special configuration applied for the specific input	device which
	 * generated the event.	You can	pass NULL for the device if you	want to	move
	 * the cursor around without any input.	*/
	wlr_cursor_move(server->cursor,	event->device,
			event->delta_x,	event->delta_y);
	process_cursor_motion(server, event->time_msec);
}

static void handle_cursor_motion_absolute(
		struct wl_listener *listener, void *data) {
	/* This	event is forwarded by the cursor when a	pointer	emits an _absolute_
	 * motion event, from 0..1 on each axis. This happens, for example, when
	 * wlroots is running under a Wayland window rather than KMS+DRM, and you
	 * move	the mouse over the window. You could enter the window from any edge,
	 * so we have to warp the mouse	there. There is	also some hardware which
	 * emits these events. */
	struct Server *server =
		wl_container_of(listener, server, cursor_motion_absolute);
	struct wlr_event_pointer_motion_absolute *event	= data;
	wlr_cursor_warp_absolute(server->cursor, event->device,	event->x, event->y);
	process_cursor_motion(server, event->time_msec);
}

static void handle_cursor_button(struct	wl_listener *listener, void *data) {
	/* This	event is forwarded by the cursor when a	pointer	emits a	button
	 * event. */
	struct Server *server =
		wl_container_of(listener, server, cursor_button);
	struct wlr_event_pointer_button	*event = data;
	/* Notify the client with pointer focus	that a button press has	occurred */
	wlr_seat_pointer_notify_button(server->seat,
			event->time_msec, event->button, event->state);
	double sx, sy;
	struct wlr_surface *surface = NULL;
	struct View *view = desktop_view_at(server,
			server->cursor->x, server->cursor->y, &surface,	&sx, &sy);
	if (event->state == WLR_BUTTON_RELEASED) {
		/* If you released any buttons,	we exit	interactive move/resize	mode. */
		server->cursor_mode = VKWC_CURSOR_PASSTHROUGH;
	} else {
		/* Focus that client if	the button was _pressed_ */
		focus_view(view, surface);
	}
}

static void handle_cursor_axis(struct wl_listener *listener, void *data) {
	/* This	event is forwarded by the cursor when a	pointer	emits an axis event,
	 * for example when you	move the scroll	wheel. */
	struct Server *server =
		wl_container_of(listener, server, cursor_axis);
	struct wlr_event_pointer_axis *event = data;
	/* Notify the client with pointer focus	of the axis event. */
	wlr_seat_pointer_notify_axis(server->seat,
			event->time_msec, event->orientation, event->delta,
			event->delta_discrete, event->source);
}

static void handle_cursor_frame(struct wl_listener *listener, void *data) {
	/* This	event is forwarded by the cursor when a	pointer	emits a	frame
	 * event. Frame	events are sent	after regular pointer events to	group
	 * multiple events together. For instance, two axis events may happen at the
	 * same	time, in which case a frame event won't	be sent	in between. */
	struct Server *server =
		wl_container_of(listener, server, cursor_frame);
	/* Notify the client with pointer focus	of the frame event. */
	wlr_seat_pointer_notify_frame(server->seat);
}

// Begin my stuff
static void mat3_to_mat4(const float mat3[9], float mat4[4][4])	{
	memset(mat4, 0,	sizeof(float) *	16);
	mat4[0][0] = mat3[0];
	mat4[0][1] = mat3[1];
	mat4[0][3] = mat3[2];

	mat4[1][0] = mat3[3];
	mat4[1][1] = mat3[4];
	mat4[1][3] = mat3[5];

	mat4[2][2] = 1.f;
	mat4[3][3] = 1.f;
}

void node_iterator(struct wlr_surface *node, int sx, int sy, void *_data) {
	printf("\t[node_iterator] Surface at %d, %d\n",	sx, sy);
	struct wlr_surface_state state = node->current;
	printf("\t[node_iterator] width: %d, height: %d, role: %s\n", state.width, state.height, node->role->name);
}

struct check_scanout_data {
	// in
	struct wlr_box viewport_box;
	// out
	struct wlr_scene_node *node;
	size_t n;
};

struct render_data {
	struct wlr_output *output;
	pixman_region32_t *damage;

	// May be NULL
	struct wlr_presentation	*presentation;
};

static void scene_node_for_each_node(struct wlr_scene_node *node,
		int lx,	int ly,	wlr_scene_node_iterator_func_t user_iterator,
		void *user_data) {
	if (!node->state.enabled) {
		return;
	}

	lx += node->state.x;
	ly += node->state.y;

	user_iterator(node, lx,	ly, user_data);

	struct wlr_scene_node *child;
	wl_list_for_each(child,	&node->state.children, state.link) {
		scene_node_for_each_node(child,	lx, ly,	user_iterator, user_data);
	}
}

void get_node_placement(struct wlr_scene_node *node, int *x, int *y, int *width, int *height) {
	/* Returns the absolute	position of a node in pixels. Necessary	because	each node only knows
	 * its position	relative to its	parent.	*/

	// Position
	if (node->parent == NULL) {
		*x = 0;
		*y = 0;
		if (width != NULL && height != NULL) {
			*width = 0;
			*height	= 0;
		}
		return;
	}

	int parent_x = 0, parent_y = 0;
	get_node_placement(node->parent, &parent_x, &parent_y, NULL, NULL);

	*x = parent_x +	node->state.x;
	*y = parent_y +	node->state.y;

	// Dimensions
	if (width != NULL && height != NULL) {
		if (node->type == WLR_SCENE_NODE_SURFACE) {
			struct wlr_scene_surface *scene_surface	= wlr_scene_surface_from_node(node);
			*width = scene_surface->surface->current.width;
			*height	= scene_surface->surface->current.height;
		} else {
			*width = 0;
			*height	= 0;
		}
	}
}

struct wlr_scene_node *get_main_node(struct wlr_scene_node *node) {
	/* If we have a	node like one of the decoration	surfaces, this will (hopefully)	return the
	 * main	surface	<node> is attached to.
	 * 
	 * We do this by:
	 *	Taking the node's parent until we hit the root node, the one before this is our	"branch"
	 *	Take the first child of	the branch until we hit	a surface
	 * That	should be the main window.
	 */

	struct wlr_scene_node *cur = node;

	// Go up until we hit the root
	assert(cur->parent != NULL);
	while (1) {
		cur = cur->parent;
		assert(cur->parent != NULL);

		if (cur->parent->type == WLR_SCENE_NODE_ROOT) break;
	}

	// Go down until we hit	a surface
	while (cur->type == WLR_SCENE_NODE_TREE) {
		cur = wl_container_of(cur->state.children.next,	cur, state.link);
		assert(cur != NULL);	// Couldn't find a surface
	}

	return cur;
}

void get_node_center(struct wlr_scene_node *node, int *x, int *y) {
	/* Returns the coordinates a node's rotation should be centered	around.	Only to	be used	on surface
	 * nodes.
	 *
	 * For an application surface this is just the coordinates of the surface's center. For	decoration,
	 * however, it will return the center of the main window so everything rotates together.
	 */

	assert(node->type == WLR_SCENE_NODE_SURFACE);		// We can only handle surface nodes

	int main_x, main_y, main_width,	main_height;
	struct wlr_scene_node *main_node = get_main_node(node);
	get_node_placement(main_node, &main_x, &main_y,	&main_width, &main_height);

	*x = main_x + main_width / 2;
	*y = main_y + main_height / 2;
}

void print_scene_graph(struct wlr_scene_node *node, int	level) {
	int x, y, width, height;
	get_node_placement(node, &x, &y, &width, &height);

	int center_x = 0, center_y = 0;
	if (node->type == WLR_SCENE_NODE_SURFACE) get_node_center(node,	&center_x, &center_y);

	for (int i = 0;	i < level; i++)	printf("\t");
	printf("Node type: %s, dims: %d	x %d, pos: %d %d, centered on %d %d\n",
		SCENE_NODE_TYPE_LOOKUP[node->type], width, height, x, y, center_x, center_y);

	struct wlr_scene_node *child;
	wl_list_for_each(child,	&node->state.children, state.link) {
		print_scene_graph(child, level + 1);
	}
}

static struct wlr_texture *scene_buffer_get_texture(
		struct wlr_scene_buffer	*scene_buffer, struct wlr_renderer *renderer) {
	struct wlr_client_buffer *client_buffer	=
		wlr_client_buffer_get(scene_buffer->buffer);
	if (client_buffer != NULL) {
		return client_buffer->texture;
	}

	if (scene_buffer->texture != NULL) {
		return scene_buffer->texture;
	}

	scene_buffer->texture =
		wlr_texture_from_buffer(renderer, scene_buffer->buffer);
	return scene_buffer->texture;
}

static void scissor_output(struct wlr_output *output, pixman_box32_t *rect) {
	struct wlr_renderer *renderer =	output->renderer;
	assert(renderer);

	struct wlr_box box = {
		.x = rect->x1,
		.y = rect->y1,
		.width = rect->x2 - rect->x1,
		.height	= rect->y2 - rect->y1,
	};

	wlr_renderer_scissor(renderer, &box);
}

static struct wlr_scene_buffer *scene_buffer_from_node(
		struct wlr_scene_node *node) {
	assert(node->type == WLR_SCENE_NODE_BUFFER);
	return (struct wlr_scene_buffer	*)node;
}

static struct wlr_scene_rect *scene_rect_from_node(
		struct wlr_scene_node *node) {
	assert(node->type == WLR_SCENE_NODE_RECT);
	return (struct wlr_scene_rect *)node;
}

static void scene_node_get_size(struct wlr_scene_node *node,
		int *width, int	*height) {
	*width = 0;
	*height	= 0;

	switch (node->type) {
	case WLR_SCENE_NODE_ROOT:
	case WLR_SCENE_NODE_TREE:
		return;
	case WLR_SCENE_NODE_SURFACE:;
		struct wlr_scene_surface *scene_surface	=
			wlr_scene_surface_from_node(node);
		*width = scene_surface->surface->current.width;
		*height	= scene_surface->surface->current.height;
		break;
	case WLR_SCENE_NODE_RECT:;
		struct wlr_scene_rect *scene_rect = scene_rect_from_node(node);
		*width = scene_rect->width;
		*height	= scene_rect->height;
		break;
	case WLR_SCENE_NODE_BUFFER:;
		struct wlr_scene_buffer	*scene_buffer =	scene_buffer_from_node(node);
		if (scene_buffer->dst_width > 0	&& scene_buffer->dst_height > 0) {
			*width = scene_buffer->dst_width;
			*height	= scene_buffer->dst_height;
		} else {
			if (scene_buffer->transform & WL_OUTPUT_TRANSFORM_90) {
				*height	= scene_buffer->buffer->width;
				*width = scene_buffer->buffer->height;
			} else {
				*width = scene_buffer->buffer->width;
				*height	= scene_buffer->buffer->height;
			}
		}
		break;
	}
}

static void check_scanout_iterator(struct wlr_scene_node *node,
		int x, int y, void *_data) {
	struct check_scanout_data *data	= _data;

	struct wlr_box node_box	= { .x = x, .y = y };
	scene_node_get_size(node, &node_box.width, &node_box.height);

	struct wlr_box intersection;
	if (!wlr_box_intersection(&intersection, &data->viewport_box, &node_box)) {
		return;
	}

	data->n++;

	if (data->viewport_box.x == node_box.x &&
			data->viewport_box.y ==	node_box.y &&
			data->viewport_box.width == node_box.width &&
			data->viewport_box.height == node_box.height) {
		data->node = node;
	}
}

static int scale_length(int length, int	offset,	float scale) {
	return round((offset + length) * scale)	- round(offset * scale);
}

static void scale_box(struct wlr_box *box, float scale)	{
	box->width = scale_length(box->width, box->x, scale);
	box->height = scale_length(box->height,	box->y,	scale);
	box->x = round(box->x *	scale);
	box->y = round(box->y *	scale);
}

static void render_rect(struct wlr_output *output,
		pixman_region32_t *output_damage, const	float color[static 4],
		const struct wlr_box *box, const float matrix[static 9]) {
	struct wlr_renderer *renderer =	output->renderer;
	assert(renderer);

	pixman_region32_t damage;
	pixman_region32_init(&damage);
	pixman_region32_init_rect(&damage, box->x, box->y, box->width, box->height);
	pixman_region32_intersect(&damage, &damage, output_damage);

	int nrects;
	pixman_box32_t *rects =	pixman_region32_rectangles(&damage, &nrects);
	for (int i = 0;	i < nrects; ++i) {
		scissor_output(output, &rects[i]);
		wlr_render_rect(renderer, box, color, matrix);
	}

	pixman_region32_fini(&damage);
}

static bool render_subtexture_with_matrix(struct wlr_renderer *wlr_renderer,
		struct wlr_scene_node *node, struct wlr_texture	*wlr_texture,
		const struct wlr_fbox *box, const float	matrix[static 9], float	alpha) {
	/*
	 * Box only has	the width and height (in pixel coordinates).
	 * box->x and box->y are always	0.
	 * 
	 * The matrix we get converts 0..1 to the pixel	coordinates of the windw.
	 * It looks like [width	0 x    0 height	y    0 0 1]
	 * 
	 * We require <node> to	figure what coordinates	to rotate around.
	 */
	printf("\t\t\t\t[render_subtexture_with_matrix]\n");
	struct wlr_vk_renderer *renderer = (struct wlr_vk_renderer *) wlr_renderer;
	VkCommandBuffer	cb = renderer->cb;

	struct wlr_vk_texture *texture = vulkan_get_texture(wlr_texture);
	assert(texture->renderer == renderer);
	if (texture->dmabuf_imported &&	!texture->owned) {
		// Store this texture in the list of textures that need	to be
		// acquired before rendering and released after	rendering.
		// We don't do it here immediately since barriers inside
		// a renderpass	are suboptimal (would require additional renderpass
		// dependency and potentially multiple barriers) and it's
		// better to issue one barrier for all used textures anyways.
		texture->owned = true;
		assert(texture->foreign_link.prev == NULL);
		assert(texture->foreign_link.next == NULL);
		wl_list_insert(&renderer->foreign_textures, &texture->foreign_link);
	}

	VkPipeline pipe	= renderer->current_render_buffer->render_setup->tex_pipe;
	if (pipe != renderer->bound_pipe) {
		vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
		renderer->bound_pipe = pipe;
	}

	vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
		renderer->pipe_layout, 0, 1, &texture->ds, 0, NULL);

	// Calculate rotation angle
	struct wlr_scene_node *main_node = get_main_node(node);
	float seed = (((long) main_node) % 100)	/ 50.0;

	struct timespec	ts;
	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
	float theta = M_PI * 2 * (ts.tv_nsec / 1000000000.0 + ts.tv_sec) * (seed - 1.0)	* 0.3 +	seed;

	// Rotate the matrix

	float rotation[9] = {cosf(theta), sinf(theta), 0.5,
		-sinf(theta), cosf(theta), 0.5,
		0, 0, 1};

	// If we don't include a translation component,	the rotation is	centered around	(0, 0) and
	// will	most likely put	the window off screen. Instead,	center it around the center of the
	// window.
	int center_x, center_y;
	get_node_center(node, &center_x, &center_y);

	rotation[2] = center_x - center_x * rotation[0]	- center_y * rotation[1];
	rotation[5] = center_y - center_x * rotation[3]	- center_y * rotation[4];

	float rotated_matrix[9];
	wlr_matrix_multiply(rotated_matrix, rotation, matrix);

	// Apply the projection	matrix to the matrix we	were given
	// render->projection takes 0..1920 and	0..1080	and maps them to -1..1
	// So for my resolution	it's always [2/1920 0 -1    0 2/1080 -1	   0 0 1]
	float final_matrix[9];
	wlr_matrix_multiply(final_matrix, renderer->projection,	rotated_matrix);

	// Draw
	struct VertPcrData VertPcrData;
	mat3_to_mat4(final_matrix, VertPcrData.mat4);

	VertPcrData.uv_off[0] =	box->x / wlr_texture->width;
	VertPcrData.uv_off[1] =	box->y / wlr_texture->height;
	VertPcrData.uv_size[0] = box->width / wlr_texture->width;
	VertPcrData.uv_size[1] = box->height / wlr_texture->height;

	vkCmdPushConstants(cb, renderer->pipe_layout,
		VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(VertPcrData), &VertPcrData);
	vkCmdPushConstants(cb, renderer->pipe_layout,
		VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(VertPcrData), sizeof(float),
		&alpha);
	vkCmdDraw(cb, 4, 1, 0, 0);

	// Draw	a copy in the original position
	/*
	float non_rotated_final_matrix[9];
	wlr_matrix_multiply(non_rotated_final_matrix, renderer->projection, matrix);
	// Remove translation so it always renders top-left
	non_rotated_final_matrix[2] = 0;
	non_rotated_final_matrix[5] = 0;
	mat3_to_mat4(non_rotated_final_matrix, VertPcrData.mat4);

	float copy_alpha = 0.5;
	vkCmdPushConstants(cb, renderer->pipe_layout,
		VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(VertPcrData), &VertPcrData);
	vkCmdPushConstants(cb, renderer->pipe_layout,
		VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(VertPcrData), sizeof(float),
		&copy_alpha);
	vkCmdDraw(cb, 4, 1, 0, 0);

	texture->last_used = renderer->frame;

	render_rect_simple(wlr_renderer, (float[4]){1.0, 1.0, 0.0, 1.0}, center_x - 1, center_y	- 1, 2,	2);
	*/

	return true;
}

static void render_texture(struct wlr_output *output,
		pixman_region32_t *output_damage,
		struct wlr_scene_node *node, struct wlr_texture	*texture,
		const struct wlr_fbox *src_box,	const struct wlr_box *dst_box,
		const float matrix[static 9]) {
	/* src_box: pixel coordinates, but only	width and height. Also floating	point.
	 * dst_box: pixel coordinates of where to render to
	 * matrix: matrix to transform 0..1 coords to where to render to
	 * 
	 * The original	tinywl only redraws damaged regions (for efficiency, I think).
	 * But screw that.
	 * 
	 * <node> is passed so render_subtexture_with_matrix knows what	to rotate around (we figure out
	 * the center of rotation by going up and down the scene graph starting	with the node we're drawing).
	 */
	printf("\t\t\t[render_texture]\n");
	fflush(stdout);

	struct wlr_renderer *renderer =	output->renderer;
	assert(renderer);

	// If the source box is	empty, make one	based on the destination box
	struct wlr_fbox	default_src_box	= {0};
	if (wlr_fbox_empty(src_box)) {
		default_src_box.width =	dst_box->width;
		default_src_box.height = dst_box->height;
		src_box	= &default_src_box;
	}

	wlr_renderer_scissor(renderer, NULL);
	render_subtexture_with_matrix(renderer,	node, texture, src_box,	matrix,	1.0);
}

static void render_node_iterator(struct	wlr_scene_node *node,
		int x, int y, void *_data) {
	struct render_data *data = _data;
	struct wlr_output *output = data->output;
	pixman_region32_t *output_damage = data->damage;

	struct wlr_box dst_box = {
		.x = x,
		.y = y,
	};
	scene_node_get_size(node, &dst_box.width, &dst_box.height);
	scale_box(&dst_box, output->scale);

	struct wlr_texture *texture;
	float matrix[9];
	enum wl_output_transform transform;
	switch (node->type) {
	case WLR_SCENE_NODE_ROOT:
	case WLR_SCENE_NODE_TREE:
		/* Root	or tree	node has nothing to render itself */
		break;
	case WLR_SCENE_NODE_SURFACE:;
		rendered_surface_count++;

		struct wlr_scene_surface *scene_surface	= wlr_scene_surface_from_node(node);
		struct wlr_surface *surface = scene_surface->surface;

		texture	= wlr_surface_get_texture(surface);
		if (texture == NULL) {
			return;
		}

		// In my case (and I think basically always) both transform and	output->transform_matrix
		// are identity	matrices
		transform = wlr_output_transform_invert(surface->current.transform);

		// The resulting matrix	looks like [w 0	x    0 h y    0	0 1]
		// So it would coordinates 0..1	to the pixel coordinates of the	window
		wlr_matrix_project_box(matrix, &dst_box, transform, 0.0,
			output->transform_matrix);

		// The source box has the size of the surface. X and Y are always 0, as	far as I can tell.
		struct wlr_fbox	src_box	= {0};

		render_texture(output, output_damage, node, texture,
			&src_box, &dst_box, matrix);

		if (data->presentation != NULL && scene_surface->primary_output	== output) {
			wlr_presentation_surface_sampled_on_output(data->presentation,
				surface, output);
		}
		break;
	case WLR_SCENE_NODE_RECT:;
		struct wlr_scene_rect *scene_rect = scene_rect_from_node(node);

		render_rect(output, output_damage, scene_rect->color, &dst_box,
			output->transform_matrix);
		break;
	case WLR_SCENE_NODE_BUFFER:;
		struct wlr_scene_buffer	*scene_buffer =	scene_buffer_from_node(node);

		struct wlr_renderer *renderer =	output->renderer;
		texture	= scene_buffer_get_texture(scene_buffer, renderer);
		if (texture == NULL) {
			return;
		}

		transform = wlr_output_transform_invert(scene_buffer->transform);
		wlr_matrix_project_box(matrix, &dst_box, transform, 0.0,
			output->transform_matrix);

		render_texture(output, output_damage, node, texture, &scene_buffer->src_box,
			&dst_box, matrix);
		break;
	}
}

void scene_render_output(struct	wlr_scene *scene, struct wlr_output *output,
		int lx,	int ly,	pixman_region32_t *damage) {
	// lx and ly are always	0 in my	case. Changing them would globally offset everything on	the screen by that
	// many	pixels.
	printf("\t[scene_render_output]");
	fflush(stdout);
	pixman_region32_t full_region;
	pixman_region32_init_rect(&full_region,	0, 0, output->width, output->height);
	if (damage == NULL) {
		damage = &full_region;
	}

	struct wlr_renderer *renderer =	output->renderer;
	assert(renderer);

	if (output->enabled && pixman_region32_not_empty(damage)) {
		struct render_data data	= {
			.output	= output,
			.damage	= damage,
			.presentation =	scene->presentation,
		};
		scene_node_for_each_node(&scene->node, -lx, -ly,
			render_node_iterator, &data);

		printf("\t[scene_render_output]Begin scene graph\n");
		print_scene_graph(&scene->node,	2);
		printf("\t[scene_render_output]End scene graph\n");
		fflush(stdout);

		wlr_renderer_scissor(renderer, NULL);
	}

	pixman_region32_fini(&full_region);
}

static bool scene_output_scanout(struct	wlr_scene_output *scene_output)	{
	struct wlr_output *output = scene_output->output;

	struct wlr_box viewport_box = {	.x = scene_output->x, .y = scene_output->y };
	wlr_output_effective_resolution(output,
		&viewport_box.width, &viewport_box.height);

	struct check_scanout_data check_scanout_data = {
		.viewport_box =	viewport_box,
	};
	scene_node_for_each_node(&scene_output->scene->node, 0,	0,
		check_scanout_iterator,	&check_scanout_data);
	if (check_scanout_data.n != 1 || check_scanout_data.node == NULL) {
		return false;
	}

	struct wlr_scene_node *node = check_scanout_data.node;
	struct wlr_buffer *buffer;
	switch (node->type) {
	case WLR_SCENE_NODE_SURFACE:;
		struct wlr_scene_surface *scene_surface	= wlr_scene_surface_from_node(node);
		if (scene_surface->surface->buffer == NULL ||
				scene_surface->surface->current.viewport.has_src ||
				scene_surface->surface->current.transform != output->transform)	{
			return false;
		}
		buffer = &scene_surface->surface->buffer->base;
		break;
	case WLR_SCENE_NODE_BUFFER:;
		struct wlr_scene_buffer	*scene_buffer =	scene_buffer_from_node(node);
		if (scene_buffer->buffer == NULL ||
				!wlr_fbox_empty(&scene_buffer->src_box)	||
				scene_buffer->transform	!= output->transform) {
			return false;
		}
		buffer = scene_buffer->buffer;
		break;
	default:
		return false;
	}

	wlr_output_attach_buffer(output, buffer);
	if (!wlr_output_test(output)) {
		wlr_output_rollback(output);
		return false;
	}

	struct wlr_presentation	*presentation =	scene_output->scene->presentation;
	if (presentation != NULL && node->type == WLR_SCENE_NODE_SURFACE) {
		struct wlr_scene_surface *scene_surface	=
			wlr_scene_surface_from_node(node);
		// Since outputs may overlap, we still need to check this even though
		// we know that	the surface size matches the size of this output.
		if (scene_surface->primary_output == output) {
			wlr_presentation_surface_sampled_on_output(presentation,
				scene_surface->surface,	output);
		}
	}

	return wlr_output_commit(output);
}

bool scene_output_commit(struct	wlr_scene_output *scene_output)	{
	printf("scene_output_commit\n");
	fflush(stdout);

	// If I	don't do this, windows aren't re-drawn when the	cursor moves.
	// So screw it.
	wlr_output_damage_add_whole(scene_output->damage);

	// Get the output, i.e.	screen
	struct wlr_output *output = scene_output->output;

	// Get the renderer, i.e. Vulkan or GLES2
	struct wlr_renderer *renderer =	output->renderer;
	assert(renderer	!= NULL);

	// Scanout is actually sending the pixels to the monitor (I think).
	bool scanout = scene_output_scanout(scene_output);
	if (scanout != scene_output->prev_scanout) {
		wlr_log(WLR_DEBUG, "Direct scan-out %s",
			scanout	? "enabled" : "disabled");
		// When	exiting	direct scan-out, damage	everything
		wlr_output_damage_add_whole(scene_output->damage);
	}
	scene_output->prev_scanout = scanout;
	if (scanout) {
		return true;
	}

	bool needs_frame;
	pixman_region32_t damage;
	pixman_region32_init(&damage);
	// wlr_output_damage_attach_render: "Attach the	renderer's buffer to the output"
	// "Must call this before rendering, then `wlr_output_set_damage` then `wlr_output_commit`
	if (!wlr_output_damage_attach_render(scene_output->damage,
			&needs_frame, &damage))	{
		pixman_region32_fini(&damage);
		return false;
	}

	if (!needs_frame) {
		pixman_region32_fini(&damage);
		wlr_output_rollback(output);
		return true;
	}

	// Try to import new buffers as	textures
	// As far as I can tell	this never gets	called
	struct wlr_scene_buffer	*scene_buffer, *scene_buffer_tmp;
	wl_list_for_each_safe(scene_buffer, scene_buffer_tmp,
			&scene_output->scene->pending_buffers, pending_link) {
		scene_buffer_get_texture(scene_buffer, renderer);
		wl_list_remove(&scene_buffer->pending_link);
		wl_list_init(&scene_buffer->pending_link);
	}

	wlr_renderer_begin(renderer, output->width, output->height);

	wlr_renderer_scissor(renderer, NULL);
	wlr_renderer_clear(renderer, (float[4]){ 0.3, 0.0, 0.1,	1.0 });

	rendered_surface_count = 0;
	scene_render_output(scene_output->scene, output,
		scene_output->x, scene_output->y, &damage);
	wlr_output_render_software_cursors(output, &damage);

	float color[4] = { rand()%2, rand()%2, rand()%2, 1.0 };
	render_rect_simple(renderer, color, 10,	10, 20,	rendered_surface_count*2 + 1);

	wlr_renderer_end(renderer);
	pixman_region32_fini(&damage);

	int tr_width, tr_height;
	wlr_output_transformed_resolution(output, &tr_width, &tr_height);

	enum wl_output_transform transform =
		wlr_output_transform_invert(output->transform);

	pixman_region32_t frame_damage;
	pixman_region32_init(&frame_damage);
	wlr_region_transform(&frame_damage, &scene_output->damage->current,
		transform, tr_width, tr_height);
	wlr_output_set_damage(output, &frame_damage);
	pixman_region32_fini(&frame_damage);

	if (must_take_screenshot) {
		/* Take	a screenshot directly */
		uint8_t	*data_xrgb = malloc(1920 * 1080	* 4);
		assert(data_xrgb != NULL);

		bool result = wlr_renderer_read_pixels(renderer, DRM_FORMAT_XRGB8888,
			NULL, 1920 * 4,	300, 300, 50, 50, 300, 50, data_xrgb);
		assert((result == true)	&& "Couldn't read pixels");

		// Get rid of the extra	channel
		// Also	flip vertically	cause BMP is the other way around
		uint8_t	*data_rgb = malloc(1920	* 1080 * 3);
		assert(data_rgb	!= NULL);

		for (int y = 0;	y < 1080; y++) {
			for (int x = 0;	x < 1920; x++) {
				size_t idx_rgb = (1080 - y - 1)	* 1920 + x;
				size_t idx_xrgb	= y * 1920 + x;
				data_rgb[3 * idx_rgb + 0] = data_xrgb[4	* idx_xrgb + 0];
				data_rgb[3 * idx_rgb + 1] = data_xrgb[4	* idx_xrgb + 1];
				data_rgb[3 * idx_rgb + 2] = data_xrgb[4	* idx_xrgb + 2];
			}
		}

		FILE *fp = fopen("out.bmp", "w");
		assert((fp != NULL) && "Couldn't open file for writing");

		write_bmp(fp, 1920, 1080, data_rgb);

		fclose(fp);

		must_take_screenshot = false;
	}

	return wlr_output_commit(output);
}

static void handle_output_frame(struct wl_listener *listener, void *data) {
	/* This	function is called every time an output	is ready to display a frame,
	 * generally at	the output's refresh rate (e.g.	60Hz). */
	struct Output *output =	wl_container_of(listener, output, frame);
	struct wlr_scene *scene	= output->server->scene;

	// wlr_scene_output: "A	viewport for an	output in the scene-graph" (include/wlr/types/wlr_scene.h)
	// It is associated with a scene
	struct wlr_scene_output	*scene_output =	wlr_scene_get_scene_output(
		scene, output->wlr_output);

	wlr_scene_node_for_each_surface(&scene->node, &node_iterator, NULL);
	fflush(stdout);

	/* Render the scene if needed and commit the output */
	scene_output_commit(scene_output);

	struct timespec	now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(scene_output, &now);
}

// End my stuff

static void handle_new_output(struct wl_listener *listener, void *data)	{
	/* This	event is raised	by the backend when a new output (aka a	display	or
	 * monitor) becomes available. */
	struct Server *server =
		wl_container_of(listener, server, new_output);
	struct wlr_output *wlr_output =	data;

	/* Configures the output created by the	backend	to use our allocator
	 * and our renderer. Must be done once,	before commiting the output */
	wlr_output_init_render(wlr_output, server->allocator, server->renderer);

	/* Some	backends don't have modes. DRM+KMS does, and we	need to	set a mode
	 * before we can use the output. The mode is a tuple of	(width,	height,
	 * refresh rate), and each monitor supports only a specific set	of modes. We
	 * just	pick the monitor's preferred mode, a more sophisticated	compositor
	 * would let the user configure	it. */
	if (!wl_list_empty(&wlr_output->modes))	{
		struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
		wlr_output_set_mode(wlr_output,	mode);
		wlr_output_enable(wlr_output, true);
		if (!wlr_output_commit(wlr_output)) {
			return;
		}
	}

	/* Allocates and configures our	state for this output */
	struct Output *output =
		calloc(1, sizeof(struct	Output));
	output->wlr_output = wlr_output;
	output->server = server;
	/* Sets	up a listener for the frame notify event. */
	output->frame.notify = handle_output_frame;
	wl_signal_add(&wlr_output->events.frame, &output->frame);
	wl_list_insert(&server->outputs, &output->link);

	/* Adds	this to	the output layout. The add_auto	function arranges outputs
	 * from	left-to-right in the order they	appear.	A more sophisticated
	 * compositor would let	the user configure the arrangement of outputs in the
	 * layout.
	 *
	 * The output layout utility automatically adds	a wl_output global to the
	 * display, which Wayland clients can see to find out information about	the
	 * output (such	as DPI,	scale factor, manufacturer, etc).
	 */
	wlr_output_layout_add_auto(server->output_layout, wlr_output);
}

static void handle_xdg_toplevel_map(struct wl_listener *listener, void *data) {
	/* Called when the surface is mapped, or ready to display on-screen. */
	struct View *view = wl_container_of(listener, view, map);

	wl_list_insert(&view->server->views, &view->link);

	focus_view(view, view->xdg_surface->surface);
}

static void handle_xdg_toplevel_unmap(struct wl_listener *listener, void *data)	{
	/* Called when the surface is unmapped,	and should no longer be	shown. */
	struct View *view = wl_container_of(listener, view, unmap);

	wl_list_remove(&view->link);
}

static void handle_xdg_toplevel_destroy(struct wl_listener *listener, void *data) {
	/* Called when the surface is destroyed	and should never be shown again. */
	struct View *view = wl_container_of(listener, view, destroy);

	wl_list_remove(&view->map.link);
	wl_list_remove(&view->unmap.link);
	wl_list_remove(&view->destroy.link);
	wl_list_remove(&view->request_move.link);
	wl_list_remove(&view->request_resize.link);

	free(view);
}

static void begin_interactive(struct View *view,
		enum CursorMode	mode, uint32_t edges) {
	/* This	function sets up an interactive	move or	resize operation, where	the
	 * compositor stops propegating	pointer	events to clients and instead
	 * consumes them itself, to move or resize windows. */
	struct Server *server =	view->server;
	struct wlr_surface *focused_surface =
		server->seat->pointer_state.focused_surface;
	if (view->xdg_surface->surface !=
			wlr_surface_get_root_surface(focused_surface)) {
		/* Deny	move/resize requests from unfocused clients. */
		return;
	}
	server->grabbed_view = view;
	server->cursor_mode = mode;

	if (mode == VKWC_CURSOR_MOVE) {
		server->grab_x = server->cursor->x - view->x;
		server->grab_y = server->cursor->y - view->y;
	} else {
		struct wlr_box geo_box;
		wlr_xdg_surface_get_geometry(view->xdg_surface,	&geo_box);

		double border_x	= (view->x + geo_box.x)	+
			((edges	& WLR_EDGE_RIGHT) ? geo_box.width : 0);
		double border_y	= (view->y + geo_box.y)	+
			((edges	& WLR_EDGE_BOTTOM) ? geo_box.height : 0);
		server->grab_x = server->cursor->x - border_x;
		server->grab_y = server->cursor->y - border_y;

		server->grab_geobox = geo_box;
		server->grab_geobox.x += view->x;
		server->grab_geobox.y += view->y;

		server->resize_edges = edges;
	}
}

static void handle_xdg_toplevel_request_move(
		struct wl_listener *listener, void *data) {
	/* This	event is raised	when a client would like to begin an interactive
	 * move, typically because the user clicked on their client-side
	 * decorations.	Note that a more sophisticated compositor should check the
	 * provided serial against a list of button press serials sent to this
	 * client, to prevent the client from requesting this whenever they want. */
	struct View *view = wl_container_of(listener, view, request_move);
	begin_interactive(view,	VKWC_CURSOR_MOVE, 0);
}

static void handle_xdg_toplevel_request_resize(
		struct wl_listener *listener, void *data) {
	/* This	event is raised	when a client would like to begin an interactive
	 * resize, typically because the user clicked on their client-side
	 * decorations.	Note that a more sophisticated compositor should check the
	 * provided serial against a list of button press serials sent to this
	 * client, to prevent the client from requesting this whenever they want. */
	struct wlr_xdg_toplevel_resize_event *event = data;
	struct View *view = wl_container_of(listener, view, request_resize);
	begin_interactive(view,	VKWC_CURSOR_RESIZE, event->edges);
}

static void handle_new_xdg_surface(struct wl_listener *listener, void *data) {
	/* This	event is raised	when wlr_xdg_shell receives a new xdg surface from a
	 * client, either a toplevel (application window) or popup. */
	struct Server *server =
		wl_container_of(listener, server, new_xdg_surface);
	struct wlr_xdg_surface *xdg_surface = data;

	/* We must add xdg popups to the scene graph so	they get rendered. The
	 * wlroots scene graph provides	a helper for this, but to use it we must
	 * provide the proper parent scene node	of the xdg popup. To enable this,
	 * we always set the user data field of	xdg_surfaces to	the corresponding
	 * scene node. */
	if (xdg_surface->role == WLR_XDG_SURFACE_ROLE_POPUP) {
		struct wlr_xdg_surface *parent = wlr_xdg_surface_from_wlr_surface(
			xdg_surface->popup->parent);
		struct wlr_scene_node *parent_node = parent->data;
		xdg_surface->data = wlr_scene_xdg_surface_create(
			parent_node, xdg_surface);
		return;
	}
	assert(xdg_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL);

	/* Allocate a View for this surface */
	struct View *view =
		calloc(1, sizeof(struct	View));
	view->type = XDG_SHELL_VIEW;
	view->server = server;
	view->xdg_surface = xdg_surface;
	view->scene_node = wlr_scene_xdg_surface_create(
			&view->server->scene->node, view->xdg_surface);
	view->scene_node->data = view;
	xdg_surface->data = view->scene_node;

	/* Listen to the various events	it can emit */
	view->map.notify = handle_xdg_toplevel_map;
	wl_signal_add(&xdg_surface->events.map,	&view->map);
	view->unmap.notify = handle_xdg_toplevel_unmap;
	wl_signal_add(&xdg_surface->events.unmap, &view->unmap);
	view->destroy.notify = handle_xdg_toplevel_destroy;
	wl_signal_add(&xdg_surface->events.destroy, &view->destroy);

	/* cotd	*/
	struct wlr_xdg_toplevel	*toplevel = xdg_surface->toplevel;
	view->request_move.notify = handle_xdg_toplevel_request_move;
	wl_signal_add(&toplevel->events.request_move, &view->request_move);
	view->request_resize.notify = handle_xdg_toplevel_request_resize;
	wl_signal_add(&toplevel->events.request_resize,	&view->request_resize);
}

static void handle_xwayland_surface_map(struct wl_listener *listener, void *data) {
	struct XWaylandView *xwayland_view = wl_container_of(listener, xwayland_view, map);
	struct View *view = &xwayland_view->view;
	struct wlr_surface *surface = xwayland_view->xwayland_surface->surface;

	view->scene_node = wlr_scene_subsurface_tree_create(&view->server->scene->node,	surface);
	assert(view->scene_node	!= NULL);	// Couldn't create scene node for XWayland window
	view->scene_node->data = view;

	wl_list_insert(&view->server->views, &view->link);
}

static void handle_xwayland_surface_new(struct wl_listener *listener, void *data) {
	struct Server *server =	wl_container_of(listener, server, new_xwayland_surface);
	struct wlr_xwayland_surface *xwayland_surface =	data;

	struct XWaylandView *xwayland_view = calloc(1, sizeof(struct XWaylandView));
	assert(xwayland_view);

	xwayland_view->xwayland_surface	= xwayland_surface;

	xwayland_view->view.server = server;
	xwayland_view->view.type = XWAYLAND_VIEW;

	xwayland_view->map.notify = handle_xwayland_surface_map;
	wl_signal_add(&xwayland_surface->events.map, &xwayland_view->map);
}

int main(int argc, char	*argv[]) {
	wlr_log_init(WLR_DEBUG,	NULL);
	char *startup_cmd = NULL;

	int c;
	while ((c = getopt(argc, argv, "s:h")) != -1) {
		switch (c) {
		case 's':
			startup_cmd = optarg;
			break;
		default:
			printf("Usage: %s [-s startup command]\n", argv[0]);
			return 0;
		}
	}
	if (optind < argc) {
		printf("Usage: %s [-s startup command]\n", argv[0]);
		return 0;
	}

	struct Server server;
	/* The Wayland display is managed by libwayland. It handles accepting
	 * clients from	the Unix socket, manging Wayland globals, and so on. */
	server.wl_display = wl_display_create();
	/* The backend is a wlroots feature which abstracts the	underlying input and
	 * output hardware. The	autocreate option will choose the most suitable
	 * backend based on the	current	environment, such as opening an	X11 window
	 * if an X11 server is running.	*/
	server.backend = wlr_backend_autocreate(server.wl_display);

	// Create a renderer, we want Vulkan
	int drm_fd = -1;
	drm_fd = wlr_backend_get_drm_fd(server.backend);

	if (drm_fd < 0)	{
		fprintf(stderr,	"Couldn't get DRM file descriptor\n");
		exit(1);
	}

	server.renderer	= wlr_vk_renderer_create_with_drm_fd(drm_fd);
	wlr_renderer_init_wl_display(server.renderer, server.wl_display);

	/* Autocreates an allocator for	us.
	 * The allocator is the	bridge between the renderer and	the backend. It
	 * handles the buffer creation,	allowing wlroots to render onto	the
	 * screen */
	 server.allocator = wlr_allocator_autocreate(server.backend,
		server.renderer);

	/* This	creates	some hands-off wlroots interfaces. The compositor is
	 * necessary for clients to allocate surfaces and the data device manager
	 * handles the clipboard. Each of these	wlroots	interfaces has room for	you
	 * to dig your fingers in and play with	their behavior if you want. Note that
	 * the clients cannot set the selection	directly without compositor approval,
	 * see the handling of the request_set_selection event below.*/
	struct wlr_compositor *compositor = wlr_compositor_create(server.wl_display, server.renderer);
	wlr_data_device_manager_create(server.wl_display);

	/* Creates an output layout, which a wlroots utility for working with an
	 * arrangement of screens in a physical	layout.	*/
	server.output_layout = wlr_output_layout_create();

	/* Configure a listener	to be notified when new	outputs	are available on the
	 * backend. */
	wl_list_init(&server.outputs);
	server.new_output.notify = handle_new_output;
	wl_signal_add(&server.backend->events.new_output, &server.new_output);

	/* Create a scene graph. This is a wlroots abstraction that handles all
	 * rendering and damage	tracking. All the compositor author needs to do
	 * is add things that should be	rendered to the	scene graph at the proper
	 * positions and then call wlr_scene_output_commit() to	render a frame if
	 * necessary.
	 */
	server.scene = wlr_scene_create();
	wlr_scene_attach_output_layout(server.scene, server.output_layout);

	/* Set up the xdg-shell. The xdg-shell is a Wayland protocol which is used
	 * for application windows. For	more detail on shells, refer to	my article:
	 *
	 * https://drewdevault.com/2018/07/29/Wayland-shells.html
	 */
	wl_list_init(&server.views);
	server.xdg_shell = wlr_xdg_shell_create(server.wl_display);
	server.new_xdg_surface.notify =	handle_new_xdg_surface;
	wl_signal_add(&server.xdg_shell->events.new_surface,
			&server.new_xdg_surface);

	/*
	 * Creates a cursor, which is a	wlroots	utility	for tracking the cursor
	 * image shown on screen.
	 */
	server.cursor =	wlr_cursor_create();
	wlr_cursor_attach_output_layout(server.cursor, server.output_layout);

	// I added this	so grim	can figure out the screen dimensions
	wlr_xdg_output_manager_v1_create(server.wl_display, server.output_layout);

	/* Creates an xcursor manager, another wlroots utility which loads up
	 * Xcursor themes to source cursor images from and makes sure that cursor
	 * images are available	at all scale factors on	the screen (necessary for
	 * HiDPI support). We add a cursor theme at scale factor 1 to begin with. */
	server.cursor_mgr = wlr_xcursor_manager_create(NULL, 24);
	wlr_xcursor_manager_load(server.cursor_mgr, 1);

	/*
	 * wlr_cursor *only* displays an image on screen. It does not move around
	 * when	the pointer moves. However, we can attach input	devices	to it, and
	 * it will generate aggregate events for all of	them. In these events, we
	 * can choose how we want to process them, forwarding them to clients and
	 * moving the cursor around. More detail on this process is described in my
	 * input handling blog post:
	 *
	 * https://drewdevault.com/2018/07/17/Input-handling-in-wlroots.html
	 *
	 * And more comments are sprinkled throughout the notify functions above.
	 */
	server.cursor_motion.notify = handle_cursor_motion_relative;
	wl_signal_add(&server.cursor->events.motion, &server.cursor_motion);
	server.cursor_motion_absolute.notify = handle_cursor_motion_absolute;
	wl_signal_add(&server.cursor->events.motion_absolute,
			&server.cursor_motion_absolute);
	server.cursor_button.notify = handle_cursor_button;
	wl_signal_add(&server.cursor->events.button, &server.cursor_button);
	server.cursor_axis.notify = handle_cursor_axis;
	wl_signal_add(&server.cursor->events.axis, &server.cursor_axis);
	server.cursor_frame.notify = handle_cursor_frame;
	wl_signal_add(&server.cursor->events.frame, &server.cursor_frame);

	/*
	 * Configures a	seat, which is a single	"seat" at which	a user sits and
	 * operates the	computer. This conceptually includes up	to one keyboard,
	 * pointer, touch, and drawing tablet device. We also rig up a listener	to
	 * let us know when new	input devices are available on the backend.
	 */
	wl_list_init(&server.keyboards);
	server.new_input.notify	= handle_new_input;
	wl_signal_add(&server.backend->events.new_input, &server.new_input);
	server.seat = wlr_seat_create(server.wl_display, "seat0");
	server.request_cursor.notify = handle_new_cursor_image;
	wl_signal_add(&server.seat->events.request_set_cursor,
			&server.request_cursor);
	server.request_set_selection.notify = handle_selection_request;
	wl_signal_add(&server.seat->events.request_set_selection,
			&server.request_set_selection);

	// Screencopy support
	struct wlr_screencopy_manager_v1 *screencopy_manager = wlr_screencopy_manager_v1_create(server.wl_display);
	printf("Screencopy manager: %p\n", (void *) screencopy_manager);

	// Set up xwayland
	struct wlr_xwayland *xwayland =	wlr_xwayland_create(server.wl_display, compositor, true);
	if (!xwayland) {
		fprintf(stderr,	"Cannot	create XWayland	server!\n");
		exit(1);
	};

	server.new_xwayland_surface.notify = handle_xwayland_surface_new;
	wl_signal_add(&xwayland->events.new_surface, &server.new_xwayland_surface);

	struct wlr_xcursor_manager *xcursor_manager = wlr_xcursor_manager_create("left_ptr", 24);
	if (!xcursor_manager) {
		fprintf(stderr,	"Can't create XCursor manager!\n");
		exit(1);
	};

	if (setenv("DISPLAY", xwayland->display_name, true) < 0) {
		fprintf(stderr,	"Couldn't set DISPLAY for XWayland!\n");
		exit(1);
	} else {
		printf("XWayland on DISPLAY=%s\n", xwayland->display_name);
		fflush(stdout);
	}

	if (!wlr_xcursor_manager_load(xcursor_manager, 1)) {
		fprintf(stderr,	"Can't load XCursor theme!\n");
		exit(1);
	}
	struct wlr_xcursor *xcursor = wlr_xcursor_manager_get_xcursor(xcursor_manager, "left_ptr", 1);
	if (xcursor) {
		struct wlr_xcursor_image *image	= xcursor->images[0];
		wlr_xwayland_set_cursor(xwayland, image->buffer, image->width *	4, image->width, image->height,
					image->hotspot_x, image->hotspot_y);
	};

	/* Add a Unix socket to	the Wayland display. */
	const char *socket = wl_display_add_socket_auto(server.wl_display);
	if (!socket) {
		wlr_backend_destroy(server.backend);
		return 1;
	}

	/* Start the backend. This will	enumerate outputs and inputs, become the DRM
	 * master, etc */
	if (!wlr_backend_start(server.backend))	{
		wlr_backend_destroy(server.backend);
		wl_display_destroy(server.wl_display);
		return 1;
	}

	/* Set the WAYLAND_DISPLAY environment variable	to our socket and run the
	 * startup command if requested. */
	setenv("WAYLAND_DISPLAY", socket, true);
	if (startup_cmd) {
		if (fork() == 0) {
			execl("/bin/sh", "/bin/sh", "-c", startup_cmd, (void *)NULL);
		}
	}
	/* Run the Wayland event loop. This does not return until you exit the
	 * compositor. Starting	the backend rigged up all of the necessary event
	 * loop	configuration to listen	to libinput events, DRM	events,	generate
	 * frame events	at the refresh rate, and so on.	*/
	wlr_log(WLR_INFO, "Running Wayland compositor on WAYLAND_DISPLAY=%s",
			socket);
	wl_display_run(server.wl_display);

	/* Once	wl_display_run returns,	we shut	down the server. */
	wl_display_destroy_clients(server.wl_display);
	wl_display_destroy(server.wl_display);
	return 0;
}
