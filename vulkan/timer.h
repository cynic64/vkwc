#ifndef vulkan_timer_h_INCLUDED
#define vulkan_timer_h_INCLUDED

#include <vulkan/vulkan.h>

#define TIMER_EVERYTHING 0
#define TIMER_RENDER_BEGIN 2
#define TIMER_RENDER_RECT 4
#define TIMER_RENDER_TEXTURE 6
#define TIMER_RENDER_END 8
#define TIMER_RENDER_END_1 10
#define TIMER_RENDER_BEGIN_1 12
#define TIMER_RENDER_TEXTURE_1 14
#define TIMER_BLUR 16
#define TIMER_BLUR_1 18
#define TIMER_COUNT 10 // Half the total number of indices

extern char *TIMER_NAMES[TIMER_COUNT];

double vulkan_get_elapsed(VkDevice device, VkQueryPool query_pool, double timestamp_period,
                int start_idx);

void vulkan_start_timer(VkCommandBuffer cbuf, VkQueryPool query_pool, int timer_idx);

void vulkan_end_timer(VkCommandBuffer cbuf, VkQueryPool query_pool, int timer_idx);

#endif // vulkan_timer_h_INCLUDED
