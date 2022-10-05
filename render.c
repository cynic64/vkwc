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

// If we took a	screenshot the instant Alt+F3 was pressed, no render buffer would be bound.
// We have to do it after output commit	instead
bool must_take_screenshot = false;

// I want to count how many surfaces I render each frame
int rendered_surface_count = 0;

// To print scene node types as	text
const char* const SCENE_NODE_TYPE_LOOKUP[] = {"ROOT", "TREE", "SURFACE", "RECT", "BUFFER", "INVALID"};

struct VertPcrData {
	float mat4[4][4];
	float uv_off[2];
	float uv_size[2];
};

void render_rect_simple(struct wlr_renderer *renderer, const float color[4], int x, int	y, int width, int height) {
	struct wlr_box box = { .x = x, .y = y, .width =	width, .height = height	};
	float identity_matrix[9] = { 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0,	0.0, 1.0 };
	wlr_render_rect(renderer, &box,	color, identity_matrix);;
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

	return wlr_output_commit(output);
}

// End my stuff

