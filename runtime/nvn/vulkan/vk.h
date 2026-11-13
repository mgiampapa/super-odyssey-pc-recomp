// Vulkan backend for the NVN layer (Windows; macOS uses runtime/nvn/metal.m + renderer.m).
// Files: device.c (instance, window, swapchain, present, screenshots, ImGui hooks), textures.c (images, uploads,
// views), exec.c (command execution: passes, draws, clears, copies, compute), shaders.c (GLSL -> SPIR-V, programs,
// pipelines). Mirrors the Metal backend's behaviour; differences are noted where they happen.
//
// Synchronisation model: every image lives in VK_IMAGE_LAYOUT_GENERAL, and a full memory barrier separates
// render passes, copies and dispatches (Metal tracks these hazards itself). Submissions are numbered; a timeline
// semaphore signals each number, which is what NVN fences (renderer_fence_seq / renderer_wait_seq) wait for.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#if __has_include(<vma/vk_mem_alloc.h>)
#include <vma/vk_mem_alloc.h>
#else
#include <vk_mem_alloc.h>  // (Debian / Ubuntu libvulkan-memory-allocator-dev)
#endif

#include "../nvn.h"
#include "../maxwell/shader.h"

void vk_device_fault_report(void);
#define VK_CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { if (r_ == VK_ERROR_DEVICE_LOST) vk_device_fault_report(); \
    FATAL("%s failed: %d (%s:%d)\n", #x, r_, __FILE__, __LINE__); } } while (0)

typedef struct {
    VkInstance inst;
    VkPhysicalDevice phys;
    VkDevice dev;
    VkQueue queue;
    uint32_t qfam;
    VmaAllocator vma;
    VkPhysicalDeviceProperties props;
    VkPipelineCache pcache;
    VkSemaphore timeline;      // value = last completed submission number
    VkCommandPool pool;        // render thread only
    bool custom_border, divisor, host_import, checkpoints;
    bool feat_aniso;  // samplerAnisotropy enabled (textures.c: the Anisotropic filtering setting)
    bool gpl;  // VK_EXT_graphics_pipeline_library with fast linking: shaders compile per program, pipelines link (exec.c)
    bool pbin;  // VK_KHR_pipeline_binary: shader libraries saved as data and created from it (exec.c lib_binary_*)
    bool headless;  // the shader preparation's process (device.c vk_headless_init)
    bool cache_control;  // pipelineCreationCacheControl: "only if the driver has it compiled" (exec.c libs_from_cache)
    bool storage_write_without_format;
    // window
    VkSurfaceKHR surface;
    VkSwapchainKHR swapchain;
    VkFormat sc_format;
    VkExtent2D sc_extent;
    uint32_t sc_count;
    VkImage sc_images[8];
    VkImageView sc_views[8];
} VkG;
extern VkG g_vk;

// ---- submissions (exec.c)
// Work is recorded into the current command buffer; uploads go into a separate one that runs first.
VkCommandBuffer vk_cmd(void);         // current command buffer (outside any rendering unless exec.c began one)
VkCommandBuffer vk_upload_cmd(void);  // upload command buffer of the pending submission
uint64_t vk_pending_seq(void);        // number the pending submission will get
void vk_render_lock(void);            // one thread at a time in the renderer (recursive; exec.c)
void vk_render_unlock(void);
uint64_t vk_completed_seq(void);
void vk_wait_seq(uint64_t seq);
void vk_full_barrier(VkCommandBuffer cb);
void vk_end_pass(void);
void vk_submit_extra(VkSemaphore wait, VkSemaphore signal, VkCommandBuffer extra);  // submit + one more cb (present)
void vk_checkpoint(VkCommandBuffer cb, const char *fmt, ...);  // (ODYSSEY_VK_CHECKPOINTS) a named GPU marker
void vk_checkpoints_report(void);                              // the last markers the GPU reached (device loss)

// Transient GPU-visible memory that lives until the submission it was allocated for completes.
typedef struct { VkBuffer buf; uint64_t off; void *ptr; } VkSpan;
VkSpan vk_transient(uint64_t size, uint64_t align);
VkSpan vk_transient_copy(const void *data, uint64_t size, uint64_t align);
VkSpan vk_readback(uint64_t size);  // host-cached GPU -> CPU staging (read right after the copy completes)
VkDescriptorSet vk_alloc_set(VkDescriptorSetLayout layout);

// ---- textures (textures.c)
typedef struct TexBackend {
    VkImage image;
    VmaAllocation mem;
    VkFormat format;
    VkFormat guest_format;  // the format of the game's bytes when the image stores another (textures.c "Banding": half
                            // floats for its RGB10A2 screen targets): uploads and readbacks convert through a blit. 0: format
    bool debandable;        // one of those targets: half floats while vk_deband()
    VkImageViewType base_type;
    uint32_t width, height, depth, layers, levels, samples;
    VkImageAspectFlags aspect;
    bool gpu_owned, uploaded, dynamic, storage;
    bool paint_cpu_pending; // explicit CPU edits must reach the image even if its next use is a render target
    uint64_t data_hash, sample_hash, checked_frame, full_frame;
    uint32_t wgen;  // page write tracking generation at the last upload check (textures.c)
    bool scalable, presented;
    uint64_t touched_frame, frame_cleared;
    uint32_t level_gen;
    float sx, sy;
    void *pending_rgba;  // host-produced pixels (video) waiting to be uploaded on the render thread
    uint32_t pending_w, pending_h, pending_stride;
    uint8_t astc_bw, astc_bh;  // ASTC without GPU support: decoded to RGBA8 on upload (runtime/nvn/astc.c)
    bool astc_srgb;
    int reg;  // slot in textures.c's live list + 1 (a reboot destroys them all)
    int8_t kbp;        // the game's button icon sheet (nvn/kbprompts.c): 1 yes, -1 no, 0 not looked at
    uint32_t kbp_gen;  // what its icons showed at its last upload (kbprompts_generation)
} TexBackend;

TexBackend *vk_tex(NvnTexture *t, bool for_render);  // backend image (uploads CPU contents unless for_render)
bool vk_deband(void);  // smooth gradients (textures.c "Banding"): the menu's setting, ODYSSEY_DEBAND=0|1 overrides it
VkFormat vk_format(uint32_t nvn_format);
bool vk_format_alpha_one(uint32_t nvn_format);
// View of t (optionally through an NVN texture view) for rendering or sampling.
VkImageView vk_view(NvnTexture *t, const NvnTextureView *v, bool has_view, bool render, int want_textype);
VkImageView vk_storage_view(NvnTexture *t, const NvnTextureView *v, const ShaderImage *img);
VkImageView vk_image_view(TexBackend *b, VkImageViewType type, VkFormat fmt, uint32_t lv0, uint32_t nlv, uint32_t ly0,
                          uint32_t nly, VkComponentMapping sw, VkImageAspectFlags aspect);
VkImageView vk_dummy_view(int textype);
VkSampler vk_sampler(NvnSamplerDesc *d);
VkSampler vk_default_sampler(bool linear);
void vk_image_init_layout(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect);
// Destruction once every submission that may use the object has completed.
void vk_defer_image(VkImage img, VmaAllocation mem);
// buffer -> image in guest texels (textures.c: a scaled allocation gets the region scaled in)
void vk_copy_buffer_to_image(VkCommandBuffer cb, TexBackend *b, VkBuffer buf, const VkBufferImageCopy *rg);
void vk_defer_view(VkImageView v);
void vk_views_forget(TexBackend *b);  // drop (deferred) the cached views of an image being replaced
uint64_t vk_frame_counter(void);

// ---- shaders / pipelines (shaders.c)
VkShaderModule vk_compile_glsl(const char *src, int stage, const char *name);  // NULL on failure (logged)
VkShaderModule vk_builtin_shader(const char *name);  // internal shaders (clear, copy, present)

// ---- device / window (device.c)
void vk_save_png(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h);
