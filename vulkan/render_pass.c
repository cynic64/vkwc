#include "render_pass.h"

#include <assert.h>

// This is the render pass for when we're rendering windows: it outputs to the
// intermediate and UV.
// In render_texture, the intermediate will have been in SHADER_READ_ONLY so
// the blur pass could read it, so we need prev_intermediate_layout.
void create_render_pass(VkDevice device, VkFormat format, VkImageLayout prev_intermediate_layout,
                bool clear, VkRenderPass *rpass) {
	// Intermediate
	VkAttachmentDescription intermediate_attach = {
		.format = format,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD,
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		.initialLayout = prev_intermediate_layout,
		.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	};

	// UV
	VkAttachmentDescription uv_attach = {
		.format = UV_FORMAT,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD,
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		.initialLayout = clear ? VK_IMAGE_LAYOUT_UNDEFINED
                        : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	};

	// Attachment references
	VkAttachmentReference intermediate_out_ref = {
		.attachment = 0,
		.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	};
	VkAttachmentReference uv_attach_ref = {
		.attachment = 1,
		.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	};

	VkAttachmentReference render_attachments[] = {intermediate_out_ref, uv_attach_ref};

	VkSubpassDescription render_subpass = {
		.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
		.colorAttachmentCount = sizeof(render_attachments) / sizeof(render_attachments[0]),
		.pColorAttachments = render_attachments,
	};

        VkSubpassDescription subpasses[] = {render_subpass};

	VkSubpassDependency deps[2] = {0};
        // TODO: what the fuck is this?
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

	VkResult res = vkCreateRenderPass(device, &rpass_info, NULL, rpass);
        assert(res == VK_SUCCESS);
}

// This is for the simpler rendering strategy used in vulkan/renderer.c, which
// starts and ends the render pass once per frame instead of once per surface
// because no fancy effects are used. So we clear the buffers before drawing.
void create_simple_render_pass(VkDevice device, VkFormat format, VkRenderPass *rpass) {
	// Intermediate - clear it!
	VkAttachmentDescription intermediate_attach = {
		.format = format,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	};

	// Attachment references
	VkAttachmentReference intermediate_out_ref = {
		.attachment = 0,
		.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	};
	VkAttachmentReference render_attachments[] = {intermediate_out_ref};

	VkSubpassDescription render_subpass = {
		.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
		.colorAttachmentCount = sizeof(render_attachments) / sizeof(render_attachments[0]),
		.pColorAttachments = render_attachments,
	};

        VkSubpassDescription subpasses[] = {render_subpass};

	VkSubpassDependency deps[2] = {0};
        // TODO: what the fuck is this?
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
        };

	VkRenderPassCreateInfo rpass_info = {0};
	rpass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	rpass_info.attachmentCount = sizeof(attachments) / sizeof(attachments[0]);
	rpass_info.pAttachments = attachments;
	rpass_info.subpassCount = sizeof(subpasses) / sizeof(subpasses[0]);
	rpass_info.pSubpasses = subpasses;
	rpass_info.dependencyCount = sizeof(deps) / sizeof(deps[0]);
	rpass_info.pDependencies = deps;

	VkResult res = vkCreateRenderPass(device, &rpass_info, NULL, rpass);
        assert(res == VK_SUCCESS);
}

// Blur
void create_blur_render_pass(VkDevice device, VkFormat format, VkRenderPass *rpass) {
	// Single output
	VkAttachmentDescription output_attach = {
		.format = format,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	};

	// Attachment references
	VkAttachmentReference output_attach_ref = {
		.attachment = 0,
		.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	};

	VkAttachmentReference render_attachments[] = {output_attach_ref};

	VkSubpassDescription render_subpass = {
		.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
		.colorAttachmentCount = sizeof(render_attachments) / sizeof(render_attachments[0]),
		.pColorAttachments = render_attachments,
	};

        VkSubpassDescription subpasses[] = {render_subpass};

	VkAttachmentDescription attachments[] = {
                output_attach,
        };

	VkRenderPassCreateInfo rpass_info = {0};
	rpass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	rpass_info.attachmentCount = sizeof(attachments) / sizeof(attachments[0]);
	rpass_info.pAttachments = attachments;
	rpass_info.subpassCount = sizeof(subpasses) / sizeof(subpasses[0]);
	rpass_info.pSubpasses = subpasses;

	VkResult res = vkCreateRenderPass(device, &rpass_info, NULL, rpass);
        assert(res == VK_SUCCESS);
}

// It's different because we output to the screen instead of UV, depth and
// intermediate. TODO: get rid of intermediate, depth and UV from here.
void create_postprocess_render_pass(VkDevice device, VkFormat format, VkRenderPass *rpass) {
	// Intermediate
	VkAttachmentDescription intermediate_attach = {
		.format = format,
		.samples = VK_SAMPLE_COUNT_1_BIT,
                // We do need to load it because we might sample it.
		.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
		.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		.initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	};

	// UV
	VkAttachmentDescription uv_attach = {
		.format = UV_FORMAT,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		.initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	};

        // Screen output
	VkAttachmentDescription screen_attach = {
		.format = format,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	};

	// Attachment references
	VkAttachmentReference screen_attach_ref = {
		.attachment = 2,
		.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	};

	VkAttachmentReference render_attachments[] = {screen_attach_ref};

	VkSubpassDescription render_subpass = {
		.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
		.colorAttachmentCount = sizeof(render_attachments) / sizeof(render_attachments[0]),
		.pColorAttachments = render_attachments,
	};

        VkSubpassDescription subpasses[] = {render_subpass};

	VkSubpassDependency deps[1] = {0};
	// Memory reads and writes must wait on this frame finishing rendering
	deps[0].srcSubpass = 0;
	deps[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	deps[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	deps[0].dstSubpass = VK_SUBPASS_EXTERNAL;
	deps[0].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT |
	        VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
	deps[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_MEMORY_READ_BIT;

	VkAttachmentDescription attachments[] = {
                intermediate_attach,
                uv_attach,
                screen_attach,
        };

	VkRenderPassCreateInfo rpass_info = {0};
	rpass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	rpass_info.attachmentCount = sizeof(attachments) / sizeof(attachments[0]);
	rpass_info.pAttachments = attachments;
	rpass_info.subpassCount = sizeof(subpasses) / sizeof(subpasses[0]);
	rpass_info.pSubpasses = subpasses;
	rpass_info.dependencyCount = sizeof(deps) / sizeof(deps[0]);
	rpass_info.pDependencies = deps;

	VkResult res = vkCreateRenderPass(device, &rpass_info, NULL, rpass);
        assert(res == VK_SUCCESS);
}

void begin_render_pass(VkCommandBuffer cbuf, VkFramebuffer framebuffer,
                VkRenderPass rpass, VkRect2D render_area,
                int screen_width, int screen_height) {
	// Clear attachments
	VkClearValue clear_values[4] = {0};
	// Intermediate color
	clear_values[0].color.float32[0] = 0;
	clear_values[0].color.float32[1] = 0;
	clear_values[0].color.float32[2] = 0;
	clear_values[0].color.float32[3] = 1;

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

// TODO: get rid of this and use begin_render_pass for everything?
void begin_postprocess_render_pass(VkCommandBuffer cbuf, VkFramebuffer framebuffer,
                VkRenderPass rpass, VkRect2D render_area,
                int screen_width, int screen_height) {
	VkRenderPassBeginInfo rpass_info = {0};
	rpass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	rpass_info.renderArea = render_area;
	rpass_info.renderPass = rpass;
	rpass_info.framebuffer = framebuffer;
        // None of the attachments actually gets cleared.
	rpass_info.clearValueCount = 0;
	rpass_info.pClearValues = NULL;
	vkCmdBeginRenderPass(cbuf, &rpass_info, VK_SUBPASS_CONTENTS_INLINE);

	VkViewport vp = {0.f, 0.f, (float) screen_width, (float) screen_height, 0.f, 1.f};
	vkCmdSetViewport(cbuf, 0, 1, &vp);
	vkCmdSetScissor(cbuf, 0, 1, &render_area);
}
