#include "timer.h"

#include <assert.h>

// Assumes the start timestamp is at start_idx and the end timestamp is at
// start_idx + 1. Result is in seconds.
double vulkan_get_elapsed(VkDevice device, VkQueryPool query_pool, double timestamp_period,
                int start_idx) {
        uint64_t timestamps[2];
        VkResult res = vkGetQueryPoolResults(device, query_pool,
                start_idx, 2, sizeof(timestamps), timestamps, sizeof(timestamps[0]),
                VK_QUERY_RESULT_64_BIT);
        assert(res == VK_SUCCESS);
        // Divide by 1000000000 to convert ns to ms
        double elapsed = ((double) (timestamps[1] - timestamps[0]))
                * timestamp_period / 1000000000;

        return elapsed;
}
