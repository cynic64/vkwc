#ifndef vulkan_timer_h_INCLUDED
#define vulkan_timer_h_INCLUDED

#include <vulkan/vulkan.h>

double vulkan_get_elapsed(VkDevice device, VkQueryPool query_pool, double timestamp_period,
                int start_idx);

#endif // vulkan_timer_h_INCLUDED
