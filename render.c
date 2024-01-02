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
	mat4 view;
	mat4 projection;
};

void render_rect_simple(struct wlr_renderer *renderer, const float color[4], int x, int	y, int width, int height) {
	struct wlr_box box = { .x = x, .y = y, .width =	width, .height = height	};
	float identity_matrix[9] = { 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0,	0.0, 1.0 };
	wlr_render_rect(renderer, &box,	color, identity_matrix);
}

static bool render_subtexture_with_matrix(struct wlr_renderer *wlr_renderer, struct wlr_texture	*wlr_texture,
		mat4 matrix, float surface_id) {
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
	// Unfortunately the rest of wlroots is row-major, otherwise I would set column-major in the shader
	// and avoid this
	struct VertPcrData VertPcrData;
	for (int i = 0; i < 4; i++) {
		for (int j = 0; j < 4; j++) {
			VertPcrData.mat4[i][j] = matrix[j][i];
		}
	};

	// This used to be more complicated. Go back to TinyWL's way if something breaks.
	VertPcrData.uv_off[0] =	0;
	VertPcrData.uv_off[1] =	0;
	VertPcrData.uv_size[0] = 1;
	VertPcrData.uv_size[1] = 1;

	vkCmdPushConstants(cb, renderer->pipe_layout,
		VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(VertPcrData), &VertPcrData);
	vkCmdPushConstants(cb, renderer->pipe_layout,
		VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(VertPcrData), sizeof(float), &surface_id);
	vkCmdDraw(cb, 4, 1, 0, 0);

	return true;
}

static void render_texture(struct wlr_output *output, struct wlr_texture *texture, mat4 matrix, float surface_id) {
	struct wlr_renderer *renderer =	output->renderer;
	assert(renderer);

	wlr_renderer_scissor(renderer, NULL);
	render_subtexture_with_matrix(renderer, texture, matrix, surface_id);
}

static void render_surface(struct wlr_output *output, struct Surface *surface) {
	struct wlr_texture *texture = wlr_surface_get_texture(surface->wlr_surface);
	if (texture == NULL) return;

	render_texture(output, texture, surface->matrix, surface->id);
}

// surfaces should be a list of struct Surface, defined in vkwc.c
// physics_width and physics_height are needed to transform coordinates from the physics engine to screenspace.
bool draw_frame(struct wlr_output *output, struct wl_list *surfaces, int cursor_x, int cursor_y) {
	// Get the renderer, i.e. Vulkan or GLES2
	struct wlr_renderer *renderer =	output->renderer;
	assert(renderer	!= NULL);

	// TinyWL used to try to do direct scanout here. But I think there's no point because
	// we want fancy effects that aren't possible with that.

	int buffer_age = -1;
	wlr_output_attach_render(output, &buffer_age);

	// Begin rendering
	wlr_renderer_begin(renderer, output->width, output->height);

	wlr_renderer_scissor(renderer, NULL);

	// Actually draw stuff
	struct Surface *surface;
	int surface_count = 0;
	wl_list_for_each(surface, surfaces, link) {
		render_surface(output, surface);
		surface_count++;
	};

	// Draw frame counter
	float color[4] = { rand()%2, rand()%2, rand()%2, 1.0 };
	render_rect_simple(renderer, color, 10,	10, 10, 10);

	// Finish
	struct wlr_vk_renderer * vk_renderer = (struct wlr_vk_renderer *) renderer;
	vk_renderer->cursor_x = cursor_x;
	vk_renderer->cursor_y = cursor_y;
	vk_renderer->should_copy_uv = true;
	wlr_renderer_end(renderer);
	vk_renderer->should_copy_uv = false;

	int tr_width, tr_height;
	wlr_output_transformed_resolution(output, &tr_width, &tr_height);

	return wlr_output_commit(output);
}

// End my stuff

