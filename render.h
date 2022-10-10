#ifndef render_h_INCLUDED
#define render_h_INCLUDED

#include <wayland-server-core.h>

struct Surface {
	struct Server *server;
	struct wl_list link;
	struct wl_listener destroy;
	struct wlr_surface *wlr_surface;

	float rotation;		// In radians
};

bool draw_frame(struct wlr_scene_output *scene_output, struct wl_list *surfaces);

void print_scene_graph(struct wlr_scene_node *node, int	level);

#endif // render_h_INCLUDED

