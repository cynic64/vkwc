#ifndef pipeline_h_INCLUDED
#define pipeline_h_INCLUDED

#include <vulkan/vulkan.h>

void create_pipeline_with_depth(VkDevice device,
                VkShaderModule vert_module, VkShaderModule frag_module,
		VkRenderPass rpass, VkPipelineLayout pipe_layout, VkPipeline *pipe);

#endif // pipeline_h_INCLUDED
