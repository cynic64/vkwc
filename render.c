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
#include "vulkan/intermediate.h"

#define	M_PI 3.14159265358979323846

struct RenderData {
	struct wlr_output *output;
	pixman_region32_t *damage;
	struct wlr_presentation	*presentation; // May be NULL
	struct wl_list *surfaces;
	mat4 view;
	mat4 projection;
};

void render_rect_simple(struct wlr_renderer *renderer, const float color[4], int x, int	y, int width, int height) {
        fprintf(stderr, "Ignoring render_rect_simple\n");
        return;
	struct wlr_box box = { .x = x, .y = y, .width =	width, .height = height	};
	float identity_matrix[9] = { 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0,	0.0, 1.0 };
	wlr_render_rect(renderer, &box,	color, identity_matrix);
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
static bool render_subtexture_with_matrix(struct wlr_renderer *wlr_renderer,
                struct wlr_texture *wlr_texture, mat4 matrix, float surface_id, bool render_uv,
                int screen_width, int screen_height,
                int framebuffer_idx) {
	struct wlr_vk_renderer *renderer = (struct wlr_vk_renderer *) wlr_renderer;
        struct wlr_vk_render_buffer *render_buf = renderer->current_render_buffer;

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
        int prev_idx = framebuffer_idx - 1;
        if (prev_idx < 0) prev_idx += INTERMEDIATE_IMAGE_COUNT;

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

        // We don't have to transition back to COLOR_ATTACHMENT_OPTIMAL - the
        // render pass does that for us.

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

	vkCmdBindDescriptorSets(cbuf, VK_PIPELINE_BIND_POINT_GRAPHICS,
		renderer->pipe_layout, 0, 1, &texture->ds, 0, NULL);

	// Draw
        // Unfortunately the rest of wlroots is row-major, otherwise I would
        // set column-major in the shader and avoid this
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

	return true;
}

static void render_texture(struct wlr_output *output, struct wlr_texture *texture, mat4 matrix,
                float surface_id, bool render_uv,
                int framebuffer_idx) {
	struct wlr_renderer *renderer =	output->renderer;
	assert(renderer);

	render_subtexture_with_matrix(renderer, texture, matrix, surface_id, render_uv,
                output->width, output->height, framebuffer_idx);
}

static void render_surface(struct wlr_output *output, struct Surface *surface,
                int framebuffer_idx) {
	struct wlr_texture *texture = wlr_surface_get_texture(surface->wlr_surface);
	if (texture == NULL) {
                //printf("Could not render surface (dims %d %d)\n", surface->width, surface->height);
                return;
        }

        // Only make the surface clickable if it's an XDG surface.
        bool render_uv = surface->xdg_surface != NULL;

	render_texture(output, texture, surface->matrix, surface->id, render_uv, framebuffer_idx);
}

static void render_begin(struct wlr_renderer *wlr_renderer, uint32_t width, uint32_t height) {
	struct wlr_vk_renderer *renderer = vulkan_get_renderer(wlr_renderer);
	assert(renderer->current_render_buffer);

	// Refresh projection matrix.
        //
        // We need a matrix that turns pixels into -1..1 for vulkan.
        memset(renderer->projection, 0, sizeof(renderer->projection[0]) * 9);
        // Scale X down by width
        renderer->projection[0] = 2.0f / width;
        // Scale Y down by height
        renderer->projection[4] = 2.0f / height;
        // Leave Z alone
        renderer->projection[8] = 1;
        // Move X down by -1
        renderer->projection[2] = -1;
        // Move Y down by -1
        renderer->projection[5] = -1;

        // Transition UV image to COLOR_ATTACHMENT_OPTIMAL. 
        vulkan_image_transition(renderer->dev->dev, renderer->dev->queue, renderer->command_pool,
                renderer->current_render_buffer->uv, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                // Want to wait for whoever was reading from it before we write to it
                VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                1);

	renderer->render_width = width;
	renderer->render_height = height;
	renderer->bound_pipe = VK_NULL_HANDLE;

        // Transition all intermediates to TRANSFER_SRC, because when we start
        // rendering surfaces, it is assumed that the previous intermediate is
        // already in TRANSFER_SRC.
        struct wlr_vk_render_buffer *render_buf = renderer->current_render_buffer;

        VkCommandBuffer cbuf;
        cbuf_alloc(renderer->dev->dev, renderer->command_pool, &cbuf);
        cbuf_begin_onetime(cbuf);

        for (int i = 0; i < INTERMEDIATE_IMAGE_COUNT; i++) {
                vulkan_image_transition_cbuf(cbuf,
                        render_buf->intermediates[i], VK_IMAGE_ASPECT_COLOR_BIT,
                        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        // We don't really have to wait for anything but I
                        // can't put STAGE_NONE. So we do
                        // COLOR_ATTACHMENT_OUTPUT instead.
                        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        1);
        }

        cbuf_submit_wait(renderer->dev->queue, cbuf);

        // Begin command buffer. TODO: Actually use it instead of submitting a
        // bajillion different ones.
        VkCommandBufferBeginInfo cbuf_begin_info = {0};
        cbuf_begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(renderer->cb, &cbuf_begin_info);
}

static void render_end(struct wlr_renderer *wlr_renderer, int framebuffer_idx) {
	struct wlr_vk_renderer *renderer = vulkan_get_renderer(wlr_renderer);
	assert(renderer->current_render_buffer);

        // Submit
        cbuf_submit_wait(renderer->dev->queue, renderer->cb);

	renderer->render_width = 0u;
	renderer->render_height = 0u;
	renderer->bound_pipe = VK_NULL_HANDLE;

        int width = renderer->current_render_buffer->wlr_buffer->width;
        int height = renderer->current_render_buffer->wlr_buffer->height;

	// Copy UV to host-visible memory, but only the pixel under the cursor
        // Transition UV to TRANSFER_SRC_OPTIMAL
        VkCommandBuffer copy_cbuf;
        cbuf_alloc(renderer->dev->dev, renderer->command_pool, &copy_cbuf);
        cbuf_begin_onetime(copy_cbuf);

        vulkan_image_transition_cbuf(copy_cbuf,
                renderer->current_render_buffer->uv, VK_IMAGE_ASPECT_COLOR_BIT,
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

        vkCmdCopyImageToBuffer(copy_cbuf,
                renderer->current_render_buffer->uv, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                renderer->current_render_buffer->host_uv,
                1, &uv_copy_region);

        cbuf_submit_wait(renderer->dev->queue, copy_cbuf);

        // Copy intermediate image to final output
        // Transition final to IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
        cbuf_alloc(renderer->dev->dev, renderer->command_pool, &copy_cbuf);
        cbuf_begin_onetime(copy_cbuf);

        vulkan_image_transition_cbuf(copy_cbuf,
                renderer->current_render_buffer->image, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_ACCESS_NONE, VK_ACCESS_TRANSFER_WRITE_BIT,
                // I'm not really sure what to put here. I think the "proper"
                // way to do it would be to wait for the image to finish being
                // presented by using a fence or something.
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 1);

        // Transition intermediate to IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
        vulkan_image_transition_cbuf(copy_cbuf,
                renderer->current_render_buffer->intermediates[framebuffer_idx],
                VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 1);

        // Now do the actual copy
        vulkan_copy_image(copy_cbuf, renderer->current_render_buffer->intermediates[framebuffer_idx],
                renderer->current_render_buffer->image,
                VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 0, 0,
                width, height
        );

        cbuf_submit_wait(renderer->dev->queue, copy_cbuf);

        // Destroy pending textures
        struct wlr_vk_texture *texture, *tmp_tex;
        wl_list_for_each_safe(texture, tmp_tex, &renderer->destroy_textures, destroy_link) {
                printf("Destroy texture %p\n", texture);
                wlr_texture_destroy(&texture->wlr_texture);
        }

        // This marks it as the most recent I think
        renderer->frame++;
        renderer->current_render_buffer->frame = renderer->frame;

        // "release stage allocations", not sure what it really does
	struct wlr_vk_shared_buffer *buf;
	wl_list_for_each(buf, &renderer->stage.buffers, link) {
		buf->allocs_size = 0u;
	}

}

// `surfaces` should be a list of struct Surface, defined in vkwc.c
bool draw_frame(struct wlr_output *output, struct wl_list *surfaces, int cursor_x, int cursor_y) {
	// Get the renderer, i.e. Vulkan or GLES2
	struct wlr_renderer *renderer =	output->renderer;
	assert(renderer	!= NULL);

	// TinyWL used to try to do direct scanout here. But I think there's no point because
	// we want fancy effects that aren't possible with that.

	int buffer_age = -1;
	wlr_output_attach_render(output, &buffer_age);

	// Begin rendering
	render_begin(renderer, output->width, output->height);

	// Actually draw stuff
	struct Surface *surface;
	int surface_count = 0;
        int framebuffer_idx = 0;
	wl_list_for_each(surface, surfaces, link) {
                if (surface->width == 0 && surface->height == 0) {
                        continue;
                }

		render_surface(output, surface, framebuffer_idx);
		surface_count++;
                framebuffer_idx = (framebuffer_idx + 1) % INTERMEDIATE_IMAGE_COUNT;
	};
        printf("Drew %d surfaces\n", surface_count);

	// Draw frame counter
	float color[4] = { rand()%2, rand()%2, rand()%2, 1.0 };
	render_rect_simple(renderer, color, 10,	10, 10, 10);

	// Finish
	struct wlr_vk_renderer * vk_renderer = (struct wlr_vk_renderer *) renderer;
	vk_renderer->cursor_x = cursor_x;
	vk_renderer->cursor_y = cursor_y;

        // Since we switch back and forth between framebuffers, we have to
        // figure out which one to rpesent.
        int last_framebuffer = framebuffer_idx - 1;
        if (last_framebuffer < 0) last_framebuffer += INTERMEDIATE_IMAGE_COUNT;
	render_end(renderer, last_framebuffer);

	int tr_width, tr_height;
	wlr_output_transformed_resolution(output, &tr_width, &tr_height);

	return wlr_output_commit(output);
}
