#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "util.h"

// To print scene node types as	text
const char* const SCENE_NODE_TYPE_LOOKUP[] = {"ROOT", "TREE", "SURFACE", "RECT", "BUFFER", "INVALID"};

struct wlr_scene_node *get_toplevel_node(struct wlr_scene_node *node) {
	/* If we have a	node like one of the decoration	surfaces, this will (hopefully)	return the
	 * toplevel surface <node> is attached to.
	 * 
	 * We do this by:
	 *	Taking the node's parent until we hit the root node, the one before this is our	"branch"
	 *	Take the first child of	the branch until we hit	a surface
	 * That	should be the toplevel window.
	 */

	struct wlr_scene_node *cur = node;

	// Go up until we hit the root
	assert(cur->parent != NULL);
	while (1) {
		cur = &cur->parent->node;
		assert(cur->parent != NULL);

		// If our parent doesn't have a parent, our parent is the root
		// node and we are the toplevel window, so stop.
		if (cur->parent->node.parent == NULL) break;
	}

	// Go down until we hit	a surface
	while (cur->type == WLR_SCENE_NODE_TREE) {
		cur = wl_container_of(cur->parent->children.next, cur, link);
		assert(cur != NULL);	// Couldn't find a surface
	}

	return cur;
}

void print_scene_graph(struct wlr_scene_node *node, int	level) {
	for (int i = 0;	i < level; i++)	printf("\t");
	printf("Node type: %s, %d children, xy relative to parent: %d %d\n",
		SCENE_NODE_TYPE_LOOKUP[node->type], wl_list_length(&node->parent->children),
		node->x, node->y);

	for (int i = 0;	i < level; i++)	printf("\t");

	if (node->type == WLR_SCENE_NODE_TREE) {
		printf("Cast as TREE. Nothing interesting\n");
	} else if (node->type == WLR_SCENE_NODE_RECT) {
		struct wlr_scene_rect *scene_rect = (struct wlr_scene_rect *) node;
		printf("Cast as RECT. Dims: %d x %d, color: %f %f %f %f\n",
			scene_rect->width, scene_rect->height,
			scene_rect->color[0], scene_rect->color[1],scene_rect->color[2],
                        scene_rect->color[3]);
	} else if (node->type == WLR_SCENE_NODE_BUFFER) {
		struct wlr_scene_buffer *scene_buffer = (struct wlr_scene_buffer *) node;
		printf("Cast as BUFFER. Destination dims: %d x %d\n",
		scene_buffer->dst_width, scene_buffer->dst_height);
	} else {
		fprintf(stderr, "Weird node type\n");
		exit(1);
	}

	struct wlr_scene_node *child;
	wl_list_for_each(child,	&node->parent->children, link) {
		print_scene_graph(child, level + 1);
	}
}

double get_time() {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
        return ts.tv_sec + (double) ts.tv_nsec / 1000000000;
}
