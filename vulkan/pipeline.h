#ifndef pipeline_h_INCLUDED
#define pipeline_h_INCLUDED

#include <vulkan/vulkan.h>

// Needed for PushConstants definition >:(
#include "../render/vulkan.h"

void create_pipeline_with_depth(VkDevice device,
                VkShaderModule vert_module, VkShaderModule frag_module,
		VkRenderPass rpass, VkPipelineLayout pipe_layout, VkPipeline *pipe);

void create_pipeline_layout(VkDevice device, VkSampler tex_sampler,
                int layout_count, VkDescriptorSetLayout *layouts,
		VkPipelineLayout *pipe_layout);

#endif // pipeline_h_INCLUDED
