#include "dlss_probe.h"
#include "fsr3_sdk.h"
#include "xess_sdk.h"

#ifdef _WIN32
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

#include "../../platform.h"
#include "../../settings.h"
#include "../rqueue.h"

// NGX Vulkan exports in the NVIDIA driver. Keep the SDK and the model DLL optional.
typedef struct NgxParameters NgxParameters;
typedef struct NgxFeature NgxFeature;
typedef unsigned (*NgxGetInt)(NgxParameters *, const char *, int *);
typedef void (*NgxSetInt)(NgxParameters *, const char *, int);
typedef void (*NgxSetUint)(NgxParameters *, const char *, unsigned);
typedef void (*NgxSetFloat)(NgxParameters *, const char *, float);
typedef void (*NgxSetPtr)(NgxParameters *, const char *, void *);
struct NgxParameters { const void *const *methods; };
static struct {
    HMODULE library;
    unsigned (*init)(unsigned long long, const wchar_t *, VkInstance, VkPhysicalDevice, VkDevice,
                     PFN_vkGetInstanceProcAddr, PFN_vkGetDeviceProcAddr, unsigned, const void *);
    unsigned (*extensions)(unsigned *, const char ***, unsigned *, const char ***);
    unsigned (*capabilities)(NgxParameters **);
    unsigned (*destroy)(NgxParameters *);
    unsigned (*shutdown)(VkDevice);
    unsigned (*allocate)(NgxParameters **);
    unsigned (*create)(VkCommandBuffer, int, NgxParameters *, NgxFeature **);
    unsigned (*evaluate)(VkCommandBuffer, const NgxFeature *, const NgxParameters *, void *);
    unsigned (*release)(NgxFeature *);
    const char **device_ext;
    unsigned device_count;
} ngx;
static atomic_bool available;
static bool forced_off;

static int temporal_mode(void) {
    return *settings_temporal_mode();
}

bool vk_temporal_enabled(void) {
    if (g_settings.temporal_backend == TEMPORAL_FSR3)
        return g_settings.fsr3_mode != DLSS_OFF && vk_fsr3_available();
    if (g_settings.temporal_backend == TEMPORAL_FSR4)
        return g_settings.fsr4_mode != DLSS_OFF && (vk_fsr4_available() || vk_fsr3_available());
    if (g_settings.temporal_backend == TEMPORAL_XESS)
        return g_settings.xess_mode != DLSS_OFF && vk_xess_available();
    return !forced_off && g_settings.dlss_mode != DLSS_OFF && atomic_load_explicit(&available, memory_order_relaxed);
}

double vk_temporal_scale(void) {
    if (!vk_temporal_enabled()) return 0;
    static const double scales[DLSS_MODE_COUNT] = {0, 1, 2.0 / 3.0, 1.0 / 1.7, 0.5};
    if (g_settings.temporal_backend == TEMPORAL_XESS) {
        static const double xess_scales[DLSS_MODE_COUNT] = {0, 1, 1.0/1.7, 0.5, 1.0/2.3};
        return xess_scales[temporal_mode()];
    }
    return scales[temporal_mode()];
}

static void close_ngx(void) {
    if (ngx.library) FreeLibrary(ngx.library);
    memset(&ngx, 0, sizeof ngx);
}

static bool add_extensions(const VkExtensionProperties *have, uint32_t available,
                           const char **ext, uint32_t *count, uint32_t capacity,
                           const char **needed, uint32_t required) {
    uint32_t next = *count;
    for (uint32_t i = 0; i < required; i++) {
        uint32_t k = 0;
        while (k < next && strcmp(ext[k], needed[i])) k++;
        if (k < next) continue;
        k = 0;
        while (k < available && strcmp(have[k].extensionName, needed[i])) k++;
        if (k == available || next == capacity) {
            LOG("[dlss] cannot enable required extension %s\n", needed[i]);
            return false;
        }
        ext[next++] = needed[i];
    }
    *count = next;
    return true;
}

void vk_dlss_probe_instance(const char **ext, uint32_t *count, uint32_t capacity) {
    // Discover the optional driver path at start, including when the selected mode is Off.
    const char *override = getenv("ODYSSEY_DLSS");  // tests can disable the driver path
    forced_off = override && !strcmp(override, "0");
    if (forced_off) return;
    // Probe capability even when initially Off, so enabling it in the menu can
    // use the Vulkan extensions already enabled on this device.
    wchar_t path[1024];
    DWORD bytes = sizeof path;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", L"FullPath",
                     RRF_RT_REG_SZ, NULL, path, &bytes) != ERROR_SUCCESS ||
        wcslen(path) + sizeof L"\\_nvngx.dll" / sizeof(wchar_t) > sizeof path / sizeof *path) {
        LOG("[dlss] NVIDIA NGX driver location unavailable\n");
        return;
    }
    wcscat(path, L"\\_nvngx.dll");
    ngx.library = LoadLibraryExW(path, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ngx.library) {
        LOG("[dlss] cannot load driver NGX core (Windows error %lu)\n", (unsigned long)GetLastError());
        return;
    }
#define GET(member, name) ngx.member = (void *)GetProcAddress(ngx.library, "NVSDK_NGX_VULKAN_" name)
    GET(init, "Init_Ext2");
    GET(extensions, "RequiredExtensions");
    GET(capabilities, "GetCapabilityParameters");
    GET(destroy, "DestroyParameters");
    GET(shutdown, "Shutdown1");
    GET(allocate, "AllocateParameters");
    GET(create, "CreateFeature");
    GET(evaluate, "EvaluateFeature");
    GET(release, "ReleaseFeature");
#undef GET
    unsigned required = 0;
    const char **needed = NULL;
    if (!ngx.init || !ngx.extensions || !ngx.capabilities || !ngx.destroy || !ngx.shutdown ||
        ngx.extensions(&required, &needed, &ngx.device_count, &ngx.device_ext) != 1) {
        LOG("[dlss] NGX Vulkan entry points or requirements unavailable\n");
        close_ngx();
        return;
    }
    uint32_t available = 0;
    VkResult r = vkEnumerateInstanceExtensionProperties(NULL, &available, NULL);
    VkExtensionProperties *have = r == VK_SUCCESS && available ? calloc(available, sizeof *have) : NULL;
    if (!have || vkEnumerateInstanceExtensionProperties(NULL, &available, have) != VK_SUCCESS ||
        !add_extensions(have, available, ext, count, capacity, needed, required)) close_ngx();
    free(have);
}

void vk_dlss_probe_device(const VkExtensionProperties *have, uint32_t available,
                          const char **ext, uint32_t *count, uint32_t capacity) {
    if (!ngx.library) return;
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(g_vk.phys, &props);
    if (props.vendorID != 0x10de) {
        LOG("[dlss] selected Vulkan device is not NVIDIA\n");
        close_ngx();
    } else if (!add_extensions(have, available, ext, count, capacity, ngx.device_ext, ngx.device_count)) {
        close_ngx();
    }
}

void vk_dlss_probe_run(void) {
    if (!ngx.library) return;
    wchar_t dir[1024];
    DWORD len = GetModuleFileNameW(NULL, dir, sizeof dir / sizeof *dir);
    wchar_t *last = len && len < sizeof dir / sizeof *dir ? wcsrchr(dir, L'\\') : NULL;
    if (last) {
        *last = L'\0';
        // Same SDK API version and development app ID as the Zelda reference.
        unsigned r = ngx.init(231313132ull, dir, g_vk.inst, g_vk.phys, g_vk.dev,
                              vkGetInstanceProcAddr, vkGetDeviceProcAddr, 0x15, NULL);
        if (r == 1) {
            NgxParameters *caps = NULL;
            int supported = 0;
            r = ngx.capabilities(&caps);
            if (r == 1 && caps) {
                // The driver's MSVC parameter interface has Get(int*) at slot 11.
                NgxGetInt get_int = (NgxGetInt)caps->methods[11];
                r = get_int(caps, "SuperSampling.Available", &supported);
                ngx.destroy(caps);
            }
            LOG("[dlss] NGX Super Resolution capability: %s (query %08x)\n",
                r == 1 && supported ? "available" : "unavailable", r);
            if (r == 1 && supported && ngx.allocate && ngx.create && ngx.evaluate && ngx.release) {
                VkPhysicalDeviceFeatures features;
                vkGetPhysicalDeviceFeatures(g_vk.phys, &features);
                if (features.shaderStorageImageExtendedFormats) {
                    atomic_store_explicit(&available, true, memory_order_relaxed);
                    return;
                }
            }
            ngx.shutdown(g_vk.dev);
        } else LOG("[dlss] Vulkan NGX initialization failed (%08x)\n", r);
    } else LOG("[dlss] cannot locate the application folder\n");
    // The instance and device have consumed their extension names by now.
    close_ngx();
}

typedef struct {
    union {
        struct { VkImageView view; VkImage image; VkImageSubresourceRange range; VkFormat format; unsigned w, h; } img;
        struct { VkBuffer buffer; unsigned bytes; } buf;
    } data;
    int type;
    bool writable;
} NgxResource;

typedef struct { VkImage image; VkImageView view; VmaAllocation memory; } DlssImage;
static struct {
    DlssCamera camera, scene, previous;
    bool camera_ready, scene_ready, history;
    VkImage source_depth, previous_depth, jitter_depth;
    uint64_t frame, last_eval, last_eval_ns, eval_count, input_seq, output_seq;
    DlssImage depth, motion, output;
    uint32_t w, h, ow, oh;
    uint64_t jitter_frame;
    float jitter[2], scene_jitter[2]; // raster displacement in input pixels, positive right/down
    VkDescriptorSetLayout motion_set;
    VkPipelineLayout motion_layout;
    VkPipeline motion_pipeline;
    NgxParameters *parameters;
    NgxFeature *feature;
    int feature_mode, feature_preset, feature_backend;
    bool feature_ml;
} dlss;

static float halton(unsigned index, unsigned base) {
    float value = 0, weight = 1;
    while (index) { weight /= base; value += weight * (index % base); index /= base; }
    return value;
}

float vk_dlss_surface_lod_bias(const TexBackend *depth) {
    if (!vk_temporal_enabled() || !depth || depth->image != dlss.source_depth || !dlss.ow || !dlss.w) return 0;
    // Choose texture detail for the reconstructed pixel size, rather than the
    // coarser input pixel size. DLAA and non-upscaled output need no adjustment.
    return dlss.w < dlss.ow ? log2f((float)dlss.w / dlss.ow) : 0;
}

void vk_dlss_viewport_jitter(const TexBackend *depth, uint64_t frame, float offset[2]) {
    offset[0] = offset[1] = 0;
    // Learn the exact main-scene depth image from the bloom input, rather than
    // jittering every depth target (which would also move shadow maps and UI).
    if (!vk_temporal_enabled() || !depth || depth->image != dlss.source_depth ||
        depth->width != dlss.w || depth->height != dlss.h || !dlss.camera_ready) return;
    double scale = vk_temporal_scale();
    if (!(scale > 0 && scale <= 1)) return; // Off can be selected between the readiness and scale checks
    unsigned phases = (unsigned)ceil(8.0 / (scale * scale));
    unsigned sample = (unsigned)(frame % phases) + 1;
    dlss.jitter_frame = frame;
    dlss.jitter_depth = depth->image;
    offset[0] = dlss.jitter[0] = halton(sample, 2) - 0.5f;
    offset[1] = dlss.jitter[1] = halton(sample, 3) - 0.5f;
}

// ODYSSEY_DLSS_TIMING=1: timestamp the motion pass and DLSS together without waiting for a frame.
#define DLSS_TIMING_SLOTS 8
static struct {
    VkQueryPool pool;
    uint64_t seq[DLSS_TIMING_SLOTS], frame[DLSS_TIMING_SLOTS];
    unsigned next;
    bool attempted;
} timing;

static int timing_begin(VkCommandBuffer cb, uint64_t frame) {
    if (!timing.attempted) {
        timing.attempted = true;
        if (getenv("ODYSSEY_DLSS_TIMING")) {
            VkQueryPoolCreateInfo info = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2 * DLSS_TIMING_SLOTS};
            if (vkCreateQueryPool(g_vk.dev, &info, NULL, &timing.pool) != VK_SUCCESS) timing.pool = VK_NULL_HANDLE;
        }
    }
    if (!timing.pool) return -1;
    unsigned slot = timing.next++ % DLSS_TIMING_SLOTS;
    if (timing.seq[slot] > vk_completed_seq()) return -1;
    if (timing.seq[slot]) {
        uint64_t timestamps[2];
        if (vkGetQueryPoolResults(g_vk.dev, timing.pool, 2 * slot, 2, sizeof timestamps, timestamps,
                                  sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            double ms = (timestamps[1] - timestamps[0]) * g_vk.props.limits.timestampPeriod / 1e6;
            if (ms > 9 || !(timing.frame[slot] % 120))
                LOG("[dlss-time] frame %llu GPU %.2f ms\n", (unsigned long long)timing.frame[slot], ms);
        }
    }
    vkCmdResetQueryPool(cb, timing.pool, 2 * slot, 2);
    vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, timing.pool, 2 * slot);
    timing.seq[slot] = vk_pending_seq();
    timing.frame[slot] = frame;
    return (int)slot;
}

void vk_dlss_set_camera(const DlssCamera *camera) {
    static bool logged;
    if (!logged && getenv("ODYSSEY_DLSS_HISTORY_TRACE")) { LOG("[dlss-history] first camera received\n"); logged = true; }
    dlss.camera = *camera;
    dlss.camera_ready = true;
}

void vk_dlss_queue_camera(const DlssCamera *camera) {
    if (!rq_enabled() || rq_on_thread()) { vk_dlss_set_camera(camera); return; }
    DlssCamera *copy = malloc(sizeof *copy);
    if (copy) {
        *copy = *camera;
        rq_push(RQ_DLSS_CAMERA, copy, 0);
    }
}

static bool failed(const char *step, unsigned result) {
    LOG("[dlss] %s failed (%08x); using the ordinary upscaler\n", step, result);
    atomic_store_explicit(&available, false, memory_order_relaxed);
    return false;
}

static bool make_image(DlssImage *img, VkCommandBuffer cb, uint32_t w, uint32_t h, VkFormat fmt,
                       VkImageUsageFlags usage, VkImageAspectFlags aspect) {
    VkImageCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = fmt, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo alloc = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkResult r = vmaCreateImage(g_vk.vma, &info, &alloc, &img->image, &img->memory, NULL);
    if (r != VK_SUCCESS) return failed("image allocation", (unsigned)r);
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt, .subresourceRange = {aspect, 0, 1, 0, 1}};
    r = vkCreateImageView(g_vk.dev, &vi, NULL, &img->view);
    if (r != VK_SUCCESS) return failed("image view", (unsigned)r);
    vk_image_init_layout(cb, img->image, aspect);
    return true;
}

static void destroy_image(DlssImage *img) {
    if (img->view) vkDestroyImageView(g_vk.dev, img->view, NULL);
    if (img->image) vmaDestroyImage(g_vk.vma, img->image, img->memory);
    *img = (DlssImage){0};
}

static bool reset_output(void) {
    if (dlss.output_seq >= vk_pending_seq()) return false;
    if (dlss.output_seq) vk_wait_seq(dlss.output_seq);
    vk_fsr3_release();
    vk_xess_release();
    if (dlss.feature) ngx.release(dlss.feature);
    destroy_image(&dlss.output);
    dlss.feature = NULL;
    dlss.ow = dlss.oh = 0;
    dlss.history = false;
    dlss.output_seq = 0;
    return true;
}

void vk_dlss_capture_depth(const TexBackend *depth, uint64_t frame) {
    if (!vk_temporal_enabled() || !dlss.camera_ready || (dlss.scene_ready && dlss.frame == frame) ||
        depth->width < 640 || depth->height < 360 || depth->samples != 1 || depth->layers != 1) return;
    vk_end_pass();
    if (dlss.w && (dlss.w != depth->width || dlss.h != depth->height)) {
        if (dlss.input_seq >= vk_pending_seq() || !reset_output()) return;
        if (dlss.input_seq) vk_wait_seq(dlss.input_seq);
        destroy_image(&dlss.depth);
        destroy_image(&dlss.motion);
        dlss.input_seq = 0;
    }
    VkCommandBuffer cb = vk_cmd();
    if (!dlss.depth.image) {
        dlss.w = depth->width;
        dlss.h = depth->height;
        if (!make_image(&dlss.depth, cb, dlss.w, dlss.h, VK_FORMAT_D32_SFLOAT,
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_DEPTH_BIT)) return;
    }
    vk_full_barrier(cb);
    VkImageSubresourceLayers layers = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    VkImageCopy region = {.srcSubresource = layers, .dstSubresource = layers, .extent = {dlss.w, dlss.h, 1}};
    vkCmdCopyImage(cb, depth->image, VK_IMAGE_LAYOUT_GENERAL, dlss.depth.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    dlss.input_seq = vk_pending_seq();
    dlss.source_depth = depth->image;
    bool jittered = dlss.jitter_frame == frame && dlss.jitter_depth == depth->image;
    dlss.scene_jitter[0] = jittered ? dlss.jitter[0] : 0;
    dlss.scene_jitter[1] = jittered ? dlss.jitter[1] : 0;
    if (getenv("ODYSSEY_DLSS_HISTORY_TRACE") && frame % 120 == 0)
        LOG("[dlss-jitter] frame %llu raster %.4f %.4f input %ux%u\n",
            (unsigned long long)frame, dlss.scene_jitter[0], dlss.scene_jitter[1], depth->width, depth->height);
    dlss.scene = dlss.camera;
    dlss.frame = frame;
    dlss.scene_ready = true;
    dlss.camera_ready = false;
}

static void view_projection(const DlssCamera *cam, double *out) {
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) {
        double v = c == 3 ? cam->projection[r * 4 + 3] : 0;
        for (int k = 0; k < 3; k++) v += (double)cam->projection[r * 4 + k] * cam->view[k * 4 + c];
        out[r * 4 + c] = v;
    }
}

// Precombine last VP with inverse current VP once on the CPU. GPU pixels use the resulting clip-to-clip transform.
static bool reprojection(float out[16]) {
    double cur[16], prev[16], a[4][8];
    view_projection(&dlss.scene, cur);
    view_projection(&dlss.previous, prev);
    for (int r = 0; r < 4; r++) for (int c = 0; c < 8; c++) {
        a[r][c] = c < 4 ? cur[r * 4 + c] : c - 4 == r;
        if (!isfinite(a[r][c])) return false;
    }
    for (int i = 0; i < 4; i++) {
        int pivot = i;
        for (int r = i + 1; r < 4; r++) if (fabs(a[r][i]) > fabs(a[pivot][i])) pivot = r;
        if (fabs(a[pivot][i]) < 1e-10) return false;
        for (int c = 0; c < 8; c++) { double t = a[i][c]; a[i][c] = a[pivot][c]; a[pivot][c] = t; }
        double d = a[i][i];
        for (int c = 0; c < 8; c++) a[i][c] /= d;
        for (int r = 0; r < 4; r++) if (r != i) {
            double m = a[r][i];
            for (int c = 0; c < 8; c++) a[r][c] -= m * a[i][c];
        }
    }
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) {
        double x = 0;
        for (int k = 0; k < 4; k++) x += prev[r * 4 + k] * a[k][c + 4];
        if (!isfinite(x)) return false;
        out[c * 4 + r] = (float)x;
    }
    return true;
}

static bool motion_init(VkCommandBuffer cb) {
    if (dlss.motion.image) return true;
    if (dlss.motion_pipeline)
        return make_image(&dlss.motion, cb, dlss.w, dlss.h, VK_FORMAT_R16G16_SFLOAT,
                          VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    static const char shader[] =
        "#version 450\n"
        "layout(local_size_x=8, local_size_y=8) in;\n"
        "layout(binding=0) uniform sampler2D depth;\n"
        "layout(binding=1, rg16f) uniform writeonly image2D motion;\n"
        "layout(push_constant) uniform Params { mat4 previousClip; vec4 jitter; } pc;\n"
        "void main() {\n"
        "    ivec2 p = ivec2(gl_GlobalInvocationID.xy), extent = imageSize(motion);\n"
        "    if (any(greaterThanEqual(p, extent))) return;\n"
        "    vec2 size = vec2(extent), uv = (vec2(p) + 0.5) / size;\n"
        "    vec2 ndc = uv * vec2(2,-2) + vec2(-1,1);\n"
        "    ndc -= pc.jitter.xy * vec2(2,-2) / size;\n"
        "    float z = texelFetch(depth,p,0).r * 2.0 - 1.0;\n"
        "    vec4 old = pc.previousClip * vec4(ndc,z,1);\n"
        "    vec2 mv = old.w > 1e-6 ? (old.xy / old.w - ndc) * size * vec2(0.5,-0.5) : vec2(0);\n"
        "    imageStore(motion,p,vec4(clamp(mv,-size,size),0,0));\n"
        "}\n";
    VkShaderModule module = vk_compile_glsl(shader, STAGE_COMPUTE, "dlss_camera_motion");
    if (!module) return failed("camera motion shader", 0);
    VkDescriptorSetLayoutBinding binds[] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo si = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2, .pBindings = binds};
    VkPushConstantRange push = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 80};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &dlss.motion_set, .pushConstantRangeCount = 1, .pPushConstantRanges = &push};
    VkComputePipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = module, .pName = "main"}};
    VkResult r = vkCreateDescriptorSetLayout(g_vk.dev, &si, NULL, &dlss.motion_set);
    if (r == VK_SUCCESS) r = vkCreatePipelineLayout(g_vk.dev, &li, NULL, &dlss.motion_layout);
    pi.layout = dlss.motion_layout;
    if (r == VK_SUCCESS) r = vkCreateComputePipelines(g_vk.dev, VK_NULL_HANDLE, 1, &pi, NULL, &dlss.motion_pipeline);
    vkDestroyShaderModule(g_vk.dev, module, NULL);
    if (r != VK_SUCCESS) return failed("camera motion pipeline", (unsigned)r);
    return make_image(&dlss.motion, cb, dlss.w, dlss.h, VK_FORMAT_R16G16_SFLOAT,
                      VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
}

static void motion_dispatch(VkCommandBuffer cb, const float transform[16]) {
    VkDescriptorSet set = vk_alloc_set(dlss.motion_set);
    VkDescriptorImageInfo z = {vk_default_sampler(false), dlss.depth.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo m = {VK_NULL_HANDLE, dlss.motion.view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet wr[] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0, .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &z},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 1, .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &m}};
    vkUpdateDescriptorSets(g_vk.dev, 2, wr, 0, NULL);
    vk_full_barrier(cb);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, dlss.motion_pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, dlss.motion_layout, 0, 1, &set, 0, NULL);
    float constants[20] = {0};
    memcpy(constants, transform, 64);
    constants[16] = dlss.scene_jitter[0]; constants[17] = dlss.scene_jitter[1];
    vkCmdPushConstants(cb, dlss.motion_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof constants, constants);
    vkCmdDispatch(cb, (dlss.w + 7) / 8, (dlss.h + 7) / 8, 1);
    vk_full_barrier(cb);
}

static NgxResource resource(VkImage img, VkImageView view, VkFormat fmt, uint32_t w, uint32_t h,
                            VkImageAspectFlags aspect, bool writable) {
    return (NgxResource){.data.img = {view, img, {aspect, 0, 1, 0, 1}, fmt, w, h}, .writable = writable};
}

static VkImageView evaluate(VkCommandBuffer cb, const TexBackend *color, VkImageView view, VkFormat fmt,
                           uint32_t ow, uint32_t oh, uint64_t frame) {
    if (!motion_init(cb)) return VK_NULL_HANDLE;
    bool fsr3 = g_settings.temporal_backend == TEMPORAL_FSR3 || g_settings.temporal_backend == TEMPORAL_FSR4;
    bool xess = g_settings.temporal_backend == TEMPORAL_XESS;
    bool nvidia = !fsr3 && !xess;
    if (nvidia && !dlss.parameters) {
        unsigned r = ngx.allocate(&dlss.parameters);
        if (r != 1 || !dlss.parameters) { failed("parameter allocation", r); return VK_NULL_HANDLE; }
    }
    NgxParameters *p = dlss.parameters;
    NgxSetPtr set_ptr = p ? (NgxSetPtr)p->methods[0] : NULL;
    NgxSetInt set_int = p ? (NgxSetInt)p->methods[3] : NULL;
    NgxSetUint set_uint = p ? (NgxSetUint)p->methods[4] : NULL;
    NgxSetFloat set_float = p ? (NgxSetFloat)p->methods[6] : NULL;
    if (!dlss.output.image) {
        if (!make_image(&dlss.output, cb, ow, oh, VK_FORMAT_R16G16B16A16_SFLOAT,
                        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT)) return VK_NULL_HANDLE;
        dlss.ow = ow; dlss.oh = oh;
        dlss.feature_mode = temporal_mode(); dlss.feature_preset = g_settings.dlss_preset;
        dlss.feature_backend = g_settings.temporal_backend;
        dlss.feature_ml = g_settings.temporal_backend == TEMPORAL_FSR4 && vk_fsr4_available();
    }
    if (nvidia && !dlss.feature) {
        set_uint(p, "CreationNodeMask", 1);
        set_uint(p, "VisibilityNodeMask", 1);
        set_uint(p, "Width", dlss.w);
        set_uint(p, "Height", dlss.h);
        set_uint(p, "OutWidth", ow);
        set_uint(p, "OutHeight", oh);
        static const int quality[] = {0, 5, 2, 1, 0};
        static const char *const names[] = {"Off", "DLAA", "Quality", "Balanced", "Performance"};
        static const char *const hints[] = {NULL, "DLSS.Hint.Render.Preset.DLAA", "DLSS.Hint.Render.Preset.Quality",
            "DLSS.Hint.Render.Preset.Balanced", "DLSS.Hint.Render.Preset.Performance"};
        static const unsigned models[] = {0, 11, 12, 13};
        int mode = g_settings.dlss_mode, preset = g_settings.dlss_preset;
        set_int(p, "PerfQualityValue", quality[mode]);
        unsigned model = preset == DLSS_PRESET_DEFAULT ? (mode == DLSS_PERFORMANCE ? 13 : 11) : models[preset];
        set_uint(p, hints[mode], model);
        set_int(p, "DLSS.Feature.Create.Flags", 1 << 1);
        unsigned r = ngx.create(cb, 1, p, &dlss.feature);
        if (r != 1 || !dlss.feature) { failed("feature creation", r); return VK_NULL_HANDLE; }
        dlss.ow = ow;
        dlss.oh = oh;
        dlss.feature_mode = mode; dlss.feature_preset = preset;
        LOG("[dlss] %s created: %ux%u -> %ux%u, preset hint %u\n", names[mode], dlss.w, dlss.h, ow, oh, model);
    }

    float matrix[16] = {0};
    uint64_t now = plat_now_ns();
    double elapsed_ms = dlss.last_eval_ns ? (now - dlss.last_eval_ns) / 1e6 : 0;
    bool reset = !dlss.history || dlss.last_eval + 1 != frame || dlss.source_depth != dlss.previous_depth || elapsed_ms > 100;
    double direction = 0;
    for (int i = 8; i < 11; i++) direction += (double)dlss.scene.view[i] * dlss.previous.view[i];
    reset = reset || direction < 0.92 || !reprojection(matrix);
    static int history_trace = -1;
    static unsigned history_samples, history_resets, history_images, history_gaps;
    if (history_trace < 0) history_trace = getenv("ODYSSEY_DLSS_HISTORY_TRACE") != NULL;
    if (history_trace) {
        history_samples++;
        history_resets += reset;
        history_images += dlss.history && dlss.source_depth != dlss.previous_depth;
        history_gaps += dlss.history && dlss.last_eval + 1 != frame;
        if (history_samples == 240) {
            LOG("[dlss-history] %u evaluations: %u resets, %u depth-image changes, %u frame gaps\n",
                history_samples, history_resets, history_images, history_gaps);
            history_samples = history_resets = history_images = history_gaps = 0;
        }
    }
    if (reset) {
        memset(matrix, 0, sizeof matrix);
        for (int i = 0; i < 16; i += 5) matrix[i] = 1;
    }
    int stamp = timing_begin(cb, frame);
    motion_dispatch(cb, matrix);
    unsigned r;
    uint64_t t0 = stamp >= 0 ? plat_now_ns() : 0;
    if (fsr3 || xess) {
        const float *proj = dlss.scene.projection;
        float near_z = fabsf(proj[11] / (proj[10] - 1)), far_z = fabsf(proj[11] / (proj[10] + 1));
        if (!isfinite(near_z) || near_z < 0.001f) near_z = 10;
        if (!isfinite(far_z) || far_z <= near_z) far_z = 1000000;
        Fsr3Frame f = {.color = color->image, .color_format = fmt, .depth = dlss.depth.image,
            .motion = dlss.motion.image, .output = dlss.output.image, .w = dlss.w, .h = dlss.h, .ow = ow, .oh = oh,
            .jitter = {dlss.scene_jitter[0], dlss.scene_jitter[1]}, .dt_ms = elapsed_ms > 0 && elapsed_ms < 100 ? elapsed_ms : 16.667f,
            .near_z = near_z, .far_z = far_z, .fov_y = 2 * atanf(1 / fabsf(proj[5])),
            .sharpness = *settings_temporal_sharpness() * 0.01f, .reset = reset,
            .ml = g_settings.temporal_backend == TEMPORAL_FSR4};
        if (xess) {
            XessFrame x = {.frame = f, .color_view = view, .depth_view = dlss.depth.view,
                .motion_view = dlss.motion.view, .output_view = dlss.output.view, .mode = temporal_mode()};
            r = vk_xess_dispatch(cb, &x) ? 1 : 0;
        } else r = vk_fsr3_dispatch(cb, &f) ? 1 : 0;
    } else {
        NgxResource c = resource(color->image, view, fmt, dlss.w, dlss.h, VK_IMAGE_ASPECT_COLOR_BIT, false);
        NgxResource d = resource(dlss.depth.image, dlss.depth.view, VK_FORMAT_D32_SFLOAT, dlss.w, dlss.h, VK_IMAGE_ASPECT_DEPTH_BIT, false);
        NgxResource m = resource(dlss.motion.image, dlss.motion.view, VK_FORMAT_R16G16_SFLOAT, dlss.w, dlss.h, VK_IMAGE_ASPECT_COLOR_BIT, false);
        NgxResource o = resource(dlss.output.image, dlss.output.view, VK_FORMAT_R16G16B16A16_SFLOAT, ow, oh, VK_IMAGE_ASPECT_COLOR_BIT, true);
        set_ptr(p, "Color", &c);
        set_ptr(p, "Depth", &d);
        set_ptr(p, "MotionVectors", &m);
        set_ptr(p, "Output", &o);
        // Match the viewport displacement in right/down input pixels (as in Astral
        // Chain's viewport/NGX pair). Camera vectors exclude jitter: MVJittered unset.
        set_float(p, "Jitter.Offset.X", dlss.scene_jitter[0]);
        set_float(p, "Jitter.Offset.Y", dlss.scene_jitter[1]);
        set_float(p, "Sharpness", 0); // current models use the explicit post-sharpening passes
        set_float(p, "MV.Scale.X", 1);
        set_float(p, "MV.Scale.Y", 1);
        set_float(p, "DLSS.Pre.Exposure", 1);
        set_float(p, "DLSS.Exposure.Scale", 1);
        set_float(p, "FrameTimeDeltaInMsec", elapsed_ms > 0 && elapsed_ms < 100 ? (float)elapsed_ms : 16.667f);
        set_uint(p, "DLSS.Render.Subrect.Dimensions.Width", dlss.w);
        set_uint(p, "DLSS.Render.Subrect.Dimensions.Height", dlss.h);
        set_int(p, "Reset", reset);
        r = ngx.evaluate(cb, dlss.feature, p, NULL);
    }
    dlss.input_seq = dlss.output_seq = vk_pending_seq();
    if (stamp >= 0) {
        double ms = (plat_now_ns() - t0) / 1e6;
        if (ms > 9 || !(frame % 120)) LOG("[dlss-time] frame %llu NGX CPU %.2f ms\n", (unsigned long long)frame, ms);
        vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, timing.pool, 2 * stamp + 1);
    }
    vk_full_barrier(cb);
    if (r != 1) { if (nvidia) failed("evaluation", r); return VK_NULL_HANDLE; }
    dlss.previous = dlss.scene;
    dlss.previous_depth = dlss.source_depth;
    dlss.history = true;
    dlss.last_eval = frame;
    dlss.last_eval_ns = now;
    if (++dlss.eval_count == 1 || dlss.eval_count % 240 == 0)
        LOG("[%s] frame %llu: %llu evaluated frames\n", xess ? "xess" : fsr3 ?
            (g_settings.temporal_backend == TEMPORAL_FSR4 && vk_fsr4_available() ? "fsr4" : "fsr3") : "dlss",
            (unsigned long long)frame, (unsigned long long)dlss.eval_count);
    return dlss.output.view;
}

static bool can_evaluate(const TexBackend *color, uint32_t ow, uint32_t oh, uint64_t frame) {
    static uint64_t next_trace;
    if (getenv("ODYSSEY_DLSS_HISTORY_TRACE") && frame >= next_trace) {
        LOG("[dlss-state] frame %llu enabled %d camera %d scene %d scene-frame %llu colour %ux%u depth %ux%u output %ux%u\n",
            (unsigned long long)frame, vk_temporal_enabled(), dlss.camera_ready, dlss.scene_ready, (unsigned long long)dlss.frame,
            color->width, color->height, dlss.w, dlss.h, ow, oh);
        next_trace = frame + 240;
    }
    if (!vk_temporal_enabled() || !dlss.scene_ready || dlss.frame != frame ||
        (dlss.history && dlss.last_eval == frame) || color->width != dlss.w || color->height != dlss.h ||
        ow < dlss.w || oh < dlss.h || (temporal_mode() != DLSS_DLAA && (ow == dlss.w || oh == dlss.h))) return false;
    if (temporal_mode() == DLSS_DLAA && (ow != dlss.w || oh != dlss.h)) return false;
    return true;
}

static bool ensure_output(uint32_t ow, uint32_t oh) {
    return !dlss.ow || (dlss.ow == ow && dlss.oh == oh && dlss.feature_mode == temporal_mode() &&
                        dlss.feature_preset == g_settings.dlss_preset && dlss.feature_backend == g_settings.temporal_backend &&
                        dlss.feature_ml == (g_settings.temporal_backend == TEMPORAL_FSR4 && vk_fsr4_available())) || reset_output();
}

VkImageView vk_dlss_upscale(VkCommandBuffer cb, const TexBackend *color, VkImageView view,
                            uint32_t ow, uint32_t oh, uint64_t frame) {
    if (!can_evaluate(color, ow, oh, frame)) return VK_NULL_HANDLE;
    VkFormat fmt = color->format;
    if (fmt == VK_FORMAT_R8G8B8A8_SRGB) fmt = VK_FORMAT_R8G8B8A8_UNORM;
    if (fmt == VK_FORMAT_B8G8R8A8_SRGB) fmt = VK_FORMAT_B8G8R8A8_UNORM;
    if (fmt != VK_FORMAT_R8G8B8A8_UNORM && fmt != VK_FORMAT_B8G8R8A8_UNORM &&
        fmt != VK_FORMAT_R16G16B16A16_SFLOAT && fmt != VK_FORMAT_A2B10G10R10_UNORM_PACK32) return VK_NULL_HANDLE;
    extern void vk_frame_suspend(void), vk_frame_resume(void);
    vk_frame_suspend();
    VkImageView output = ensure_output(ow, oh) ? evaluate(cb, color, view, fmt, ow, oh, frame) : VK_NULL_HANDLE;
    vk_frame_resume();
    return output;
}
#endif
