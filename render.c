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
#include <limits.h>

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
#include "vulkan/util.h"
#include "vulkan/timer.h"
#include "util.h"
#include "render/vulkan.h"
#include "vulkan/render_pass.h"

static double start_time = 0;
static int frame_count = 0;

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

        // There might have already been a rect drawn, so reset the timers
        vkCmdResetQueryPool(cbuf, renderer->query_pool, TIMER_RENDER_RECT, 2);

        vulkan_start_timer(cbuf, renderer->query_pool, TIMER_RENDER_RECT);

        // Bind pipeline, if necessary
        VkPipeline pipe = render_buf->render_setup->quad_pipe;
        if (pipe != renderer->bound_pipe) {
                vkCmdBindPipeline(cbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
                renderer->bound_pipe = pipe;
        };

        VkRect2D rect = {{0, 0}, {screen_width, screen_height}};
        renderer->scissor = rect;

        begin_render_pass(cbuf, render_buf->framebuffers[framebuffer_idx],
                render_buf->render_setup->rpass, rect, screen_width, screen_height);

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

        vulkan_end_timer(cbuf, renderer->query_pool, TIMER_RENDER_RECT);
}

// Sometimes we want to set a tight scissor around a window that might be
// rotated weirdly. This figures out the screen coordinates.
void get_rect_for_matrix(int screen_width, int screen_height, mat4 matrix, VkRect2D *rect) {
        // The whole point of making the fragment shader fill was so I'd have
        // some space. This adds it back in.
        int padding = 32;

        // Figure out where the corners end up
        float corners[4][4] = {
                {0, 0, 0, 1},
                {1, 0, 0, 1},
                {0, 1, 0, 1},
                {1, 1, 0, 1}
        };
        int min_x = INT_MAX, min_y = INT_MAX, max_x = INT_MIN, max_y = INT_MIN;
        for (int i = 0; i < 4; i++) {
                float dest[4];
                glm_mat4_mulv(matrix, corners[i], dest);
                dest[0] /= dest[3];
                dest[1] /= dest[3];
                int x = (dest[0] * 0.5 + 0.5) * screen_width;
                int y = (dest[1] * 0.5 + 0.5) * screen_height;

                if (x < min_x) min_x = x;
                if (y < min_y) min_y = y;
                if (x > max_x) max_x = x;
                if (y > max_y) max_y = y;
        }
        min_x -= padding;
        min_y -= padding;
        max_x += padding;
        max_y += padding;

        if (min_x < 0) min_x = 0;
        if (min_y < 0) min_y = 0;
        if (max_x > screen_width) max_x = screen_width;
        if (max_y > screen_height) max_y = screen_height;

        rect->offset.x = min_x;
        rect->offset.y = min_y;
        rect->extent.width = max_x - min_x;
        rect->extent.height = max_y - min_y;
}


// Set render_uv to false to, well, not render to the UV texture. That will
// make it so mouse events go "through" the surface and to whatever's below
// instead.
void render_texture(struct wlr_renderer *wlr_renderer,
                struct wlr_texture *wlr_texture, mat4 matrix, float surface_id, bool render_uv) {
	struct wlr_vk_renderer *renderer = (struct wlr_vk_renderer *) wlr_renderer;
        struct wlr_vk_render_buffer *render_buf = renderer->current_render_buffer;

        double start_time = get_time();

        int screen_width = render_buf->wlr_buffer->width;
        int screen_height = render_buf->wlr_buffer->height;

        VkCommandBuffer cbuf = renderer->cb;
        assert(render_buf != NULL);
        assert(cbuf != NULL);

        // There might have already been a texture rendered, so reset the timers
        vkCmdResetQueryPool(cbuf, renderer->query_pool, TIMER_RENDER_TEXTURE, 2);
        vkCmdResetQueryPool(cbuf, renderer->query_pool, TIMER_RENDER_TEXTURE_1, 2);
        // Start GPU timer
        vulkan_start_timer(cbuf, renderer->query_pool, TIMER_RENDER_TEXTURE);

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

        // Actually copy - this costs about 0.5 ms, or 1/3rd of entire
        // render_texture. Not good!
        vulkan_copy_image(cbuf, render_buf->intermediates[prev_idx],
                render_buf->intermediates[framebuffer_idx],
                VK_IMAGE_ASPECT_COLOR_BIT,
                0, 0, 0, 0, screen_width, screen_height);

        // Transition back to TRANSFER_SRC_OPTIMAL because that's what the
        // render pass expects. It's a bit of a weird setup, but it's easier
        // because render_rect_simple and the other render_subtexture bind the
        // render pass without copying first so TRANSFER_SRC is better for
        // them. Alternatively, I could change the render pass to expect
        // TRANSFER_DST and make render_rect_simple and render_subtexture do
        // transitions.
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

        // Bind pipeline and descriptor sets
	VkPipeline pipe	= renderer->current_render_buffer->render_setup->tex_pipe;
	if (pipe != renderer->bound_pipe) {
		vkCmdBindPipeline(cbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
		renderer->bound_pipe = pipe;
	}

        // I could scissor to only the region being drawn to. I'm not sure it's
        // worth it though, especially because it gets complicated with
        // spinning surfaces and such.
        VkRect2D rect;
        get_rect_for_matrix(screen_width, screen_height, matrix, &rect);
        renderer->scissor = rect;

        // Starts the command buffer and enters the render pass
        begin_render_pass(cbuf, render_buf->framebuffers[framebuffer_idx],
                render_buf->render_setup->rpass, rect, screen_width, screen_height);

        VkDescriptorSet desc_sets[] =
                {render_buf->intermediate_sets[prev_idx], texture->ds};

	vkCmdBindDescriptorSets(cbuf, VK_PIPELINE_BIND_POINT_GRAPHICS,
		renderer->pipe_layout, 0, sizeof(desc_sets) / sizeof(desc_sets[0]),
                desc_sets, 0, NULL);

	// Draw
	struct PushConstants push_constants;
        glm_mat4_inv(matrix, push_constants.mat4);

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

        // This costs about 0.9ms, which sucks! It's actually not the fragment
        // shader - we can discard() literally every fragment and it only
        // decreases to 0.8. I guess it's just the cost of outputting to two
        // fullscreen buffers.
        vulkan_start_timer(cbuf, renderer->query_pool, TIMER_RENDER_TEXTURE_1);
	vkCmdDraw(cbuf, 4, 1, 0, 0);
        vulkan_end_timer(cbuf, renderer->query_pool, TIMER_RENDER_TEXTURE_1);

        // Finish
	vkCmdEndRenderPass(cbuf);

        // Transition back to TRANSFER_SRC_OPTIMAL
        vulkan_image_transition_cbuf(cbuf,
                render_buf->intermediates[framebuffer_idx], VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                1);
        // End GPU timer
        vulkan_end_timer(cbuf, renderer->query_pool, TIMER_RENDER_TEXTURE);

        // I don't really know what this does, vulkan_texture_destroy uses it
        texture->last_used = renderer->frame;

        printf("\t[CPU] render_texture: %5.3f ms\n", (get_time() - start_time) * 1000);

}

static void render_surface(struct wlr_output *output, struct Surface *surface) {
	struct wlr_texture *texture = wlr_surface_get_texture(surface->wlr_surface);
	if (texture == NULL) {
                //printf("Could not render surface (dims %d %d)\n", surface->width, surface->height);
                return;
        }

        // Only make the surface clickable if it's an XDG surface.
        bool render_uv = surface->xdg_surface != NULL;

	render_texture(output->renderer, texture, surface->matrix, surface->id, render_uv);
}

// Comparison function so we can qsort surfaces by Z.
int surface_comp(const void *a, const void *b) {
        // That's a lot of parentheses!
        float a_z = (*((struct Surface **) a))->z;
        float b_z = (*((struct Surface **) b))->z;

        return (a_z > b_z) - (a_z < b_z);
}

static void render_begin(struct wlr_renderer *wlr_renderer, uint32_t width, uint32_t height) {
	struct wlr_vk_renderer *renderer = vulkan_get_renderer(wlr_renderer);
	assert(renderer->current_render_buffer);
        struct wlr_vk_render_buffer *render_buf = renderer->current_render_buffer;
        assert(render_buf != NULL);
        VkCommandBuffer cbuf = renderer->cb;

        render_buf->framebuffer_idx = 0;

        if (!renderer->stage.recording) {
                // Sometimes stage.recording *is* true, unlike vulkan_begin. So
                // I guess the cursor renders before us? Hardware cursors are
                // whack, man.
                cbuf_begin_onetime(cbuf);
                renderer->stage.recording = true;
        }

        // Reset timers
        vkCmdResetQueryPool(cbuf, renderer->query_pool, 0, TIMER_COUNT);

        // Start GPU timers
        vulkan_start_timer(cbuf, renderer->query_pool, TIMER_RENDER_BEGIN);
        vulkan_start_timer(cbuf, renderer->query_pool, TIMER_EVERYTHING);

	renderer->render_width = width;
	renderer->render_height = height;
	renderer->bound_pipe = VK_NULL_HANDLE;

        // Clear the first intermediate image, otherwise we have leftover junk
        // from the previous frame.
        // Transition it to TRANSFER_DST_OPTIMAL so we can clear it
        vulkan_image_transition_cbuf(cbuf,
                render_buf->intermediates[0], VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_ACCESS_NONE, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                1);

        vulkan_clear_image(cbuf, render_buf->intermediates[0], (float [4]) {0.1, 0.1, 0.1, 1});

        // Clear the UV buffer too
        // Transition it to TRANSFER_DST_OPTIMAL so we can clear it
        vulkan_image_transition_cbuf(cbuf,
                render_buf->uv, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_ACCESS_NONE, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                1);

        // The clears are very slow, about 0.3ms each.
        vulkan_start_timer(cbuf, renderer->query_pool, TIMER_RENDER_BEGIN_1);
        vulkan_clear_image(cbuf, render_buf->uv, (float [4]){0, 0, 0, 1});
        vulkan_end_timer(cbuf, renderer->query_pool, TIMER_RENDER_BEGIN_1);

        // Transition UV back to COLOR_ATTACHMENT_OPTIMAL
        vulkan_image_transition_cbuf(cbuf,
                render_buf->uv, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT,
                // Make fragment shader reads wait on it as well as color
                // attachment output
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                1);


        // Transition all intermediates to TRANSFER_SRC, because when we start
        // rendering surfaces, it is assumed that the previous intermediate is
        // already in TRANSFER_SRC.

        for (int i = 0; i < INTERMEDIATE_IMAGE_COUNT; i++) {
                VkImageLayout src_layout = VK_IMAGE_LAYOUT_UNDEFINED;

                // We just transitioned one to TRANSFER_DST, so take that into account
                if (i == 0) src_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

                vulkan_image_transition_cbuf(cbuf,
                        render_buf->intermediates[i], VK_IMAGE_ASPECT_COLOR_BIT,
                        src_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT
                                | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                        1);
        }

        // Transition depth to DEPTH_STENCIL_ATTACHMENT_OPTIMAL - usually the
        // render passes in the draw commands do this automatically. But if
        // nothing gets drawn, that doesn't happen, and render_end excepts the
        // depth buffer to be DEPTH_STENCIL
        vulkan_image_transition_cbuf(cbuf,
                render_buf->depth, VK_IMAGE_ASPECT_DEPTH_BIT,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                VK_ACCESS_NONE, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                1);

        // End GPU timer
        vulkan_end_timer(cbuf, renderer->query_pool, TIMER_RENDER_BEGIN);
}

void render_end(struct wlr_renderer *wlr_renderer) {
	struct wlr_vk_renderer *renderer = vulkan_get_renderer(wlr_renderer);
	assert(renderer->current_render_buffer);
        struct wlr_vk_render_buffer *render_buf = renderer->current_render_buffer;
        VkCommandBuffer cbuf = renderer->cb;

        double start_time = get_time();

        int width = renderer->render_width;
        int height = renderer->render_height;

        // Start GPU timer
        vulkan_start_timer(cbuf, renderer->query_pool, TIMER_RENDER_END);

	// Copy UV to host-visible memory, but only the pixel under the cursor
        // Transition UV to TRANSFER_SRC_OPTIMAL
        vulkan_image_transition_cbuf(cbuf,
                render_buf->uv, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, 1);

        assert(renderer->cursor_x < width);
        assert(renderer->cursor_y < height);

        VkBufferImageCopy uv_copy_region = {
                .bufferOffset = 0,
                .bufferRowLength = width,
                .bufferImageHeight = height,
                .imageSubresource = {
                        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                        .mipLevel = 0,
                        .baseArrayLayer = 0,
                        .layerCount = 1,
                },
                .imageOffset = { .x = renderer->cursor_x, .y = renderer->cursor_y, .z = 0 },
                .imageExtent = {
                        .width = 1,
                        .height = 1,
                        .depth = 1,
                },
        };

        vkCmdCopyImageToBuffer(cbuf,
                render_buf->uv, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                render_buf->host_uv,
                1, &uv_copy_region);

        // Postprocess pass
        struct wlr_vk_render_format_setup *setup = render_buf->render_setup;
        vkCmdBindPipeline(cbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, setup->postprocess_pipe);
        renderer->bound_pipe = setup->postprocess_pipe;

        int framebuffer_idx = render_buf->framebuffer_idx;

        VkRect2D rect = {{0, 0}, {width, height}};
        renderer->scissor = rect;

        // Transition intermediate ot SHADER_READ_ONLY_OPTIMAL - I wanted the
        // render pass to this for me but it doesn't for whatever reason.
        vulkan_image_transition_cbuf(cbuf,
                render_buf->intermediates[framebuffer_idx], VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                1);

        // Transition UV to SHADER_READ_ONLY
        vulkan_image_transition_cbuf(cbuf,
                render_buf->uv, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                1);

        // Begin render pass
        begin_postprocess_render_pass(renderer->cb,
                render_buf->postprocess_framebuffers[framebuffer_idx],
                setup->postprocess_rpass, rect, width, height);

        // Bind descriptors
        VkDescriptorSet desc_sets[] =
                {render_buf->intermediate_sets[framebuffer_idx], render_buf->uv_set};

	vkCmdBindDescriptorSets(cbuf, VK_PIPELINE_BIND_POINT_GRAPHICS,
		renderer->pipe_layout, 0, sizeof(desc_sets) / sizeof(desc_sets[0]),
                desc_sets, 0, NULL);

        // We don't actually use the PushConstants struct, so this is a bit
        // cheeky. But the int fits so it's OK.
	vkCmdPushConstants(cbuf, renderer->pipe_layout,
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                0, sizeof(renderer->postprocess_mode), &renderer->postprocess_mode);
        vkCmdDraw(cbuf, 4, 1, 0, 0);

        vkCmdEndRenderPass(cbuf);

        // End GPU timer
        vulkan_end_timer(cbuf, renderer->query_pool, TIMER_EVERYTHING);
        vulkan_end_timer(cbuf, renderer->query_pool, TIMER_RENDER_END);

        // Submit
        double pre_submit_time = get_time();
        double elapsed = (pre_submit_time - start_time) * 1000;
        printf("\t[CPU] render_end up to submit: %5.3f ms\n", elapsed);

        cbuf_submit_wait(renderer->dev->queue, renderer->cb);

        elapsed = (get_time() - pre_submit_time) * 1000;
        printf("\t[CPU] Submit: %5.2f ms\n", elapsed);

        renderer->stage.recording = false;
	renderer->bound_pipe = VK_NULL_HANDLE;
	renderer->render_width = 0;
	renderer->render_height = 0;

        // Check GPU timestamps
        printf("\t[GPU] render_begin: %5.3f ms\n",
                vulkan_get_elapsed(renderer->dev->dev, renderer->query_pool,
                        renderer->dev->instance->timestamp_period, TIMER_RENDER_BEGIN) * 1000);
        printf("\t[GPU] render_begin subsection: %5.3f ms\n",
                vulkan_get_elapsed(renderer->dev->dev, renderer->query_pool,
                        renderer->dev->instance->timestamp_period, TIMER_RENDER_BEGIN_1) * 1000);
        printf("\t[GPU] Most recent render_rect: %5.3f ms\n",
                vulkan_get_elapsed(renderer->dev->dev, renderer->query_pool,
                        renderer->dev->instance->timestamp_period, TIMER_RENDER_RECT) * 1000);
        printf("\t[GPU] Most recent render_texture: %5.3f ms\n",
                vulkan_get_elapsed(renderer->dev->dev, renderer->query_pool,
                        renderer->dev->instance->timestamp_period, TIMER_RENDER_TEXTURE) * 1000);
        printf("\t[GPU] Most recent render_texture subsection: %5.3f ms\n",
                vulkan_get_elapsed(renderer->dev->dev, renderer->query_pool,
                        renderer->dev->instance->timestamp_period, TIMER_RENDER_TEXTURE_1) * 1000);
        printf("\t[GPU] render_end: %5.3f ms\n",
                vulkan_get_elapsed(renderer->dev->dev, renderer->query_pool,
                        renderer->dev->instance->timestamp_period, TIMER_RENDER_END) * 1000);
        printf("\t[GPU] Entire pipeline: %5.3f ms\n",
                vulkan_get_elapsed(renderer->dev->dev, renderer->query_pool,
                        renderer->dev->instance->timestamp_period, TIMER_EVERYTHING) * 1000);

        // Destroy pending textures
        struct wlr_vk_texture *texture, *tmp_tex;
        wl_list_for_each_safe(texture, tmp_tex, &renderer->destroy_textures, destroy_link) {
                printf("Destroy texture %p\n", texture);
                wlr_texture_destroy(&texture->wlr_texture);
        }

        // This marks it as the most recent I think
        renderer->frame++;
        render_buf->frame = renderer->frame;

        // "release stage allocations", not sure what it really does
	struct wlr_vk_shared_buffer *buf;
	wl_list_for_each(buf, &renderer->stage.buffers, link) {
		buf->allocs_size = 0u;
	}

        renderer->render_width = 0;
        renderer->render_height = 0;
}

// `surfaces` should be a list of struct Surface, defined in vkwc.c
bool draw_frame(struct wlr_output *output, struct wl_list *surfaces, int cursor_x, int cursor_y) {
        if (start_time == 0) {
                start_time = get_time();
        }

        double frame_start_time = get_time();

	// Get the renderer, i.e. Vulkan or GLES2
	struct wlr_renderer *renderer =	output->renderer;
	assert(renderer	!= NULL);

	int buffer_age = -1;
	wlr_output_attach_render(output, &buffer_age);

	struct wlr_vk_renderer *vk_renderer = (struct wlr_vk_renderer *) renderer;
	render_begin(renderer, output->width, output->height);

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
	render_end(renderer);

        renderer->rendering = false;

        double total_elapsed = get_time() - start_time;
        double framerate = (double) frame_count / total_elapsed;
        double frame_ms = (get_time() - frame_start_time) * 1000;

	vk_renderer->cursor_x = cursor_x;
	vk_renderer->cursor_y = cursor_y;

	int tr_width, tr_height;
	wlr_output_transformed_resolution(output, &tr_width, &tr_height);

        printf("Average FPS: %10.5f, ms this frame: %5.2f\n", framerate, frame_ms);

        frame_count++;

	return wlr_output_commit(output);
}

// How the layouts work:
// render_begin / vulkan_begin sets everything to TRANSFER_SRC
//
// render_subexture in vulkan/renderer.c expects [framebuffer_idx] to be
// TRANSFER_SRC, and sets it to TRANSFER_SRC when it's done.
//
// render_rect_simple is the same
//
// render_subtexture here expects [prev_idx] to be 
