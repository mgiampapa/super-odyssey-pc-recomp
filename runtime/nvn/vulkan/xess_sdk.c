// Optional Intel XeSS Vulkan integration. Official MIT API headers are vendored;
// Intel's unmodified signed runtime is staged separately with its licence.
#include "xess_sdk.h"
#ifdef _WIN32
#include <stdatomic.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>
#include "shaders.h"
#include "../../settings.h"
#include "../../../third_party/xess/include/xess_vk.h"

static struct {
    HMODULE library;
    bool tried, device_ok;
    const VkBaseOutStructure *requested;
    __typeof__(&xessGetVersion) version;
    __typeof__(&xessVKGetRequiredInstanceExtensions) instance_ext;
    __typeof__(&xessVKGetRequiredDeviceExtensions) device_ext;
    __typeof__(&xessVKGetRequiredDeviceFeatures) device_features;
    __typeof__(&xessVKCreateContext) create;
    __typeof__(&xessVKBuildPipelines) build;
    __typeof__(&xessVKInit) init;
    __typeof__(&xessVKExecute) execute;
    __typeof__(&xessDestroyContext) destroy;
    __typeof__(&xessSetLoggingCallback) logging;
    xess_context_handle_t context;
    VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT mutable;
    uint32_t w, h, ow, oh;
    struct { VkImage image; VkImageView view; VmaAllocation memory; } linear, resolved;
    VkDescriptorSetLayout convert_set;
    VkPipelineLayout convert_layout;
    VkPipeline convert_pipeline;
} xs;
static atomic_bool ready;

bool vk_xess_available(void) { return atomic_load_explicit(&ready, memory_order_relaxed); }

static bool load_library(void) {
    if (xs.tried) return xs.library != NULL;
    xs.tried = true;
    wchar_t path[2048];
    DWORD n = GetModuleFileNameW(NULL, path, sizeof path / sizeof *path);
    wchar_t *name = n && n < sizeof path / sizeof *path ? wcsrchr(path, L'\\') : NULL;
    if (!name || name - path > 1900) return false;
    wcscpy(name + 1, L"libxess.dll");
    xs.library = LoadLibraryExW(path, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!xs.library) { LOG("[xess] optional Intel runtime absent (or its VC++ runtime dependency is missing)\n"); return false; }
#define GET(field, function) xs.field = (void *)GetProcAddress(xs.library, #function)
    GET(version, xessGetVersion); GET(instance_ext, xessVKGetRequiredInstanceExtensions);
    GET(device_ext, xessVKGetRequiredDeviceExtensions); GET(device_features, xessVKGetRequiredDeviceFeatures);
    GET(create, xessVKCreateContext); GET(build, xessVKBuildPipelines); GET(init, xessVKInit);
    GET(execute, xessVKExecute); GET(destroy, xessDestroyContext); GET(logging, xessSetLoggingCallback);
#undef GET
    if (!xs.version || !xs.instance_ext || !xs.device_ext || !xs.device_features || !xs.create || !xs.init || !xs.execute || !xs.destroy) {
        LOG("[xess] Vulkan entry points unavailable\n"); FreeLibrary(xs.library); xs.library = NULL; return false;
    }
    xess_version_t version = {0}; xs.version(&version);
    LOG("[xess] runtime %u.%u.%u loaded\n", version.major, version.minor, version.patch);
    return true;
}

static bool listed(const char **list, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++) if (!strcmp(list[i], name)) return true;
    return false;
}

void vk_xess_instance_extensions(const char **ext, uint32_t *count, uint32_t cap) {
    if (!load_library()) return;
    uint32_t n = 0, api = 0, nh = 0;
    const char *const *need = NULL;
    if (xs.instance_ext(&n, &need, &api) < 0 || api > VK_API_VERSION_1_3) goto unsupported;
    vkEnumerateInstanceExtensionProperties(NULL, &nh, NULL);
    VkExtensionProperties *have = calloc(nh, sizeof *have);
    if (!have) goto unsupported;
    vkEnumerateInstanceExtensionProperties(NULL, &nh, have);
    bool ok = *count + n <= cap;
    for (uint32_t i = 0; i < n && ok; i++) {
        bool found = false;
        for (uint32_t j = 0; j < nh; j++) found |= !strcmp(need[i], have[j].extensionName);
        ok = found;
    }
    free(have);
    if (!ok) goto unsupported;
    for (uint32_t i = 0; i < n; i++) if (!listed(ext, *count, need[i])) ext[(*count)++] = need[i];
    return;
unsupported:
    LOG("[xess] required Vulkan instance API/extensions unavailable\n");
    FreeLibrary(xs.library); xs.library = NULL;
}

// Map legacy feature structures onto the Vulkan 1.1/1.2/1.3 aggregates already
// used by the application. Appending aliases alongside aggregates is invalid.
typedef struct { VkStructureType type; size_t bytes, offset; unsigned target, count; } Feature;
#define MAP(st, T, first, last, target_, U, field) {VK_STRUCTURE_TYPE_##st, sizeof(T), offsetof(U, field), target_, (offsetof(T, last) - offsetof(T, first))/sizeof(VkBool32)+1}
static const Feature feature_map[] = {
    MAP(PHYSICAL_DEVICE_FEATURES_2, VkPhysicalDeviceFeatures2, features.robustBufferAccess, features.inheritedQueries, 0, VkPhysicalDeviceFeatures2, features),
    MAP(PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, VkPhysicalDeviceVulkan11Features, storageBuffer16BitAccess, shaderDrawParameters, 1, VkPhysicalDeviceVulkan11Features, storageBuffer16BitAccess),
    MAP(PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, VkPhysicalDeviceVulkan12Features, samplerMirrorClampToEdge, subgroupBroadcastDynamicId, 2, VkPhysicalDeviceVulkan12Features, samplerMirrorClampToEdge),
    MAP(PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, VkPhysicalDeviceVulkan13Features, robustImageAccess, maintenance4, 3, VkPhysicalDeviceVulkan13Features, robustImageAccess),
    MAP(PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES, VkPhysicalDevice16BitStorageFeatures, storageBuffer16BitAccess, storageInputOutput16, 1, VkPhysicalDeviceVulkan11Features, storageBuffer16BitAccess),
    MAP(PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES, VkPhysicalDevice8BitStorageFeatures, storageBuffer8BitAccess, storagePushConstant8, 2, VkPhysicalDeviceVulkan12Features, storageBuffer8BitAccess),
    MAP(PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES, VkPhysicalDeviceShaderFloat16Int8Features, shaderFloat16, shaderInt8, 2, VkPhysicalDeviceVulkan12Features, shaderFloat16),
    MAP(PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES, VkPhysicalDeviceShaderIntegerDotProductFeatures, shaderIntegerDotProduct, shaderIntegerDotProduct, 3, VkPhysicalDeviceVulkan13Features, shaderIntegerDotProduct),
    MAP(PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES, VkPhysicalDeviceSubgroupSizeControlFeatures, subgroupSizeControl, computeFullSubgroups, 3, VkPhysicalDeviceVulkan13Features, subgroupSizeControl),
    MAP(PHYSICAL_DEVICE_SHADER_SUBGROUP_EXTENDED_TYPES_FEATURES, VkPhysicalDeviceShaderSubgroupExtendedTypesFeatures, shaderSubgroupExtendedTypes, shaderSubgroupExtendedTypes, 2, VkPhysicalDeviceVulkan12Features, shaderSubgroupExtendedTypes),
    MAP(PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES, VkPhysicalDeviceDescriptorIndexingFeatures, shaderInputAttachmentArrayDynamicIndexing, runtimeDescriptorArray, 2, VkPhysicalDeviceVulkan12Features, shaderInputAttachmentArrayDynamicIndexing),
    MAP(PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES, VkPhysicalDeviceScalarBlockLayoutFeatures, scalarBlockLayout, scalarBlockLayout, 2, VkPhysicalDeviceVulkan12Features, scalarBlockLayout),
    MAP(PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES, VkPhysicalDeviceTimelineSemaphoreFeatures, timelineSemaphore, timelineSemaphore, 2, VkPhysicalDeviceVulkan12Features, timelineSemaphore),
    MAP(PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES, VkPhysicalDeviceBufferDeviceAddressFeatures, bufferDeviceAddress, bufferDeviceAddressMultiDevice, 2, VkPhysicalDeviceVulkan12Features, bufferDeviceAddress),
    MAP(PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES, VkPhysicalDeviceVulkanMemoryModelFeatures, vulkanMemoryModel, vulkanMemoryModelAvailabilityVisibilityChains, 2, VkPhysicalDeviceVulkan12Features, vulkanMemoryModel),
    MAP(PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES, VkPhysicalDeviceHostQueryResetFeatures, hostQueryReset, hostQueryReset, 2, VkPhysicalDeviceVulkan12Features, hostQueryReset),
    MAP(PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES, VkPhysicalDeviceSynchronization2Features, synchronization2, synchronization2, 3, VkPhysicalDeviceVulkan13Features, synchronization2),
    MAP(PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES, VkPhysicalDeviceDynamicRenderingFeatures, dynamicRendering, dynamicRendering, 3, VkPhysicalDeviceVulkan13Features, dynamicRendering),
    MAP(PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES, VkPhysicalDeviceMaintenance4Features, maintenance4, maintenance4, 3, VkPhysicalDeviceVulkan13Features, maintenance4),
    MAP(PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES, VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures, shaderDemoteToHelperInvocation, shaderDemoteToHelperInvocation, 3, VkPhysicalDeviceVulkan13Features, shaderDemoteToHelperInvocation),
    MAP(PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT, VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT, mutableDescriptorType, mutableDescriptorType, 4, VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT, mutableDescriptorType),
};
#undef MAP

static const Feature *feature(VkStructureType type) {
    for (size_t i = 0; i < sizeof feature_map / sizeof *feature_map; i++) if (feature_map[i].type == type) return &feature_map[i];
    return NULL;
}

void vk_xess_device_extensions(const VkExtensionProperties *have, uint32_t nh, const char **ext, uint32_t *count, uint32_t cap) {
    if (!xs.library) return;
    uint32_t n = 0; const char *const *need = NULL;
    if (xs.device_ext(g_vk.inst, g_vk.phys, &n, &need) < 0 || *count + n > cap) return;
    for (uint32_t i = 0; i < n; i++) {
        bool found = false;
        for (uint32_t j = 0; j < nh; j++) found |= !strcmp(have[j].extensionName, need[i]);
        if (!found) { LOG("[xess] device extension unavailable: %s\n", need[i]); return; }
    }
    void *request = NULL;
    if (xs.device_features(g_vk.inst, g_vk.phys, &request) < 0) return;
    unsigned structures = 0;
    for (const VkBaseOutStructure *r = request; r; r = r->pNext) {
        const Feature *m = feature(r->sType);
        if (!m || ++structures > 32 || m->bytes > 1024) { LOG("[xess] unsupported feature structure %u\n", r->sType); return; }
        _Alignas(16) uint8_t data[1024] = {0};
        ((VkBaseOutStructure *)data)->sType = r->sType;
        VkPhysicalDeviceFeatures2 supported = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        if (r->sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) supported.pNext = data;
        vkGetPhysicalDeviceFeatures2(g_vk.phys, &supported);
        const VkBool32 *want = (const void *)((const char *)r + sizeof(VkBaseOutStructure));
        const VkBool32 *got = r->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 ? (const void *)&supported.features : (const void *)(data + sizeof(VkBaseOutStructure));
        for (unsigned j = 0; j < m->count; j++) if (want[j] && !got[j]) { LOG("[xess] required feature %u of structure %u unavailable\n", j, r->sType); return; }
        LOG("[xess] feature structure %u supported\n", r->sType);
    }
    for (uint32_t i = 0; i < n; i++) if (!listed(ext, *count, need[i])) ext[(*count)++] = need[i];
    xs.requested = request;
    xs.device_ok = true;
}

void vk_xess_enable_features(VkPhysicalDeviceFeatures2 *f, VkPhysicalDeviceVulkan11Features *v11,
                            VkPhysicalDeviceVulkan12Features *v12, VkPhysicalDeviceVulkan13Features *v13) {
    if (!xs.device_ok) return;
    xs.mutable.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT;
    void *targets[] = {f, v11, v12, v13, &xs.mutable};
    for (const VkBaseOutStructure *r = xs.requested; r; r = r->pNext) {
        const Feature *m = feature(r->sType);
        const VkBool32 *want = (const void *)((const char *)r + sizeof(VkBaseOutStructure));
        VkBool32 *out = (void *)((char *)targets[m->target] + m->offset);
        for (unsigned j = 0; j < m->count; j++) out[j] |= want[j];
    }
    if (xs.mutable.mutableDescriptorType) { xs.mutable.pNext = f->pNext; f->pNext = &xs.mutable; }
}

void vk_xess_device_created(void) { atomic_store(&ready, xs.device_ok); }

typedef struct { VkImage image; VkImageView view; VmaAllocation memory; } XessImage;
static void destroy_image(void *ptr) {
    XessImage *image = ptr;
    if (image->view) vkDestroyImageView(g_vk.dev, image->view, NULL);
    if (image->image) vmaDestroyImage(g_vk.vma, image->image, image->memory);
    memset(image, 0, sizeof *image);
}
void vk_xess_release(void) {
    if (xs.context) xs.destroy(xs.context);
    xs.context = NULL;
    destroy_image(&xs.linear); destroy_image(&xs.resolved);
    xs.w = xs.h = xs.ow = xs.oh = 0;
}

static bool make_image(void *ptr, VkCommandBuffer cb, uint32_t w, uint32_t h) {
    XessImage *image = ptr;
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R16G16B16A16_SFLOAT, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(g_vk.vma, &ci, &ai, &image->image, &image->memory, NULL) != VK_SUCCESS) return false;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = image->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = ci.format, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (vkCreateImageView(g_vk.dev, &vi, NULL, &image->view) != VK_SUCCESS) return false;
    vk_image_init_layout(cb, image->image, VK_IMAGE_ASPECT_COLOR_BIT);
    return true;
}

static bool conversion_pipeline(void) {
    if (xs.convert_pipeline) return true;
    const char *glsl = "#version 450\nlayout(local_size_x=8,local_size_y=8) in;\n"
        "layout(binding=0) uniform sampler2D src; layout(binding=1,rgba16f) uniform writeonly image2D dst;\n"
        "layout(push_constant) uniform P { ivec4 p; } pc;\n"
        "void main(){ivec2 q=ivec2(gl_GlobalInvocationID.xy);if(any(greaterThanEqual(q,pc.p.xy)))return;"
        "vec3 c=max(texelFetch(src,q,0).rgb,vec3(0));"
        "if(pc.p.z==0)c=mix(c/12.92,pow((c+0.055)/1.055,vec3(2.4)),greaterThan(c,vec3(0.04045)));"
        "else c=mix(12.92*c,1.055*pow(c,vec3(1.0/2.4))-0.055,greaterThan(c,vec3(0.0031308)));"
        "imageStore(dst,q,vec4(c,1));}\n";
    VkShaderModule module = vk_compile_glsl(glsl, STAGE_COMPUTE, "xess_srgb_conversion");
    if (!module) return false;
    VkDescriptorSetLayoutBinding b[] = {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo si = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = b};
    VkResult r = vkCreateDescriptorSetLayout(g_vk.dev, &si, NULL, &xs.convert_set);
    VkPushConstantRange push = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &xs.convert_set, .pushConstantRangeCount = 1, .pPushConstantRanges = &push};
    if (r == VK_SUCCESS) r = vkCreatePipelineLayout(g_vk.dev, &li, NULL, &xs.convert_layout);
    VkComputePipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = xs.convert_layout,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main"}};
    if (r == VK_SUCCESS) r = vkCreateComputePipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &xs.convert_pipeline);
    vkDestroyShaderModule(g_vk.dev, module, NULL);
    return r == VK_SUCCESS;
}

static void convert(VkCommandBuffer cb, VkImageView source, VkImageView output, uint32_t w, uint32_t h, bool encode) {
    VkDescriptorSet set = vk_alloc_set(xs.convert_set);
    VkDescriptorImageInfo images[] = {{vk_default_sampler(false), source, VK_IMAGE_LAYOUT_GENERAL}, {VK_NULL_HANDLE, output, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet writes[2];
    for (unsigned i = 0; i < 2; i++) writes[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
        .dstBinding = i, .descriptorCount = 1, .descriptorType = i ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = images + i};
    vkUpdateDescriptorSets(g_vk.dev, 2, writes, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, xs.convert_pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, xs.convert_layout, 0, 1, &set, 0, NULL);
    int p[4] = {w, h, encode, 0};
    vkCmdPushConstants(cb, xs.convert_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof p, p);
    vkCmdDispatch(cb, (w+7)/8, (h+7)/8, 1);
    vk_full_barrier(cb);
}

static xess_vk_image_view_info image_info(VkImage image, VkImageView view, VkFormat format, uint32_t w, uint32_t h) {
    return (xess_vk_image_view_info){view, image, {format == VK_FORMAT_D32_SFLOAT ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}, format, w, h};
}
static void input_layouts(VkCommandBuffer cb, const XessFrame *f, bool reading) {
    VkImage images[] = {xs.linear.image, f->frame.depth, f->frame.motion};
    VkImageMemoryBarrier2 barriers[3] = {0};
    for (unsigned i = 0; i < 3; i++) barriers[i] = (VkImageMemoryBarrier2){.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
        .oldLayout = reading ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .newLayout = reading ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = images[i],
        .subresourceRange = {i == 1 ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 3, .pImageMemoryBarriers = barriers};
    vkCmdPipelineBarrier2(cb, &di);
}

static void sdk_log(const char *message, xess_logging_level_t level) { if (level >= XESS_LOGGING_LEVEL_WARNING) LOG("[xess] %s\n", message); }
static bool fail(const char *step, xess_result_t r) { LOG("[xess] %s failed: %d; ordinary rendering fallback\n", step, r); atomic_store(&ready, false); return false; }

bool vk_xess_dispatch(VkCommandBuffer cb, const XessFrame *f) {
    if (!vk_xess_available()) return false;
    const Fsr3Frame *v = &f->frame;
    if (!xs.context) {
        if (!conversion_pipeline()) return fail("colour pipeline", XESS_RESULT_ERROR_DEVICE);
        xess_result_t r = xs.create(g_vk.inst, g_vk.phys, g_vk.dev, &xs.context);
        if (r < 0) return fail("context creation", r);
        if (xs.logging) xs.logging(xs.context, XESS_LOGGING_LEVEL_WARNING, sdk_log);
        uint32_t flags = XESS_INIT_FLAG_LDR_INPUT_COLOR;
        if (xs.build && (r = xs.build(xs.context, g_vk.pcache, true, flags)) < 0) return fail("pipeline build", r);
        static const xess_quality_settings_t qualities[] = {XESS_QUALITY_SETTING_AA, XESS_QUALITY_SETTING_AA,
            XESS_QUALITY_SETTING_QUALITY, XESS_QUALITY_SETTING_BALANCED, XESS_QUALITY_SETTING_PERFORMANCE};
        xess_vk_init_params_t ip = {.outputResolution = {v->ow, v->oh}, .qualitySetting = qualities[f->mode], .initFlags = flags,
            .pipelineCache = g_vk.pcache};
        extern void vk_queue_lock(void), vk_queue_unlock(void);
        vk_queue_lock();
        r = xs.init(xs.context, &ip);
        vk_queue_unlock();
        if (r < 0) return fail("initialisation", r);
        if (!make_image(&xs.linear, cb, v->w, v->h) || !make_image(&xs.resolved, cb, v->ow, v->oh)) return fail("image allocation", XESS_RESULT_ERROR_DEVICE_OUT_OF_MEMORY);
        xs.w = v->w; xs.h = v->h; xs.ow = v->ow; xs.oh = v->oh;
        LOG("[xess] context created: %ux%u -> %ux%u, quality %u\n", v->w, v->h, v->ow, v->oh, ip.qualitySetting);
    }
    if (xs.w != v->w || xs.h != v->h || xs.ow != v->ow || xs.oh != v->oh) return false;
    convert(cb, f->color_view, xs.linear.view, v->w, v->h, false);
    xess_vk_execute_params_t p = {
        .colorTexture = image_info(xs.linear.image, xs.linear.view, VK_FORMAT_R16G16B16A16_SFLOAT, v->w, v->h),
        .velocityTexture = image_info(v->motion, f->motion_view, VK_FORMAT_R16G16_SFLOAT, v->w, v->h),
        .depthTexture = image_info(v->depth, f->depth_view, VK_FORMAT_D32_SFLOAT, v->w, v->h),
        .outputTexture = image_info(xs.resolved.image, xs.resolved.view, VK_FORMAT_R16G16B16A16_SFLOAT, v->ow, v->oh),
        .jitterOffsetX = v->jitter[0], .jitterOffsetY = v->jitter[1], .exposureScale = 1,
        .resetHistory = v->reset, .inputWidth = v->w, .inputHeight = v->h};
    input_layouts(cb, f, true);
    xess_result_t r = xs.execute(xs.context, cb, &p);
    input_layouts(cb, f, false);
    if (r < 0) return fail("execution", r);
    convert(cb, xs.resolved.view, f->output_view, v->ow, v->oh, true);
    return true;
}
#endif
