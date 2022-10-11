#ifndef util_h_INCLUDED
#define util_h_INCLUDED

#include <cglm/cglm.h>
#include <wlr/types/wlr_scene.h>

void get_node_placement(struct wlr_scene_node *node, int *x, int *y, int *width, int *height);

struct wlr_scene_node *get_main_node(struct wlr_scene_node *node);

void get_node_center(struct wlr_scene_node *node, int *x, int *y);

void print_scene_graph(struct wlr_scene_node *node, int	level);

void print_matrix(mat4 matrix);

#endif // util_h_INCLUDED

