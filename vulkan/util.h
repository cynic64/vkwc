#ifndef vulkan_util_h_INCLUDED
#define vulkan_util_h_INCLUDED

// This file is for stuff that isn't specific to my compositor, just generic
// helper functions.

bool vulkan_has_extension(size_t count, const char **exts, const char *find);

void vulkan_image_transition(VkDevice device, VkQueue queue, VkCommandPool cpool,
                VkImage image, VkImageAspectFlags aspect,
                VkImageLayout old_lt, VkImageLayout new_lt,
                VkAccessFlags src_access, VkAccessFlags dst_access,
                VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage,
                uint32_t mip_levels);

void vulkan_image_transition_cbuf(VkCommandBuffer cbuf,
                VkImage image, VkImageAspectFlags aspect,
                VkImageLayout old_lt, VkImageLayout new_lt,
                VkAccessFlags src_access, VkAccessFlags dst_access,
                VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage,
                uint32_t mip_levels);

void vulkan_copy_image(VkCommandBuffer cbuf, VkImage src, VkImage dst,
                VkImageAspectFlagBits aspect,
                int src_x, int src_y, int dst_x, int dst_y,
                int width, int height);

void cbuf_alloc(VkDevice device, VkCommandPool cpool, VkCommandBuffer *cbuf);

void cbuf_submit_wait(VkQueue queue, VkCommandBuffer cbuf);

void cbuf_begin_onetime(VkCommandBuffer cbuf);

void vulkan_clear_image(VkCommandBuffer cbuf, VkImage image, float clear_color[4]);

#endif // vulkan_util_h_INCLUDED
