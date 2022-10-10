#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "util.h"

// To print scene node types as	text
const char* const SCENE_NODE_TYPE_LOOKUP[] = {"ROOT", "TREE", "SURFACE", "RECT", "BUFFER", "INVALID"};

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
	for (int i = 0;	i < level; i++)	printf("\t");
	printf("Node type: %s, %d children, xy relative to parent: %d %d\n",
		SCENE_NODE_TYPE_LOOKUP[node->type], wl_list_length(&node->state.children),
		node->state.x, node->state.y);

	for (int i = 0;	i < level; i++)	printf("\t");
	if (node->type == WLR_SCENE_NODE_ROOT) {
		struct wlr_scene *scene = (struct wlr_scene *) node;
		printf("Cast as ROOT. %d outputs\n", wl_list_length(&scene->outputs));
	} else if (node->type == WLR_SCENE_NODE_TREE) {
	} else if (node->type == WLR_SCENE_NODE_SURFACE) {
		struct wlr_scene_surface *scene_surface = (struct wlr_scene_surface *) node;
		struct wlr_surface_state surface_state = scene_surface->surface->current;
		printf("Cast as SURFACE. Dims: %d x %d\n", surface_state.width, surface_state.height);
	} else if (node->type == WLR_SCENE_NODE_RECT) {
		struct wlr_scene_rect *scene_rect = (struct wlr_scene_rect *) node;
		printf("Cast as RECT. Dims: %d x %d, color: %f %f %f %f\n",
			scene_rect->width, scene_rect->height,
			scene_rect->color[0], scene_rect->color[1],scene_rect->color[2], scene_rect->color[3]);
	} else if (node->type == WLR_SCENE_NODE_BUFFER) {
		struct wlr_scene_buffer *scene_buffer = (struct wlr_scene_buffer *) node;
		printf("Cast as BUFFER. Destination dims: %d x %d\n",
		scene_buffer->dst_width, scene_buffer->dst_height);
	} else {
		fprintf(stderr, "Weird node type\n");
		exit(1);
	}

	struct wlr_scene_node *child;
	wl_list_for_each(child,	&node->state.children, state.link) {
		print_scene_graph(child, level + 1);
	}
}

void print_matrix(float matrix[9]) {
	for (int i = 0; i < 9; i++) {
		printf("%f ", matrix[i]);
	}
	printf("\n");
}
