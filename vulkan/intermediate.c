#include "intermediate.h"

// This is all stuff for the intermediate render stage, where we draw from one
// buffer into the other and then swap and so on.

// Since we swap back and forth between two framebuffers, we have to begin a
// command buffer and enter a render pass each time we want to draw something.
// This handles that.
void begin_render_operation(VkCommandBuffer cbuf, VkFramebuffer framebuffer,
                VkRenderPass rpass, VkRect2D render_area,
                int screen_width, int screen_height) {
	// Clear attachments
	VkClearValue clear_values[4] = {0};
	// intermediate color
	clear_values[0].color.float32[0] = 0.1;
	clear_values[0].color.float32[1] = 0.1;
	clear_values[0].color.float32[2] = 0.1;
	clear_values[0].color.float32[3] = 1.0;
	// depth
	clear_values[1].depthStencil.depth = 1.0;
	clear_values[1].depthStencil.stencil = 0;
	// uv
	clear_values[2].color.float32[0] = 0.0;
	clear_values[2].color.float32[1] = 0.0;
	clear_values[2].color.float32[2] = 0.0;
	// postprocess out
	clear_values[3].color.float32[0] = 0.0;
	clear_values[3].color.float32[1] = 0.0;
	clear_values[3].color.float32[2] = 0.0;

	VkRenderPassBeginInfo rpass_info = {0};
	rpass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	rpass_info.renderArea = render_area;
	rpass_info.renderPass = rpass;
	rpass_info.framebuffer = framebuffer;
	rpass_info.clearValueCount = sizeof(clear_values) / sizeof(clear_values[0]);
	rpass_info.pClearValues = clear_values;
	vkCmdBeginRenderPass(cbuf, &rpass_info, VK_SUBPASS_CONTENTS_INLINE);

	VkViewport vp = {0.f, 0.f, (float) screen_width, (float) screen_height, 0.f, 1.f};
	vkCmdSetViewport(cbuf, 0, 1, &vp);
	vkCmdSetScissor(cbuf, 0, 1, &render_area);
}

void end_render_operation(VkCommandBuffer cbuf) {
        vkCmdEndRenderPass(cbuf);
}
