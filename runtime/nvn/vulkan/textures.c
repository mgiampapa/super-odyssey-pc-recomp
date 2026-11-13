// Vulkan textures: NVN formats, images, change-detected uploads from guest memory, views and samplers
// (counterpart of the texture half of metal.m and the view helpers of renderer.m).
#include <SDL3/SDL_timer.h>
#include <float.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "vk.h"
#include "../astc.h"
#include "../output_scale.h"
#include "../renderer.h"
#include "../linear_texture.h"
#include "../kbprompts.h"
#include "../../settings.h"

// ================================================================== formats
typedef struct { VkFormat vf; uint8_t expand_rgb, alpha_one; } VkFmt;

// DEPTH24_STENCIL8 as itself where the GPU has it for everything the game does with depth (NVIDIA, Intel): half the
// bytes of D32_SFLOAT_S8_UINT (4 instead of 8 per pixel) for every depth test, clear and copy - the game copies its
// full-size depth twice a frame. AMD has no D24S8 attachments: D32S8 there (and on Metal). ODYSSEY_VK_D24=0: D32S8.
static VkFormat depth24_stencil8(void) {
    static VkFormat f;
    if (!f) {
        const char *e = getenv("ODYSSEY_VK_D24");
        VkFormatProperties p;
        vkGetPhysicalDeviceFormatProperties(g_vk.phys, VK_FORMAT_D24_UNORM_S8_UINT, &p);
        VkFormatFeatureFlags want = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                    VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
                                    VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
        bool ok = (p.optimalTilingFeatures & want) == want && !(e && !atoi(e));
        f = ok ? VK_FORMAT_D24_UNORM_S8_UINT : VK_FORMAT_D32_SFLOAT_S8_UINT;
        LOG("[vk] depth24 stencil8: %s\n", ok ? "D24_UNORM_S8_UINT" : "D32_SFLOAT_S8_UINT");
    }
    return f;
}

static VkFmt fmt_of(uint32_t f) {
    switch (f) {
    case 0x01: return (VkFmt){VK_FORMAT_R8_UNORM};
    case 0x02: return (VkFmt){VK_FORMAT_R8_SNORM};
    case 0x03: return (VkFmt){VK_FORMAT_R8_UINT};
    case 0x04: return (VkFmt){VK_FORMAT_R8_SINT};
    case 0x05: return (VkFmt){VK_FORMAT_R16_SFLOAT};
    case 0x06: return (VkFmt){VK_FORMAT_R16_UNORM};
    case 0x07: return (VkFmt){VK_FORMAT_R16_SNORM};
    case 0x08: return (VkFmt){VK_FORMAT_R16_UINT};
    case 0x09: return (VkFmt){VK_FORMAT_R16_SINT};
    case 0x0A: return (VkFmt){VK_FORMAT_R32_SFLOAT};
    case 0x0B: return (VkFmt){VK_FORMAT_R32_UINT};
    case 0x0C: return (VkFmt){VK_FORMAT_R32_SINT};
    case 0x0D: return (VkFmt){VK_FORMAT_R8G8_UNORM};
    case 0x0E: return (VkFmt){VK_FORMAT_R8G8_SNORM};
    case 0x0F: return (VkFmt){VK_FORMAT_R8G8_UINT};
    case 0x10: return (VkFmt){VK_FORMAT_R8G8_SINT};
    case 0x11: return (VkFmt){VK_FORMAT_R16G16_SFLOAT};
    case 0x12: return (VkFmt){VK_FORMAT_R16G16_UNORM};
    case 0x13: return (VkFmt){VK_FORMAT_R16G16_SNORM};
    case 0x14: return (VkFmt){VK_FORMAT_R16G16_UINT};
    case 0x15: return (VkFmt){VK_FORMAT_R16G16_SINT};
    case 0x16: return (VkFmt){VK_FORMAT_R32G32_SFLOAT};
    case 0x17: return (VkFmt){VK_FORMAT_R32G32_UINT};
    case 0x18: return (VkFmt){VK_FORMAT_R32G32_SINT};
    case 0x19: return (VkFmt){VK_FORMAT_R8G8B8A8_UNORM, 1, 1};  // RGB8: expanded on upload
    case 0x25: return (VkFmt){VK_FORMAT_R8G8B8A8_UNORM};
    case 0x26: return (VkFmt){VK_FORMAT_R8G8B8A8_SNORM};
    case 0x27: return (VkFmt){VK_FORMAT_R8G8B8A8_UINT};
    case 0x28: return (VkFmt){VK_FORMAT_R8G8B8A8_SINT};
    case 0x29: return (VkFmt){VK_FORMAT_R16G16B16A16_SFLOAT};
    case 0x2A: return (VkFmt){VK_FORMAT_R16G16B16A16_UNORM};
    case 0x2B: return (VkFmt){VK_FORMAT_R16G16B16A16_SNORM};
    case 0x2C: return (VkFmt){VK_FORMAT_R16G16B16A16_UINT};
    case 0x2D: return (VkFmt){VK_FORMAT_R16G16B16A16_SINT};
    case 0x2E: return (VkFmt){VK_FORMAT_R32G32B32A32_SFLOAT};
    case 0x2F: return (VkFmt){VK_FORMAT_R32G32B32A32_UINT};
    case 0x30: return (VkFmt){VK_FORMAT_R32G32B32A32_SINT};
    case 0x31: return (VkFmt){VK_FORMAT_S8_UINT};
    case 0x32: return (VkFmt){VK_FORMAT_D16_UNORM};
    case 0x33: return (VkFmt){VK_FORMAT_D32_SFLOAT};  // DEPTH24 (as on Metal: no 24-bit depth there)
    case 0x34: return (VkFmt){VK_FORMAT_D32_SFLOAT};
    case 0x35: return (VkFmt){depth24_stencil8()};
    case 0x36: return (VkFmt){VK_FORMAT_D32_SFLOAT_S8_UINT};
    case 0x37: return (VkFmt){VK_FORMAT_R8G8B8A8_SRGB, 0, 1};
    case 0x38: return (VkFmt){VK_FORMAT_R8G8B8A8_SRGB};
    case 0x39: return (VkFmt){VK_FORMAT_R4G4B4A4_UNORM_PACK16};   // Metal ABGR4Unorm
    case 0x3A: return (VkFmt){VK_FORMAT_R5G6B5_UNORM_PACK16};     // Metal B5G6R5Unorm
    case 0x3B: return (VkFmt){VK_FORMAT_R5G5B5A1_UNORM_PACK16};   // Metal A1BGR5Unorm
    case 0x3C: return (VkFmt){VK_FORMAT_R5G6B5_UNORM_PACK16};
    case 0x3D: return (VkFmt){VK_FORMAT_A2B10G10R10_UNORM_PACK32};
    case 0x3E: return (VkFmt){VK_FORMAT_A2B10G10R10_UINT_PACK32};
    case 0x3F: return (VkFmt){VK_FORMAT_B10G11R11_UFLOAT_PACK32};
    case 0x40: return (VkFmt){VK_FORMAT_E5B9G9R9_UFLOAT_PACK32};
    case 0x41: case 0x42: return (VkFmt){VK_FORMAT_BC1_RGBA_UNORM_BLOCK};
    case 0x43: return (VkFmt){VK_FORMAT_BC2_UNORM_BLOCK};
    case 0x44: return (VkFmt){VK_FORMAT_BC3_UNORM_BLOCK};
    case 0x45: case 0x46: return (VkFmt){VK_FORMAT_BC1_RGBA_SRGB_BLOCK};
    case 0x47: return (VkFmt){VK_FORMAT_BC2_SRGB_BLOCK};
    case 0x48: return (VkFmt){VK_FORMAT_BC3_SRGB_BLOCK};
    case 0x49: return (VkFmt){VK_FORMAT_BC4_UNORM_BLOCK};
    case 0x4A: return (VkFmt){VK_FORMAT_BC4_SNORM_BLOCK};
    case 0x4B: return (VkFmt){VK_FORMAT_BC5_UNORM_BLOCK};
    // 0x4C = signed RGTC2; ODYSSEY_BC5_UNORM=1 decodes it unsigned (see metal.m / notes/PLAN.md)
    case 0x4C: return (VkFmt){getenv("ODYSSEY_BC5_UNORM") ? VK_FORMAT_BC5_UNORM_BLOCK : VK_FORMAT_BC5_SNORM_BLOCK};
    case 0x4D: return (VkFmt){VK_FORMAT_BC7_UNORM_BLOCK};
    case 0x4E: return (VkFmt){VK_FORMAT_BC7_SRGB_BLOCK};
    case 0x4F: return (VkFmt){VK_FORMAT_BC6H_SFLOAT_BLOCK};
    case 0x50: return (VkFmt){VK_FORMAT_BC6H_UFLOAT_BLOCK};
    case 0x6D: return (VkFmt){VK_FORMAT_R8G8B8A8_UNORM, 0, 1};
    case 0x71: return (VkFmt){VK_FORMAT_R16G16B16A16_SFLOAT, 0, 1};
    case 0x76: return (VkFmt){VK_FORMAT_R32G32B32A32_SFLOAT, 0, 1};
    case 0x79: return (VkFmt){VK_FORMAT_ASTC_4x4_UNORM_BLOCK};
    case 0x7A: return (VkFmt){VK_FORMAT_ASTC_5x4_UNORM_BLOCK};
    case 0x7B: return (VkFmt){VK_FORMAT_ASTC_5x5_UNORM_BLOCK};
    case 0x7C: return (VkFmt){VK_FORMAT_ASTC_6x5_UNORM_BLOCK};
    case 0x7D: return (VkFmt){VK_FORMAT_ASTC_6x6_UNORM_BLOCK};
    case 0x7E: return (VkFmt){VK_FORMAT_ASTC_8x5_UNORM_BLOCK};
    case 0x7F: return (VkFmt){VK_FORMAT_ASTC_8x6_UNORM_BLOCK};
    case 0x80: return (VkFmt){VK_FORMAT_ASTC_8x8_UNORM_BLOCK};
    case 0x81: return (VkFmt){VK_FORMAT_ASTC_10x5_UNORM_BLOCK};
    case 0x82: return (VkFmt){VK_FORMAT_ASTC_10x6_UNORM_BLOCK};
    case 0x83: return (VkFmt){VK_FORMAT_ASTC_10x8_UNORM_BLOCK};
    case 0x84: return (VkFmt){VK_FORMAT_ASTC_10x10_UNORM_BLOCK};
    case 0x85: return (VkFmt){VK_FORMAT_ASTC_12x10_UNORM_BLOCK};
    case 0x86: return (VkFmt){VK_FORMAT_ASTC_12x12_UNORM_BLOCK};
    case 0x87: return (VkFmt){VK_FORMAT_ASTC_4x4_SRGB_BLOCK};
    case 0x88: return (VkFmt){VK_FORMAT_ASTC_5x4_SRGB_BLOCK};
    case 0x89: return (VkFmt){VK_FORMAT_ASTC_5x5_SRGB_BLOCK};
    case 0x8A: return (VkFmt){VK_FORMAT_ASTC_6x5_SRGB_BLOCK};
    case 0x8B: return (VkFmt){VK_FORMAT_ASTC_6x6_SRGB_BLOCK};
    case 0x8C: return (VkFmt){VK_FORMAT_ASTC_8x5_SRGB_BLOCK};
    case 0x8D: return (VkFmt){VK_FORMAT_ASTC_8x6_SRGB_BLOCK};
    case 0x8E: return (VkFmt){VK_FORMAT_ASTC_8x8_SRGB_BLOCK};
    case 0x8F: return (VkFmt){VK_FORMAT_ASTC_10x5_SRGB_BLOCK};
    case 0x90: return (VkFmt){VK_FORMAT_ASTC_10x6_SRGB_BLOCK};
    case 0x91: return (VkFmt){VK_FORMAT_ASTC_10x8_SRGB_BLOCK};
    case 0x92: return (VkFmt){VK_FORMAT_ASTC_10x10_SRGB_BLOCK};
    case 0x93: return (VkFmt){VK_FORMAT_ASTC_12x10_SRGB_BLOCK};
    case 0x94: return (VkFmt){VK_FORMAT_ASTC_12x12_SRGB_BLOCK};
    case 0x95: case 0x96: case 0x97: return (VkFmt){VK_FORMAT_R5G5B5A1_UNORM_PACK16};
    case 0x98: return (VkFmt){VK_FORMAT_B8G8R8A8_UNORM, 0, 1};
    case 0x99: return (VkFmt){VK_FORMAT_B8G8R8A8_UNORM};
    case 0x9A: return (VkFmt){VK_FORMAT_B8G8R8A8_SRGB, 0, 1};
    case 0x9B: return (VkFmt){VK_FORMAT_B8G8R8A8_SRGB};
    default: return (VkFmt){VK_FORMAT_R8G8B8A8_UNORM};
    }
}

bool renderer_format_is_depth(uint32_t f) { return f >= 0x31 && f <= 0x36; }
bool renderer_format_has_stencil(uint32_t f) { return f == 0x31 || f == 0x35 || f == 0x36; }
VkFormat vk_format(uint32_t f) { return fmt_of(f).vf; }
bool vk_format_alpha_one(uint32_t f) { return fmt_of(f).alpha_one; }
bool renderer_format_alpha_one(uint32_t f) { return fmt_of(f).alpha_one; }

static VkImageAspectFlags aspect_of(uint32_t f) {
    if (f == 0x31) return VK_IMAGE_ASPECT_STENCIL_BIT;
    if (f == 0x35 || f == 0x36) return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    if (renderer_format_is_depth(f)) return VK_IMAGE_ASPECT_DEPTH_BIT;
    return VK_IMAGE_ASPECT_COLOR_BIT;
}

static bool format_supports(VkFormat f, VkFormatFeatureFlags want) {
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, f, &p);
    return (p.optimalTilingFeatures & want) == want;
}

// ================================================================== output resolution / per-frame clears
// Same rules as metal.m (see the comments there).
static float g_rs_x = 1, g_rs_y = 1;
void renderer_get_scale(float *sx, float *sy) { *sx = g_rs_x; *sy = g_rs_y; }
void renderer_set_scale(float sx, float sy) { g_rs_x = sx; g_rs_y = sy; }
uint64_t renderer_present_count(void);

static bool tex_scalable(const NvnTexture *t) {
    if (t->flags & 0x4) return false;  // NVN_TEXTURE_FLAGS_IMAGE: compute uses the guest's integer texel coordinates.
    // Odyssey's quarter-resolution R11G11B10F bloom chain uses taps spaced for the guest texel grid.
    // Enlarging these targets separates the taps around small highlights and produces visible grids.
    if (t->format == 0x3f && t->width <= 400 && t->height <= 228) return false;
    return output_scale_texture(t);
}

bool renderer_texture_first_write(NvnTexture *t) {
    TexBackend *b = t ? (TexBackend *)t->backend : NULL;
    uint64_t total_frames = renderer_present_count();
    if (!b || !b->presented || (b->sx == b->sy) || b->touched_frame == total_frames + 1) return false;
    b->touched_frame = total_frames + 1;
    return true;
}

static uint32_t g_level_gen = 1;  // (a new generation drops what renderer_texture_frame_fresh tracks)

// (Link's Awakening's pits and generated rooms: its scene targets were cleared at their first use in each frame. Odyssey
// keeps content across frames in some of its screen-sized targets, which are scaled now: off unless ODYSSEY_FRAME_CLEAR=1.)
bool renderer_texture_frame_fresh(NvnTexture *t) {
    static int on = -1;
    if (on < 0) on = getenv("ODYSSEY_FRAME_CLEAR") != NULL;
    TexBackend *b = t ? (TexBackend *)t->backend : NULL;
    uint64_t total_frames = renderer_present_count();
    if (!on || !b || !b->scalable || b->frame_cleared == total_frames + 1) return false;
    b->frame_cleared = total_frames + 1;
    return true;
}

bool renderer_texture_level_fresh(NvnTexture *t) {
    TexBackend *b = t ? (TexBackend *)t->backend : NULL;
    static int keep = -1;
    if (keep < 0) keep = getenv("ODYSSEY_KEEP_TARGETS") != NULL;
    if (!b || keep || b->level_gen == g_level_gen) return false;
    if (!b->scalable) { b->level_gen = g_level_gen; return false; }
    bool fresh = b->level_gen != 0;
    b->level_gen = g_level_gen;
    return fresh;
}

void renderer_texture_mark_presented(NvnTexture *t) {
    TexBackend *b = t ? (TexBackend *)t->backend : NULL;
    if (b) b->presented = true;
}

bool renderer_texture_presented(NvnTexture *t) {
    TexBackend *b = t ? (TexBackend *)t->backend : NULL;
    return b && b->presented;
}

void renderer_texture_scale(NvnTexture *t, float *sx, float *sy) {
    TexBackend *b = t ? (TexBackend *)t->backend : NULL;
    if (b && b->scalable) { *sx = b->sx; *sy = b->sy; }
    else { *sx = 1; *sy = 1; }
}

// ================================================================== images
void vk_image_init_layout(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect) {
    vk_checkpoint(cb, "init layout image %p aspect %#x", (void *)img, (unsigned)aspect);
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                               .srcStageMask = VK_PIPELINE_STAGE_2_NONE, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL, .image = img,
                               .subresourceRange = {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS},
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &di);
}

// Images destroyed by the guest are kept until the submissions that may use them have completed.
typedef struct { VkImage img; VmaAllocation mem; VkImageView view; uint64_t seq; } Dead;
static Dead *dead;
static int ndead, cap_dead;
static void views_forget(TexBackend *b);

void vk_collect_dead(void) {
    uint64_t done = vk_completed_seq();
    int k = 0;
    static int log_dead = -1;
    if (log_dead < 0) log_dead = getenv("ODYSSEY_LOG_DEAD") != NULL;
    for (int i = 0; i < ndead; i++) {
        if (dead[i].seq > done) { dead[k++] = dead[i]; continue; }
        if (log_dead && dead[i].view)
            LOG("[dead] view %p destroyed (deferred at %llu, done %llu, pending %llu)\n", (void *)dead[i].view,
                (unsigned long long)dead[i].seq, (unsigned long long)done, (unsigned long long)vk_pending_seq());
        if (dead[i].view) vkDestroyImageView(g_vk.dev, dead[i].view, NULL);
        else vmaDestroyImage(g_vk.vma, dead[i].img, dead[i].mem);
    }
    ndead = k;
}

static void create_image(NvnTexture *t, TexBackend *b) {
    VkFmt mf = fmt_of(t->format);
    b->sx = b->scalable ? g_rs_x : 1;
    b->sy = b->scalable ? g_rs_y : 1;
    uint32_t tw = t->width ? t->width : 1, th = t->height ? t->height : 1;
    if (b->scalable) {
        tw = (uint32_t)lroundf(tw * b->sx); th = (uint32_t)lroundf(th * b->sy);
        if (tw < 1) tw = 1;
        if (th < 1) th = 1;
    }
    VkImageType type = VK_IMAGE_TYPE_2D;
    uint32_t layers = 1, depth = 1;
    switch (t->target) {
    case 0: type = VK_IMAGE_TYPE_1D; th = 1; break;
    case 2: type = VK_IMAGE_TYPE_3D; depth = t->depth ? t->depth : 1; break;
    case 3: type = VK_IMAGE_TYPE_1D; layers = t->height ? t->height : 1; th = 1; break;
    case 4: case 6: layers = t->depth ? t->depth : 1; break;
    case 8: layers = 6; break;
    case 9: layers = t->depth ? t->depth : 6; break;
    default: break;
    }
    uint32_t levels = t->levels ? t->levels : 1;
    {   // never more levels than the size allows (scaled targets, odd guest values)
        uint32_t m = 1, sz = tw > th ? tw : th;
        if (depth > sz) sz = depth;
        while (sz > 1) { sz >>= 1; m++; }
        if (levels > m) levels = m;
    }
    uint32_t samples = t->samples > 1 ? t->samples : 1;
    if (samples > 1) levels = 1;
    VkFormat vf = mf.vf;
    VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    b->astc_bw = b->astc_bh = 0;
    if (!format_supports(vf, need)) {
        bool astc = vf >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && vf <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK;
        const FormatInfo *fi = t->layout.fmt;
        if (astc && fi && fi->bpb == 16) {  // desktop GPUs: decoded on the CPU into an RGBA8 image of the same colour space
            b->astc_bw = fi->bw;
            b->astc_bh = fi->bh;
            b->astc_srgb = t->format >= 0x87;
            vf = b->astc_srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
        } else {
            static int nlog;
            if (nlog++ < 20) LOG("[vk] format %d (nvn %#x) not supported: RGBA8 instead\n", vf, t->format);
            vf = VK_FORMAT_R8G8B8A8_UNORM;
        }
    }
    b->guest_format = VK_FORMAT_UNDEFINED;
    {   // Banding: Odyssey composes its picture (tonemapped scene and layouts) in an RGB10A2 target with linear values and
        // converts it to the screen's sRGB at the end - 10 linear bits are coarser than 8 sRGB bits in dark tones (one step
        // ~1.6 sRGB levels near 25/255): the steps in its night skies and fog, on the Switch too. Its screen-size RGB10A2
        // targets get half floats (views follow b->format, copies to other formats blit, uploads / readbacks convert:
        // guest_format), and the last copy into the window dithers (exec.c draw_). What is drawn into them stays in
        // 0..1 as before (the tonemapper's colors come from its 8x8x8 grading map; anti-aliasing and layouts blend those),
        // so only the precision differs. Off (menu "Smooth gradients", ODYSSEY_DEBAND=0): as the game made them.
        const VkFormatFeatureFlags blit = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
        b->debandable = b->scalable && vf == VK_FORMAT_A2B10G10R10_UNORM_PACK32 && type == VK_IMAGE_TYPE_2D && layers == 1 &&
                        levels == 1 && samples == 1 && format_supports(vf, blit) &&
                        format_supports(VK_FORMAT_R16G16B16A16_SFLOAT, need | blit | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT);
        if (b->debandable && vk_deband()) {
            b->guest_format = vf;
            vf = VK_FORMAT_R16G16B16A16_SFLOAT;
        }
    }
    bool depthfmt = renderer_format_is_depth(t->format);
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (depthfmt) usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    else if (format_supports(vf, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if ((t->flags & 0x4) && !depthfmt && samples == 1 && format_supports(vf, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT))
        usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    VkImageCreateFlags flags = 0;
    if (!depthfmt) flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;  // format views (NVN texture views, raw sRGB views)
    if (type == VK_IMAGE_TYPE_2D && layers >= 6 && tw == th && samples == 1) flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    // 3D color targets are drawn into one depth slice at a time (Odyssey's color grading maps: a 2D view per slice)
    if (type == VK_IMAGE_TYPE_3D && (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = flags, .imageType = type, .format = vf,
                            .extent = {tw, th, depth}, .mipLevels = levels, .arrayLayers = layers,
                            .samples = (VkSampleCountFlagBits)samples, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkResult r = vmaCreateImage(g_vk.vma, &ci, &ai, &b->image, &b->mem, NULL);
    if (getenv("ODYSSEY_LOG_IMAGES"))
        LOG("[img] %p: nvn %p fmt %#x target %u %ux%ux%u (guest %ux%ux%u) layers %u levels %u samples %u flags %#x usage %#x vk fmt %d -> %d nvn flags %#x%s\n",
            (void *)b->image, (void *)t, t->format, t->target, tw, th, depth, t->width, t->height, t->depth, layers, levels, samples,
            (unsigned)flags, (unsigned)usage, (int)vf, (int)r, t->flags, b->scalable ? " scalable" : "");
    if (r != VK_SUCCESS) {
        LOG("[vk] image creation failed (%d): nvn fmt %#x %ux%ux%u target %u\n", r, t->format, t->width, t->height, t->depth, t->target);
        ci.format = vf = VK_FORMAT_R8G8B8A8_UNORM;
        ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        ci.flags &= ~VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &b->image, &b->mem, NULL));
        depthfmt = false;
    }
    b->storage = (ci.usage & VK_IMAGE_USAGE_STORAGE_BIT) != 0;
    b->format = vf;
    b->width = tw; b->height = th; b->depth = depth; b->layers = layers; b->levels = levels; b->samples = samples;
    b->aspect = depthfmt ? aspect_of(t->format) : VK_IMAGE_ASPECT_COLOR_BIT;
    b->base_type = type == VK_IMAGE_TYPE_1D ? (layers > 1 ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D)
                 : type == VK_IMAGE_TYPE_3D ? VK_IMAGE_VIEW_TYPE_3D
                 : t->target == 8 ? VK_IMAGE_VIEW_TYPE_CUBE : t->target == 9 ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY
                 : layers > 1 || t->target == 4 || t->target == 6 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    vk_image_init_layout(vk_upload_cmd(), b->image, b->aspect);
    if (depthfmt) {
        // New depth / stencil images start zeroed (as fresh memory reads on Apple GPUs). The game builds its depth
        // pyramid with depth-only copies, so the stencil of those images was never written: whatever the memory
        // held, different per image and per run, then failed or passed the depth-of-field passes' stencil test in
        // patches (flicker that came and went between runs, every other frame, below 1x internal resolution).
        VkCommandBuffer cb = vk_upload_cmd();
        VkClearDepthStencilValue zero = {0.0f, 0};
        VkImageSubresourceRange rg = {b->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
        vkCmdClearDepthStencilImage(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &rg);
        VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
        VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
        vkCmdPipelineBarrier2(cb, &di);
    }
}

static void defer(Dead d) {
    if (ndead == cap_dead) { cap_dead = cap_dead ? cap_dead * 2 : 256; dead = realloc(dead, cap_dead * sizeof *dead); }
    d.seq = vk_pending_seq();
    static int log_dead = -1;
    if (log_dead < 0) log_dead = getenv("ODYSSEY_LOG_DEAD") != NULL;
    if (log_dead && d.view) {
        LOG("[dead] view %p deferred at %llu (thread %lu)\n", (void *)d.view, (unsigned long long)d.seq, (unsigned long)(uintptr_t)pthread_self());
    }
    dead[ndead++] = d;
}

void vk_defer_image(VkImage img, VmaAllocation mem) { defer((Dead){.img = img, .mem = mem}); }
void vk_defer_view(VkImageView v) { defer((Dead){.view = v}); }

void vk_dl_note_upload(void *img, uint32_t w, uint32_t h, uint32_t lv);  // (exec.c: ODYSSEY_VK_DEVLOST_TRACE)

static uint32_t level_w(const TexBackend *b, uint32_t lv) { uint32_t v = b->width >> lv; return v ? v : 1; }
static uint32_t level_h(const TexBackend *b, uint32_t lv) { uint32_t v = b->height >> lv; return v ? v : 1; }

static void destroy_image(TexBackend *b) {
    views_forget(b);
    if (b->image || b->mem) defer((Dead){.img = b->image, .mem = b->mem});  // (never the same allocation twice)
    b->image = VK_NULL_HANDLE;
    b->mem = VK_NULL_HANDLE;
}

// Live game textures: an in-place reboot (vk_textures_reset) destroys them all.
static NvnTexture **g_live;
static int g_nlive, g_cap_live;
int vk_live_textures(void) { return g_nlive; }
static void live_add(NvnTexture *t, TexBackend *b) {
    if (g_nlive == g_cap_live) { g_cap_live = g_cap_live ? g_cap_live * 2 : 1024; g_live = realloc(g_live, g_cap_live * sizeof *g_live); }
    g_live[g_nlive++] = t;
    b->reg = g_nlive;
}
static void live_remove(TexBackend *b) {
    if (!b->reg) return;
    NvnTexture *last = g_live[--g_nlive];
    g_live[b->reg - 1] = last;
    if (last->backend) ((TexBackend *)last->backend)->reg = b->reg;
    b->reg = 0;
}

bool vk_deband(void) {
    static int env = -2;
    if (env == -2) env = getenv("ODYSSEY_DEBAND") ? atoi(getenv("ODYSSEY_DEBAND")) != 0 : -1;
    return env >= 0 ? env : g_settings.deband;
}

static TexBackend *tex_backend(NvnTexture *t) {
    TexBackend *b = t->backend;
    if (b) {
        // output resolution changed, or smooth gradients switched on / off (create_image "Banding"): reallocate
        if (b->scalable && (b->sx != g_rs_x || b->sy != g_rs_y || (b->debandable && !b->guest_format == vk_deband()))) {
            destroy_image(b);
            create_image(t, b);
        }
        return b;
    }
    vk_collect_dead();
    b = calloc(1, sizeof *b);
    b->scalable = tex_scalable(t);
    if (b->scalable && renderer_format_is_depth(t->format)) {  // the scene's depth buffer follows the game's own
        static uint32_t last_w, last_h;                         // dynamic resolution (it recreates its targets)
        static int nlog;
        if ((t->width != last_w || t->height != last_h) && t->width >= 640 && nlog < 50) {
            nlog++;
            LOG("[res] the game renders its scene at %ux%u (frame %llu)\n", t->width, t->height,
                (unsigned long long)renderer_present_count());
            last_w = t->width; last_h = t->height;
        }
    }
    create_image(t, b);
    t->backend = b;
    live_add(t, b);
    return b;
}

static void twin_forget(NvnTexture *t);
void backend_texture_destroyed(NvnTexture *t) {
    vk_render_lock();  // (any game thread: Odyssey finalizes on its loading threads while another one renders)
    twin_forget(t);
    TexBackend *b = t->backend;
    if (b) {
        extern void vk_alias_texture_destroyed(NvnTexture *t);  // (exec.c copy aliases: a source's contents handed over)
        vk_alias_texture_destroyed(t);
        live_remove(b);
        destroy_image(b);
        free(b->pending_rgba);
        free(b);
    }
    t->backend = NULL;
    vk_render_unlock();
}

// Reboot (device.c backend_reset; GPU idle): every game texture and its views go.
void vk_textures_reset(void) {
    int n = g_nlive;
    extern void vk_alias_reset(void);
    vk_alias_reset();
    while (g_nlive > 0) backend_texture_destroyed(g_live[g_nlive - 1]);
    vk_collect_dead();
    LOG("[vk] reboot: %d textures released\n", n);
}

// ================================================================== uploads (same change detection as metal.m)
static uint64_t full_hash_budget, full_hash_budget_frame;

// Four independent lanes (one serial multiply chain ran at ~1.5 bytes/cycle; dynamic textures are hashed every
// frame). Values are only compared within a run.
static uint64_t hash_full(const uint8_t *p, uint64_t n) {
    uint64_t h0 = 0x9E3779B97F4A7C15ull ^ n, h1 = 0xC2B2AE3D27D4EB4Full, h2 = 0x165667B19E3779F9ull, h3 = 0x27D4EB2F165667C5ull;
    uint64_t i = 0;
    for (; i + 32 <= n; i += 32) {
        uint64_t v[4];
        memcpy(v, p + i, 32);
        h0 = (h0 ^ v[0]) * 0x100000001B3ull; h0 ^= h0 >> 29;
        h1 = (h1 ^ v[1]) * 0x100000001B3ull; h1 ^= h1 >> 29;
        h2 = (h2 ^ v[2]) * 0x100000001B3ull; h2 ^= h2 >> 29;
        h3 = (h3 ^ v[3]) * 0x100000001B3ull; h3 ^= h3 >> 29;
    }
    uint64_t h = h0 ^ (h1 * 0x9E3779B97F4A7C15ull) ^ (h2 * 0xC2B2AE3D27D4EB4Full) ^ (h3 * 0x165667B19E3779F9ull);
    for (; i < n; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    return h ^ (h >> 31);
}

static uint64_t fnv_sample(const uint8_t *p, uint64_t n) {
    uint64_t h = 1469598103934665603ull;
    uint64_t step = n > 4096 ? n / 4096 : 1;
    for (uint64_t i = 0; i < n; i += step) { h ^= p[i]; h *= 1099511628211ull; }
    return h ^ n;
}

static pthread_mutex_t pending_lock = PTHREAD_MUTEX_INITIALIZER;

static bool tex_scaled(const TexBackend *b) { return b->scalable && (b->sx != 1 || b->sy != 1); }

// A buffer -> image copy in guest texels. A scaled allocation (a screen-shaped texture the CPU fills: a video frame,
// a target the game uploads into) gets the region through a temporary guest-sized image, scaled into place - copied
// directly, the data covered only the guest-sized corner of the image. Depth / stencil can't be blitted: direct.
// An image in another format than the game's bytes (guest_format) gets them through such an image too, converted.
void vk_copy_buffer_to_image(VkCommandBuffer cb, TexBackend *b, VkBuffer buf, const VkBufferImageCopy *rg) {
    if (!b->guest_format && (!tex_scaled(b) || b->depth > 1 || (b->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) ||
                             !format_supports(b->format, VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT))) {
        vkCmdCopyBufferToImage(cb, buf, b->image, VK_IMAGE_LAYOUT_GENERAL, 1, rg);
        return;
    }
    uint32_t w = rg->imageExtent.width, h = rg->imageExtent.height, nl = rg->imageSubresource.layerCount, lv = rg->imageSubresource.mipLevel;
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                            .format = b->guest_format ? b->guest_format : b->format,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = nl, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkImage tmp;
    VmaAllocation mem;
    if (vmaCreateImage(g_vk.vma, &ci, &ai, &tmp, &mem, NULL) != VK_SUCCESS) {
        vkCmdCopyBufferToImage(cb, buf, b->image, VK_IMAGE_LAYOUT_GENERAL, 1, rg);
        return;
    }
    vk_image_init_layout(cb, tmp, VK_IMAGE_ASPECT_COLOR_BIT);
    VkBufferImageCopy c = *rg;
    c.imageSubresource.mipLevel = 0;
    c.imageSubresource.baseArrayLayer = 0;
    c.imageOffset = (VkOffset3D){0, 0, 0};
    vkCmdCopyBufferToImage(cb, buf, tmp, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    vk_full_barrier(cb);
    int32_t lw = (int32_t)level_w(b, lv), lh = (int32_t)level_h(b, lv);
    int32_t x0 = (int32_t)lroundf(rg->imageOffset.x * b->sx), y0 = (int32_t)lroundf(rg->imageOffset.y * b->sy);
    int32_t x1 = (int32_t)lroundf((rg->imageOffset.x + (float)w) * b->sx), y1 = (int32_t)lroundf((rg->imageOffset.y + (float)h) * b->sy);
    if (x1 > lw) x1 = lw;
    if (y1 > lh) y1 = lh;
    if (x1 > x0 && y1 > y0) {
        VkImageBlit bl = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, nl}, .srcOffsets = {{0, 0, 0}, {(int32_t)w, (int32_t)h, 1}},
                          .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, lv, rg->imageSubresource.baseArrayLayer, nl},
                          .dstOffsets = {{x0, y0, 0}, {x1, y1, 1}}};
        bool linear = tex_scaled(b) && format_supports(b->format, VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
        vkCmdBlitImage(cb, tmp, VK_IMAGE_LAYOUT_GENERAL, b->image, VK_IMAGE_LAYOUT_GENERAL, 1, &bl,
                       linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
        vk_full_barrier(cb);
    }
    vk_defer_image(tmp, mem);
}

static void upload_pending(TexBackend *b) {
    pthread_mutex_lock(&pending_lock);
    void *px = b->pending_rgba;
    uint32_t w = b->pending_w, h = b->pending_h, stride = b->pending_stride;
    b->pending_rgba = NULL;
    pthread_mutex_unlock(&pending_lock);
    if (!px) return;
    if (!tex_scaled(b)) {  // (a scaled allocation takes any size: vk_copy_buffer_to_image)
        if (w > b->width) w = b->width;
        if (h > b->height) h = b->height;
    }
    VkSpan s = vk_transient_copy(px, (uint64_t)stride * h, 16);
    free(px);
    VkBufferImageCopy rg = {.bufferOffset = s.off, .bufferRowLength = stride / 4,
                            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
    VkCommandBuffer cb = vk_upload_cmd();
    vk_dl_note_upload((void *)b->image, rg.imageExtent.width, rg.imageExtent.height, rg.imageSubresource.mipLevel);
    vk_checkpoint(cb, "upload image %p %ux%ux%u lv %u layer %u fmt %d", (void *)b->image, rg.imageExtent.width, rg.imageExtent.height, rg.imageExtent.depth, rg.imageSubresource.mipLevel, rg.imageSubresource.baseArrayLayer, (int)b->format);
                vk_copy_buffer_to_image(cb, b, s.buf, &rg);
}

uint64_t prof_tex_checks, prof_tex_samples, prof_tex_full, prof_tex_full_bytes, prof_tex_dyn, prof_tex_watch_skips;
static void upload_texture(NvnTexture *t, TexBackend *b) {
    if (!t->addr || t->target == 10) return;
    bool linear = nvn_flags_linear(t->flags);
    if (linear) b->dynamic = true; // CPU-writable rows must be checked before the static-texture throttle.
    uint64_t frame = vk_frame_counter();
    // the button icon sheet whose icons should change (keyboard / controller, a rebind): uploaded again, its bytes
    // unchanged (nvn/kbprompts.c)
    // Nothing of it before the keyboard / mouse is used (kbprompts_active): a controller player's textures go as ever.
    bool kb = kbprompts_active();
    bool look = kb && !b->kbp && b->uploaded && t->layout.fmt && t->layout.layers <= 1 &&
                kbprompts_candidate(t->width, t->height, t->layout.fmt->bw, t->layout.fmt->bh, t->layout.fmt->bpb);
    bool repatch = look || (b->kbp > 0 && b->kbp_gen != kbprompts_generation());
    if (b->uploaded && b->checked_frame == frame && !repatch) return;
    if (b->uploaded && !b->dynamic && ((frame + ((uintptr_t)b >> 6)) & 7) && !repatch) return;
    const FormatInfo *fi = t->layout.fmt;
    if (!fi || !fi->bpb) return;
    VkFmt mf = fmt_of(t->format);
    uint64_t layer_size = linear ? t->stride * t->height : tegra_layer_size(&t->layout);
    uint64_t total = linear ? layer_size : tegra_total_size(&t->layout);
    if (linear && (t->layout.layers != 1 || t->layout.depth != 1 || t->layout.levels != 1 ||
        t->stride < ((uint64_t)t->width + fi->bw - 1) / fi->bw * fi->bpb ||
        (t->pool && (t->pool_offset > t->pool->size || total > t->pool->size - t->pool_offset)))) return;
    b->checked_frame = frame;
    prof_tex_checks++;
    if (full_hash_budget_frame != frame) { full_hash_budget_frame = frame; full_hash_budget = 4 << 20; }
    const uint8_t *mem = (const uint8_t *)(uintptr_t)t->addr;
    static int always_full = -1;
    if (always_full < 0) always_full = getenv("ODYSSEY_TEX_FULLHASH") ? 1 : 0;
    bool full = b->dynamic || !b->uploaded || always_full || repatch;
    bool periodic = frame - b->full_frame >= 120 && full_hash_budget >= total;  // (safety net: a full hash now and then)
    if (!full && periodic) { full = true; full_hash_budget -= total; }
    // The OS's page write tracking (exec.c vk_pages_changed, shared with the vertex cache): texture memory the CPU
    // hasn't written since the last check is not hashed at all (dynamic textures were fully hashed every frame,
    // ~2.4 MB a frame in the overworld). The periodic full hash stays as a safety net. ODYSSEY_VK_TEX_WATCH=0: off.
    bool by_watch = false;  // (made full because the pages were written)
    static int watch = -1;
    if (watch < 0) { const char *e = getenv("ODYSSEY_VK_TEX_WATCH"); watch = !e || atoi(e) != 0; }
    if (watch && !always_full) {
        extern int vk_pages_changed(uint64_t addr, uint64_t size, uint32_t since, uint32_t *now);
        uint32_t now = 0;
        int ch = vk_pages_changed(t->addr, total, b->wgen, &now);
        if (ch >= 0) {
            b->wgen = now;
            if (!ch && b->uploaded && !repatch) {
                if (!periodic) { prof_tex_watch_skips++; return; }
                if (b->dynamic) full_hash_budget -= total;  // (the others took theirs above)
            }
            // Written since the last check: compared whole, not by its sampled bytes - a texture the CPU changes in a
            // few spots (the poison's paint map, PaintObj: Mario hurt by poison the screen no longer showed, or never
            // showed - issue #3) kept its sampled bytes and stayed stale until the periodic full hash, which a busy
            // frame's budget can put off
            if (ch > 0 && !full) full = by_watch = true;
        }
    }
    if (!full) {
        prof_tex_samples++;
        uint64_t sh = fnv_sample(mem, total);
        if (sh == b->sample_hash) return;
    }
    prof_tex_full++; prof_tex_full_bytes += total; if (b->dynamic) prof_tex_dyn++;
    uint64_t h = hash_full(mem, total);
    b->full_frame = frame;
    uint64_t old_sample = b->sample_hash;
    b->sample_hash = fnv_sample(mem, total);
    if (by_watch && b->uploaded && h != b->data_hash && b->sample_hash == old_sample) {
        static int n;  // (what the sampled check alone would have missed)
        if (n++ < 30) LOG("[tex] a change the sampled check missed: %p %ux%u f%#x addr %llx (%llu KB, frame %llu)\n", (void *)t,
                          t->width, t->height, t->format, (unsigned long long)t->addr, (unsigned long long)(total >> 10),
                          (unsigned long long)frame);
    }
    if (b->uploaded && h == b->data_hash && !repatch) return;
    if (b->uploaded && h != b->data_hash) b->dynamic = true;
    if (!b->uploaded || h != b->data_hash || look)  // (new contents: the button icon sheet? looked at deswizzled, below)
        b->kbp = !kb ? 0 : t->layout.layers <= 1 && kbprompts_candidate(t->width, t->height, fi->bw, fi->bh, fi->bpb) ? 2 : -1;
    if (b->kbp > 0) b->kbp_gen = kbprompts_generation();
    b->data_hash = h;
    b->uploaded = true;
    if ((t->target == 8 || t->target == 9) && getenv("ODYSSEY_LOG_CUBES")) {  // (where cube map contents come from)
        static int n;
        if (n++ < 200)
            LOG("[cube] upload %p %ux%u f%#x levels %u layers %u addr %llx size %#llx hash %016llx flags %#x (frame %llu)\n", (void *)t,
                t->width, t->height, t->format, t->layout.levels, t->layout.layers, (unsigned long long)t->addr,
                (unsigned long long)total, (unsigned long long)h, t->flags, (unsigned long long)frame);
    }
    if (b->format != mf.vf && !b->astc_bw) return;  // unsupported format: nothing sensible to copy
    VkCommandBuffer cb = vk_upload_cmd();
    uint32_t layers = t->layout.layers ? t->layout.layers : 1;
    if (layers > b->layers && b->depth == 1) layers = b->layers;
    uint32_t levels = t->layout.levels < b->levels ? t->layout.levels : b->levels;
    for (uint32_t layer = 0; layer < layers; layer++) {
        for (uint32_t lv = 0; lv < levels; lv++) {
            LevelLayout l = tegra_level(&t->layout, lv);
            if (b->astc_bw) {  // blocks -> linear block rows -> RGBA8 texels
                const uint8_t *src = (const uint8_t *)(uintptr_t)t->addr + layer * layer_size + l.offset;
                uint8_t *blocks = malloc((uint64_t)l.wb * l.hb * 16);
                if (linear) {
                    if (!nvn_linear_copy_rows(blocks, (uint64_t)l.wb * l.hb * 16, src, total, t->stride, (uint64_t)l.wb * 16, l.hb)) {
                        free(blocks); return;
                    }
                }
                else tegra_deswizzle(src, blocks, l.wb, l.hb, 1, 16, l.block_height_log2, l.block_depth_log2);
                VkSpan s = vk_transient((uint64_t)l.w * l.h * 4, 16);
                uint32_t bad = astc_bad_blocks;
                astc_decode_ldr(blocks, l.w, l.h, b->astc_bw, b->astc_bh, b->astc_srgb, s.ptr);
                if (astc_bad_blocks != bad) {  // (magenta in it: HDR content or error blocks - not decoded here)
                    static int nlog;
                    if (nlog++ < 40)
                        LOG("[tex] ASTC %ux%u (%ux%u blocks%s) level %u: %u blocks magenta (HDR or invalid), texture at %llx\n",
                            l.w, l.h, b->astc_bw, b->astc_bh, b->astc_srgb ? ", sRGB" : "", (unsigned)lv,
                            (unsigned)(astc_bad_blocks - bad), (unsigned long long)t->addr);
                }
                free(blocks);
                VkBufferImageCopy rg = {.bufferOffset = s.off, .bufferRowLength = l.w, .bufferImageHeight = l.h,
                                        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, lv, layer, 1},
                                        .imageExtent = {l.w < level_w(b, lv) || tex_scaled(b) ? l.w : level_w(b, lv),
                                                        l.h < level_h(b, lv) || tex_scaled(b) ? l.h : level_h(b, lv), 1}};
                vk_dl_note_upload((void *)b->image, rg.imageExtent.width, rg.imageExtent.height, rg.imageSubresource.mipLevel);
    vk_checkpoint(cb, "upload image %p %ux%ux%u lv %u layer %u fmt %d", (void *)b->image, rg.imageExtent.width, rg.imageExtent.height, rg.imageExtent.depth, rg.imageSubresource.mipLevel, rg.imageSubresource.baseArrayLayer, (int)b->format);
                vk_copy_buffer_to_image(cb, b, s.buf, &rg);
                continue;
            }
            uint32_t row = l.wb * fi->bpb;
            uint64_t sz = (uint64_t)row * l.hb * l.d;
            bool expand = mf.expand_rgb && fi->bpb == 3;
            uint32_t out_row = expand ? l.wb * 4 : row;
            VkSpan s = vk_transient((uint64_t)out_row * l.hb * l.d, 16);
            uint8_t *lin = expand ? malloc(sz) : s.ptr;
            const uint8_t *src = (const uint8_t *)(uintptr_t)t->addr + layer * layer_size + l.offset;
            if (linear) {
                if (!nvn_linear_copy_rows(lin, (uint64_t)row * l.hb, src, total, t->stride, row, l.hb)) {
                    if (expand) free(lin);
                    return;
                }
            } else {
                tegra_deswizzle(src, lin, l.wb, l.hb, l.d, fi->bpb, l.block_height_log2, l.block_depth_log2);
            }
            if (b->kbp == 2 && lv == 0) b->kbp = !expand && kbprompts_identify(lin, l.wb, l.hb) ? 1 : -1;
            if (b->kbp == 1 && !expand) kbprompts_apply(lv, lin, l.wb, l.hb);  // (keyboard / mouse icons)
            if (expand) {  // RGB8 -> RGBA8
                uint8_t *o = s.ptr;
                uint64_t n = (uint64_t)l.wb * l.hb * l.d;
                for (uint64_t i = 0; i < n; i++) { o[i * 4] = lin[i * 3]; o[i * 4 + 1] = lin[i * 3 + 1]; o[i * 4 + 2] = lin[i * 3 + 2]; o[i * 4 + 3] = 255; }
                free(lin);
            }
            uint32_t w = l.w, hh = l.h, d = l.d;
            // the copy stays inside the image's level: a texture allocated at the internal resolution (16:9, render
            // target sized) can still be filled from the CPU - the save slots' screenshots on the title screen. Its
            // full-size data overran the scaled image (only the width was clamped): GPU writes past the allocation,
            // a device loss below 1x after a seed switch.
            if (!tex_scaled(b)) {  // (a scaled one takes the guest size: vk_copy_buffer_to_image scales it in)
                if (w > level_w(b, lv)) w = level_w(b, lv);
                if (hh > level_h(b, lv)) hh = level_h(b, lv);
            }
            VkBufferImageCopy rg = {.bufferOffset = s.off, .bufferRowLength = l.wb * fi->bw, .bufferImageHeight = l.hb * fi->bh,
                                    .imageSubresource = {b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : b->aspect, lv,
                                                         b->depth > 1 ? 0 : layer, 1},
                                    .imageExtent = {w, b->base_type == VK_IMAGE_VIEW_TYPE_1D || b->base_type == VK_IMAGE_VIEW_TYPE_1D_ARRAY ? 1 : hh,
                                                    b->depth > 1 ? d : 1}};
            if (b->aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) continue;  // packed D/S: no CPU layout
            vk_dl_note_upload((void *)b->image, rg.imageExtent.width, rg.imageExtent.height, rg.imageSubresource.mipLevel);
    vk_checkpoint(cb, "upload image %p %ux%ux%u lv %u layer %u fmt %d", (void *)b->image, rg.imageExtent.width, rg.imageExtent.height, rg.imageExtent.depth, rg.imageSubresource.mipLevel, rg.imageSubresource.baseArrayLayer, (int)b->format);
                vk_copy_buffer_to_image(cb, b, s.buf, &rg);
        }
    }
}

// ---- frame interpolation's replays (runtime/nvn/interp.c) draw into twins of the game's images
// A replay re-runs a frame's command lists: drawn into the game's own images it left its in-between in everything the
// game reads again later - a target updated every other frame (a shadow cascade, an effect), a feedback (water ripples,
// a light's fading glare) - and the next game frames showed that state half a frame off: shadows and surfaces flickering
// with in-betweens on. Each image a replay writes is a twin (made once, with the original's description); within the
// replay, reads of what it wrote go to the twin, of anything else to the game's image. A twin starts from its original's
// content at the replay's first write (a pass may load it). The game's images are never written by a replay.
// Frame-start contents: an image the game reads in a frame before writing it (a shadow map drawn for the next frame,
// a buffer used in turn by several effects) held this frame's end by the time the replay ran - the replay read the
// state the frame left, not the one it started from: Tostarena's shop keeper cast a long dark shadow across the sand
// in every third in-between (the depth shadow map, drawn later in the same frame for another use), ice showed holes.
// So the first write of such an image in a logged frame first copies it to its twin, and the replay of that frame
// starts from there; images the frame writes without reading them first need no copy (the replay draws them anew).
extern bool g_interp_log;  // (interp.h: this frame's lists are logged for in-betweens)
static uint32_t twin_gen = 1, replay_epoch;
uint32_t g_tex_epoch = 1;           // the game frame whose lists run (device.c frame_decide counts them)
uint32_t g_interp_replay_epoch;     // (device.c, for the replay to come) the replayed frame's
__thread bool g_twin_redirect;      // (exec.c renderer_interp_replay: this thread replays)
uint32_t vk_twin_epoch = 1;         // (exec.c's replay view memo: changes whenever reads may move to a twin)
void vk_twins_begin(uint32_t epoch) { twin_gen++, vk_twin_epoch++, replay_epoch = epoch; }  // (render lock: a replay starts)
static void image_copy_all(TexBackend *s, TexBackend *d) {
    extern void vk_end_pass(void);
    vk_end_pass();
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    VkImageCopy c[16];
    uint32_t n = 0;
    for (uint32_t lv = 0; lv < s->levels && n < 16; lv++) {
        uint32_t w = s->width >> lv, h = s->height >> lv, dd = s->depth >> lv;
        c[n++] = (VkImageCopy){.srcSubresource = {s->aspect, lv, 0, s->layers}, .dstSubresource = {s->aspect, lv, 0, s->layers},
                               .extent = {w ? w : 1, h ? h : 1, dd ? dd : 1}};
    }
    vkCmdCopyImage(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, n, c);
    vk_full_barrier(cb);
}
static NvnTexture *twin_of(NvnTexture *t) {
    if (!t->twin) {
        NvnTexture *w = malloc(sizeof *w);
        *w = *t;
        w->backend = NULL, w->twin = NULL, w->twin_gen = w->rd_epoch = w->wr_epoch = w->snap_epoch = 0, w->carry = false;
        t->twin = w;
    }
    return t->twin;
}
static bool copy_to_twin(NvnTexture *t) {
    TexBackend *src = tex_backend(t), *dst = tex_backend(twin_of(t));
    dst->gpu_owned = true;
    if (!src->image || !dst->image || src->width != dst->width || src->height != dst->height) return false;
    image_copy_all(src, dst);
    return true;
}
// (a logged frame's lists, render lock) t read or about to be written
static void frame_access(NvnTexture *t, bool write) {
    if (!write) {
        if (t->wr_epoch != g_tex_epoch) t->rd_epoch = g_tex_epoch;
        return;
    }
    if (t->wr_epoch == g_tex_epoch) return;
    t->wr_epoch = g_tex_epoch;
    if (t->rd_epoch == g_tex_epoch) t->carry = true;
    if (t->carry && copy_to_twin(t)) t->snap_epoch = g_tex_epoch;
}
static NvnTexture *replay_redirect(NvnTexture *t, bool for_render) {
    // Only images a frame reads before writing need a twin: every other one the next frame draws anew before reading
    // it, so a replay may draw into the game's own (a twin of each cost ~230 MB at 1600 x 900, 1.3 GB in 4K)
    if (!t->carry) return t;
    bool start = t->twin && t->snap_epoch == replay_epoch;  // (the twin holds the replayed frame's start)
    if (!for_render) return start || (t->twin && t->twin_gen == twin_gen) ? t->twin : t;
    twin_of(t);
    if (t->twin_gen != twin_gen) {
        t->twin_gen = twin_gen;
        vk_twin_epoch++;
        if (!start) copy_to_twin(t);
    }
    return t->twin;
}
// (after a replay, render lock) the image it left for t: the twin when it wrote t, else the game's
TexBackend *vk_tex_replayed(NvnTexture *t) {
    return t->twin && t->twin_gen == twin_gen ? tex_backend(t->twin) : vk_tex(t, true);
}
static void twin_forget(NvnTexture *t) {  // (render lock) the game's image goes: its twin with it
    NvnTexture *w = t->twin;
    if (!w) return;
    t->twin = NULL, t->twin_gen = 0;
    backend_texture_destroyed(w);
    free(w);
}

TexBackend *vk_tex(NvnTexture *t, bool for_render) {
    if (g_twin_redirect) t = replay_redirect(t, for_render);
    else if (g_interp_log) frame_access(t, for_render);
    TexBackend *b = tex_backend(t);
    if (b->pending_rgba) upload_pending(b);
    if (b->paint_cpu_pending) {
        upload_texture(t, b);
        if (b->uploaded) b->paint_cpu_pending = false;
    }
    if (for_render) b->gpu_owned = true;
    else if (!b->gpu_owned) {
        extern uint64_t prof_up_ns;
        uint64_t t0 = prof_up_ns != ~0ull ? SDL_GetTicksNS() : 0;
        upload_texture(t, b);
        if (prof_up_ns != ~0ull) prof_up_ns += SDL_GetTicksNS() - t0;
    }
    return b;
}

void renderer_mark_gpu_owned(NvnTexture *t) { tex_backend(t)->gpu_owned = true; }
bool renderer_is_gpu_owned(NvnTexture *t) { return tex_backend(t)->gpu_owned; }
void backend_pool_written(uint64_t addr, uint64_t size) { (void)addr; (void)size; }

// Host-produced texel data (decoded video, any thread): applied on the render thread at the texture's next use.
void renderer_upload_rgba(NvnTexture *t, const void *rgba, uint32_t w, uint32_t h, uint32_t stride) {
    vk_render_lock();  // (the backend could be going on another thread)
    TexBackend *b = t->backend;
    if (!b) { vk_render_unlock(); return; }  // the game binds the texture before decoding into it; first frames may arrive earlier
    void *copy = malloc((size_t)stride * h);
    memcpy(copy, rgba, (size_t)stride * h);
    pthread_mutex_lock(&pending_lock);
    free(b->pending_rgba);
    b->pending_rgba = copy;
    b->pending_w = w; b->pending_h = h; b->pending_stride = stride;
    b->gpu_owned = true;
    pthread_mutex_unlock(&pending_lock);
    vk_render_unlock();
}

// ================================================================== views
#define VIEW_TAB 16384
typedef struct { TexBackend *b; uint64_t k1, k2; VkImageView view; } ViewEnt;
static ViewEnt view_tab[VIEW_TAB];

// Views may still be referenced by submitted or pending work: destroyed once that has completed.
void vk_views_forget(TexBackend *b) { views_forget(b); }
uint32_t vk_view_gen;  // bumped whenever a cached view may be destroyed (exec.c's per-frame texture memo)
static void views_forget(TexBackend *b) {
    vk_view_gen++;
    for (int i = 0; i < VIEW_TAB; i++)
        if (view_tab[i].b == b) { defer((Dead){.view = view_tab[i].view}); view_tab[i].b = NULL; view_tab[i].view = VK_NULL_HANDLE; }
}

VkImageView vk_image_view(TexBackend *b, VkImageViewType type, VkFormat fmt, uint32_t lv0, uint32_t nlv, uint32_t ly0,
                          uint32_t nly, VkComponentMapping sw, VkImageAspectFlags aspect) {
    uint64_t k1 = (uint64_t)type | (uint64_t)fmt << 8 | (uint64_t)lv0 << 40 | (uint64_t)nlv << 48 | (uint64_t)aspect << 56;
    uint64_t k2 = (uint64_t)ly0 | (uint64_t)nly << 16 | (uint64_t)sw.r << 32 | (uint64_t)sw.g << 40 | (uint64_t)sw.b << 48 | (uint64_t)sw.a << 56;
    uint64_t h = ((uint64_t)(uintptr_t)b * 0x9E3779B97F4A7C15ull) ^ (k1 * 0xC2B2AE3D27D4EB4Full) ^ (k2 * 0x165667B19E3779F9ull);
    uint32_t slot = (uint32_t)(h ^ (h >> 32)) & (VIEW_TAB - 1);
    for (int probe = 0; probe < 16; probe++, slot = (slot + 1) & (VIEW_TAB - 1)) {
        ViewEnt *e = &view_tab[slot];
        if (e->b == b && e->k1 == k1 && e->k2 == k2) return e->view;
        if (!e->b) break;
    }
    extern uint64_t prof_views_created;
    prof_views_created++;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = b->image, .viewType = type,
                                .format = fmt, .components = sw, .subresourceRange = {aspect, lv0, nlv, ly0, nly}};
    VkImageView v;
    if (vkCreateImageView(g_vk.dev, &vi, NULL, &v) != VK_SUCCESS) {
        static int nfail;
        if (nfail++ < 20) LOG("[vk] view failed: type %d fmt %d (image fmt %d) levels %u+%u layers %u+%u\n", type, fmt, b->format, lv0, nlv, ly0, nly);
        return VK_NULL_HANDLE;
    }
    ViewEnt *e = &view_tab[slot];
    if (e->b) { defer((Dead){.view = e->view}); vk_view_gen++; }  // evict (16 probes taken)
    *e = (ViewEnt){b, k1, k2, v};
    return v;
}

static VkImageViewType view_type_for(int textype) {
    switch (textype) {
    case TEXTYPE_1D: return VK_IMAGE_VIEW_TYPE_1D;
    case TEXTYPE_3D: return VK_IMAGE_VIEW_TYPE_3D;
    case TEXTYPE_CUBE: return VK_IMAGE_VIEW_TYPE_CUBE;
    case TEXTYPE_2D_ARRAY: return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    case TEXTYPE_CUBE_ARRAY: return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    case TEXTYPE_1D_ARRAY: return VK_IMAGE_VIEW_TYPE_1D_ARRAY;
    default: return VK_IMAGE_VIEW_TYPE_2D;
    }
}

static const VkComponentMapping IDENTITY = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                            VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};

VkImageView vk_storage_view(NvnTexture *t, const NvnTextureView *v, const ShaderImage *img) {
    TexBackend *b = vk_tex(t, false);
    VkImageViewType type = view_type_for(img->type);
    if (!b->storage || b->base_type != type || b->guest_format || b->samples != 1) return VK_NULL_HANDLE;
    uint32_t level = v && (v->setmask & 1) ? v->base_level : 0;
    uint32_t format = v && (v->setmask & 4) ? v->format : t->format;
    VkFormat vf = v && (v->setmask & 4) ? vk_format(format) : b->format;
    if (level >= b->levels || fmt_of(format).expand_rgb || !format_supports(vf, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT))
        return VK_NULL_HANDLE;
    if (img->access == SHADER_IMAGE_WRITE) {
        VkFormatProperties3 caps = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3};
        VkFormatProperties2 query = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, .pNext = &caps};
        vkGetPhysicalDeviceFormatProperties2(g_vk.phys, vf, &query);
        if (!(caps.optimalTilingFeatures & VK_FORMAT_FEATURE_2_STORAGE_WRITE_WITHOUT_FORMAT_BIT)) return VK_NULL_HANDLE;
    } else {
        const FormatInfo *actual = nvn_format_info(t->format);
        if (actual->bpb != 4 || actual->bw != 1 || actual->bh != 1) return VK_NULL_HANDLE;
        vf = VK_FORMAT_R32_UINT;
        if (img->access == SHADER_IMAGE_ATOMIC && !format_supports(vf, VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT))
            return VK_NULL_HANDLE;
    }
    return vk_image_view(b, type, vf, level, 1, 0, 1, IDENTITY, VK_IMAGE_ASPECT_COLOR_BIT);
}

// render: attachment view (one level, one layer). Otherwise a sampled view of type want_textype (TEXTYPE_*),
// with the NVN component swizzle, converting between compatible types the way renderer.m does.
VkImageView vk_view(NvnTexture *t, const NvnTextureView *v, bool has_view, bool render, int want_textype) {
    TexBackend *b = vk_tex(t, render);
    if (!has_view) v = NULL;
    VkFormat fmt = b->format;
    if (v && (v->setmask & 4) && !(b->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))) {
        VkFormat vf = fmt_of(v->format).vf;
        if (vk_format(t->format) == b->format) fmt = vf;  // (fallback images keep their own format)
    }
    uint32_t lv0 = v && (v->setmask & 1) ? v->base_level : 0;
    uint32_t nlv = v && (v->setmask & 1) && v->num_levels ? v->num_levels : b->levels - (lv0 < b->levels ? lv0 : 0);
    uint32_t ly0 = v && (v->setmask & 2) ? v->min_layer : 0;
    uint32_t nly = v && (v->setmask & 2) && v->num_layers ? v->num_layers : b->layers - (ly0 < b->layers ? ly0 : 0);
    if (lv0 >= b->levels) lv0 = b->levels - 1;
    if (lv0 + nlv > b->levels) nlv = b->levels - lv0;
    if (ly0 >= b->layers) ly0 = b->layers - 1;
    if (ly0 + nly > b->layers) nly = b->layers - ly0;
    if (render) {
        VkImageViewType type = b->base_type == VK_IMAGE_VIEW_TYPE_3D ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
        if (b->base_type == VK_IMAGE_VIEW_TYPE_3D) {  // one depth slice (the view's first layer) of a 3D target
            uint32_t slice = v && (v->setmask & 2) ? v->min_layer : 0, d = b->depth >> lv0;
            if (!d) d = 1;
            if (slice >= d) slice = d - 1;
            return vk_image_view(b, VK_IMAGE_VIEW_TYPE_2D, fmt, lv0, 1, slice, 1, IDENTITY, b->aspect);
        }
        return vk_image_view(b, type, fmt, lv0, 1, ly0, 1, IDENTITY, b->aspect);
    }
    VkImageAspectFlags aspect = b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : b->aspect;
    VkComponentMapping sw = IDENTITY;
    if (!(b->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) && b->samples == 1) {
        const uint8_t *s = v && (v->setmask & 8) ? v->swizzle : t->swizzle;
        uint8_t c[4];
        for (int i = 0; i < 4; i++) c[i] = s[i] <= 5 ? s[i] : (uint8_t)(2 + i);
        if (c[0] || c[1] || c[2] || c[3]) {  // never set: identity
            if (fmt_of(t->format).alpha_one)
                for (int i = 0; i < 4; i++) if (c[i] == 5) c[i] = 1;
            // NVN swizzle 0 zero, 1 one, 2..5 r,g,b,a == VkComponentSwizzle - 1
            sw = (VkComponentMapping){(VkComponentSwizzle)(c[0] + 1), (VkComponentSwizzle)(c[1] + 1), (VkComponentSwizzle)(c[2] + 1),
                                      (VkComponentSwizzle)(c[3] + 1)};
        } else if (fmt_of(t->format).alpha_one) {
            sw.a = VK_COMPONENT_SWIZZLE_ONE;
        }
    }
    VkImageViewType want = view_type_for(want_textype);
    VkImageViewType have = b->base_type;
    if ((b->samples > 1) != (want_textype == TEXTYPE_2D_MS)) return vk_dummy_view(want_textype);
    if (want == have) {
        if (have == VK_IMAGE_VIEW_TYPE_CUBE) nly = 6;
        return vk_image_view(b, want, fmt, lv0, nlv, ly0, nly, sw, aspect);
    }
    if (want == VK_IMAGE_VIEW_TYPE_2D && (have == VK_IMAGE_VIEW_TYPE_2D_ARRAY || have == VK_IMAGE_VIEW_TYPE_CUBE))
        return vk_image_view(b, want, fmt, lv0, nlv, ly0, 1, sw, aspect);
    if (want == VK_IMAGE_VIEW_TYPE_2D_ARRAY && (have == VK_IMAGE_VIEW_TYPE_2D || have == VK_IMAGE_VIEW_TYPE_CUBE))
        return vk_image_view(b, want, fmt, lv0, nlv, ly0, have == VK_IMAGE_VIEW_TYPE_CUBE ? nly : 1, sw, aspect);
    if (want == VK_IMAGE_VIEW_TYPE_CUBE && have == VK_IMAGE_VIEW_TYPE_2D_ARRAY && b->layers >= 6 && b->width == b->height)
        return vk_image_view(b, want, fmt, lv0, nlv, ly0 + 6 <= b->layers ? ly0 : 0, 6, sw, aspect);
    if (want == VK_IMAGE_VIEW_TYPE_1D_ARRAY && have == VK_IMAGE_VIEW_TYPE_1D)
        return vk_image_view(b, want, fmt, lv0, nlv, 0, 1, sw, aspect);
    static int nlog;
    if (nlog++ < 30) LOG("[tex] type mismatch: want %d have %d (%ux%ux%u target %u fmt %#x layers %u)\n", want, have, t->width,
                         t->height, t->depth, t->target, t->format, b->layers);
    return vk_dummy_view(want_textype);
}

// 1x1 black textures for unbound slots, per type
VkImageView vk_dummy_view(int textype) {
    static TexBackend dummies[10];
    static VkImageView views[10];
    if (textype < 0 || textype >= 10) textype = TEXTYPE_2D;
    if (views[textype]) return views[textype];
    TexBackend *b = &dummies[textype];
    VkImageViewType vt = view_type_for(textype);
    bool cube = vt == VK_IMAGE_VIEW_TYPE_CUBE || vt == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    if (textype == TEXTYPE_2D_MS) {
        VkSampleCountFlags supported = g_vk.props.limits.sampledImageColorSampleCounts;
        samples = supported & VK_SAMPLE_COUNT_4_BIT ? VK_SAMPLE_COUNT_4_BIT : VK_SAMPLE_COUNT_2_BIT;
    }
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0,
                            .imageType = vt == VK_IMAGE_VIEW_TYPE_3D ? VK_IMAGE_TYPE_3D
                                       : vt == VK_IMAGE_VIEW_TYPE_1D || vt == VK_IMAGE_VIEW_TYPE_1D_ARRAY ? VK_IMAGE_TYPE_1D : VK_IMAGE_TYPE_2D,
                            .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {1, 1, 1}, .mipLevels = 1, .arrayLayers = cube ? 6 : 1,
                            .samples = samples, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &b->image, &b->mem, NULL));
    VkCommandBuffer cb = vk_upload_cmd();
    vk_image_init_layout(cb, b->image, VK_IMAGE_ASPECT_COLOR_BIT);
    VkClearColorValue zero = {{0, 0, 0, 0}};
    VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, cube ? 6 : 1};
    vkCmdClearColorImage(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &all);
    b->format = VK_FORMAT_R8G8B8A8_UNORM;
    b->levels = 1;
    b->layers = cube ? 6 : 1;
    views[textype] = vk_image_view(b, vt, b->format, 0, 1, 0, cube ? 6 : 1, IDENTITY, VK_IMAGE_ASPECT_COLOR_BIT);
    return views[textype];
}

// ================================================================== samplers
static VkCompareOp compare_op(uint32_t f) {
    switch (f) {
    case 1: return VK_COMPARE_OP_NEVER;
    case 2: return VK_COMPARE_OP_LESS;
    case 3: return VK_COMPARE_OP_EQUAL;
    case 4: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case 5: return VK_COMPARE_OP_GREATER;
    case 6: return VK_COMPARE_OP_NOT_EQUAL;
    case 7: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    default: return VK_COMPARE_OP_ALWAYS;
    }
}

VkSampler vk_sampler(NvnSamplerDesc *d) {
    if (d->backend) return (VkSampler)d->backend;
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = d->mag_filter == 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    uint32_t mf = d->min_filter;
    si.minFilter = (mf == 1 || mf == 3 || mf == 5) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    si.mipmapMode = mf == 4 || mf == 5 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    VkSamplerAddressMode modes[3];
    bool border = false;
    for (int i = 0; i < 3; i++) {
        switch (d->wrap[i]) {
        case 0: case 7: modes[i] = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; break;
        case 1: modes[i] = VK_SAMPLER_ADDRESS_MODE_REPEAT; break;
        case 2: case 3: modes[i] = VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE; break;
        case 4: case 5: modes[i] = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER; border = true; break;
        case 6: modes[i] = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT; break;
        default: modes[i] = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        }
    }
    si.addressModeU = modes[0]; si.addressModeV = modes[1]; si.addressModeW = modes[2];
    si.minLod = d->lod_min;
    si.maxLod = d->lod_max > 1000 ? VK_LOD_CLAMP_NONE : d->lod_max;
    if (mf <= 1) si.maxLod = si.minLod + 0.25f;  // not mipmapped: stay on the base level
    if (si.maxLod < si.minLod) si.maxLod = si.minLod;
    si.mipLodBias = d->lod_bias;
    if (d->max_aniso > 1) { si.anisotropyEnable = VK_TRUE; si.maxAnisotropy = fminf(d->max_aniso, 16); }
    {   // Anisotropic filtering setting (menu, Video: 2x-16x; read at start): surface textures - filtered both ways,
        // mipmapped, tiling (repeat / mirrored repeat) and no depth compare - get at least that level, so ground and
        // walls at a shallow angle stay sharp into the distance. Shadow maps, data maps and screen-sized buffers (clamped,
        // not mipmapped or compared) keep the game's setting, as TOTK's recomp does.
        static int level = -1;
        if (level < 0) level = settings_aniso_at_start() && g_vk.feat_aniso ? settings_aniso_at_start() : 0;
        bool tiles = false;
        for (int i = 0; i < 2; i++)
            tiles |= modes[i] == VK_SAMPLER_ADDRESS_MODE_REPEAT || modes[i] == VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        if (level > 1 && mf >= 2 && si.magFilter == VK_FILTER_LINEAR && si.minFilter == VK_FILTER_LINEAR && tiles &&
            !d->compare_mode) {
            float lv = fminf((float)level, g_vk.props.limits.maxSamplerAnisotropy);
            if (!si.anisotropyEnable || si.maxAnisotropy < lv) { si.anisotropyEnable = VK_TRUE; si.maxAnisotropy = lv; }
        }
    }
    static float lodmin = -1;
    if (lodmin < 0) lodmin = getenv("ODYSSEY_LOD_MIN") ? (float)atof(getenv("ODYSSEY_LOD_MIN")) : 0;
    if (lodmin > 0 && mf > 1) si.minLod = fmaxf(si.minLod, lodmin);
    static int stest = -1;
    if (stest < 0) stest = getenv("ODYSSEY_SAMPLER_TEST") != NULL;
    if (stest && mf > 1) { si.minLod = 0; si.maxLod = VK_LOD_CLAMP_NONE; si.anisotropyEnable = VK_TRUE; si.maxAnisotropy = 16; }
    if (d->compare_mode) {
        si.compareEnable = VK_TRUE;
        si.compareOp = getenv("ODYSSEY_NO_SHADOW") ? VK_COMPARE_OP_ALWAYS : compare_op(d->compare_func);
    }
    VkSamplerCustomBorderColorCreateInfoEXT cb = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT};
    if (border) {
        const float *c = d->border;
        if (c[0] == 0 && c[1] == 0 && c[2] == 0 && c[3] == 0) si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        else if (c[0] == 0 && c[1] == 0 && c[2] == 0 && c[3] == 1) si.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        else if (c[0] == 1 && c[1] == 1 && c[2] == 1 && c[3] == 1) si.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        else if (g_vk.custom_border) {
            memcpy(cb.customBorderColor.float32, c, 16);
            si.borderColor = VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
            si.pNext = &cb;
        } else si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    }
    // One VkSampler per distinct state: the game registers samplers again and again (Odyssey: thousands a second in a
    // cutscene), and a sampler per registration passed the device's limit of 4000 (then the GPU faulted). The key is
    // everything vkCreateSampler reads above.
    typedef struct {
        VkFilter mag, min;
        VkSamplerMipmapMode mip;
        VkSamplerAddressMode u, v, w;
        float min_lod, max_lod, bias, aniso;
        VkBool32 aniso_on, compare_on;
        VkCompareOp compare;
        VkBorderColor border;
        float custom[4];
    } SamplerKey;
    SamplerKey k;
    memset(&k, 0, sizeof k);  // (padding too: the key is compared bytewise)
    k.mag = si.magFilter, k.min = si.minFilter, k.mip = si.mipmapMode;
    k.u = si.addressModeU, k.v = si.addressModeV, k.w = si.addressModeW;
    k.min_lod = si.minLod, k.max_lod = si.maxLod, k.bias = si.mipLodBias, k.aniso = si.maxAnisotropy;
    k.aniso_on = si.anisotropyEnable, k.compare_on = si.compareEnable, k.compare = si.compareOp;
    k.border = si.borderColor;
    if (si.borderColor == VK_BORDER_COLOR_FLOAT_CUSTOM_EXT) memcpy(k.custom, cb.customBorderColor.float32, sizeof k.custom);
    static struct { SamplerKey key; VkSampler s; } *cache;
    static int ncache, cap;
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    VkSampler s = VK_NULL_HANDLE;
    for (int i = 0; i < ncache && !s; i++)
        if (!memcmp(&cache[i].key, &k, sizeof k)) s = cache[i].s;
    if (!s) {
        VK_CHECK(vkCreateSampler(g_vk.dev, &si, NULL, &s));
        if (ncache == cap) {
            cap = cap ? cap * 2 : 64;
            cache = realloc(cache, cap * sizeof *cache);
        }
        cache[ncache].key = k;
        cache[ncache++].s = s;
        if ((ncache & (ncache - 1)) == 0 && ncache >= 64) LOG("[vk] %d distinct samplers\n", ncache);
    }
    pthread_mutex_unlock(&lock);
    d->backend = (void *)s;
    return s;
}

VkSampler vk_default_sampler(bool linear) {
    static VkSampler s[2];
    if (!s[linear]) {
        VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                  .magFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
                                  .minFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
                                  .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                  .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                  .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0.25f};
        VK_CHECK(vkCreateSampler(g_vk.dev, &si, NULL, &s[linear]));
    }
    return s[linear];
}
