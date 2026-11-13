#pragma once

#include "vk.h"

#ifdef _WIN32
typedef struct { float view[12], projection[16]; } DlssCamera;
void vk_dlss_probe_instance(const char **ext, uint32_t *count, uint32_t capacity);
void vk_dlss_probe_device(const VkExtensionProperties *have, uint32_t available,
                          const char **ext, uint32_t *count, uint32_t capacity);
void vk_dlss_probe_run(void);
bool vk_temporal_enabled(void);
double vk_temporal_scale(void);
void vk_dlss_queue_camera(const DlssCamera *camera);
void vk_dlss_set_camera(const DlssCamera *camera);
void vk_dlss_capture_depth(const TexBackend *depth, uint64_t frame);
void vk_dlss_viewport_jitter(const TexBackend *depth, uint64_t frame, float offset[2]);
float vk_dlss_surface_lod_bias(const TexBackend *depth);
VkImageView vk_dlss_upscale(VkCommandBuffer cb, const TexBackend *color, VkImageView view,
                            uint32_t ow, uint32_t oh, uint64_t frame);
#endif
