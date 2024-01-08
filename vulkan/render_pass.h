#ifndef render_pass_h_INCLUDED
#define render_pass_h_INCLUDED

#include <vulkan/vulkan.h>

void begin_render_pass(VkCommandBuffer cbuf, VkFramebuffer framebuffer,
                VkRenderPass rpass, VkRect2D render_area,
                int screen_width, int screen_height);

#endif // render_pass_h_INCLUDED
