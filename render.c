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
#include <assert.h>

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
#include "vulkan/intermediate.h"

struct RenderData {
	struct wlr_output *output;
	pixman_region32_t *damage;
	struct wlr_presentation	*presentation; // May be NULL
	struct wl_list *surfaces;
	mat4 view;
	mat4 projection;
};

void render_rect_simple(struct wlr_renderer *wlr_renderer, const float color[4],
                int x, int y, int width, int height) {
	struct wlr_vk_renderer *renderer = (struct wlr_vk_renderer *) wlr_renderer;
        struct wlr_vk_render_buffer *render_buf = renderer->current_render_buffer;

        int framebuffer_idx = render_buf->framebuffer_idx;

        int screen_width = render_buf->wlr_buffer->width;
        int screen_height = render_buf->wlr_buffer->height;

        VkCommandBuffer cbuf = renderer->cb;
        assert(render_buf != NULL);
        assert(cbuf != NULL);

	VkRect2D rect = {{0, 0}, {screen_width, screen_height}};
	renderer->scissor = rect;

        VkPipeline pipe = render_buf->render_setup->quad_pipe;
        if (pipe != renderer->bound_pipe) {
                vkCmdBindPipeline(cbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
                renderer->bound_pipe = pipe;
        };

        // We don't bother rendering from one surface to the other because we
        // don't support fancy blurred transparency stuff here. So we don't
        // have to copy one image to the other, just transition it to
        // TRANSFER_DST (which the render pass expects) and draw.
 
        struct PushConstants push_constants;
        memcpy(push_constants.color, color, 4 * sizeof(color[0]));

        mat4 matrix;
        glm_mat4_identity(matrix);

        // These are in backwards order
        // Move 0..2, 0..2 to -1..1, -1..1
        glm_translate(matrix, (vec3) {-1, -1, 0});
        // Scale it down from 0..width, 0..height to 0..2, 0..2
        glm_scale(matrix, (vec3) {2.0 / screen_width, 2.0 / screen_height, 1});
        // Move it over by x, y
        glm_translate(matrix, (vec3) {x, y, 0});
        // Scale 0..1, 0..1 up to 0..width, 0..height
        glm_scale(matrix, (vec3) {width, height, 1});

        // Unfortunately the rest of wlroots is row-major, otherwise I would
        // set column-major in the shader and avoid this
	for (int i = 0; i < 4; i++) {
		for (int j = 0; j < 4; j++) {
			push_constants.mat4[i][j] = matrix[j][i];
		}
	};

        // Begin render pass
        begin_render_operation(cbuf, render_buf->framebuffers[framebuffer_idx],
                render_buf->render_setup->render_pass, rect, screen_width, screen_height);

	vkCmdPushConstants(cbuf, renderer->pipe_layout,
		VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                0, sizeof(push_constants), &push_constants);
        vkCmdDraw(cbuf, 4, 1, 0, 0);

        vkCmdEndRenderPass(cbuf);

        // Transition back to TRANSFER_SRC_OPTIMAL
        vulkan_image_transition_cbuf(cbuf,
                render_buf->intermediates[framebuffer_idx], VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                // If someone else draws another quad, we want them to wait. So
                // we have both here.
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT, 
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                1);
}

// This is separate from vulkan_render_subtexture_with_matrix in
// vulkan/renderer.c - that has the signature needed to fill wlr_renderer_impl.
// I think it only gets used for the cursor, because I do everything else "by
// hand". Anyway, this one has whatever signature I want, so we can support
// stuff like setting the surface ID or not drawing UV at all.
//
// Set render_uv to false to, well, not render to the UV texture. That will
// make it so mouse events go "through" the surface and to whatever's below
// instead.
void render_subtexture_with_matrix(struct wlr_renderer *wlr_renderer,
                struct wlr_texture *wlr_texture, mat4 matrix, float surface_id, bool render_uv) {
	struct wlr_vk_renderer *renderer = (struct wlr_vk_renderer *) wlr_renderer;
        struct wlr_vk_render_buffer *render_buf = renderer->current_render_buffer;

        int screen_width = render_buf->wlr_buffer->width;
        int screen_height = render_buf->wlr_buffer->height;

        VkCommandBuffer cbuf = renderer->cb;
        assert(render_buf != NULL);
        assert(cbuf != NULL);

        // I could scissor to only the region being drawn to. I'm not sure it's
        // worth it though, especially because it gets complicated with
        // spinning surfaces and such.
	VkRect2D rect = {{0, 0}, {screen_width, screen_height}};
	renderer->scissor = rect;

        // Copy the pixels from the previous buffer into this one
        // Previous image is already in IMAGE_LAYOUT_TRANSFER_SRC
        int prev_idx = render_buf->framebuffer_idx;
        int framebuffer_idx = (prev_idx + 1) % INTERMEDIATE_IMAGE_COUNT;
        render_buf->framebuffer_idx = framebuffer_idx;

        // Transition current image to TRANSFER_DST
        vulkan_image_transition_cbuf(cbuf,
                render_buf->intermediates[framebuffer_idx], VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                1);

        // Actually copy
        vulkan_copy_image(cbuf, render_buf->intermediates[prev_idx],
                render_buf->intermediates[framebuffer_idx],
                VK_IMAGE_ASPECT_COLOR_BIT,
                0, 0, 0, 0, screen_width, screen_height);

        // Transition back to TRANSFER_SRC_OPTIMAL because that's what the
        // render pass expects. It's a bit of a weird setup, but it's easier
        // because render_rect_simple and the other render_subtexture bind the
        // render pass without copying first so TRANSFER_SRC is better for
        // them.
        vulkan_image_transition_cbuf(cbuf,
                render_buf->intermediates[framebuffer_idx], VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                1);

        // Transition the previous image to SHADER_READ_ONLY_OPTIMAL
        vulkan_image_transition_cbuf(cbuf,
                render_buf->intermediates[prev_idx], VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                1);

        // Setup stuff for the texture we're about to render
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

        // Starts the command buffer and enters the render pass
        // TODO: get rid of vulkan/intermediate.c or at least make it clearer what it does
        begin_render_operation(cbuf, render_buf->framebuffers[framebuffer_idx],
                render_buf->render_setup->render_pass, rect, screen_width, screen_height);

        // Bind pipeline and descriptor sets
	VkPipeline pipe	= renderer->current_render_buffer->render_setup->tex_pipe;
	if (pipe != renderer->bound_pipe) {
		vkCmdBindPipeline(cbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
		renderer->bound_pipe = pipe;
	}

        VkDescriptorSet desc_sets[] =
                {render_buf->intermediate_sets[prev_idx], texture->ds};

	vkCmdBindDescriptorSets(cbuf, VK_PIPELINE_BIND_POINT_GRAPHICS,
		renderer->pipe_layout, 0, sizeof(desc_sets) / sizeof(desc_sets[0]),
                desc_sets, 0, NULL);

	// Draw
        // Unfortunately the rest of wlroots is row-major, otherwise I would
        // set column-major in the shader and avoid this
        // TODO: since I've replaced all the wlroots stuff I could switch to
        // column-major
	struct PushConstants push_constants;
	for (int i = 0; i < 4; i++) {
		for (int j = 0; j < 4; j++) {
			push_constants.mat4[i][j] = matrix[j][i];
		}
	};

	// This used to be more complicated. Go back to TinyWL's way if something breaks.
	push_constants.uv_off[0] = 0;
	push_constants.uv_off[1] = 0;
	push_constants.uv_size[0] = 1;
	push_constants.uv_size[1] = 1;
        push_constants.surface_id[0] = surface_id;
        push_constants.surface_id[1] = render_uv ? 1 : 0;
        push_constants.screen_dims[0] = screen_width;
        push_constants.screen_dims[1] = screen_height;

	vkCmdPushConstants(cbuf, renderer->pipe_layout,
		VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                0, sizeof(push_constants), &push_constants);
	vkCmdDraw(cbuf, 4, 1, 0, 0);

        // Finish
	vkCmdEndRenderPass(cbuf);

        // Transition back to TRANSFER_SRC_OPTIMAL
        vulkan_image_transition_cbuf(cbuf,
                render_buf->intermediates[framebuffer_idx], VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                1);

        // I don't really know what this does, vulkan_texture_destroy uses it
        texture->last_used = renderer->frame;
}

static void render_texture(struct wlr_output *output, struct wlr_texture *texture, mat4 matrix,
                float surface_id, bool render_uv) {
	struct wlr_renderer *renderer =	output->renderer;
	assert(renderer);

	render_subtexture_with_matrix(renderer, texture, matrix, surface_id, render_uv);
}

static void render_surface(struct wlr_output *output, struct Surface *surface) {
	struct wlr_texture *texture = wlr_surface_get_texture(surface->wlr_surface);
	if (texture == NULL) {
                //printf("Could not render surface (dims %d %d)\n", surface->width, surface->height);
                return;
        }

        // Only make the surface clickable if it's an XDG surface.
        bool render_uv = surface->xdg_surface != NULL;

	render_texture(output, texture, surface->matrix, surface->id, render_uv);
}

// Comparison function so we can qsort surfaces by Z.
int surface_comp(const void *a, const void *b) {
        // That's a lot of parentheses!
        float a_z = (*((struct Surface **) a))->z;
        float b_z = (*((struct Surface **) b))->z;

        return (a_z > b_z) - (a_z < b_z);
}

// `surfaces` should be a list of struct Surface, defined in vkwc.c
bool draw_frame(struct wlr_output *output, struct wl_list *surfaces, int cursor_x, int cursor_y) {
	// Get the renderer, i.e. Vulkan or GLES2
	struct wlr_renderer *renderer =	output->renderer;
	assert(renderer	!= NULL);

	int buffer_age = -1;
	wlr_output_attach_render(output, &buffer_age);

	wlr_renderer_begin(renderer, output->width, output->height);

        // Sort the surfaces by distance from the camera
        int surface_count = 0;
        struct Surface *surface;
	wl_list_for_each(surface, surfaces, link) {
                surface_count++;
	};

        struct Surface **surfaces_sorted = malloc(sizeof(surfaces_sorted[0]) * surface_count);
        int surface_idx = 0;
	wl_list_for_each(surface, surfaces, link) {
                surfaces_sorted[surface_idx++] = surface;
	};
        assert(surface_idx == surface_count);

        qsort(surfaces_sorted, surface_count, sizeof(surfaces_sorted[0]), surface_comp);

        // Draw frame counter. render_rect_simple doesn't draw from one
        // framebuffer into the other, we don't have to increment framebuffer_idx
	float color[4] = { rand()%2, rand()%2, rand()%2, 1.0 };
	render_rect_simple(renderer, color, 10, 10, 10, 10);

	// Draw each surface
        for (int i = 0; i < surface_count; i++) {
                struct Surface *surface = surfaces_sorted[i];
                if (surface->width == 0 && surface->height == 0) {
                        continue;
                }

		render_surface(output, surface);
	};

	// Finish
	wlr_renderer_end(renderer);

	struct wlr_vk_renderer * vk_renderer = (struct wlr_vk_renderer *) renderer;
	vk_renderer->cursor_x = cursor_x;
	vk_renderer->cursor_y = cursor_y;

	int tr_width, tr_height;
	wlr_output_transformed_resolution(output, &tr_width, &tr_height);

	return wlr_output_commit(output);
}

// How the layouts work:
// vulkan_begin sets everything to TRANSFER_SRC
//
// render_subexture in vulkan/renderer.c expects [framebuffer_idx] to be
// TRANSFER_SRC, and sets it to TRANSFER_SRC when it's done.
//
// render_rect_simple is the same
//
// render_subtexture here expects [prev_idx] to be 
