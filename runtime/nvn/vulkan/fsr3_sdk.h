#pragma once
#include "vk.h"

#ifdef _WIN32
typedef struct {
    VkImage color, depth, motion, output;
    VkFormat color_format;
    uint32_t w, h, ow, oh;
    float jitter[2], dt_ms, near_z, far_z, fov_y, sharpness;
    bool reset, ml;
} Fsr3Frame;
void vk_fsr3_init(void);
bool vk_fsr3_available(void);
bool vk_fsr4_available(void);
void vk_fsr3_release(void); // caller must wait for the last dispatch's submission
bool vk_fsr3_dispatch(VkCommandBuffer cb, const Fsr3Frame *frame);
#endif
