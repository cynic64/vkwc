#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <fcntl.h>
#include <math.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdio.h>
#include <drm_fourcc.h>
#include <vulkan/vulkan.h>
#include <wlr/render/interface.h>
#include <wlr/types/wlr_drm.h>
#include <wlr/types/wlr_matrix.h>
#include <wlr/util/box.h>
#include <wlr/util/log.h>
#include <wlr/render/vulkan.h>
#include <wlr/backend/interface.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <cglm/cglm.h>

#include "render/pixel_format.h"
#include "render/vulkan.h"
#include "vulkan/intermediate.h"
#include "vulkan/shaders/common.vert.h"
#include "vulkan/shaders/texture.frag.h"
#include "vulkan/shaders/quad.frag.h"
#include <wlr/types/wlr_buffer.h>

static const VkDeviceSize min_stage_size = 1024 * 1024; // 1MB
static const VkDeviceSize max_stage_size = 64 * min_stage_size; // 64MB
static const size_t start_descriptor_pool_size = 256u;
static bool default_debug = true;
static const VkFormat DEPTH_FORMAT = VK_FORMAT_D32_SFLOAT;
static const VkFormat UV_FORMAT = VK_FORMAT_R16G16B16A16_UNORM;

static const struct wlr_renderer_impl renderer_impl;

struct wlr_vk_renderer *vulkan_get_renderer(struct wlr_renderer *wlr_renderer) {
	assert(wlr_renderer->impl == &renderer_impl);
	return (struct wlr_vk_renderer *)wlr_renderer;
}

static struct wlr_vk_render_format_setup *find_or_create_render_setup(
		struct wlr_vk_renderer *renderer, VkFormat format);

// https://www.w3.org/Graphics/Color/srgb
static float color_to_linear(float non_linear) {
	return (non_linear > 0.04045) ?
		pow((non_linear + 0.055) / 1.055, 2.4) :
		non_linear / 12.92;
}

// renderer
// util
static void mat3_to_mat4(const float mat3[9], float mat4[4][4]) {
	memset(mat4, 0, sizeof(float) * 16);
	mat4[0][0] = mat3[0];
	mat4[0][1] = mat3[1];
	mat4[0][3] = mat3[2];

	mat4[1][0] = mat3[3];
	mat4[1][1] = mat3[4];
	mat4[1][3] = mat3[5];

	mat4[2][2] = 1.f;
	mat4[3][3] = 1.f;
}

struct wlr_vk_descriptor_pool *vulkan_alloc_texture_ds(struct wlr_vk_renderer *renderer,
                VkDescriptorSet *ds) {
        printf("Allocate texture descriptor set\n");

	VkResult res;
	VkDescriptorSetAllocateInfo ds_info = {0};
	ds_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	ds_info.descriptorSetCount = 1;
	ds_info.pSetLayouts = &renderer->ds_layout;

	bool found = false;
	struct wlr_vk_descriptor_pool *pool;
	wl_list_for_each(pool, &renderer->descriptor_pools, link) {
		if (pool->free > 0) {
			found = true;
			break;
		}
	}

	if (!found) { // create new pool
		pool = calloc(1, sizeof(*pool));
		if (!pool) {
			wlr_log_errno(WLR_ERROR, "allocation failed");
			return NULL;
		}

		size_t count = renderer->last_pool_size;
		if (!count) {
			count = start_descriptor_pool_size;
		}

		pool->free = count;
		VkDescriptorPoolSize pool_size = {0};
		pool_size.descriptorCount = count;
		pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;

		VkDescriptorPoolCreateInfo dpool_info = {0};
		dpool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		dpool_info.maxSets = count;
		dpool_info.poolSizeCount = 1;
		dpool_info.pPoolSizes = &pool_size;
		dpool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;

		res = vkCreateDescriptorPool(renderer->dev->dev, &dpool_info, NULL,
			&pool->pool);
		if (res != VK_SUCCESS) {
			wlr_vk_error("vkCreateDescriptorPool", res);
			free(pool);
			return NULL;
		}

		wl_list_insert(&renderer->descriptor_pools, &pool->link);
	}

	ds_info.descriptorPool = pool->pool;
	res = vkAllocateDescriptorSets(renderer->dev->dev, &ds_info, ds);
	if (res != VK_SUCCESS) {
		wlr_vk_error("vkAllocateDescriptorSets", res);
		return NULL;
	}

	--pool->free;
	return pool;
}

void vulkan_free_ds(struct wlr_vk_renderer *renderer,
		struct wlr_vk_descriptor_pool *pool, VkDescriptorSet ds) {
	vkFreeDescriptorSets(renderer->dev->dev, pool->pool, 1, &ds);
	++pool->free;
}

static void destroy_render_format_setup(struct wlr_vk_renderer *renderer,
		struct wlr_vk_render_format_setup *setup) {
	if (!setup) {
		return;
	}

	VkDevice dev = renderer->dev->dev;
	vkDestroyRenderPass(dev, setup->render_pass, NULL);
	vkDestroyPipeline(dev, setup->tex_pipe, NULL);
	vkDestroyPipeline(dev, setup->quad_pipe, NULL);
}

static void shared_buffer_destroy(struct wlr_vk_renderer *r,
		struct wlr_vk_shared_buffer *buffer) {
	if (!buffer) {
		return;
	}

	if (buffer->allocs_size > 0) {
		wlr_log(WLR_ERROR, "shared_buffer_finish: %d allocations left",
			(unsigned) buffer->allocs_size);
	}

	free(buffer->allocs);
	if (buffer->buffer) {
		vkDestroyBuffer(r->dev->dev, buffer->buffer, NULL);
	}
	if (buffer->memory) {
		vkFreeMemory(r->dev->dev, buffer->memory, NULL);
	}

	wl_list_remove(&buffer->link);
	free(buffer);
}

struct wlr_vk_buffer_span vulkan_get_stage_span(struct wlr_vk_renderer *r,
		VkDeviceSize size) {
	// try to find free span
	// simple greedy allocation algorithm - should be enough for this usecase
	// since all allocations are freed together after the frame
	struct wlr_vk_shared_buffer *buf;
	wl_list_for_each_reverse(buf, &r->stage.buffers, link) {
		VkDeviceSize start = 0u;
		if (buf->allocs_size > 0) {
			struct wlr_vk_allocation *last = &buf->allocs[buf->allocs_size - 1];
			start = last->start + last->size;
		}

		assert(start <= buf->buf_size);
		if (buf->buf_size - start < size) {
			continue;
		}

		++buf->allocs_size;
		if (buf->allocs_size > buf->allocs_capacity) {
			buf->allocs_capacity = buf->allocs_size * 2;
			void *allocs = realloc(buf->allocs,
				buf->allocs_capacity * sizeof(*buf->allocs));
			if (!allocs) {
				wlr_log_errno(WLR_ERROR, "Allocation failed");
				goto error_alloc;
			}

			buf->allocs = allocs;
		}

		struct wlr_vk_allocation *a = &buf->allocs[buf->allocs_size - 1];
		a->start = start;
		a->size = size;
		return (struct wlr_vk_buffer_span) {
			.buffer = buf,
			.alloc = *a,
		};
	}

	// we didn't find a free buffer - create one
	// size = clamp(max(size * 2, prev_size * 2), min_size, max_size)
	VkDeviceSize bsize = size * 2;
	bsize = bsize < min_stage_size ? min_stage_size : bsize;
	if (!wl_list_empty(&r->stage.buffers)) {
		struct wl_list *last_link = r->stage.buffers.prev;
		struct wlr_vk_shared_buffer *prev = wl_container_of(
			last_link, prev, link);
		VkDeviceSize last_size = 2 * prev->buf_size;
		bsize = bsize < last_size ? last_size : bsize;
	}

	if (bsize > max_stage_size) {
		wlr_log(WLR_INFO, "vulkan stage buffers have reached max size");
		bsize = max_stage_size;
	}

	// create buffer
	buf = calloc(1, sizeof(*buf));
	if (!buf) {
		wlr_log_errno(WLR_ERROR, "Allocation failed");
		goto error_alloc;
	}

	VkResult res;
	VkBufferCreateInfo buf_info = {0};
	buf_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	buf_info.size = bsize;
	buf_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT |
		VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	buf_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	res = vkCreateBuffer(r->dev->dev, &buf_info, NULL, &buf->buffer);
	if (res != VK_SUCCESS) {
		wlr_vk_error("vkCreateBuffer", res);
		goto error;
	}

	VkMemoryRequirements mem_reqs;
	vkGetBufferMemoryRequirements(r->dev->dev, buf->buffer, &mem_reqs);

	VkMemoryAllocateInfo mem_info = {0};
	mem_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	mem_info.allocationSize = mem_reqs.size;
	mem_info.memoryTypeIndex = vulkan_find_mem_type(r->dev,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
		VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, mem_reqs.memoryTypeBits);
	res = vkAllocateMemory(r->dev->dev, &mem_info, NULL, &buf->memory);
	if (res != VK_SUCCESS) {
		wlr_vk_error("vkAllocatorMemory", res);
		goto error;
	}

	res = vkBindBufferMemory(r->dev->dev, buf->buffer, buf->memory, 0);
	if (res != VK_SUCCESS) {
		wlr_vk_error("vkBindBufferMemory", res);
		goto error;
	}

	size_t start_count = 8u;
	buf->allocs = calloc(start_count, sizeof(*buf->allocs));
	if (!buf->allocs) {
		wlr_log_errno(WLR_ERROR, "Allocation failed");
		goto error;
	}

	wlr_log(WLR_DEBUG, "Created new vk staging buffer of size %" PRIu64, bsize);
	buf->buf_size = bsize;
	wl_list_insert(&r->stage.buffers, &buf->link);

	buf->allocs_capacity = start_count;
	buf->allocs_size = 1u;
	buf->allocs[0].start = 0u;
	buf->allocs[0].size = size;
	return (struct wlr_vk_buffer_span) {
		.buffer = buf,
		.alloc = buf->allocs[0],
	};

error:
	shared_buffer_destroy(r, buf);

error_alloc:
	return (struct wlr_vk_buffer_span) {
		.buffer = NULL,
		.alloc = (struct wlr_vk_allocation) {0, 0},
	};
}

VkCommandBuffer vulkan_record_stage_cb(struct wlr_vk_renderer *renderer) {
	if (!renderer->stage.recording) {
		VkCommandBufferBeginInfo begin_info = {0};
		begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		vkBeginCommandBuffer(renderer->stage.cb, &begin_info);
		renderer->stage.recording = true;
	}

	return renderer->stage.cb;
}

bool vulkan_submit_stage_wait(struct wlr_vk_renderer *renderer) {
	if (!renderer->stage.recording) {
		return false;
	}

	vkEndCommandBuffer(renderer->stage.cb);
	renderer->stage.recording = false;

	VkSubmitInfo submit_info = {0};
	submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit_info.commandBufferCount = 1u;
	submit_info.pCommandBuffers = &renderer->stage.cb;
	VkResult res = vkQueueSubmit(renderer->dev->queue, 1,
		&submit_info, renderer->fence);
	if (res != VK_SUCCESS) {
		wlr_vk_error("vkQueueSubmit", res);
		return false;
	}

	res = vkWaitForFences(renderer->dev->dev, 1, &renderer->fence, true,
		UINT64_MAX);
	if (res != VK_SUCCESS) {
		wlr_vk_error("vkWaitForFences", res);
		return false;
	}

	// NOTE: don't release stage allocations here since they may still be
	// used for reading. Will be done next frame.
	res = vkResetFences(renderer->dev->dev, 1, &renderer->fence);
	if (res != VK_SUCCESS) {
		wlr_vk_error("vkResetFences", res);
		return false;
	}

	return true;
}

struct wlr_vk_format_props *vulkan_format_props_from_drm(
		struct wlr_vk_device *dev, uint32_t drm_fmt) {
	for (size_t i = 0u; i < dev->format_prop_count; ++i) {
		if (dev->format_props[i].format.drm_format == drm_fmt) {
			return &dev->format_props[i];
		}
	}
	return NULL;
}

// buffer import
static void destroy_render_buffer(struct wlr_vk_render_buffer *buffer) {
	wl_list_remove(&buffer->link);
	wl_list_remove(&buffer->buffer_destroy.link);

	assert(buffer->renderer->current_render_buffer != buffer);

	VkDevice dev = buffer->renderer->dev->dev;

	vkDestroyImageView(dev, buffer->image_view, NULL);
	vkDestroyImage(dev, buffer->image, NULL);

        for (int i = 0; i < INTERMEDIATE_IMAGE_COUNT; i++) {
                vkDestroyImage(dev, buffer->intermediates[i], NULL);
                vkDestroyImageView(dev, buffer->intermediate_views[i], NULL);
                vkFreeMemory(dev, buffer->intermediate_mems[i], NULL);

                vkDestroyFramebuffer(dev, buffer->framebuffers[i], NULL);
        }

	vkDestroyImage(dev, buffer->depth, NULL);
	vkDestroyImageView(dev, buffer->depth_view, NULL);
	vkFreeMemory(dev, buffer->depth_mem, NULL);

	vkDestroyImage(dev, buffer->uv, NULL);
	vkDestroyImageView(dev, buffer->uv_view, NULL);
	vkFreeMemory(dev, buffer->uv_mem, NULL);

	vkDestroyBuffer(dev, buffer->host_uv, NULL);
	vkFreeMemory(dev, buffer->host_uv_mem, NULL);

	for (size_t i = 0u; i < buffer->mem_count; ++i) {
		vkFreeMemory(dev, buffer->memories[i], NULL);
	}

	free(buffer);
}

static struct wlr_vk_render_buffer *get_render_buffer(
		struct wlr_vk_renderer *renderer, struct wlr_buffer *wlr_buffer) {
	struct wlr_vk_render_buffer *buffer;
	wl_list_for_each(buffer, &renderer->render_buffers, link) {
		if (buffer->wlr_buffer == wlr_buffer) {
			return buffer;
		}
	}
	return NULL;
}

static void handle_render_buffer_destroy(struct wl_listener *listener, void *data) {
	struct wlr_vk_render_buffer *buffer =
		wl_container_of(listener, buffer, buffer_destroy);
	destroy_render_buffer(buffer);
}

static void create_image(struct wlr_vk_renderer *renderer,
		VkFormat format, VkFormatFeatureFlagBits features,
                int width, int height,
                VkImageUsageFlagBits usage,
                VkImage *image) {
	VkFormatProperties format_props;
	vkGetPhysicalDeviceFormatProperties(renderer->dev->phdev, format, &format_props);
	if ((format_props.optimalTilingFeatures & features) != features) {
		fprintf(stderr, "Format %d doesn't support necessary features %d", format, features);
		exit(1);
	}

	struct VkImageCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = format,
		.extent.width = width,
		.extent.height = height,
		.extent.depth = 1,
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = usage,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};

	VkResult res = vkCreateImage(renderer->dev->dev, &info, NULL, image);
	if (res != VK_SUCCESS) {
		fprintf(stderr, "Couldn't create image\n");
		exit(1);
	}
}

static void alloc_memory(struct wlr_vk_renderer *renderer,
		VkMemoryRequirements requirements, VkMemoryPropertyFlagBits properties,
                VkDeviceMemory *memory) {
	int type = vulkan_find_mem_type(renderer->dev, properties, requirements.memoryTypeBits);
	if (type < 0) {
		wlr_log(WLR_ERROR, "Couldn't find suitable memory type");
		exit(1);
	}

	VkMemoryAllocateInfo alloc_info = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = requirements.size,
		.memoryTypeIndex = type,
	};

	VkResult res = vkAllocateMemory(renderer->dev->dev, &alloc_info, NULL, memory);
	if (res != VK_SUCCESS) {
		wlr_vk_error("vkAllocateMemory failed", res);
		exit(1);
	}
}

static struct wlr_vk_render_buffer *create_render_buffer(
		struct wlr_vk_renderer *renderer, struct wlr_buffer *wlr_buffer) {
	VkResult res;

	// Create render buffer
	struct wlr_vk_render_buffer *buffer = calloc(1, sizeof(*buffer));
	if (buffer == NULL) {
		wlr_log_errno(WLR_ERROR, "Allocation failed");
		return NULL;
	}
	buffer->wlr_buffer = wlr_buffer;
	buffer->renderer = renderer;
	buffer->frame = 0;

	struct wlr_dmabuf_attributes dmabuf = {0};
	if (!wlr_buffer_get_dmabuf(wlr_buffer, &dmabuf)) {
		goto error_buffer;
	}

	wlr_log(WLR_DEBUG, "vulkan create_render_buffer: %.4s, %dx%d",
		(const char*) &dmabuf.format, dmabuf.width, dmabuf.height);

	// This is what gets presented
	buffer->image = vulkan_import_dmabuf(renderer, &dmabuf,
		buffer->memories, &buffer->mem_count, true);
	if (!buffer->image) {
		goto error_buffer;
	}

	VkDevice dev = renderer->dev->dev;
	const struct wlr_vk_format_props *fmt = vulkan_format_props_from_drm(
		renderer->dev, dmabuf.format);
	if (fmt == NULL) {
		wlr_log(WLR_ERROR, "Unsupported pixel format %"PRIx32 " (%.4s)",
			dmabuf.format, (const char*) &dmabuf.format);
		goto error_buffer;
	}

	VkImageViewCreateInfo view_info = {0};
	view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view_info.image = buffer->image;
	view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view_info.format = fmt->format.vk_format;
	view_info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
	view_info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
	view_info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
	view_info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
	view_info.subresourceRange = (VkImageSubresourceRange) {
		VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1
	};

	res = vkCreateImageView(dev, &view_info, NULL, &buffer->image_view);
	if (res != VK_SUCCESS) {
		wlr_vk_error("vkCreateImageView failed", res);
		goto error_view;
	}

	buffer->render_setup = find_or_create_render_setup(
		renderer, fmt->format.vk_format);
	if (!buffer->render_setup) {
		goto error_view;
	}

        // Create two intermediate images - we need two for things like blurred
        // transparency. Whatever window is being drawn on top must be able to
        // sample the pixels behind it.
        for (int i = 0; i < INTERMEDIATE_IMAGE_COUNT; i++) {
                create_image(renderer, fmt->format.vk_format,
                        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT,
                        dmabuf.width, dmabuf.height,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                                | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT
                                | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                                | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                                | VK_IMAGE_USAGE_SAMPLED_BIT,
                        &buffer->intermediates[i]);

                VkMemoryRequirements intermediate_mem_reqs;
                vkGetImageMemoryRequirements(renderer->dev->dev, buffer->intermediates[i],
                        &intermediate_mem_reqs);

	        alloc_memory(renderer, intermediate_mem_reqs, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                        &buffer->intermediate_mems[i]);

                res = vkBindImageMemory(renderer->dev->dev, buffer->intermediates[i],
                        buffer->intermediate_mems[i], 0);
                assert(res == VK_SUCCESS);

                VkImageViewCreateInfo intermediate_view_info = {
                        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                        .image = buffer->intermediates[i],
                        .viewType = VK_IMAGE_VIEW_TYPE_2D,
                        .format = fmt->format.vk_format,
                        .subresourceRange = {
                                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                .baseMipLevel = 0,
                                .levelCount = 1,
                                .baseArrayLayer = 0,
                                .layerCount = 1,
                        }
                };
                res = vkCreateImageView(renderer->dev->dev, &intermediate_view_info, NULL,
                        &buffer->intermediate_views[i]);
                assert(res == VK_SUCCESS);

                // Create a descriptor set to be able to sample the image in
                // the fragment shader
                // TODO: Make sure this dpool gets destroyed
                struct wlr_vk_descriptor_pool *dpool = vulkan_alloc_texture_ds(renderer,
                        &buffer->intermediate_sets[i]);
                assert(dpool != NULL);

                VkDescriptorImageInfo ds_img_info = {0};
                ds_img_info.imageView = buffer->intermediate_views[i];
                ds_img_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

                VkWriteDescriptorSet ds_write = {0};
                ds_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                ds_write.descriptorCount = 1;
                ds_write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                ds_write.dstSet = buffer->intermediate_sets[i];
                ds_write.pImageInfo = &ds_img_info;

                vkUpdateDescriptorSets(dev, 1, &ds_write, 0, NULL);
        }

	// Create depth buffer
	create_image(renderer, DEPTH_FORMAT,
		VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT,
	        dmabuf.width, dmabuf.height,
	        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
	        | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT,
	        &buffer->depth);

	VkMemoryRequirements depth_mem_reqs;
	vkGetImageMemoryRequirements(renderer->dev->dev, buffer->depth, &depth_mem_reqs);
	alloc_memory(renderer, depth_mem_reqs, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &buffer->depth_mem);

	res = vkBindImageMemory(renderer->dev->dev, buffer->depth, buffer->depth_mem, 0);
	assert(res == VK_SUCCESS);

	VkImageViewCreateInfo depth_view_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = buffer->depth,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = DEPTH_FORMAT,
		.subresourceRange = {
			.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
			.baseMipLevel = 0,
			.levelCount = 1,
			.baseArrayLayer = 0,
			.layerCount = 1,
		}
	};
	res = vkCreateImageView(renderer->dev->dev, &depth_view_info, NULL, &buffer->depth_view);
	assert(res == VK_SUCCESS);

	// Create attachment to write UV coordinates into
	create_image(renderer, UV_FORMAT, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT,
		dmabuf.width, dmabuf.height,
		VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT 
 		| VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT, &buffer->uv);

	VkMemoryRequirements uv_mem_reqs;
	vkGetImageMemoryRequirements(renderer->dev->dev, buffer->uv, &uv_mem_reqs);
	alloc_memory(renderer, uv_mem_reqs, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &buffer->uv_mem);

	res = vkBindImageMemory(renderer->dev->dev, buffer->uv, buffer->uv_mem, 0);
	assert(res == VK_SUCCESS);

	VkImageViewCreateInfo uv_view_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = buffer->uv,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = UV_FORMAT,
		.subresourceRange = {
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.baseMipLevel = 0,
			.levelCount = 1,
			.baseArrayLayer = 0,
			.layerCount = 1,
		}
	};
	res = vkCreateImageView(renderer->dev->dev, &uv_view_info, NULL, &buffer->uv_view);
	assert(res == VK_SUCCESS);

	// Create host-visible UV buffer
	VkBufferCreateInfo host_uv_info = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = dmabuf.width * dmabuf.height * 12,
		.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};

	res = vkCreateBuffer(renderer->dev->dev, &host_uv_info, NULL, &buffer->host_uv);
	assert(res == VK_SUCCESS);

	VkMemoryRequirements host_uv_mem_reqs;
	vkGetBufferMemoryRequirements(renderer->dev->dev, buffer->host_uv, &host_uv_mem_reqs);
	alloc_memory(renderer, host_uv_mem_reqs,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
		| VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
		| VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
	        &buffer->host_uv_mem);

	res = vkBindBufferMemory(renderer->dev->dev, buffer->host_uv, buffer->host_uv_mem, 0);
	assert(res == VK_SUCCESS);

	// Create framebuffers
        for (int i = 0; i < INTERMEDIATE_IMAGE_COUNT; i++) {
                VkImageView attachments[] = {
                        buffer->intermediate_views[i],
                        buffer->depth_view,
                        buffer->uv_view,
                };
                VkFramebufferCreateInfo fb_info = {0};
                fb_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
                fb_info.attachmentCount = sizeof(attachments) / sizeof(attachments[0]);
                fb_info.pAttachments = attachments;
                fb_info.flags = 0u;
                fb_info.width = dmabuf.width;
                fb_info.height = dmabuf.height;
                fb_info.layers = 1u;
                fb_info.renderPass = buffer->render_setup->render_pass;

                res = vkCreateFramebuffer(dev, &fb_info, NULL, &buffer->framebuffers[i]);
                if (res != VK_SUCCESS) {
                        wlr_vk_error("vkCreateFramebuffer", res);
                        goto error_depth_view;
                }
        }

	buffer->buffer_destroy.notify = handle_render_buffer_destroy;
	wl_signal_add(&wlr_buffer->events.destroy, &buffer->buffer_destroy);
	wl_list_insert(&renderer->render_buffers, &buffer->link);

	return buffer;

error_depth_view:
	vkDestroyImageView(renderer->dev->dev, buffer->depth_view, NULL);
	vkFreeMemory(renderer->dev->dev, buffer->depth_mem, NULL);
	vkDestroyImage(renderer->dev->dev, buffer->depth, NULL);
error_view:
        for (int i = 0; i < INTERMEDIATE_IMAGE_COUNT; i++) {
                vkDestroyFramebuffer(dev, buffer->framebuffers[i], NULL);
        }
	vkDestroyImageView(dev, buffer->image_view, NULL);
	vkDestroyImage(dev, buffer->image, NULL);
	for (size_t i = 0u; i < buffer->mem_count; ++i) {
		vkFreeMemory(dev, buffer->memories[i], NULL);
	}
error_buffer:
	wlr_dmabuf_attributes_finish(&dmabuf);
	free(buffer);
	return NULL;
}

// interface implementation
static bool vulkan_bind_buffer(struct wlr_renderer *wlr_renderer,
		struct wlr_buffer *wlr_buffer) {
	struct wlr_vk_renderer *renderer = vulkan_get_renderer(wlr_renderer);

	if (renderer->current_render_buffer) {
		wlr_buffer_unlock(renderer->current_render_buffer->wlr_buffer);
		renderer->current_render_buffer = NULL;
	}

	if (!wlr_buffer) {
		return true;
	}

	struct wlr_vk_render_buffer *buffer = get_render_buffer(renderer, wlr_buffer);
	if (!buffer) {
		buffer = create_render_buffer(renderer, wlr_buffer);
		if (!buffer) {
			return false;
		}
	}

	wlr_buffer_lock(wlr_buffer);
	renderer->current_render_buffer = buffer;
	return true;
}

static void vulkan_begin(struct wlr_renderer *wlr_renderer, uint32_t width, uint32_t height) {
        fprintf(stderr, "Ignore vulkan_begin\n");
}

static void vulkan_end(struct wlr_renderer *wlr_renderer) {
        fprintf(stderr, "Ignore vulkan_end\n");
}

// This only gets used by the cursor I think? Not sure. I use the function with
// the same name in ../render.c for drawing window textures. I know, I know...
static bool vulkan_render_subtexture_with_matrix(struct wlr_renderer *wlr_renderer,
		struct wlr_texture *wlr_texture, const struct wlr_fbox *box,
		const float matrix[static 9], float alpha) {
        fprintf(stderr, "render_subtexture_with_matrix not implemented\n");
        return true;

	struct wlr_vk_renderer *renderer = vulkan_get_renderer(wlr_renderer);
	VkCommandBuffer cb = renderer->cb;

	struct wlr_vk_texture *texture = vulkan_get_texture(wlr_texture);
	assert(texture->renderer == renderer);
	if (texture->dmabuf_imported && !texture->owned) {
		// Store this texture in the list of textures that need to be
		// acquired before rendering and released after rendering.
		// We don't do it here immediately since barriers inside
		// a renderpass are suboptimal (would require additional renderpass
		// dependency and potentially multiple barriers) and it's
		// better to issue one barrier for all used textures anyways.
		texture->owned = true;
		assert(texture->foreign_link.prev == NULL);
		assert(texture->foreign_link.next == NULL);
		wl_list_insert(&renderer->foreign_textures, &texture->foreign_link);
	}

	VkPipeline pipe = renderer->current_render_buffer->render_setup->tex_pipe;
	if (pipe != renderer->bound_pipe) {
		vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
		renderer->bound_pipe = pipe;
	}

	vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
		renderer->pipe_layout, 0, 1, &texture->ds, 0, NULL);

	float final_matrix[9];
	wlr_matrix_multiply(final_matrix, renderer->projection, matrix);

	struct PushConstants vert_pcr_data;
	mat3_to_mat4(final_matrix, vert_pcr_data.mat4);

	vert_pcr_data.uv_off[0] = box->x / wlr_texture->width;
	vert_pcr_data.uv_off[1] = box->y / wlr_texture->height;
	vert_pcr_data.uv_size[0] = box->width / wlr_texture->width;
	vert_pcr_data.uv_size[1] = box->height / wlr_texture->height;

	vkCmdPushConstants(cb, renderer->pipe_layout,
		VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                0, sizeof(vert_pcr_data), &vert_pcr_data);
	vkCmdDraw(cb, 4, 1, 0, 0);
	texture->last_used = renderer->frame;

	return true;
}

static void vulkan_clear(struct wlr_renderer *wlr_renderer,
		const float color[static 4]) {
        // For some ungodly reason the hardware cursor calls this? Whatever.
        fprintf(stderr, "Ignoring vulkan_clear\n");
        return;

	struct wlr_vk_renderer *renderer = vulkan_get_renderer(wlr_renderer);
	VkCommandBuffer cb = renderer->cb;

	VkClearAttachment att = {0};
	att.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	att.colorAttachment = 0u;

	// Input color values are given in srgb space, vulkan expects
	// them in linear space. We explicitly import argb8 render buffers
	// as srgb, vulkan will convert the input values we give here to
	// srgb first.
	// But in other parts of wlroots we just always assume
	// srgb so that's why we have to convert here.
	att.clearValue.color.float32[0] = color_to_linear(color[0]);
	att.clearValue.color.float32[1] = color_to_linear(color[1]);
	att.clearValue.color.float32[2] = color_to_linear(color[2]);
	att.clearValue.color.float32[3] = color[3]; // no conversion for alpha

	VkClearRect rect = {0};
	rect.rect = renderer->scissor;
	rect.layerCount = 1;
	vkCmdClearAttachments(cb, 1, &att, 1, &rect);
}

static void vulkan_scissor(struct wlr_renderer *wlr_renderer,
		struct wlr_box *box) {
        fprintf(stderr, "Ignore vulkan_scissor\n");
        return;

	struct wlr_vk_renderer *renderer = vulkan_get_renderer(wlr_renderer);
	VkCommandBuffer cb = renderer->cb;

	uint32_t w = renderer->render_width;
	uint32_t h = renderer->render_height;
	struct wlr_box dst = {0, 0, w, h};
	if (box && !wlr_box_intersection(&dst, box, &dst)) {
		dst = (struct wlr_box) {0, 0, 0, 0}; // empty
	}

	VkRect2D rect = (VkRect2D) {{dst.x, dst.y}, {dst.width, dst.height}};
	renderer->scissor = rect;
	vkCmdSetScissor(cb, 0, 1, &rect);
}

static const uint32_t *vulkan_get_shm_texture_formats(
		struct wlr_renderer *wlr_renderer, size_t *len) {
	struct wlr_vk_renderer *renderer = vulkan_get_renderer(wlr_renderer);
	*len = renderer->dev->shm_format_count;
	return renderer->dev->shm_formats;
}

static void vulkan_render_quad_with_matrix(struct wlr_renderer *wlr_renderer,
		const float color[static 4], const float matrix[static 9]) {
        fprintf(stderr, "Ignoring render_quad...\n");
        return;

	struct wlr_vk_renderer *renderer = vulkan_get_renderer(wlr_renderer);
	VkCommandBuffer cbuf = renderer->cb;

        // TODO: just set the scissor to the whole screen in vulkan_begin and
        // forget about it afterwards. Fuck scissors.
	VkRect2D rect = {{0, 0}, {renderer->render_width, renderer->render_height}};
	renderer->scissor = rect;

        // Start the command buffer and enter the render pass
        struct wlr_vk_render_buffer *render_buf = renderer->current_render_buffer;
        assert(render_buf != NULL);
        // FIXME: Fuck man, what do I put for index here? I guess I'll have to
        // store it in vk_renderer
        begin_render_operation(cbuf, render_buf->framebuffers[0],
                render_buf->render_setup->render_pass, renderer->scissor,
                renderer->render_width, renderer->render_height);

        // Bind pipe
	VkPipeline pipe = renderer->current_render_buffer->render_setup->quad_pipe;
	if (pipe != renderer->bound_pipe) {
		vkCmdBindPipeline(cbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
		renderer->bound_pipe = pipe;
	}

	float final_matrix[9];
	wlr_matrix_multiply(final_matrix, renderer->projection, matrix);

	struct PushConstants push_constants;
	mat3_to_mat4(final_matrix, push_constants.mat4);
	push_constants.uv_off[0] = 0.f;
	push_constants.uv_off[1] = 0.f;
	push_constants.uv_size[0] = 1.f;
	push_constants.uv_size[1] = 1.f;

	// Input color values are given in srgb space, shader expects
	// them in linear space. The shader does all computation in linear
	// space and expects in inputs in linear space since it outputs
	// colors in linear space as well (and vulkan then automatically
	// does the conversion for out SRGB render targets).
	// But in other parts of wlroots we just always assume
	// srgb so that's why we have to convert here.
	push_constants.color[0] = color_to_linear(color[0]);
	push_constants.color[1] = color_to_linear(color[1]);
	push_constants.color[2] = color_to_linear(color[2]);
	push_constants.color[3] = color[3]; // no conversion for alpha

	vkCmdPushConstants(cbuf, renderer->pipe_layout,
		VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                0, sizeof(push_constants), &push_constants);
	vkCmdDraw(cbuf, 4, 1, 0, 0);
}

static const struct wlr_drm_format_set *vulkan_get_dmabuf_texture_formats(
		struct wlr_renderer *wlr_renderer) {
	struct wlr_vk_renderer *renderer = vulkan_get_renderer(wlr_renderer);
	return &renderer->dev->dmabuf_texture_formats;
}

static const struct wlr_drm_format_set *vulkan_get_render_formats(
		struct wlr_renderer *wlr_renderer) {
	struct wlr_vk_renderer *renderer = vulkan_get_renderer(wlr_renderer);
	return &renderer->dev->dmabuf_render_formats;
}

static uint32_t vulkan_preferred_read_format(
		struct wlr_renderer *wlr_renderer) {
	struct wlr_vk_renderer *renderer = vulkan_get_renderer(wlr_renderer);
	struct wlr_dmabuf_attributes dmabuf = {0};
	if (!wlr_buffer_get_dmabuf(renderer->current_render_buffer->wlr_buffer,
				&dmabuf)) {
		wlr_log(WLR_ERROR, "vulkan_preferred_read_format: Failed to get dmabuf of current render buffer");
		return DRM_FORMAT_INVALID;
	}
	return dmabuf.format;
}

static void vulkan_destroy(struct wlr_renderer *wlr_renderer) {
	struct wlr_vk_renderer *renderer = vulkan_get_renderer(wlr_renderer);
	struct wlr_vk_device *dev = renderer->dev;
	if (!dev) {
		free(renderer);
		return;
	}

	assert(!renderer->current_render_buffer);

	// stage.cb automatically freed with command pool
	struct wlr_vk_shared_buffer *buf, *tmp_buf;
	wl_list_for_each_safe(buf, tmp_buf, &renderer->stage.buffers, link) {
		shared_buffer_destroy(renderer, buf);
	}

	struct wlr_vk_texture *tex, *tex_tmp;
	wl_list_for_each_safe(tex, tex_tmp, &renderer->textures, link) {
		vulkan_texture_destroy(tex);
	}

	struct wlr_vk_render_buffer *render_buffer, *render_buffer_tmp;
	wl_list_for_each_safe(render_buffer, render_buffer_tmp,
			&renderer->render_buffers, link) {
		destroy_render_buffer(render_buffer);
	}

	struct wlr_vk_render_format_setup *setup, *tmp_setup;
	wl_list_for_each_safe(setup, tmp_setup,
			&renderer->render_format_setups, link) {
		destroy_render_format_setup(renderer, setup);
	}

	struct wlr_vk_descriptor_pool *pool, *tmp_pool;
	wl_list_for_each_safe(pool, tmp_pool, &renderer->descriptor_pools, link) {
		vkDestroyDescriptorPool(dev->dev, pool->pool, NULL);
		free(pool);
	}

	vkDestroyShaderModule(dev->dev, renderer->vert_module, NULL);
	vkDestroyShaderModule(dev->dev, renderer->tex_frag_module, NULL);

	vkDestroyFence(dev->dev, renderer->fence, NULL);
	vkDestroyPipelineLayout(dev->dev, renderer->pipe_layout, NULL);
	vkDestroyDescriptorSetLayout(dev->dev, renderer->ds_layout, NULL);
	vkDestroySampler(dev->dev, renderer->sampler, NULL);
	vkDestroyCommandPool(dev->dev, renderer->command_pool, NULL);

	struct wlr_vk_instance *ini = dev->instance;
	vulkan_device_destroy(dev);
	vulkan_instance_destroy(ini);
	free(renderer);
}

static bool vulkan_read_pixels(struct wlr_renderer *wlr_renderer,
		uint32_t drm_format, uint32_t stride,
		uint32_t width, uint32_t height, uint32_t src_x, uint32_t src_y,
		uint32_t dst_x, uint32_t dst_y, void *data) {
	bool success = false;
	struct wlr_vk_renderer *vk_renderer = vulkan_get_renderer(wlr_renderer);
	VkDevice dev = vk_renderer->dev->dev;
	VkImage src_image = vk_renderer->current_render_buffer->image;

	const struct wlr_pixel_format_info *pixel_format_info = drm_get_pixel_format_info(drm_format);
	if (!pixel_format_info) {
		wlr_log(WLR_ERROR, "vulkan_read_pixels: could not find pixel format info "
				"for DRM format 0x%08x", drm_format);
		return false;
	}

	const struct wlr_vk_format *wlr_vk_format = vulkan_get_format_from_drm(drm_format);
	if (!wlr_vk_format) {
		wlr_log(WLR_ERROR, "vulkan_read_pixels: no vulkan format "
				"matching drm format 0x%08x available", drm_format);
		return false;
	}
	VkFormat dst_format = wlr_vk_format->vk_format;
	VkFormat src_format = vk_renderer->current_render_buffer->render_setup->render_format;
	VkFormatProperties dst_format_props = {0}, src_format_props = {0};
	vkGetPhysicalDeviceFormatProperties(vk_renderer->dev->phdev, dst_format, &dst_format_props);
	vkGetPhysicalDeviceFormatProperties(vk_renderer->dev->phdev, src_format, &src_format_props);

	bool blit_supported = src_format_props.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT &&
		dst_format_props.linearTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT;
	if (!blit_supported && src_format != dst_format) {
		wlr_log(WLR_ERROR, "vulkan_read_pixels: blit unsupported and no manual "
					"conversion available from src to dst format.");
		return false;
	}

	VkImageCreateInfo image_create_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = dst_format,
		.extent.width = width,
		.extent.height = height,
		.extent.depth = 1,
		.arrayLayers = 1,
		.mipLevels = 1,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_LINEAR,
		.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT
	};
	VkImage dst_image;
	VkResult res = vkCreateImage(dev, &image_create_info, NULL, &dst_image);
	if (res != VK_SUCCESS) {
		wlr_vk_error("vkCreateImage", res);
		return false;
	}

	VkMemoryRequirements mem_reqs;
	vkGetImageMemoryRequirements(dev, dst_image, &mem_reqs);

	int mem_type = vulkan_find_mem_type(vk_renderer->dev,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
			VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
			VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
			mem_reqs.memoryTypeBits);
	if (mem_type < 0) {
		wlr_log(WLR_ERROR, "vulkan_read_pixels: could not find adequate memory type");
		goto destroy_image;
	}

	VkMemoryAllocateInfo mem_alloc_info = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	};
	mem_alloc_info.allocationSize = mem_reqs.size;
	mem_alloc_info.memoryTypeIndex = mem_type;

	VkDeviceMemory dst_img_memory;
	res = vkAllocateMemory(dev, &mem_alloc_info, NULL, &dst_img_memory);
	if (res != VK_SUCCESS) {
		wlr_vk_error("vkAllocateMemory", res);
		goto destroy_image;
	}
	res = vkBindImageMemory(dev, dst_image, dst_img_memory, 0);
	if (res != VK_SUCCESS) {
		wlr_vk_error("vkBindImageMemory", res);
		goto free_memory;
	}

	VkCommandBuffer cb = vulkan_record_stage_cb(vk_renderer);

        vulkan_image_transition(vk_renderer->dev->dev, vk_renderer->dev->queue,
                vk_renderer->command_pool,
                dst_image, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_ACCESS_NONE, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                1);

        vulkan_image_transition(vk_renderer->dev->dev, vk_renderer->dev->queue,
                vk_renderer->command_pool,
                src_image, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_MEMORY_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                1);

	if (blit_supported) {
		VkOffset3D blit_size = {
			.x = width,
			.y = height,
			.z = 1
		};
		VkImageBlit image_blit_region = {
			.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.srcSubresource.layerCount = 1,
			.srcOffsets[0] = {
				.x = src_x,
				.y = src_y,
			},
			.srcOffsets[1] = blit_size,
			.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.dstSubresource.layerCount = 1,
			.dstOffsets[1] = blit_size
		};
		vkCmdBlitImage(cb, src_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				dst_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				1, &image_blit_region, VK_FILTER_NEAREST);
	} else {
		wlr_log(WLR_DEBUG, "vulkan_read_pixels: blit unsupported, falling back to vkCmdCopyImage.");
		VkImageCopy image_region = {
			.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.srcSubresource.layerCount = 1,
			.srcOffset = {
				.x = src_x,
				.y = src_y,
			},
			.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.dstSubresource.layerCount = 1,
			.extent = {
				.width = width,
				.height = height,
				.depth = 1,
			}
		};
		vkCmdCopyImage(cb, src_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				dst_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &image_region);
	}

        vulkan_image_transition(vk_renderer->dev->dev, vk_renderer->dev->queue,
                vk_renderer->command_pool,
                dst_image, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_NONE,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                1);
        vulkan_image_transition(vk_renderer->dev->dev, vk_renderer->dev->queue,
                vk_renderer->command_pool,
                src_image, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                1);

	if (!vulkan_submit_stage_wait(vk_renderer)) {
		goto free_memory;
	}

	VkImageSubresource img_sub_res = {
		.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		.arrayLayer = 0,
		.mipLevel = 0
	};
	VkSubresourceLayout img_sub_layout;
	vkGetImageSubresourceLayout(dev, dst_image, &img_sub_res, &img_sub_layout);

	const char *d;
	res = vkMapMemory(dev, dst_img_memory, 0, VK_WHOLE_SIZE, 0, (void **)&d);
	if (res != VK_SUCCESS) {
		wlr_vk_error("vkMapMemory", res);
		goto free_memory;
	}
	d += img_sub_layout.offset;

	unsigned char *p = (unsigned char *)data + dst_y * stride;
	uint32_t bpp = pixel_format_info->bpp;
	uint32_t pack_stride = img_sub_layout.rowPitch;
	if (pack_stride == stride && dst_x == 0) {
		memcpy(p, d, height * stride);
	} else {
		for (size_t i = 0; i < height; ++i) {
			memcpy(p + i * stride + dst_x * bpp / 8, d + i * pack_stride, width * bpp / 8);
		}
	}

	success = true;
	vkUnmapMemory(dev, dst_img_memory);
free_memory:
	vkFreeMemory(dev, dst_img_memory, NULL);
destroy_image:
	vkDestroyImage(dev, dst_image, NULL);

	return success;
}

static int vulkan_get_drm_fd(struct wlr_renderer *wlr_renderer) {
	struct wlr_vk_renderer *renderer = vulkan_get_renderer(wlr_renderer);
	return renderer->dev->drm_fd;
}

static uint32_t vulkan_get_render_buffer_caps(struct wlr_renderer *wlr_renderer) {
	return WLR_BUFFER_CAP_DMABUF;
}

static const struct wlr_renderer_impl renderer_impl = {
	.bind_buffer = vulkan_bind_buffer,
	.begin = vulkan_begin,
	.end = vulkan_end,
	.clear = vulkan_clear,
	.scissor = vulkan_scissor,
	.render_subtexture_with_matrix = vulkan_render_subtexture_with_matrix,
	.render_quad_with_matrix = vulkan_render_quad_with_matrix,
	.get_shm_texture_formats = vulkan_get_shm_texture_formats,
	.get_dmabuf_texture_formats = vulkan_get_dmabuf_texture_formats,
	.get_render_formats = vulkan_get_render_formats,
	.preferred_read_format = vulkan_preferred_read_format,
	.read_pixels = vulkan_read_pixels,
	.destroy = vulkan_destroy,
	.get_drm_fd = vulkan_get_drm_fd,
	.get_render_buffer_caps = vulkan_get_render_buffer_caps,
	.texture_from_buffer = vulkan_texture_from_buffer,
};

// Initializes the VkDescriptorSetLayout and VkPipelineLayout needed
// for the texture rendering pipeline using the given VkSampler.
static bool init_tex_layouts(struct wlr_vk_renderer *renderer,
		VkSampler tex_sampler,
                VkDescriptorSetLayout *out_window_layout,
		VkPipelineLayout *out_pipe_layout) {
	VkResult res;
	VkDevice dev = renderer->dev->dev;

	// Descriptor set layouts - one for each set
        // This is for sampling the windows that have already been drawn
	VkDescriptorSetLayoutBinding background_binding = {
                .binding = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
                .pImmutableSamplers = &tex_sampler,
        };

        // This is for the window texture
	VkDescriptorSetLayoutBinding window_binding = {
                .binding = 0,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
                .pImmutableSamplers = &tex_sampler,
        };

        // Layout for the background
        VkDescriptorSetLayout background_layout;
	VkDescriptorSetLayoutCreateInfo background_layout_info = {
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                .bindingCount = 1,
                .pBindings = &background_binding,
        };

	res = vkCreateDescriptorSetLayout(dev, &background_layout_info, NULL, &background_layout);
	assert(res == VK_SUCCESS);

        // Aaaaaand for the window texture
	VkDescriptorSetLayoutCreateInfo window_layout_info = {
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                .bindingCount = 1,
                .pBindings = &window_binding,
        };

	res = vkCreateDescriptorSetLayout(dev, &window_layout_info, NULL, out_window_layout);
	assert(res == VK_SUCCESS);

        VkDescriptorSetLayout layouts[] = {background_layout, *out_window_layout};

	// Pipeline layout
	VkPushConstantRange pc_ranges[1] = {0};
	pc_ranges[0].size = sizeof(struct PushConstants);
	pc_ranges[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

	VkPipelineLayoutCreateInfo pl_info = {0};
	pl_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pl_info.setLayoutCount = sizeof(layouts) / sizeof(layouts[0]);
	pl_info.pSetLayouts = layouts;
	pl_info.pushConstantRangeCount = sizeof(pc_ranges) / sizeof(pc_ranges[0]);
	pl_info.pPushConstantRanges = pc_ranges;

	res = vkCreatePipelineLayout(dev, &pl_info, NULL, out_pipe_layout);
	if (res != VK_SUCCESS) {
		wlr_vk_error("vkCreatePipelineLayout", res);
		return false;
	}

	return true;
}

// Initializes the pipeline for rendering textures and using the given
// VkRenderPass and VkPipelineLayout.
static bool init_tex_pipeline(struct wlr_vk_renderer *renderer,
		VkRenderPass rp, VkPipelineLayout pipe_layout, VkPipeline *pipe) {
	VkResult res;
	VkDevice dev = renderer->dev->dev;

	// Shaders
	VkPipelineShaderStageCreateInfo vert_stage = {
		.sType= VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		.stage = VK_SHADER_STAGE_VERTEX_BIT,
                .module = renderer->vert_module,
		.pName = "main",
	};
	VkPipelineShaderStageCreateInfo frag_stage = {
		.sType= VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		.stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                .module = renderer->tex_frag_module,
		.pName = "main",
	};

	VkPipelineShaderStageCreateInfo tex_stages[] = {vert_stage, frag_stage};

	// Info
	VkPipelineInputAssemblyStateCreateInfo assembly = {0};
	assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;

        // Rasterizer
	VkPipelineRasterizationStateCreateInfo rasterization = {0};
	rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rasterization.polygonMode = VK_POLYGON_MODE_FILL;
	rasterization.cullMode = VK_CULL_MODE_NONE;
	rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rasterization.lineWidth = 1.f;

        // Blending
	VkPipelineColorBlendAttachmentState blend_attachment = {0};
	blend_attachment.blendEnable = VK_TRUE;
	// We generally work with pre-multiplied alpha
	blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
	blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
	blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
	blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;
	blend_attachment.colorWriteMask =
		VK_COLOR_COMPONENT_R_BIT |
		VK_COLOR_COMPONENT_G_BIT |
		VK_COLOR_COMPONENT_B_BIT |
		VK_COLOR_COMPONENT_A_BIT;

        VkPipelineColorBlendAttachmentState blend_attachments[] =
                {blend_attachment, blend_attachment};

	VkPipelineColorBlendStateCreateInfo blend = {0};
	blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	blend.attachmentCount = sizeof(blend_attachments) / sizeof(blend_attachments[0]);
	blend.pAttachments = blend_attachments;

        // Multisamping
	VkPipelineMultisampleStateCreateInfo multisample = {0};
	multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        // Viewport
	VkPipelineViewportStateCreateInfo viewport = {0};
	viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewport.viewportCount = 1;
	viewport.scissorCount = 1;

        // Dynamic state
	VkDynamicState dynStates[2] = {
		VK_DYNAMIC_STATE_VIEWPORT,
		VK_DYNAMIC_STATE_SCISSOR,
	};
	VkPipelineDynamicStateCreateInfo dynamic = {0};
	dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamic.pDynamicStates = dynStates;
	dynamic.dynamicStateCount = 2;

        // Vertex
	VkPipelineVertexInputStateCreateInfo vertex = {0};
	vertex.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

        // Depth
	VkPipelineDepthStencilStateCreateInfo depth_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
		.depthTestEnable = VK_TRUE,
		.depthWriteEnable = VK_TRUE,
		.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL,
		.depthBoundsTestEnable = VK_FALSE,
		.stencilTestEnable = VK_FALSE,
	};

        // Final info
	VkGraphicsPipelineCreateInfo pinfo = {0};
	pinfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pinfo.layout = pipe_layout;
	pinfo.renderPass = rp;
	pinfo.subpass = 0;
	pinfo.stageCount = 2;
	pinfo.pStages = tex_stages;

	pinfo.pInputAssemblyState = &assembly;
	pinfo.pRasterizationState = &rasterization;
	pinfo.pColorBlendState = &blend;
	pinfo.pMultisampleState = &multisample;
	pinfo.pViewportState = &viewport;
	pinfo.pDynamicState = &dynamic;
	pinfo.pVertexInputState = &vertex;

	pinfo.pDepthStencilState = &depth_info;

	// NOTE: use could use a cache here for faster loading
	// store it somewhere like $XDG_CACHE_HOME/wlroots/vk_pipe_cache
	VkPipelineCache cache = VK_NULL_HANDLE;
	res = vkCreateGraphicsPipelines(dev, cache, 1, &pinfo, NULL, pipe);
	if (res != VK_SUCCESS) {
		wlr_vk_error("failed to create vulkan pipelines:", res);
		return false;
	}

	return true;
}

static void init_quad_pipeline(struct wlr_vk_renderer *renderer,
		VkRenderPass render_pass, VkPipelineLayout pipe_layout, VkPipeline *pipe) {
	VkDevice dev = renderer->dev->dev;

	// shaders
	VkPipelineShaderStageCreateInfo vert_stage = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		.stage = VK_SHADER_STAGE_VERTEX_BIT,
                .module = renderer->vert_module,
		.pName = "main",
	};
	VkPipelineShaderStageCreateInfo frag_stage = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		.stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                .module = renderer->quad_frag_module,
		.pName = "main",
	};

	VkPipelineShaderStageCreateInfo quad_stages[2] = {vert_stage, frag_stage};

	// info
	VkPipelineInputAssemblyStateCreateInfo assembly = {0};
	assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;

	VkPipelineRasterizationStateCreateInfo rasterization = {0};
	rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rasterization.polygonMode = VK_POLYGON_MODE_FILL;
	rasterization.cullMode = VK_CULL_MODE_NONE;
	rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rasterization.lineWidth = 1.f;

	VkPipelineColorBlendAttachmentState blend_attachment = {0};
	blend_attachment.blendEnable = VK_TRUE;
	blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
	blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
	blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
	blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
	blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;
	blend_attachment.colorWriteMask =
		VK_COLOR_COMPONENT_R_BIT |
		VK_COLOR_COMPONENT_G_BIT |
		VK_COLOR_COMPONENT_B_BIT |
		VK_COLOR_COMPONENT_A_BIT;

	VkPipelineColorBlendAttachmentState blend_attachments[] = {blend_attachment, blend_attachment};

	VkPipelineColorBlendStateCreateInfo blend = {0};
	blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	blend.attachmentCount = sizeof(blend_attachments) / sizeof(blend_attachments[0]);
	blend.pAttachments = blend_attachments;

	VkPipelineMultisampleStateCreateInfo multisample = {0};
	multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	VkPipelineViewportStateCreateInfo viewport = {0};
	viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewport.viewportCount = 1;
	viewport.scissorCount = 1;

	VkDynamicState dynStates[2] = {
		VK_DYNAMIC_STATE_VIEWPORT,
		VK_DYNAMIC_STATE_SCISSOR,
	};
	VkPipelineDynamicStateCreateInfo dynamic = {0};
	dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamic.pDynamicStates = dynStates;
	dynamic.dynamicStateCount = 2;

	VkPipelineVertexInputStateCreateInfo vertex = {0};
	vertex.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

	VkPipelineDepthStencilStateCreateInfo depth_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
		.depthTestEnable = VK_TRUE,
		.depthWriteEnable = VK_TRUE,
		.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL,
		.depthBoundsTestEnable = VK_FALSE,
		.stencilTestEnable = VK_FALSE,
	};

	VkGraphicsPipelineCreateInfo pinfo = {0};
	pinfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pinfo.layout = pipe_layout;
	pinfo.renderPass = render_pass;
	pinfo.subpass = 0;
	pinfo.stageCount = 2;
	pinfo.pStages = quad_stages;

	pinfo.pInputAssemblyState = &assembly;
	pinfo.pRasterizationState = &rasterization;
	pinfo.pColorBlendState = &blend;
	pinfo.pMultisampleState = &multisample;
	pinfo.pViewportState = &viewport;
	pinfo.pDynamicState = &dynamic;
	pinfo.pVertexInputState = &vertex;

	pinfo.pDepthStencilState = &depth_info;

	// NOTE: use could use a cache here for faster loading
	// store it somewhere like $XDG_CACHE_HOME/wlroots/vk_pipe_cache.bin
	VkPipelineCache cache = VK_NULL_HANDLE;
	VkResult res = vkCreateGraphicsPipelines(dev, cache, 1, &pinfo, NULL, pipe);
	assert(res == VK_SUCCESS);
}

// Creates static render data, such as sampler, layouts and shader modules
// for the given rednerer.
// Cleanup is done by destroying the renderer.
static bool init_static_render_data(struct wlr_vk_renderer *renderer) {
	VkResult res;
	VkDevice dev = renderer->dev->dev;

	// default sampler (non ycbcr)
	VkSamplerCreateInfo sampler_info = {0};
	sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	sampler_info.magFilter = VK_FILTER_NEAREST;
	sampler_info.minFilter = VK_FILTER_NEAREST;
	sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	sampler_info.maxAnisotropy = 1.f;
	sampler_info.minLod = 0.f;
	sampler_info.maxLod = 0.25f;
	sampler_info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;

	res = vkCreateSampler(dev, &sampler_info, NULL, &renderer->sampler);
	if (res != VK_SUCCESS) {
		wlr_vk_error("Failed to create sampler", res);
		return false;
	}

	if (!init_tex_layouts(renderer, renderer->sampler,
			&renderer->ds_layout, &renderer->pipe_layout)) {
		return false;
	}

	// load vert module and tex frag module since they are needed to
	// initialize the tex pipeline
	VkShaderModuleCreateInfo sinfo = {0};
	sinfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	sinfo.codeSize = sizeof(common_vert_data);
	sinfo.pCode = common_vert_data;
	res = vkCreateShaderModule(dev, &sinfo, NULL, &renderer->vert_module);
	if (res != VK_SUCCESS) {
		wlr_vk_error("Failed to create vertex shader module", res);
		return false;
	}

	// tex frag
	sinfo.codeSize = sizeof(texture_frag_data);
	sinfo.pCode = texture_frag_data;
	res = vkCreateShaderModule(dev, &sinfo, NULL, &renderer->tex_frag_module);
	if (res != VK_SUCCESS) {
		wlr_vk_error("Failed to create tex fragment shader module", res);
		return false;
	}

	// quad frag
	sinfo.codeSize = sizeof(quad_frag_data);
	sinfo.pCode = quad_frag_data;
	res = vkCreateShaderModule(dev, &sinfo, NULL, &renderer->quad_frag_module);
	if (res != VK_SUCCESS) {
		wlr_vk_error("Failed to create quad fragment shader module", res);
		return false;
	}

	return true;
}

static struct wlr_vk_render_format_setup *find_or_create_render_setup(
		struct wlr_vk_renderer *renderer, VkFormat format) {
	struct wlr_vk_render_format_setup *setup;
	wl_list_for_each(setup, &renderer->render_format_setups, link) {
		if (setup->render_format == format) {
			return setup;
		}
	}

	setup = calloc(1u, sizeof(*setup));
	if (!setup) {
		wlr_log(WLR_ERROR, "Allocation failed");
		return NULL;
	}

	setup->render_format = format;

	// util
	VkDevice dev = renderer->dev->dev;
	VkResult res;

	// Intermediate
	VkAttachmentDescription intermediate_attach = {
		.format = format,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
		.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		.initialLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	};

	// Depth
	VkAttachmentDescription depth_attach = {
		.format = DEPTH_FORMAT,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
		.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
	};

	// UV
	VkAttachmentDescription uv_attach = {
		.format = UV_FORMAT,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	};

	// First subpass outputs
	VkAttachmentReference intermediate_out_ref = {
		.attachment = 0,
		.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	};
	VkAttachmentReference depth_ref = {
		.attachment = 1,
		.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
	};
	VkAttachmentReference uv_attach_ref = {
		.attachment = 2,
		.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	};

	VkAttachmentReference render_attachments[] = {intermediate_out_ref, uv_attach_ref};

	VkSubpassDescription render_subpass = {
		.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
		.colorAttachmentCount = sizeof(render_attachments) / sizeof(render_attachments[0]),
		.pColorAttachments = render_attachments,
		.pDepthStencilAttachment = &depth_ref,
	};

        VkSubpassDescription subpasses[] = {render_subpass};

	VkSubpassDependency deps[2] = {0};
        // I think there's multiple things rolled into one here. Among other
        // things, vertex buffer reads wait on transfer writes and texture
        // reads wait on color attachment output
	deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
	deps[0].srcStageMask = VK_PIPELINE_STAGE_HOST_BIT |
		VK_PIPELINE_STAGE_TRANSFER_BIT |
		VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT |
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	deps[0].srcAccessMask = VK_ACCESS_HOST_WRITE_BIT |
		VK_ACCESS_TRANSFER_WRITE_BIT |
		VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	deps[0].dstSubpass = 0;
	deps[0].dstStageMask = VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT;
	deps[0].dstAccessMask = VK_ACCESS_UNIFORM_READ_BIT |
		VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT |
		VK_ACCESS_INDIRECT_COMMAND_READ_BIT |
		VK_ACCESS_SHADER_READ_BIT;

	// Memory reads and writes must wait on this frame finishing rendering
	deps[1].srcSubpass = 0;
	deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
	deps[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT |
		VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
	deps[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT |
		VK_ACCESS_MEMORY_READ_BIT;

	VkAttachmentDescription attachments[] = {
                intermediate_attach,
                depth_attach,
                uv_attach,
        };

	VkRenderPassCreateInfo rpass_info = {0};
	rpass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	rpass_info.attachmentCount = sizeof(attachments) / sizeof(attachments[0]);
	rpass_info.pAttachments = attachments;
	rpass_info.subpassCount = sizeof(subpasses) / sizeof(subpasses[0]);
	rpass_info.pSubpasses = subpasses;
	rpass_info.dependencyCount = sizeof(deps) / sizeof(deps[0]);
	rpass_info.pDependencies = deps;

	res = vkCreateRenderPass(dev, &rpass_info, NULL, &setup->render_pass);
	if (res != VK_SUCCESS) {
		wlr_vk_error("Failed to create render pass", res);
		free(setup);
		return NULL;
	}

	if (!init_tex_pipeline(renderer, setup->render_pass, renderer->pipe_layout, &setup->tex_pipe)) {
		goto error;
	}

	// No error checking because these just die on error
	init_quad_pipeline(renderer, setup->render_pass, renderer->pipe_layout, &setup->quad_pipe);

	wl_list_insert(&renderer->render_format_setups, &setup->link);
	return setup;

error:
	destroy_render_format_setup(renderer, setup);
	return NULL;
}

struct wlr_renderer *vulkan_renderer_create_for_device(struct wlr_vk_device *dev) {
	struct wlr_vk_renderer *renderer;
	VkResult res;
	if (!(renderer = calloc(1, sizeof(*renderer)))) {
		wlr_log_errno(WLR_ERROR, "failed to allocate wlr_vk_renderer");
		return NULL;
	}

	renderer->dev = dev;
	wlr_renderer_init(&renderer->wlr_renderer, &renderer_impl);
	wl_list_init(&renderer->stage.buffers);
	wl_list_init(&renderer->destroy_textures);
	wl_list_init(&renderer->foreign_textures);
	wl_list_init(&renderer->textures);
	wl_list_init(&renderer->descriptor_pools);
	wl_list_init(&renderer->render_format_setups);
	wl_list_init(&renderer->render_buffers);

	if (!init_static_render_data(renderer)) {
		goto error;
	}

	// command pool
	VkCommandPoolCreateInfo cpool_info = {0};
	cpool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	cpool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	cpool_info.queueFamilyIndex = dev->queue_family;
	res = vkCreateCommandPool(dev->dev, &cpool_info, NULL,
		&renderer->command_pool);
	if (res != VK_SUCCESS) {
		wlr_vk_error("vkCreateCommandPool", res);
		goto error;
	}

	VkCommandBufferAllocateInfo cbuf_alloc_info = {0};
	cbuf_alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	cbuf_alloc_info.commandBufferCount = 1u;
	cbuf_alloc_info.commandPool = renderer->command_pool;
	cbuf_alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	res = vkAllocateCommandBuffers(dev->dev, &cbuf_alloc_info, &renderer->cb);
	if (res != VK_SUCCESS) {
		wlr_vk_error("vkAllocateCommandBuffers", res);
		goto error;
	}

	VkFenceCreateInfo fence_info = {0};
	fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	res = vkCreateFence(dev->dev, &fence_info, NULL,
		&renderer->fence);
	if (res != VK_SUCCESS) {
		wlr_vk_error("vkCreateFence", res);
		goto error;
	}

	// staging command buffer
	VkCommandBufferAllocateInfo cmd_buf_info = {0};
	cmd_buf_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	cmd_buf_info.commandPool = renderer->command_pool;
	cmd_buf_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cmd_buf_info.commandBufferCount = 1u;
	res = vkAllocateCommandBuffers(dev->dev, &cmd_buf_info,
		&renderer->stage.cb);
	if (res != VK_SUCCESS) {
		wlr_vk_error("vkAllocateCommandBuffers", res);
		goto error;
	}

	return &renderer->wlr_renderer;

error:
	vulkan_destroy(&renderer->wlr_renderer);
	return NULL;
}

struct wlr_renderer *wlr_vk_renderer_create_with_drm_fd(int drm_fd) {
	wlr_log(WLR_INFO, "The vulkan renderer is only experimental and "
		"not expected to be ready for daily use");

	// NOTE: we could add functionality to allow the compositor passing its
	// name and version to this function. Just use dummies until then,
	// shouldn't be relevant to the driver anyways
	struct wlr_vk_instance *ini = vulkan_instance_create(0, NULL, default_debug);
	if (!ini) {
		wlr_log(WLR_ERROR, "creating vulkan instance for renderer failed");
		return NULL;
	}

	VkPhysicalDevice phdev = vulkan_find_drm_phdev(ini, drm_fd);
	if (!phdev) {
		// We rather fail here than doing some guesswork
		wlr_log(WLR_ERROR, "Could not match drm and vulkan device");
		return NULL;
	}

	// queue families
	uint32_t qfam_count;
	vkGetPhysicalDeviceQueueFamilyProperties(phdev, &qfam_count, NULL);
	VkQueueFamilyProperties queue_props[qfam_count];
	vkGetPhysicalDeviceQueueFamilyProperties(phdev, &qfam_count,
		queue_props);

	struct wlr_vk_device *dev = vulkan_device_create(ini, phdev, 0, NULL);
	if (!dev) {
		wlr_log(WLR_ERROR, "Failed to create vulkan device");
		vulkan_instance_destroy(ini);
		return NULL;
	}

	// We duplicate it so it's not closed while we still need it.
	dev->drm_fd = fcntl(drm_fd, F_DUPFD_CLOEXEC, 0);
	if (dev->drm_fd < 0) {
		wlr_log_errno(WLR_ERROR, "fcntl(F_DUPFD_CLOEXEC) failed");
		vulkan_device_destroy(dev);
		vulkan_instance_destroy(ini);
		return NULL;
	}

	return vulkan_renderer_create_for_device(dev);
}
