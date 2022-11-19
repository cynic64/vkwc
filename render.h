#ifndef render_h_INCLUDED
#define render_h_INCLUDED

#include <wayland-server-core.h>

#include "surface.h"

// surfaces has type struct Surface from surface.h
bool draw_frame(struct wlr_output *output, struct wl_list *surfaces, int cursor_x, int cursor_y);

void print_scene_graph(struct wlr_scene_node *node, int	level);

#endif // render_h_INCLUDED

