#include "pipeline.h"

#include <assert.h>
#include <stdlib.h>

// Generic pipeline, it turns out all of ours are pretty similar. The window
// rendering pass renders to color and UV targets, but the postprocess only
// renders to final color. So that's why we have output_attach_count.
void create_pipeline(VkDevice device,
                VkShaderModule vert_module, VkShaderModule frag_module,
		VkRenderPass rpass, int output_attach_count,
                VkPipelineLayout pipe_layout, VkPipeline *pipe) {
	// Shaders
	VkPipelineShaderStageCreateInfo vert_stage = {
		.sType= VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		.stage = VK_SHADER_STAGE_VERTEX_BIT,
                .module = vert_module,
		.pName = "main",
	};
	VkPipelineShaderStageCreateInfo frag_stage = {
		.sType= VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		.stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                .module = frag_module,
		.pName = "main",
	};

	VkPipelineShaderStageCreateInfo shader_stages[] = {vert_stage, frag_stage};

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

        
        VkPipelineColorBlendAttachmentState *blend_attachments =
                malloc(output_attach_count * sizeof(blend_attachments[0]));
        for (int i = 0; i < output_attach_count; i++) {
                memcpy(&blend_attachments[i], &blend_attachment, sizeof(blend_attachments[0]));
        }

	VkPipelineColorBlendStateCreateInfo blend = {0};
	blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	blend.attachmentCount = output_attach_count;
	blend.pAttachments = blend_attachments;

        // Multisampling
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

        // Vertex input state
	VkPipelineVertexInputStateCreateInfo vertex = {0};
	vertex.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

        // Final info
	VkGraphicsPipelineCreateInfo pinfo = {0};
	pinfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pinfo.layout = pipe_layout;
	pinfo.renderPass = rpass;
	pinfo.subpass = 0;
	pinfo.stageCount = sizeof(shader_stages) / sizeof(shader_stages[0]);
	pinfo.pStages = shader_stages;

	pinfo.pInputAssemblyState = &assembly;
	pinfo.pRasterizationState = &rasterization;
	pinfo.pColorBlendState = &blend;
	pinfo.pMultisampleState = &multisample;
	pinfo.pViewportState = &viewport;
	pinfo.pDynamicState = &dynamic;
	pinfo.pVertexInputState = &vertex;

	VkResult res = vkCreateGraphicsPipelines(device, NULL, 1, &pinfo, NULL, pipe);
	assert(res == VK_SUCCESS);

        free(blend_attachments);
}

// Create a pipeline layout with PushConstants. You have to make the descriptor layouts first.
void create_pipeline_layout(VkDevice device, VkSampler tex_sampler,
                int layout_count, VkDescriptorSetLayout *layouts,
		VkPipelineLayout *pipe_layout) {
	// Pipeline layout
	VkPushConstantRange pc_ranges[1] = {0};
	pc_ranges[0].size = sizeof(struct PushConstants);
	pc_ranges[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

	VkPipelineLayoutCreateInfo pl_info = {0};
	pl_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pl_info.setLayoutCount = layout_count;
	pl_info.pSetLayouts = layouts;
	pl_info.pushConstantRangeCount = sizeof(pc_ranges) / sizeof(pc_ranges[0]);
	pl_info.pPushConstantRanges = pc_ranges;

	VkResult res = vkCreatePipelineLayout(device, &pl_info, NULL, pipe_layout);
        assert(res == VK_SUCCESS);
}
