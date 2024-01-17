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

        begin_render_pass(cbuf, render_buf->framebuffer,
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

        vulkan_end_timer(cbuf, renderer->query_pool, TIMER_RENDER_RECT);
}

// Sometimes we want to set a tight scissor around a window that might be
// rotated weirdly. This figures out the screen coordinates.
void get_rect_for_matrix(int screen_width, int screen_height, mat4 matrix, VkRect2D *rect) {
        // The whole point of making the fragment shader fill was so I'd have
        // some space. This adds it back in.
        int padding = 128;

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
        if (max_x <= min_x) max_x = min_x + 1;
        if (max_y <= min_y) max_y = min_y + 1;

        rect->offset.x = min_x;
        rect->offset.y = min_y;
        rect->extent.width = max_x - min_x;
        rect->extent.height = max_y - min_y;
}

void debug_images(struct wlr_renderer *wlr_renderer) {
	struct wlr_vk_renderer *renderer = (struct wlr_vk_renderer *) wlr_renderer;
        struct wlr_vk_render_buffer *render_buf = renderer->current_render_buffer;

        wlr_log(WLR_DEBUG, "Intermediate image is at %p", render_buf->intermediate);
        wlr_log(WLR_DEBUG, "UV is at %p", render_buf->uv);
}

// Assumes image is in SHADER_READ_ONLY. If with_threshold is set, a threshold
// will first be applied to the image. So you end up with just the bright parts
// blurred.
void blur_image(struct wlr_vk_renderer *renderer, VkRect2D rect,
                int screen_width, int screen_height, int pass_count, VkDescriptorSet *src_image_set,
                bool with_threshold) {
        assert(pass_count <= BLUR_PASSES);

        VkCommandBuffer cbuf = renderer->cb;
        struct wlr_vk_render_buffer *render_buf = renderer->current_render_buffer;

        double start_time = get_time();

        // There might have already been a texture rendered, so reset the timers
        vkCmdResetQueryPool(cbuf, renderer->query_pool, TIMER_BLUR, 2);
        vkCmdResetQueryPool(cbuf, renderer->query_pool, TIMER_BLUR_1, 2);

        vulkan_start_timer(cbuf, renderer->query_pool, TIMER_BLUR);

        int last_image_idx = 0;
        for (int i = 0; i < 2 * pass_count - 1; i++) {
                int image_idx;
                if (i < pass_count) {
                        // Always blur to the next image while downsampling
                        image_idx = i;
                } else {
                        // When i == pass_count, we want to blur to
                        // [pass_count - 2], since that's the one before last.
                        image_idx = 2 * pass_count - i - 2;
                }

                float blur_scale = 1.0 / (2 << image_idx);
                int width = screen_width * blur_scale;
                int height = screen_height * blur_scale;

                if (i != 0) {
                        // Unless we're on the first pass, we have to transition
                        // the previous blur image to SHADER_READ_ONLY
                        vulkan_image_transition_cbuf(cbuf,
                                render_buf->blurs[last_image_idx], VK_IMAGE_ASPECT_COLOR_BIT,
                                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                VK_ACCESS_SHADER_READ_BIT,
                                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                1);
                }

                VkPipeline pipe	=
                        renderer->current_render_buffer->render_setup->blur_pipes[image_idx];
                if (pipe != renderer->bound_pipe) {
                        vkCmdBindPipeline(cbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
                        renderer->bound_pipe = pipe;
                }

                VkRect2D blur_rect;
                blur_rect.offset.x = rect.offset.x * blur_scale;
                blur_rect.offset.y = rect.offset.y * blur_scale;
                blur_rect.extent.width = rect.extent.width * blur_scale;
                blur_rect.extent.height = rect.extent.height * blur_scale;
                if (blur_rect.extent.width < 1) blur_rect.extent.width = 1;
                if (blur_rect.extent.height < 1) blur_rect.extent.height = 1;

                begin_render_pass(cbuf, render_buf->blur_framebuffers[image_idx],
                        render_buf->render_setup->blur_rpass[image_idx],
                        blur_rect, width, height);

                VkDescriptorSet *in_set;
                if (i == 0) {
                        in_set = src_image_set;
                } else {
                        in_set = &render_buf->blur_sets[last_image_idx];
                }

                vkCmdBindDescriptorSets(cbuf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        renderer->pipe_layout, 0, 1, in_set, 0, NULL);

                struct PushConstants push_constants = {0};
                // Important to feed in the dimensions of the image we're
                // sampling and not our own image here
                push_constants.screen_dims[0] = width;
                push_constants.screen_dims[1] = height;
                if (i >= pass_count) {
                        // We're upsampling, we reuse is_focused to set that
                        push_constants.is_focused = 1;
                } else if (i == 0 && with_threshold) {
                        // Downsample and use threshold. TODO: Less hacky method...
                        push_constants.is_focused = 2;
                }

                vkCmdPushConstants(cbuf, renderer->pipe_layout,
                        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                        0, sizeof(push_constants), &push_constants);

                vkCmdDraw(cbuf, 4, 1, 0, 0);

                vkCmdEndRenderPass(cbuf);

                last_image_idx = image_idx;
        }

        vulkan_end_timer(cbuf, renderer->query_pool, TIMER_BLUR);

        wlr_log(WLR_DEBUG, "\t[CPU] blur: %5.3f ms", (get_time() - start_time) * 1000);
}

// Set render_uv to false to, well, not render to the UV texture. That will
// make it so mouse events go "through" the surface and to whatever's below
// instead.
void render_texture(struct wlr_renderer *wlr_renderer,
                struct wlr_texture *wlr_texture, mat4 matrix,
                int surface_width, int surface_height, bool is_focused,
                float time_since_spawn,
                float surface_id, bool render_uv) {
        wlr_log(WLR_DEBUG, "Render texture with dims %d %d", surface_width, surface_height);
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

        // Scissor to only the window being drawn + some padding
        VkRect2D rect;
        get_rect_for_matrix(screen_width, screen_height, matrix, &rect);
        renderer->scissor = rect;

        // Blur
        // Transition intermediate to SHADER_READ
        vulkan_start_timer(cbuf, renderer->query_pool, TIMER_RENDER_TEXTURE_1);
        vulkan_image_transition_cbuf(cbuf,
                render_buf->intermediate, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                1);

        blur_image(renderer, rect, screen_width, screen_height, BLUR_PASSES,
                &render_buf->intermediate_set, false);

        // The blur takes almost 1ms on the CPU! Not great. I think I could
        // reduce this by using a render pass with many subpasses for the
        // transitions and only binding the blur descriptors once.
        wlr_log(WLR_DEBUG, "\t[CPU] render_texture subsection: %5.3f ms",
                (get_time() - start_time) * 1000);

        // Transition intermediate back to COLOR_ATTACH
        vulkan_image_transition_cbuf(cbuf,
                render_buf->intermediate, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                1);

        // Transition blur image to SHADER_READ_ONLY
        vulkan_image_transition_cbuf(cbuf,
                render_buf->blurs[0], VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                1);

        vulkan_end_timer(cbuf, renderer->query_pool, TIMER_RENDER_TEXTURE_1);

        // Bind pipeline and descriptor sets
	VkPipeline pipe = renderer->current_render_buffer->render_setup->tex_pipe;
	if (pipe != renderer->bound_pipe) {
		vkCmdBindPipeline(cbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
		renderer->bound_pipe = pipe;
	}

        // Starts the command buffer and enters the render pass
        begin_render_pass(cbuf, render_buf->framebuffer,
                render_buf->render_setup->rpass, rect, screen_width, screen_height);

        VkDescriptorSet desc_sets[] = {render_buf->blur_sets[0], texture->ds};

	vkCmdBindDescriptorSets(cbuf, VK_PIPELINE_BIND_POINT_GRAPHICS,
		renderer->pipe_layout, 0, sizeof(desc_sets) / sizeof(desc_sets[0]),
                desc_sets, 0, NULL);

	// Draw
        struct PushConstants push_constants = {0};
        glm_mat4_inv(matrix, push_constants.mat4);

        push_constants.surface_id[0] = surface_id;
        push_constants.surface_id[1] = render_uv ? 1 : 0;
        push_constants.surface_dims[0] = surface_width;
        push_constants.surface_dims[1] = surface_height;
        push_constants.screen_dims[0] = screen_width;
        push_constants.screen_dims[1] = screen_height;
        push_constants.is_focused = is_focused;
        push_constants.time_since_spawn = time_since_spawn;

	vkCmdPushConstants(cbuf, renderer->pipe_layout,
		VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                0, sizeof(push_constants), &push_constants);

        // This costs about 0.8ms in fullscreen.
	vkCmdDraw(cbuf, 4, 1, 0, 0);

        // Finish
	vkCmdEndRenderPass(cbuf);

        // End GPU timer
        vulkan_end_timer(cbuf, renderer->query_pool, TIMER_RENDER_TEXTURE);

        // I don't really know what this does, vulkan_texture_destroy uses it
        texture->last_used = renderer->frame;

        wlr_log(WLR_DEBUG, "\t[CPU] render_texture: %5.3f ms", (get_time() - start_time) * 1000);

}

static void render_surface(struct wlr_output *output, struct Surface *surface, bool is_focused) {
	struct wlr_texture *texture = wlr_surface_get_texture(surface->wlr_surface);
        if (texture == NULL) {
                return;
        }

        // Only make the surface clickable if it's an XDG surface.
        bool render_uv = surface->xdg_surface != NULL;

	render_texture(output->renderer, texture, surface->matrix,
                surface->width, surface->height,
                is_focused, get_time() - surface->spawn_time, surface->id, render_uv);
}

// Comparison function so we can qsort surfaces by Z.
int surface_comp(const void *a, const void *b) {
        // That's a lot of parentheses!
        float a_z = (*((struct Surface **) a))->z;
        float b_z = (*((struct Surface **) b))->z;

        return (a_z > b_z) - (a_z < b_z);
}

void render_begin(struct wlr_renderer *wlr_renderer, uint32_t width, uint32_t height) {
	struct wlr_vk_renderer *renderer = vulkan_get_renderer(wlr_renderer);
	assert(renderer->current_render_buffer);
        struct wlr_vk_render_buffer *render_buf = renderer->current_render_buffer;
        assert(render_buf != NULL);
        VkCommandBuffer cbuf = renderer->cb;

        cbuf_begin_onetime(cbuf);

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
                render_buf->intermediate, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_ACCESS_NONE, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                1);

        vulkan_clear_image(cbuf, render_buf->intermediate, (float [4]) {0, 0, 0, 1});

        // Clear the UV buffer too
        // Transition it to TRANSFER_DST_OPTIMAL so we can clear it. Maybe in
        // the future I will have a separate render pass for the first drawn
        // "thing" that clears everything.
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

        // Transition intermediate to COLOR_ATTACHMENT_OPTIMAL
        vulkan_image_transition_cbuf(cbuf,
                render_buf->intermediate, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                1);

        // End GPU timer
        vulkan_end_timer(cbuf, renderer->query_pool, TIMER_RENDER_BEGIN);
}

void insert_barriers(struct wlr_vk_renderer *renderer) {
	VkCommandBuffer cbuf = renderer->cb;
        // We need to insert the acquire barriers before everything else
        // executes. But we already recorded all the draw commands! So we need
        // a second command buffer that we submit first.
	VkCommandBuffer pre_cbuf = renderer->stage.cb;

	// Insert acquire and release barriers for dmabuf-images
	unsigned barrier_count = wl_list_length(&renderer->foreign_textures) + 1;
	VkImageMemoryBarrier* acquire_barriers = calloc(barrier_count,
                sizeof(VkImageMemoryBarrier));
	VkImageMemoryBarrier* release_barriers = calloc(barrier_count,
                sizeof(VkImageMemoryBarrier));

	struct wlr_vk_texture *texture, *tmp_tex;
	unsigned idx = 0;

	wl_list_for_each_safe(texture, tmp_tex, &renderer->foreign_textures, foreign_link) {
                wlr_log(WLR_DEBUG, "Acquire image at %p", texture->image);
                // I'm not sure exactly what a "foreign texture" is. foot
                // doesn't create any but imv does, for example. Anyway, if
                // there is a foreign texture we have to transition it to
                // SHADER_READ_ONLY_OPTIMAL.

		VkImageLayout src_layout = VK_IMAGE_LAYOUT_GENERAL;
		if (!texture->transitioned) {
			src_layout = VK_IMAGE_LAYOUT_UNDEFINED;
			texture->transitioned = true;
		}

                // Acquire: make sure it's in SHADER_READ_ONLY before any
                // shader reads
		acquire_barriers[idx].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		acquire_barriers[idx].srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
		acquire_barriers[idx].dstQueueFamilyIndex = renderer->dev->queue_family;
		acquire_barriers[idx].image = texture->image;
		acquire_barriers[idx].oldLayout = src_layout;
		acquire_barriers[idx].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		acquire_barriers[idx].srcAccessMask = 0u; // ignored anyways
		acquire_barriers[idx].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		acquire_barriers[idx].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		acquire_barriers[idx].subresourceRange.layerCount = 1;
		acquire_barriers[idx].subresourceRange.levelCount = 1;

                // Release: put it back in LAYOUT_GENERAL? I guess we do it so
                // they can write a new image? idk.
		release_barriers[idx].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		release_barriers[idx].srcQueueFamilyIndex = renderer->dev->queue_family;
		release_barriers[idx].dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
		release_barriers[idx].image = texture->image;
		release_barriers[idx].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		release_barriers[idx].newLayout = VK_IMAGE_LAYOUT_GENERAL;
		release_barriers[idx].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
		release_barriers[idx].dstAccessMask = 0u; // ignored anyways
		release_barriers[idx].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		release_barriers[idx].subresourceRange.layerCount = 1;
		release_barriers[idx].subresourceRange.levelCount = 1;
		++idx;

		wl_list_remove(&texture->foreign_link);
		texture->owned = false;
	}

        // Also add acquire/release barriers for the current render buffer.
        // It's worth noting that I used to not include this, and everything
        // worked fine. But it's in the original code for vulkan/renderer.c, so
        // I guess it must do something.
	VkImageLayout src_layout = VK_IMAGE_LAYOUT_GENERAL;
	if (!renderer->current_render_buffer->transitioned) {
		src_layout = VK_IMAGE_LAYOUT_PREINITIALIZED;
		renderer->current_render_buffer->transitioned = true;
	}

        // Acquire render buffer before rendering: Transition output image to
        // LAYOUT_GENERAL before any reads and writes to it.
	acquire_barriers[idx].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	acquire_barriers[idx].srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
	acquire_barriers[idx].dstQueueFamilyIndex = renderer->dev->queue_family;
	acquire_barriers[idx].image = renderer->current_render_buffer->screen;
	acquire_barriers[idx].oldLayout = src_layout;
	acquire_barriers[idx].newLayout = VK_IMAGE_LAYOUT_GENERAL;
	acquire_barriers[idx].srcAccessMask = 0u; // ignored anyways
        // Including READ here seems a bit weird because we never read from the
        // output image. But it was in the original code so fuck it, they know
        // better than me
	acquire_barriers[idx].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
		VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	acquire_barriers[idx].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	acquire_barriers[idx].subresourceRange.layerCount = 1;
	acquire_barriers[idx].subresourceRange.levelCount = 1;

        // Release render buffer after rendering. This doesn't actually change
        // the layout but does change the queue family, which I guess is
        // important.
	release_barriers[idx].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	release_barriers[idx].srcQueueFamilyIndex = renderer->dev->queue_family;
	release_barriers[idx].dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
	release_barriers[idx].image = renderer->current_render_buffer->screen;
        // We transition the screen image to COLOR_ATTACHMENT_OPTIMAL when we
        // render to it, so now we have to transition it back to GENERAL
	release_barriers[idx].oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	release_barriers[idx].newLayout = VK_IMAGE_LAYOUT_GENERAL;
	release_barriers[idx].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
		VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	release_barriers[idx].dstAccessMask = 0u; // ignored anyways
	release_barriers[idx].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	release_barriers[idx].subresourceRange.layerCount = 1;
	release_barriers[idx].subresourceRange.levelCount = 1;
	++idx;

        cbuf_begin_onetime(pre_cbuf);

	vkCmdPipelineBarrier(pre_cbuf, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                        | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		0, 0, NULL, 0, NULL, barrier_count, acquire_barriers);

	vkCmdPipelineBarrier(cbuf, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
		VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL,
		barrier_count, release_barriers);

	free(acquire_barriers);
	free(release_barriers);

        cbuf_submit_wait(renderer->dev->queue, pre_cbuf);
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
                .bufferRowLength = 1, .bufferImageHeight = 1,
                .imageSubresource = {
                        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                        .layerCount = 1,
                },
                .imageOffset = { .x = renderer->cursor_x, .y = renderer->cursor_y, .z = 0 },
                .imageExtent = { .width = 1, .height = 1, .depth = 1,
                },
        };

        vkCmdCopyImageToBuffer(cbuf,
                render_buf->uv, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                render_buf->host_uv,
                1, &uv_copy_region);

        VkRect2D rect = {{0, 0}, {width, height}};
        renderer->scissor = rect;

        // Transition intermediate to TRANSFER_SRC
        vulkan_image_transition_cbuf(cbuf,
                render_buf->intermediate, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                1);

        // Blur entire intermediate
        vulkan_start_timer(cbuf, renderer->query_pool, TIMER_RENDER_END_1);
        // Only do 3 passes
        blur_image(renderer, rect, width, height, 4, &render_buf->intermediate_set, true);
        vulkan_end_timer(cbuf, renderer->query_pool, TIMER_RENDER_END_1);

        // Transition blur to SHADER_READ_ONLY
        vulkan_image_transition_cbuf(cbuf,
                render_buf->blurs[0], VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                1);

        // Postprocess pass
        struct wlr_vk_render_format_setup *setup = render_buf->render_setup;
        vkCmdBindPipeline(cbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, setup->postprocess_pipe);
        renderer->bound_pipe = setup->postprocess_pipe;

        // Transition UV to SHADER_READ_ONLY
        vulkan_image_transition_cbuf(cbuf,
                render_buf->uv, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                1);

        // Begin render pass
        begin_postprocess_render_pass(renderer->cb,
                render_buf->postprocess_framebuffer,
                setup->postprocess_rpass, rect, width, height);

        // Bind descriptors
        VkDescriptorSet desc_sets[] = {render_buf->intermediate_set,
                render_buf->uv_set, render_buf->blur_sets[0]};

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

        // Acquire and release window textures and output image correctly. It
        // might seem weird to this in render_end and not render_begin. But the
        // list of foreign textures gets populated by render_texture, so we
        // have to do it here.
        insert_barriers(renderer);

        // End GPU timers
        vulkan_end_timer(cbuf, renderer->query_pool, TIMER_EVERYTHING);
        vulkan_end_timer(cbuf, renderer->query_pool, TIMER_RENDER_END);

        // Submit
        double pre_submit_time = get_time();
        double elapsed = (pre_submit_time - start_time) * 1000;
        wlr_log(WLR_DEBUG, "\t[CPU] render_end up to submit: %5.3f ms", elapsed);

        cbuf_submit_wait(renderer->dev->queue, renderer->cb);

        elapsed = (get_time() - pre_submit_time) * 1000;
        wlr_log(WLR_DEBUG, "\t[CPU] Submit: %5.2f ms", elapsed);

	renderer->bound_pipe = VK_NULL_HANDLE;
	renderer->render_width = 0;
	renderer->render_height = 0;

        // Check GPU timestamps
        wlr_log(WLR_DEBUG, "\t[GPU] render_begin: %5.3f ms",
                vulkan_get_elapsed(renderer->dev->dev, renderer->query_pool,
                        renderer->dev->instance->timestamp_period, TIMER_RENDER_BEGIN) * 1000);
        wlr_log(WLR_DEBUG, "\t[GPU] render_begin subsection: %5.3f ms",
                vulkan_get_elapsed(renderer->dev->dev, renderer->query_pool,
                        renderer->dev->instance->timestamp_period, TIMER_RENDER_BEGIN_1) * 1000);
        wlr_log(WLR_DEBUG, "\t[GPU] Most recent render_rect: %5.3f ms",
                vulkan_get_elapsed(renderer->dev->dev, renderer->query_pool,
                        renderer->dev->instance->timestamp_period, TIMER_RENDER_RECT) * 1000);
        wlr_log(WLR_DEBUG, "\t[GPU] Most recent render_texture: %5.3f ms",
                vulkan_get_elapsed(renderer->dev->dev, renderer->query_pool,
                        renderer->dev->instance->timestamp_period, TIMER_RENDER_TEXTURE) * 1000);
        wlr_log(WLR_DEBUG, "\t[GPU] Most recent render_texture subsection: %5.3f ms",
                vulkan_get_elapsed(renderer->dev->dev, renderer->query_pool,
                        renderer->dev->instance->timestamp_period, TIMER_RENDER_TEXTURE_1) * 1000);
        wlr_log(WLR_DEBUG, "\t[GPU] render_end: %5.3f ms",
                vulkan_get_elapsed(renderer->dev->dev, renderer->query_pool,
                        renderer->dev->instance->timestamp_period, TIMER_RENDER_END) * 1000);
        wlr_log(WLR_DEBUG, "\t[GPU] render_end subsection: %5.3f ms",
                vulkan_get_elapsed(renderer->dev->dev, renderer->query_pool,
                        renderer->dev->instance->timestamp_period, TIMER_RENDER_END_1) * 1000);
        wlr_log(WLR_DEBUG, "\t[GPU] Most recent blur: %5.3f ms",
                vulkan_get_elapsed(renderer->dev->dev, renderer->query_pool,
                        renderer->dev->instance->timestamp_period, TIMER_BLUR) * 1000);
        wlr_log(WLR_DEBUG, "\t[GPU] Most recent blur subsection: %5.3f ms",
                vulkan_get_elapsed(renderer->dev->dev, renderer->query_pool,
                        renderer->dev->instance->timestamp_period, TIMER_BLUR_1) * 1000);
        wlr_log(WLR_DEBUG, "\t[GPU] Entire pipeline: %5.3f ms",
                vulkan_get_elapsed(renderer->dev->dev, renderer->query_pool,
                        renderer->dev->instance->timestamp_period, TIMER_EVERYTHING) * 1000);

        // Destroy pending textures
        struct wlr_vk_texture *texture, *tmp_tex;
        wl_list_for_each_safe(texture, tmp_tex, &renderer->destroy_textures, destroy_link) {
                wlr_log(WLR_DEBUG, "Destroy texture %p", texture);
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
bool draw_frame(struct wlr_output *output, struct wl_list *surfaces,
                struct Surface *focused_surface, int cursor_x, int cursor_y) {
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

        // Draw frame counter.
	float color[4] = { rand()%2, rand()%2, rand()%2, 1.0 };
	render_rect_simple(renderer, color, 10, 10, 10, 10);
        wlr_log(WLR_DEBUG, "----");

	// Draw each surface
        for (int i = 0; i < surface_count; i++) {
                struct Surface *surface = surfaces_sorted[i];
                if (surface->width == 0 && surface->height == 0) {
                        wlr_log(WLR_DEBUG, "Skip surface, toplevel has dims %d %d",
                                surface->toplevel->width, surface->toplevel->height);
                        continue;
                }

                wlr_log(WLR_DEBUG, "Draw surface with dims %d %d",
                        surface->width, surface->height);
		render_surface(output, surface, surface == focused_surface);
	};
        wlr_log(WLR_DEBUG, "----");

	// Finish
        debug_images(renderer);

	render_end(renderer);

        renderer->rendering = false;

        double total_elapsed = get_time() - start_time;
        double framerate = (double) frame_count / total_elapsed;
        double frame_ms = (get_time() - frame_start_time) * 1000;

	vk_renderer->cursor_x = cursor_x;
	vk_renderer->cursor_y = cursor_y;

	int tr_width, tr_height;
	wlr_output_transformed_resolution(output, &tr_width, &tr_height);

        wlr_log(WLR_DEBUG, "Average FPS: %10.5f, ms this frame: %5.2f", framerate, frame_ms);

        frame_count++;

	return wlr_output_commit(output);
}
