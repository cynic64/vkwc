#ifndef util_h_INCLUDED
#define util_h_INCLUDED

#include <cglm/cglm.h>
#include <wlr/types/wlr_scene.h>

struct wlr_scene_node *get_toplevel_node(struct wlr_scene_node *node);

void print_scene_graph(struct wlr_scene_node *node, int	level);

void print_matrix(mat4 matrix);

#endif // util_h_INCLUDED

