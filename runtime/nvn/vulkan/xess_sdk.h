#pragma once
#include "fsr3_sdk.h"
#ifdef _WIN32
typedef struct {
    Fsr3Frame frame;
    VkImageView color_view, depth_view, motion_view, output_view;
    int mode;
} XessFrame;
void vk_xess_instance_extensions(const char **extensions, uint32_t *count, uint32_t capacity);
void vk_xess_device_extensions(const VkExtensionProperties *have, uint32_t count, const char **extensions, uint32_t *n, uint32_t capacity);
void vk_xess_enable_features(VkPhysicalDeviceFeatures2 *features, VkPhysicalDeviceVulkan11Features *v11,
                            VkPhysicalDeviceVulkan12Features *v12, VkPhysicalDeviceVulkan13Features *v13);
void vk_xess_device_created(void);
bool vk_xess_available(void);
void vk_xess_release(void); // caller waits for the last dispatch's submission
bool vk_xess_dispatch(VkCommandBuffer cb, const XessFrame *frame);
#endif
