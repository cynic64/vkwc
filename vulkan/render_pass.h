#ifndef render_pass_h_INCLUDED
#define render_pass_h_INCLUDED

#include <vulkan/vulkan.h>

static const VkFormat DEPTH_FORMAT = VK_FORMAT_D32_SFLOAT;
static const VkFormat UV_FORMAT = VK_FORMAT_R8G8B8A8_UNORM;

void begin_render_pass(VkCommandBuffer cbuf, VkFramebuffer framebuffer,
                VkRenderPass rpass, VkRect2D render_area,
                int screen_width, int screen_height);

void create_render_pass(VkDevice device, VkFormat format, VkRenderPass *rpass);

void create_postprocess_render_pass(VkDevice device, VkFormat format, VkRenderPass *rpass);

void create_simple_render_pass(VkDevice device, VkFormat format, VkRenderPass *rpass);

void begin_postprocess_render_pass(VkCommandBuffer cbuf, VkFramebuffer framebuffer,
                VkRenderPass rpass, VkRect2D render_area,
                int screen_width, int screen_height);

void create_blur_render_pass(VkDevice device, VkFormat format, VkRenderPass *rpass);

#endif // render_pass_h_INCLUDED
