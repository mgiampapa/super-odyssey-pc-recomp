#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// What the performance overlay shows (runtime/ui/menu.mm draw_performance), sampled once a second on a thread of its
// own while the overlay is visible (runtime/nvn/vulkan/performance.c). -1 / 0: not known on this PC - the overlay
// shows "-" (a CPU's temperature on Windows needs a kernel driver; a GPU's sensors come from NVIDIA's NVML, or on
// Linux from the kernel's hwmon files for AMD and Intel).
typedef struct {
    const char *cpu_name, *gpu_name;
    unsigned vram_mib, vram_budget_mib;
    int cpu_usage, gpu_usage;                 // percent
    int game_cpu_usage;                       // this process, percent of the whole CPU
    int gpu_temp_c, gpu_power_w, gpu_clock_mhz, cpu_temp_c;
    unsigned ram_used_mib, ram_total_mib, game_ram_mib;
    unsigned render_w, render_h, out_w, out_h;  // the game's internal resolution -> its picture in the window
} VulkanOverlayStats;

VulkanOverlayStats vk_overlay_stats(void);
void vk_overlay_memory(uint64_t used, uint64_t budget);
void vk_overlay_resolution(unsigned render_w, unsigned render_h, unsigned out_w, unsigned out_h);

// Mario's position in the stage (runtime/hle/odyssey_120.c), in the game's units; false: none in the last half second
bool odyssey_player_position(float p[3]);

#ifdef __cplusplus
}
#endif
