// AMD's official Vulkan runtime, loaded optionally. SDK headers: third_party/fsr3_sdk.
#include "fsr3_sdk.h"
#ifdef _WIN32
#include <math.h>
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>
#include "../../../third_party/fsr3_sdk/api/include/vk/ffx_api_vk.h"
#include "../../../third_party/fsr3_sdk/upscalers/include/ffx_upscale.h"

static struct {
    HMODULE loader, runtime;
    PfnFfxCreateContext create;
    PfnFfxDestroyContext destroy;
    PfnFfxQuery query;
    PfnFfxDispatch dispatch;
    ffxContext context;
    uint64_t version[2];
    bool ml;
    uint32_t w, h, ow, oh;
} fsr;
static atomic_bool ready[2];

bool vk_fsr3_available(void) { return atomic_load_explicit(&ready[0], memory_order_relaxed); }
bool vk_fsr4_available(void) { return atomic_load_explicit(&ready[1], memory_order_relaxed); }

void vk_fsr3_init(void) {
    wchar_t path[2048];
    DWORD len = GetModuleFileNameW(NULL, path, sizeof path / sizeof *path);
    if (!len || len >= sizeof path / sizeof *path) return;
    wchar_t *name = wcsrchr(path, L'\\');
    if (!name || name - path > 1900) return;
    wcscpy(name + 1, L"amd_fidelityfx_upscaler_vk.dll");
    fsr.runtime = LoadLibraryExW(path, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!fsr.runtime) { LOG("[fsr3] optional AMD Vulkan runtime absent\n"); return; }
    wcscpy(name + 1, L"amd_fidelityfx_loader_vk.dll");
    fsr.loader = LoadLibraryExW(path, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!fsr.loader) { LOG("[fsr3] AMD Vulkan loader unavailable\n"); return; }
    fsr.create = (void *)GetProcAddress(fsr.loader, "ffxCreateContext");
    fsr.destroy = (void *)GetProcAddress(fsr.loader, "ffxDestroyContext");
    fsr.query = (void *)GetProcAddress(fsr.loader, "ffxQuery");
    fsr.dispatch = (void *)GetProcAddress(fsr.loader, "ffxDispatch");
    if (!fsr.create || !fsr.destroy || !fsr.query || !fsr.dispatch) return;
    VkDeviceContext device = {g_vk.dev, g_vk.phys, vkGetDeviceProcAddr};
    uint64_t count = 16, ids[16] = {0};
    const char *names[16] = {0};
    struct ffxQueryDescGetVersions versions = {.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS,
        .createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE, .device = &device,
        .outputCount = &count, .versionIds = ids, .versionNames = names};
    ffxReturnCode_t r = fsr.query(NULL, &versions.header);
    if (r) { LOG("[fsr3] provider query failed: %u\n", r); return; }
    if (count > 16) count = 16;
    for (uint64_t i = 0; i < count; i++) {
        LOG("[fsr-sdk] provider %llu: %s\n", (unsigned long long)ids[i], names[i] ? names[i] : "unknown");
        unsigned major = 0, minor = 0;
        if (names[i]) sscanf(names[i], "%u.%u", &major, &minor);
        if (major == 3 && minor == 1 && !fsr.version[0]) fsr.version[0] = ids[i];
        if (major == 4 && !fsr.version[1]) fsr.version[1] = ids[i];
    }
    atomic_store_explicit(&ready[0], fsr.version[0] != 0, memory_order_relaxed);
    atomic_store_explicit(&ready[1], fsr.version[1] != 0, memory_order_relaxed);
    if (!fsr.version[1]) LOG("[fsr4] no ML provider supports this GPU; FSR 3.1 fallback is %s\n", fsr.version[0] ? "available" : "unavailable");
}

void vk_fsr3_release(void) {
    if (fsr.context) fsr.destroy(&fsr.context, NULL);
    fsr.context = NULL;
    fsr.w = fsr.h = fsr.ow = fsr.oh = 0;
}

static uint32_t surface_format(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8G8B8A8_UNORM: return FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_UNORM: return FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM;
    case VK_FORMAT_R16G16B16A16_SFLOAT: return FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT;
    case VK_FORMAT_R16G16_SFLOAT: return FFX_API_SURFACE_FORMAT_R16G16_FLOAT;
    case VK_FORMAT_D32_SFLOAT: return FFX_API_SURFACE_FORMAT_R32_FLOAT;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return FFX_API_SURFACE_FORMAT_R10G10B10A2_UNORM;
    default: return FFX_API_SURFACE_FORMAT_UNKNOWN;
    }
}

static struct FfxApiResource resource(VkImage image, VkFormat format, uint32_t w, uint32_t h, bool output) {
    return (struct FfxApiResource){.resource = (void *)(uintptr_t)image,
        .description = {.type = FFX_API_RESOURCE_TYPE_TEXTURE2D, .format = surface_format(format),
            .width = w, .height = h, .depth = 1, .mipCount = 1,
            .usage = output ? FFX_API_RESOURCE_USAGE_UAV : format == VK_FORMAT_D32_SFLOAT ? FFX_API_RESOURCE_USAGE_DEPTHTARGET : 0},
        .state = output ? FFX_API_RESOURCE_STATE_UNORDERED_ACCESS : FFX_API_RESOURCE_STATE_COMPUTE_READ};
}

static void input_layouts(VkCommandBuffer cb, const Fsr3Frame *f, bool reading) {
    VkImageMemoryBarrier2 b[3] = {0};
    VkImage images[3] = {f->color, f->depth, f->motion};
    for (int i = 0; i < 3; i++) b[i] = (VkImageMemoryBarrier2){.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
        .oldLayout = reading ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .newLayout = reading ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = images[i], .subresourceRange = {i == 1 ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 3, .pImageMemoryBarriers = b};
    vkCmdPipelineBarrier2(cb, &d);
}

bool vk_fsr3_dispatch(VkCommandBuffer cb, const Fsr3Frame *f) {
    bool ml = f->ml && vk_fsr4_available();
    if (!ml && !vk_fsr3_available()) return false;
    if (!fsr.context) {
        struct ffxCreateBackendVKDesc backend = {.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK,
            .deviceContext = {g_vk.dev, g_vk.phys, vkGetDeviceProcAddr}};
        struct ffxOverrideVersion version = {.header = {FFX_API_DESC_TYPE_OVERRIDE_VERSION, &backend.header}, .versionId = fsr.version[ml]};
        struct ffxCreateContextDescUpscaleVersion api = {.header = {FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION, &version.header}, .version = FFX_UPSCALER_VERSION};
        struct ffxCreateContextDescUpscale desc = {.header = {FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE, ml ? &api.header : &version.header},
            .flags = FFX_UPSCALE_ENABLE_AUTO_EXPOSURE | FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE,
            .maxRenderSize = {f->w, f->h}, .maxUpscaleSize = {f->ow, f->oh}};
        ffxReturnCode_t r = fsr.create(&fsr.context, &desc.header, NULL);
        if (r) {
            LOG("[fsr%d] context creation failed: %u\n", ml ? 4 : 3, r);
            atomic_store(&ready[ml], false);
            vk_fsr3_release();
            if (ml && vk_fsr3_available()) return vk_fsr3_dispatch(cb, f);
            return false;
        }
        fsr.ml = ml;
        fsr.w = f->w; fsr.h = f->h; fsr.ow = f->ow; fsr.oh = f->oh;
        struct ffxQueryGetProviderVersion provider = {.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION};
        fsr.query(&fsr.context, &provider.header);
        LOG("[fsr%d] context created: %ux%u -> %ux%u, provider %s%s\n", ml ? 4 : 3, f->w, f->h, f->ow, f->oh,
            provider.versionName ? provider.versionName : "unknown", f->ml && !ml ? " (FSR 4 unavailable: analytical fallback)" : "");
    }
    if (fsr.w != f->w || fsr.h != f->h || fsr.ow != f->ow || fsr.oh != f->oh) return false;
    struct ffxDispatchDescUpscale desc = {.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE, .commandList = cb,
        .color = resource(f->color, f->color_format, f->w, f->h, false),
        .depth = resource(f->depth, VK_FORMAT_D32_SFLOAT, f->w, f->h, false),
        .motionVectors = resource(f->motion, VK_FORMAT_R16G16_SFLOAT, f->w, f->h, false),
        .output = resource(f->output, VK_FORMAT_R16G16B16A16_SFLOAT, f->ow, f->oh, true),
        .jitterOffset = {f->jitter[0], f->jitter[1]}, .motionVectorScale = {1, 1},
        .renderSize = {f->w, f->h}, .upscaleSize = {f->ow, f->oh},
        .enableSharpening = f->sharpness > 0, .sharpness = f->sharpness,
        .frameTimeDelta = f->dt_ms, .preExposure = 1, .reset = f->reset,
        .cameraNear = f->near_z, .cameraFar = f->far_z, .cameraFovAngleVertical = f->fov_y, .viewSpaceToMetersFactor = 0.01f,
        .flags = FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB};
    input_layouts(cb, f, true);
    ffxReturnCode_t r = fsr.dispatch(&fsr.context, &desc.header);
    input_layouts(cb, f, false);
    if (r) { LOG("[fsr%d] dispatch failed: %u\n", fsr.ml ? 4 : 3, r); atomic_store(&ready[fsr.ml], false); return false; }
    return true;
}
#endif
