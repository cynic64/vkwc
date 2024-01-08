#include <vulkan/vulkan.h>
#include <wlr/util/log.h>
#include <assert.h>
#include "../render/vulkan.h"

#include "util.h"

int vulkan_find_mem_type(struct wlr_vk_device *dev,
		VkMemoryPropertyFlags flags, uint32_t req_bits) {

	VkPhysicalDeviceMemoryProperties props;
	vkGetPhysicalDeviceMemoryProperties(dev->phdev, &props);

	for (unsigned i = 0u; i < props.memoryTypeCount; ++i) {
		if (req_bits & (1 << i)) {
			if ((props.memoryTypes[i].propertyFlags & flags) == flags) {
				return i;
			}
		}
	}

	return -1;
}

void cbuf_alloc(VkDevice device, VkCommandPool cpool, VkCommandBuffer *cbuf) {
        VkCommandBufferAllocateInfo info = {0};
        info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        info.commandPool = cpool;
        info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        info.commandBufferCount = 1;

        VkResult res = vkAllocateCommandBuffers(device, &info, cbuf);
        assert(res == VK_SUCCESS);
}

void cbuf_submit_wait(VkQueue queue, VkCommandBuffer cbuf) {
        vkEndCommandBuffer(cbuf);

        VkSubmitInfo info = {0};
        info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        info.commandBufferCount = 1;
        info.pCommandBuffers = &cbuf;

        vkQueueSubmit(queue, 1, &info, VK_NULL_HANDLE);
        vkQueueWaitIdle(queue);
}

void cbuf_begin_onetime(VkCommandBuffer cbuf) {
        VkCommandBufferBeginInfo info = {0};
        info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cbuf, &info);
}

void vulkan_image_transition(VkDevice device, VkQueue queue, VkCommandPool cpool,
                VkImage image, VkImageAspectFlags aspect,
                VkImageLayout old_lt, VkImageLayout new_lt,
                VkAccessFlags src_access, VkAccessFlags dst_access,
                VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage,
                uint32_t mip_levels) {

	VkCommandBuffer cbuf;
	cbuf_alloc(device, cpool, &cbuf);
	cbuf_begin_onetime(cbuf);

        vulkan_image_transition_cbuf(cbuf, image, aspect, old_lt, new_lt,
                src_access, dst_access,
                src_stage, dst_stage,
                mip_levels);

	cbuf_submit_wait(queue, cbuf);
	vkFreeCommandBuffers(device, cpool, 1, &cbuf);
}

void vulkan_image_transition_cbuf(VkCommandBuffer cbuf,
                VkImage image, VkImageAspectFlags aspect,
                VkImageLayout old_lt, VkImageLayout new_lt,
                VkAccessFlags src_access, VkAccessFlags dst_access,
                VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage,
                uint32_t mip_levels) {
	VkImageMemoryBarrier barrier = {0};
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.oldLayout = old_lt;
	barrier.newLayout = new_lt;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image;
	barrier.subresourceRange.aspectMask = aspect;
	barrier.subresourceRange.baseMipLevel = 0;
	barrier.subresourceRange.levelCount = mip_levels;
	barrier.subresourceRange.baseArrayLayer = 0;
	barrier.subresourceRange.layerCount = 1;
	barrier.srcAccessMask = src_access;
	barrier.dstAccessMask = dst_access;

	vkCmdPipelineBarrier(cbuf, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &barrier);
}

// Assumes src is in TRANSFER_SRC and dst is in TRANSFER_DST
void vulkan_copy_image(VkCommandBuffer cbuf, VkImage src, VkImage dst,
                VkImageAspectFlagBits aspect,
                int src_x, int src_y, int dst_x, int dst_y,
                int width, int height) {
        VkImageCopy region = {
                .srcSubresource = {
                        .aspectMask = aspect,
                        .mipLevel = 0,
                        .baseArrayLayer = 0,
                        .layerCount = 1,
                },
                .dstSubresource = {
                        .aspectMask = aspect,
                        .mipLevel = 0,
                        .baseArrayLayer = 0,
                        .layerCount = 1,
                },
                .srcOffset = {
                        .x = src_x,
                        .y = src_y,
                        .z = 0,
                },
                .dstOffset = {
                        .x = dst_x,
                        .y = dst_y,
                        .z = 0,
                },
                .extent = {
                        .width = width,
                        .height = height,
                        .depth = 1,
                },
        };

        vkCmdCopyImage(cbuf, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
}

bool vulkan_has_extension(size_t count, const char **exts, const char *find) {
	for (unsigned i = 0; i < count; ++i) {
		if (strcmp(exts[i], find) == 0u) {
			return true;
		}
	}

	return false;
}

// Image must already be in TRANSFER_DST_OPTIMAL.
void vulkan_clear_image(VkCommandBuffer cbuf, VkImage image, float color[4]) {
        VkImageSubresourceRange clear_range = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1
        };

        VkClearColorValue clear_color = {
                .float32 = {color[0], color[1], color[2], color[3]}
        };

        vkCmdClearColorImage(cbuf,
                image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                &clear_color, 1, &clear_range);
}
