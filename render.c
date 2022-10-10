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
#include <wlr/render/vulkan.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/xwayland.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/render/vulkan.h>

#include "render.h"
#include "util.h"
#include "render/vulkan.h"

#define	M_PI 3.14159265358979323846

struct VertPcrData {
	float mat4[4][4];
	float uv_off[2];
	float uv_size[2];
};

struct RenderData {
	struct wlr_output *output;
	pixman_region32_t *damage;
	struct wlr_presentation	*presentation; // May be NULL
	struct wl_list *surfaces;
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

	// Draw
	struct VertPcrData VertPcrData;
	mat3_to_mat4(matrix, VertPcrData.mat4);

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
	struct RenderData *data = _data;
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
		struct wlr_scene_surface *scene_surface	= wlr_scene_surface_from_node(node);
		struct wlr_surface *wlr_surface = scene_surface->surface;
		struct Surface *surface = find_surface(wlr_surface, data->surfaces);

		texture	= wlr_surface_get_texture(wlr_surface);
		if (texture == NULL) {
			return;
		}

		// In my case (and I think basically always) both transform and	output->transform_matrix
		// are identity	matrices
		transform = wlr_output_transform_invert(wlr_surface->current.transform);

		// The resulting matrix	looks like [w 0	x    0 h y    0	0 1]
		// So it would project coordinates 0..1 to the pixel coordinates of the window
		wlr_matrix_project_box(matrix, &dst_box, transform, 0.0, output->transform_matrix);

		// The source box has the size of the surface. X and Y are always 0, as	far as I can tell.
		struct wlr_fbox	src_box	= {0};

		assert(surface->toplevel != NULL);
		render_texture(output, output_damage, node, texture,
			&src_box, &dst_box, surface->matrix);

		if (data->presentation != NULL && scene_surface->primary_output	== output) {
			wlr_presentation_surface_sampled_on_output(data->presentation, wlr_surface, output);
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

// surfaces should be a list of struct Surface, defined in vkwc.c
bool draw_frame(struct wlr_scene_output *scene_output, struct wl_list *surfaces) {
	// If I	don't do this, windows aren't re-drawn when the	cursor moves.
	// So just damage everything instead. It's inefficient, but I don't care for now.
	wlr_output_damage_add_whole(scene_output->damage);

	// Get the output, i.e.	screen
	struct wlr_output *output = scene_output->output;

	// Get the renderer, i.e. Vulkan or GLES2
	struct wlr_renderer *renderer =	output->renderer;
	assert(renderer	!= NULL);

	// TinyWL used to try to do direct scanout here. But I think there's no point because
	// we want fancy effects that aren't possible with that.

	bool needs_frame;
	pixman_region32_t damage;
	pixman_region32_init(&damage);
	// wlr_output_damage_attach_render: "Attach the	renderer's buffer to the output"
	// "Must call this before rendering, then `wlr_output_set_damage` then `wlr_output_commit`"
	// "needs_frame will be set to true if a frame should be submitted"
	if (!wlr_output_damage_attach_render(scene_output->damage,
			&needs_frame, &damage))	{
		pixman_region32_fini(&damage);
		return false;
	}

	if (!needs_frame) {
		fprintf(stderr, "wlr_output_damage_attach_render says we don't need to draw a frame. This shouldn't "
			" happen.\n");
		exit(1);
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

	// Begin rendering
	wlr_renderer_begin(renderer, output->width, output->height);

	wlr_renderer_scissor(renderer, NULL);
	wlr_renderer_clear(renderer, (float[4]){ 0.3, 0.0, 0.1,	1.0 });

	// Actually draw stuff
	pixman_region32_t full_region;
	pixman_region32_init_rect(&full_region,	0, 0, output->width, output->height);

	struct wlr_scene *scene = scene_output->scene;

	if (output->enabled && pixman_region32_not_empty(&damage)) {
		struct RenderData data = {
			.output	= output,
			.damage	= &damage,
			.presentation =	scene->presentation,
			.surfaces = surfaces
		};
		// scene_output->[xy] determines offset, useful for multiple outputs
		scene_node_for_each_node(&scene->node, -scene_output->x, -scene_output->y,
			render_node_iterator, &data);

		wlr_renderer_scissor(renderer, NULL);
	} else {
		fprintf(stderr, "Output is disabled, dunno what to do\n");
		exit(1);
	}

	pixman_region32_fini(&full_region);

	wlr_output_render_software_cursors(output, &damage);

	// Draw frame counter
	float color[4] = { rand()%2, rand()%2, rand()%2, 1.0 };
	render_rect_simple(renderer, color, 10,	10, 10,	10);

	// Finish
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

