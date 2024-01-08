#ifndef error_h_INCLUDED
#define error_h_INCLUDED

#include <vulkan/vulkan.h>

const char *vulkan_strerror(VkResult err);

#endif // error_h_INCLUDED
