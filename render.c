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
                int x, int y, int width, int height, bool clear) {
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

        VkRenderPass rpass = clear ? render_buf->render_setup->quad_clear_rpass
                : render_buf->render_setup->quad_rpass;
        begin_render_pass(cbuf, render_buf->framebuffer,
                rpass, rect, screen_width, screen_height);

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

void debug_images(struct wlr_renderer *wlr_renderer) {
	struct wlr_vk_renderer *renderer = (struct wlr_vk_renderer *) wlr_renderer;
        struct wlr_vk_render_buffer *render_buf = renderer->current_render_buffer;

        wlr_log(WLR_DEBUG, "Intermediate image is at %p", render_buf->intermediate);
        wlr_log(WLR_DEBUG, "UV is at %p", render_buf->uv);
}

// Sometimes we want to set a tight scissor around a window that might be
// rotated weirdly. This figures out the screen coordinates.
void get_rect_for_matrix(int screen_width, int screen_height, mat4 matrix, int padding,
                VkRect2D *rect) {
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

// Assumes image is in SHADER_READ_ONLY. If with_threshold is set, a threshold
// will first be applied to the image. So you end up with just the bright parts
// blurred.
void blur_image(struct wlr_vk_renderer *renderer,
                int screen_width, int screen_height, int pass_count, VkDescriptorSet *src_image_set,
                mat4 matrix, bool with_threshold) {
        assert(pass_count <= BLUR_PASSES);

        VkCommandBuffer cbuf = renderer->cb;
        struct wlr_vk_render_buffer *render_buf = renderer->current_render_buffer;

        double start_time = get_time();


        VkRect2D rect;
        // We need to clear a bigger area so junk from previous frames doesn't bleed into ours
        int padding = 32;
        get_rect_for_matrix(screen_width, screen_height, matrix, padding, &rect);

        // There might have already been a texture rendered, so reset the timers
        vkCmdResetQueryPool(cbuf, renderer->query_pool, TIMER_BLUR, 2);
        vkCmdResetQueryPool(cbuf, renderer->query_pool, TIMER_BLUR_1, 2);

        vulkan_start_timer(cbuf, renderer->query_pool, TIMER_BLUR);

        int last_image_idx = 0;
        int idx_to_time = 1;
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
                memcpy(push_constants.mat4, matrix, sizeof(push_constants.mat4));
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

                if (i == idx_to_time) {
                        vulkan_start_timer(cbuf, renderer->query_pool, TIMER_BLUR_1);
                }
                vkCmdDraw(cbuf, 4, 1, 0, 0);
                if (i == idx_to_time) {
                        vulkan_end_timer(cbuf, renderer->query_pool, TIMER_BLUR_1);
                }

                vkCmdEndRenderPass(cbuf);

                last_image_idx = image_idx;
        }

        vulkan_end_timer(cbuf, renderer->query_pool, TIMER_BLUR);

        wlr_log(WLR_DEBUG, "\t[CPU] blur: %5.3f ms", (get_time() - start_time) * 1000);
}

static void render_surface(struct wlr_output *output, struct Surface *surface, bool is_focused,
                bool clear) {
	struct wlr_texture *wlr_texture = wlr_surface_get_texture(surface->wlr_surface);
        if (wlr_texture == NULL) {
                return;
        }

        wlr_log(WLR_DEBUG, "Render texture with dims %d %d", surface->width, surface->height);
        // Only make the surface clickable if it's an XDG surface.
        bool render_uv = surface->xdg_surface != NULL;

        struct wlr_vk_renderer *renderer = (struct wlr_vk_renderer *) output->renderer;
        float time_since_spawn = get_time() - surface->spawn_time;

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
        bool is_foreign = texture->dmabuf_imported && !texture->owned;

        if (is_foreign) {
                // Acquire it
	        VkImageMemoryBarrier barrier;

		VkImageLayout src_layout = VK_IMAGE_LAYOUT_GENERAL;
		if (!texture->transitioned) {
			src_layout = VK_IMAGE_LAYOUT_UNDEFINED;
			texture->transitioned = true;
		}

                // Acquire: make sure it's in SHADER_READ_ONLY before any
                // shader reads
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
		barrier.dstQueueFamilyIndex = renderer->dev->queue_family;
		barrier.image = texture->image;
		barrier.oldLayout = src_layout;
		barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		barrier.srcAccessMask = 0u; // ignored anyways
		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barrier.subresourceRange.layerCount = 1;
		barrier.subresourceRange.levelCount = 1;

                vkCmdPipelineBarrier(cbuf, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                                | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                        0, 0, NULL, 0, NULL, 1, &barrier);
        }

        VkRect2D rect;
        get_rect_for_matrix(screen_width, screen_height, surface->matrix, 0, &rect);

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

        blur_image(renderer, screen_width, screen_height, BLUR_PASSES,
                &render_buf->intermediate_set, surface->inner_matrix, false);

        wlr_log(WLR_DEBUG, "\t[CPU] render_texture subsection: %5.3f ms",
                (get_time() - start_time) * 1000);

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
        VkRenderPass rpass = clear ? render_buf->render_setup->rpass_clear
                : render_buf->render_setup->rpass;
        begin_render_pass(cbuf, render_buf->framebuffer,
                rpass, rect, screen_width, screen_height);
        renderer->scissor = rect;

        VkDescriptorSet desc_sets[] = {render_buf->blur_sets[0], texture->ds};

	vkCmdBindDescriptorSets(cbuf, VK_PIPELINE_BIND_POINT_GRAPHICS,
		renderer->pipe_layout, 0, sizeof(desc_sets) / sizeof(desc_sets[0]),
                desc_sets, 0, NULL);

	// Draw
        struct PushConstants push_constants = {0};
        memcpy(push_constants.mat4, surface->matrix, sizeof(push_constants.mat4));

        push_constants.surface_id[0] = surface->id;
        push_constants.surface_id[1] = render_uv ? 1 : 0;
        push_constants.surface_dims[0] = surface->width;
        push_constants.surface_dims[1] = surface->height;
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

        if (is_foreign) {
                // Release: put it back in LAYOUT_GENERAL? I guess we do it so
                // they can write a new image? idk.
                VkImageMemoryBarrier barrier = {0};
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.srcQueueFamilyIndex = renderer->dev->queue_family;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
		barrier.image = texture->image;
		barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
		barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
		barrier.dstAccessMask = 0u; // ignored anyways
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barrier.subresourceRange.layerCount = 1;
		barrier.subresourceRange.levelCount = 1;

                vkCmdPipelineBarrier(cbuf, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
                        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL,
                        1, &barrier);

                texture->owned = true;
        }

        // End GPU timer
        vulkan_end_timer(cbuf, renderer->query_pool, TIMER_RENDER_TEXTURE);

        // I don't really know what this does, vulkan_texture_destroy uses it
        texture->last_used = renderer->frame;

        wlr_log(WLR_DEBUG, "\t[CPU] render_texture: %5.3f ms", (get_time() - start_time) * 1000);
}

// Comparison function so we can qsort surfaces by Z.
int surface_comp(const void *a, const void *b) {
        // That's a lot of parentheses!
        float a_z = (*((struct Surface **) a))->z;
        float b_z = (*((struct Surface **) b))->z;

        return (a_z > b_z) - (a_z < b_z);
}

// Inserts pipeline barriers so noone else is using our images before we do.
void insert_acquire_barrier(struct wlr_vk_renderer *renderer) {
	VkCommandBuffer cbuf = renderer->cb;

	VkImageMemoryBarrier acquire_barrier = {0};

        // Add acquire/release barriers for the current render buffer.
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
	acquire_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	acquire_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
	acquire_barrier.dstQueueFamilyIndex = renderer->dev->queue_family;
	acquire_barrier.image = renderer->current_render_buffer->screen;
	acquire_barrier.oldLayout = src_layout;
	acquire_barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
	acquire_barrier.srcAccessMask = 0u; // ignored anyways
        // Including READ here seems a bit weird because we never read from the
        // output image. But it was in the original code so fuck it, they know
        // better than me
	acquire_barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
		VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	acquire_barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	acquire_barrier.subresourceRange.layerCount = 1;
	acquire_barrier.subresourceRange.levelCount = 1;

	vkCmdPipelineBarrier(cbuf, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                        | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		0, 0, NULL, 0, NULL, 1, &acquire_barrier);
}

void render_begin(struct wlr_renderer *wlr_renderer, uint32_t width, uint32_t height) {
	struct wlr_vk_renderer *renderer = vulkan_get_renderer(wlr_renderer);
	assert(renderer->current_render_buffer);
        struct wlr_vk_render_buffer *render_buf = renderer->current_render_buffer;
        assert(render_buf != NULL);
        VkCommandBuffer cbuf = renderer->cb;

        double start_time = get_time();

        cbuf_begin_onetime(cbuf);

        // Reset timers
        vkCmdResetQueryPool(cbuf, renderer->query_pool, 0, TIMER_COUNT * 2);

        // Start GPU timers
        vulkan_start_timer(cbuf, renderer->query_pool, TIMER_RENDER_BEGIN);
        vulkan_start_timer(cbuf, renderer->query_pool, TIMER_EVERYTHING);

	renderer->render_width = width;
	renderer->render_height = height;
	renderer->bound_pipe = VK_NULL_HANDLE;

        // Acquire images
        insert_acquire_barrier(renderer);

        // End GPU timer
        vulkan_end_timer(cbuf, renderer->query_pool, TIMER_RENDER_BEGIN);

        wlr_log(WLR_DEBUG, "\t[CPU] render_begin: %5.3f ms", (get_time() - start_time) * 1000);
}

void render_end(struct wlr_renderer *wlr_renderer, float colorscheme_ratio,
                int src_colorscheme_idx, int dst_colorscheme_idx) {
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
        // Only do 3 passes
        mat4 matrix = {{2, 0, 0, 0}, {0, 2, 0, 0}, {0, 0, 1, 0}, {-1, -1, 0, 1}};
        blur_image(renderer, width, height, 3, &render_buf->intermediate_set, matrix, true);

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
        // cheeky. But the int and float fit so it's OK. TODO: Make postprocess
        // pushconstants its own struct.
	vkCmdPushConstants(cbuf, renderer->pipe_layout,
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                0, 4, &renderer->postprocess_mode);
	vkCmdPushConstants(cbuf, renderer->pipe_layout,
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                4, 8, &colorscheme_ratio);
	vkCmdPushConstants(cbuf, renderer->pipe_layout,
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                8, 12, &src_colorscheme_idx);
	vkCmdPushConstants(cbuf, renderer->pipe_layout,
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                12, 16, &dst_colorscheme_idx);
        vulkan_start_timer(cbuf, renderer->query_pool, TIMER_RENDER_END_1);
        vkCmdDraw(cbuf, 4, 1, 0, 0);
        vulkan_end_timer(cbuf, renderer->query_pool, TIMER_RENDER_END_1);

        vkCmdEndRenderPass(cbuf);

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
        for (int i = 0; i < TIMER_COUNT; i++) {
                // There's always the start and the end timer, so the index goes up by 2s.
                int timer_idx = 2*i;
                elapsed = vulkan_get_elapsed(renderer->dev->dev, renderer->query_pool,
                        renderer->dev->instance->timestamp_period, timer_idx);
                if (elapsed != -1) {
                        renderer->timer_sums[i] += elapsed;
                        renderer->timer_counts[i] ++;
                }
                float avg = renderer->timer_sums[i] / renderer->timer_counts[i];

                wlr_log(WLR_DEBUG, "\t[GPU] %s: %5.3f ms (%5.3f ms avg)", TIMER_NAMES[i],
                        elapsed * 1000, avg * 1000);
        }

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
// TODO: struct for colorscheme stuff
bool draw_frame(struct wlr_output *output, struct wl_list *surfaces,
                struct Surface *focused_surface, int cursor_x, int cursor_y,
                float colorscheme_ratio, int src_colorscheme_idx, int dst_colorscheme_idx) {
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
	render_rect_simple(renderer, color, 10, 10, 10, 10, true);
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
		render_surface(output, surface, surface == focused_surface, false);
	};
        wlr_log(WLR_DEBUG, "----");

	// Finish
        debug_images(renderer);

	render_end(renderer, colorscheme_ratio, src_colorscheme_idx, dst_colorscheme_idx);

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
