#ifndef intermediate_h_INCLUDED
#define intermediate_h_INCLUDED

#include <vulkan/vulkan.h>

void begin_render_operation(VkCommandBuffer cbuf, VkFramebuffer framebuffer,
                VkRenderPass rpass, VkRect2D render_area,
                int screen_width, int screen_height);

#endif // intermediate_h_INCLUDED
