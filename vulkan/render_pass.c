#include "render_pass.h"

void begin_render_pass(VkCommandBuffer cbuf, VkFramebuffer framebuffer,
                VkRenderPass rpass, VkRect2D render_area,
                int screen_width, int screen_height) {
	// Clear attachments
	VkClearValue clear_values[4] = {0};
	// intermediate color - don't set, we keep what's there
	// depth
	clear_values[1].depthStencil.depth = 1.0;
	clear_values[1].depthStencil.stencil = 0;
	// postprocess out
	clear_values[3].color.float32[0] = 0.0;
	clear_values[3].color.float32[1] = 0.0;
	clear_values[3].color.float32[2] = 0.0;

	VkRenderPassBeginInfo rpass_info = {0};
	rpass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	rpass_info.renderArea = render_area;
	rpass_info.renderPass = rpass;
	rpass_info.framebuffer = framebuffer;
	rpass_info.clearValueCount = 4;
	rpass_info.pClearValues = clear_values;
	vkCmdBeginRenderPass(cbuf, &rpass_info, VK_SUBPASS_CONTENTS_INLINE);

	VkViewport vp = {0.f, 0.f, (float) screen_width, (float) screen_height, 0.f, 1.f};
	vkCmdSetViewport(cbuf, 0, 1, &vp);
	vkCmdSetScissor(cbuf, 0, 1, &render_area);
}
