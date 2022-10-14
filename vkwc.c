#define	_POSIX_C_SOURCE	200112L
#define CGLM_DEFINE_PRINTS
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
#include <drm_fourcc.h>
#include <cglm/cglm.h>

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
#include <wlr/types/wlr_output_damage.h>
#include <wlr/types/wlr_matrix.h>
#include <wlr/render/interface.h>
#include <wlr/util/log.h>
#include <wlr/util/region.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/xwayland.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>

#include "vulkan.h"
#include "render.h"
#include "util.h"
#include "render/vulkan.h"

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

struct Window {
	struct wl_list link;
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

	struct wl_list windows;

	struct wl_listener new_surface;
	struct wl_list surfaces;
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

static void surface_handle_destroy(struct wl_listener *listener, void *data) {
	struct Surface *surface = wl_container_of(listener, surface, destroy);

	wl_list_remove(&surface->link);
	wl_list_remove(&surface->destroy.link);

	printf("Surface destroyed! There are now %d\n", wl_list_length(&surface->server->surfaces));
	fflush(stdout);

	free(surface);
}

static void surface_handle_new(struct wl_listener *listener,
		void *data) {
	struct wlr_surface *wlr_surface = data;

	struct Surface *surface = calloc(1, sizeof(struct Surface));
	memset(surface, 0, sizeof(*surface));
	surface->wlr_surface = wlr_surface;
	surface->toplevel = NULL;
	surface->id = (double) rand() / RAND_MAX;

	struct Server *server;
	server = wl_container_of(listener, server, new_surface);
	surface->server = server;

	surface->destroy.notify = surface_handle_destroy;
	wl_signal_add(&wlr_surface->events.destroy, &surface->destroy);

	wl_list_insert(server->surfaces.prev, &surface->link);

	printf("Surface created\n");
}

void relink_nodes(struct wl_list *surfaces, struct wlr_scene_node *node) {
	// Each Surface contains a reference to the Surface of its toplevel window. Whenever surfaces are added or
	// removed, these links need to be rebuilt.
	// This will rebuild all links on the specified node and its children
	if (node->type == WLR_SCENE_NODE_SURFACE) {
		// Find the Surface this node corresponds to
		struct wlr_scene_surface *scene_surface = wlr_scene_surface_from_node(node);
		struct wlr_surface *wlr_surface = scene_surface->surface;
		struct Surface *surface = find_surface(wlr_surface, surfaces);

		// Find the toplevel node and its Surface
		struct wlr_scene_node *toplevel_node = get_toplevel_node(node);
		struct wlr_scene_surface *toplevel_scene_surface = wlr_scene_surface_from_node(toplevel_node);
		struct wlr_surface *toplevel_wlr_surface = toplevel_scene_surface->surface;
		struct Surface *toplevel_surface = find_surface(toplevel_wlr_surface, surfaces);

		surface->toplevel = toplevel_surface;
		surface->is_toplevel = toplevel_surface == surface;
	}

	struct wlr_scene_node *cur;
	wl_list_for_each(cur, &node->state.children, state.link) {
		relink_nodes(surfaces, cur);
	};
}

// Surfaces track their position and dimensions so these must be updated
void calc_placements(struct wl_list *surfaces, struct wlr_scene_node *node, int x, int y) {
	x += node->state.x;
	y += node->state.y;

	if (node->type == WLR_SCENE_NODE_SURFACE) {
		struct wlr_scene_surface *scene_surface = wlr_scene_surface_from_node(node);
		struct wlr_surface *wlr_surface = scene_surface->surface;
		struct Surface *surface = find_surface(wlr_surface, surfaces);

		surface->x = x;
		surface->y = y;
		surface->width = wlr_surface->current.width;
		surface->height = wlr_surface->current.height;
	}

	struct wlr_scene_node *cur;
	wl_list_for_each(cur, &node->state.children, state.link) {
		calc_placements(surfaces, cur, x, y);
	};
}

// When windows are resized, their projection matrices in their Surfaces must be updated.
// This will recalculate the matrices of the specified node and all children
// x and y is the position of the parent node, since a surface only knows its position relative to its parent
void calc_matrices(struct wl_list *surfaces, struct wlr_scene_node *node, int output_width, int output_height) {
	if (node->type == WLR_SCENE_NODE_SURFACE) {
		struct wlr_scene_surface *scene_surface = wlr_scene_surface_from_node(node);
		struct wlr_surface *wlr_surface = scene_surface->surface;
		struct Surface *surface = find_surface(wlr_surface, surfaces);

		if (surface->is_toplevel) {
			// These are in backwards order
			// Turn 0..2 into -1..1
			glm_translate_make(surface->matrix, (vec3) {-1, -1, 0});
			// Turn 0..1920, 0..1080 into 0..2, 0..2
			glm_scale(surface->matrix, (vec3) {2.0/output_width, 2.0/output_height, 1.0});
			// Move it
			glm_translate(surface->matrix, (vec3) {surface->x, surface->y, 0});
			// Rotate it
			glm_rotate_z(surface->matrix, 0.5, surface->matrix);
			// Scale from 0..1, 0..1 to surface->width, surface->height
			glm_scale(surface->matrix, (vec3) {surface->width, surface->height, 1.0});

			glm_translate_z(surface->matrix, surface->id);
		} else {
			// First we translate ourselves relative to toplevel, then apply toplevel transform
			// This allows for child transforms to be relative to parent transform
			struct Surface *toplevel = surface->toplevel;
			assert(toplevel != NULL);

			glm_mat4_identity(surface->matrix);

			glm_mat4_mul(surface->toplevel->matrix, surface->matrix, surface->matrix);

			glm_translate(surface->matrix, (vec3) {
				((float) surface->x - toplevel->x) / toplevel->width,
				((float) surface->y - toplevel->y) / toplevel->height,
				0,
			});
			glm_scale(surface->matrix, (vec3) {(float) surface->width / toplevel->width,
				(float) surface->height / toplevel->height, 1});

			surface->matrix[3][2] = surface->id;
		}
	}

	struct wlr_scene_node *cur;
	wl_list_for_each(cur, &node->state.children, state.link) {
		calc_matrices(surfaces, cur, output_width, output_height);
	};
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
			fprintf(stderr,	"vkwc: execvp %s", ((char **)arg)[0]);
			perror(" failed");
			exit(EXIT_SUCCESS);
		}
		break;
	case XKB_KEY_F4:
	{
		if (fork() == 0) {
			const char *arg[] = {"my-chvt", "1", NULL };
			setsid();
			execvp(((char **)arg)[0], (char	**)arg);
			fprintf(stderr,	"vkwc: execvp %s", ((char **)arg)[0]);
			perror(" failed");
			exit(EXIT_SUCCESS);
		}
		break;
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

struct Surface *get_surface_at_pos(struct Server *server, int x, int y) {
	// x and y are the absolute position of the cursor
	
	// There are multiple render buffers, so we have to find the right one. I do this just by checking whether
	// the render buffer's dimensions match those of the first output, which isn't a great way but works for now.
	struct wlr_vk_renderer *renderer = (struct wlr_vk_renderer *) server->renderer;
	struct wlr_vk_render_buffer *render_buffer = NULL;
	struct Output *output = (struct Output *) server->outputs.next;
	assert(output != NULL);

	struct wlr_vk_render_buffer *cur;
	wl_list_for_each(cur, &renderer->render_buffers, link) {
		if (cur->wlr_buffer->width == output->wlr_output->width
				&& cur->wlr_buffer->height == output->wlr_output->height) {
			if (render_buffer == NULL || render_buffer->frame < cur->frame) {
				// Always choose the most recent one
				render_buffer = cur;
			}
		}
	};
	assert(render_buffer != NULL);

	// Map the depth buffer
	int width = render_buffer->wlr_buffer->width, height = render_buffer->wlr_buffer->height;
	VkDeviceSize depth_buf_byte_count = width * height * 4;
	void *depth_buf_mem;

	VkResult res = vkMapMemory(renderer->dev->dev,
		render_buffer->host_depth_mem, 0, depth_buf_byte_count, 0, &depth_buf_mem);
	if (res != VK_SUCCESS) {
		fprintf(stderr, "Couldn't map depth buffer memory for reading\n");
		exit(1);
	}

	float *pixels = depth_buf_mem;

	// Print it out
	char chars[] = "!@#$%^&*()_+1234567890-=[],./<>?;':";
	for (size_t y = 0; y < height; y += 16) {
		for (size_t x = 0; x < width; x += 16) {
			float pixel = pixels[y * width + x];
			if (pixel == 0) {
				printf(". ");
			} else {
				printf("%c ", chars[(int) (pixel * sizeof(chars))]);
			}
		}
		printf("\n");
	}

	float pixel = pixels[((size_t) server->cursor->y) * width + ((size_t) server->cursor->x)];
	printf("Pixel under cursor: %f\n", pixel);

	vkUnmapMemory(renderer->dev->dev, render_buffer->host_depth_mem);

	// 0 means the cursor is above the background, so no surface
	if (pixel == 0) {
		return NULL;
	}

	// Otherwise, go through all surfaces until we find the one with a matching id
	struct Surface *surface = NULL;
	wl_list_for_each(surface, &server->surfaces, link) {
		if (surface->id == pixel) {
			break;
		}
	}

	if (surface->id != pixel) {
		// Something went wrong
		fprintf(stderr, "Troublesome pixel: %f\n", pixel);
		exit(1);
	}

	return surface;
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

	/* Otherwise, find the Surface under the pointer and send the event along.	*/
	struct wlr_seat	*seat =	server->seat;

	struct Surface *surface = get_surface_at_pos(server, server->cursor->x, server->cursor->y);

	if (surface == NULL) {
		// If there's no view under the	cursor,	set the	cursor image to	a
		// default. This is what makes the cursor image	appear when you	move it
		// around the screen, not over any views.
		wlr_xcursor_manager_set_cursor_image(
				server->cursor_mgr, "left_ptr",	server->cursor);
	} else {
		//
		// Send	pointer	enter and motion events.
		//
		// The enter event gives the surface "pointer focus", which is distinct
		// from	keyboard focus.	You get	pointer	focus by moving	the pointer over
		// a window.
		//
		// Note	that wlroots will avoid	sending	duplicate enter/motion events if
		// the surface has already has pointer focus or	if the client is already
		// aware of the	coordinates passed.
		//
		// Transform cursor position to -1..1, invert matrix, multiply by width and height

		struct wlr_output *cur_output = ((struct Output *) server->outputs.next)->wlr_output;
		assert(cur_output != NULL);

		float cursor_x_norm = server->cursor->x * 2.0 / cur_output->width - 1.0;
		float cursor_y_norm = server->cursor->y * 2.0 / cur_output->height - 1.0;

		struct wlr_surface *wlr_surface = surface->wlr_surface;

		mat4 inverted;
		glm_mat4_inv(surface->matrix, inverted);
		vec4 pos;
		glm_mat4_mulv(inverted, (vec4) {cursor_x_norm, cursor_y_norm, 0.0, 1.0}, pos);

		float surface_x = pos[0] * wlr_surface->current.width;
		float surface_y = pos[1] * wlr_surface->current.height;
		printf("Cursor relative to surface: %f %f\n", surface_x, surface_y);

		wlr_seat_pointer_notify_enter(seat, wlr_surface, surface_x, surface_y);
		wlr_seat_pointer_notify_motion(seat, time, surface_x, surface_y);
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

	if (event->state == WLR_BUTTON_RELEASED) {
		/* If you released any buttons,	we exit	interactive move/resize	mode. */
		server->cursor_mode = VKWC_CURSOR_PASSTHROUGH;
		return;
	}

	struct Surface *surface = get_surface_at_pos(server, server->cursor->x, server->cursor->y);
	if (surface == NULL) {
		// Nothing under cursor
		return;
	};

	// Focus view
	struct View *view;
	struct wlr_surface *wlr_surface;
	wl_list_for_each(view, &server->views, link) {
		if (view->type == XWAYLAND_VIEW) {
			struct wlr_surface *xwayland_surface =
				((struct XWaylandView *) view)->xwayland_surface->surface;
			if (xwayland_surface == surface->toplevel->wlr_surface) {
				wlr_surface = xwayland_surface;
				break;
			}
		} else if (view->xdg_surface->surface == surface->toplevel->wlr_surface) {
			wlr_surface = view->xdg_surface->surface;
			break;
		}
	}

	/* Focus that client if	the button was _pressed_ */
	focus_view(view, wlr_surface);
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

static void handle_output_frame(struct wl_listener *listener, void *data) {
	/* This	function is called every time an output	is ready to display a frame,
	 * generally at	the output's refresh rate (e.g.	60Hz). */
	struct Output *output =	wl_container_of(listener, output, frame);
	struct wlr_scene *scene	= output->server->scene;

	// Pre-frame processing
	struct wlr_scene_node *root_node = &output->server->scene->node;
	struct wl_list *surfaces = &output->server->surfaces;
	relink_nodes(surfaces, root_node);
	calc_placements(surfaces, root_node, 0, 0);
	calc_matrices(surfaces, root_node, output->wlr_output->width, output->wlr_output->height);

	// wlr_scene_output: "A	viewport for an	output in the scene-graph" (include/wlr/types/wlr_scene.h)
	// It is associated with a scene
	struct wlr_scene_output	*scene_output =	wlr_scene_get_scene_output(scene, output->wlr_output);

	/* Render the scene if needed and commit the output */
	draw_frame(scene_output, &output->server->surfaces);

	struct timespec	now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(scene_output, &now);
}

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
	printf("XDG toplevel map\n");
	/* Called when the surface is mapped, or ready to display on-screen. */
	struct View *view = wl_container_of(listener, view, map);

	wl_list_insert(&view->server->views, &view->link);

	focus_view(view, view->xdg_surface->surface);

	print_scene_graph(&view->server->scene->node, 0);
	fflush(stdout);
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

	// Surface counting stuff
	wl_list_init(&server.surfaces);
	server.new_surface.notify = surface_handle_new;
	wl_signal_add(&compositor->events.new_surface, &server.new_surface);

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
	wlr_screencopy_manager_v1_create(server.wl_display);

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
