// Debug hooks in the game's code: `make gen` with ODYSSEY_HOOKS=<addr>[,<addr>...] (tools/odyssey_patches.py) makes
// each listed instruction call odyssey_debug_hook first. What a hit logs: ODYSSEY_HOOK_LOG, per address
//   <addr>=<item>[,<item>...][;<addr>=...]   items: x<n> / s<n> (a register), x<n>+<hex offset>:<f|w|x><count>
// (floats / 32-bit / 64-bit words at that register plus offset), e.g.
//   ODYSSEY_HOOK_LOG="7000a17770=s0,x0+150:f6,x0+168:f3;7000a17ab8=x19+f8:f6,x19+110:f16"
// Without an entry for the address: x0-x3, x30, s0-s1. At most ODYSSEY_HOOK_MAX hits per address (default 50), only in
// the frames ODYSSEY_HOOK_FRAMES=<from>:<to> if given.
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "odyssey_v141.h"
#include "../rt.h"
#include "../odyssey_pose.h"
#ifdef _WIN32
#include "../nvn/vulkan/dlss_probe.h"
#endif

uint64_t renderer_present_count(void);

typedef struct {
    char kind;  // 'x' / 's' register, 'm' memory
    int reg;
    uint64_t off;
    char fmt;   // memory: f / w / x
    int count;
} HookItem;
typedef struct {
    uint64_t pc;
    HookItem items[16];
    int nitems, hits;
} HookSpec;
static HookSpec g_specs[32];
static int g_nspecs, g_max = 50;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static void parse_specs(void) {
    const char *m = getenv("ODYSSEY_HOOK_MAX");
    if (m) g_max = atoi(m);
    const char *e = getenv("ODYSSEY_HOOK_LOG");
    for (const char *p = e; p && *p && g_nspecs < 32;) {
        HookSpec *s = &g_specs[g_nspecs];
        char *end;
        s->pc = strtoull(p, &end, 16);
        if (*end != '=') break;
        p = end + 1;
        while (*p && *p != ';' && s->nitems < 16) {
            HookItem *it = &s->items[s->nitems];
            char k = *p++;
            it->reg = (int)strtol(p, &end, 10);
            p = end;
            if (*p == '+') {
                it->kind = 'm';
                it->off = strtoull(p + 1, &end, 16);
                p = end;
                if (*p == ':') { it->fmt = p[1]; it->count = (int)strtol(p + 2, &end, 10); p = end; }
                else it->fmt = 'x', it->count = 1;
            } else it->kind = k;
            s->nitems++;
            if (*p == ',') p++;
        }
        g_nspecs++;
        if (*p == ';') p++;
    }
}

static float sreg(Cpu *c, int n) { float f; memcpy(&f, &c->v[n], 4); return f; }

void odyssey_debug_hook(Cpu *c, uint64_t pc) {
    // the hooked places in 1.0.0, or in 1.4.1 (runtime/odyssey_v141.h)
    static int v141 = -1;
    if (v141 < 0) v141 = loader_main_is_v141();
    uint64_t camera_at = v141 ? ODYSSEY_V141_DRAWMAIN_CAMERA : 0x70004ccbc8ull;
    uint64_t pose_at = v141 ? ODYSSEY_V141_MODELKEEPER_CALC : 0x700094c428ull;
    uint64_t view_at = v141 ? ODYSSEY_V141_UPDATE_VIEW_GPU : 0x7000879c98ull;
    if (pc == camera_at) {
        extern void odyssey_camera_draw(Cpu *);
        odyssey_camera_draw(c);
#ifdef _WIN32
        // This camera-copy hook is part of every generated game library. The
        // older updateViewGpu probe was only inserted by an optional debug build,
        // so ordinary installs never supplied a camera and DLSS silently skipped.
        if (vk_temporal_enabled()) {
            static uint64_t get_projection, get_matrix;
            if (!get_projection) {
                get_projection = loader_find_symbol("_ZN2al13getProjectionEPKNS_10IUseCameraEi");
                get_matrix = loader_find_symbol("_ZNK4sead10Projection25getDeviceProjectionMatrixEv");
            }
            if (get_projection && get_matrix) {
                Cpu probe = *c;
                probe.x[0] = c->x[19] + 0x18;  // Scene's IUseCamera subobject, view zero
                probe.x[1] = 0;
                guest_call(&probe, get_projection);
                uint64_t projection = probe.x[0];
                uint64_t copied_camera = v141 ? c->x[29] - 0xf0 : c->x[23];
                if (projection >= GUEST_LO && projection < GUEST_HI - 0x8c &&
                    copied_camera >= GUEST_LO && copied_camera < GUEST_HI - 0x38) {
                    // The projection is lazy: obtain this frame's matrix rather
                    // than copying last frame's cached field after an FOV change.
                    probe.x[0] = projection;
                    guest_call(&probe, get_matrix);
                    uint64_t matrix = probe.x[0];
                    if (matrix < GUEST_LO || matrix >= GUEST_HI - 64) return;
                    DlssCamera camera;
                    memcpy(camera.view, (const void *)(uintptr_t)(copied_camera + 8), sizeof camera.view);
                    memcpy(camera.projection, (const void *)(uintptr_t)matrix, sizeof camera.projection);
                    vk_dlss_queue_camera(&camera);
                }
            }
        }
#endif
        return;
    }
    if (pc == pose_at) {
        odyssey_pose_capture(c);
        return;
    }
#ifdef _WIN32
    // GraphicsSystemInfo::updateViewGpu, view zero. Copy before this guest data can be reused by the next frame.
    // (1.4.1: the same arguments as far as tools/dis141.py shows; to be checked in game with DLSS on)
    if (pc == view_at && (uint32_t)c->x[1] == 0 && c->x[2] && c->x[3] && vk_temporal_enabled()) {
        DlssCamera camera;
        memcpy(camera.view, (const void *)(uintptr_t)(c->x[2] + 8), sizeof camera.view);
        memcpy(camera.projection, (const void *)(uintptr_t)(c->x[3] + 0x4c), sizeof camera.projection);
        vk_dlss_queue_camera(&camera);
        if (!getenv("ODYSSEY_HOOK_LOG")) return;
    }
#endif
    static int once;
    static unsigned long long from, to = ~0ull;  // ODYSSEY_HOOK_FRAMES=<from>:<to>: only hits in those frames (counted too)
    if (once && (renderer_present_count() < from || renderer_present_count() > to)) return;
    pthread_mutex_lock(&g_lock);
    if (!once) {
        once = 1;
        parse_specs();
        if (getenv("ODYSSEY_HOOK_FRAMES")) sscanf(getenv("ODYSSEY_HOOK_FRAMES"), "%llu:%llu", &from, &to);
    }
    if (renderer_present_count() < from || renderer_present_count() > to) { pthread_mutex_unlock(&g_lock); return; }
    HookSpec *s = NULL, any = {0};
    for (int i = 0; i < g_nspecs; i++)
        if (g_specs[i].pc == pc) s = &g_specs[i];
    if (!s) {
        static uint64_t seen[64]; static int counts[64], nseen;
        int k = 0;
        while (k < nseen && seen[k] != pc) k++;
        if (k == nseen && nseen < 64) seen[nseen++] = pc;
        if (k < 64 && counts[k]++ >= g_max) { pthread_mutex_unlock(&g_lock); return; }
        s = &any;
    } else if (s->hits++ >= g_max) { pthread_mutex_unlock(&g_lock); return; }
    char line[4096];
    int n = snprintf(line, sizeof line, "[hook] %llx frame %llu lr %llx", (unsigned long long)pc,
                     (unsigned long long)renderer_present_count(), (unsigned long long)c->x[30]);
    if (!s->nitems)
        n += snprintf(line + n, sizeof line - n, " x0 %llx x1 %llx x2 %llx x3 %llx s0 %g s1 %g", (unsigned long long)c->x[0],
                      (unsigned long long)c->x[1], (unsigned long long)c->x[2], (unsigned long long)c->x[3], sreg(c, 0), sreg(c, 1));
    for (int i = 0; i < s->nitems && n < (int)sizeof line - 64; i++) {
        HookItem *it = &s->items[i];
        if (it->kind == 'x') {
            extern const char *hle_slot_name(uint64_t addr);
            const char *hn = hle_slot_name(c->x[it->reg]);  // (an HLE function's fake address: its name)
            n += snprintf(line + n, sizeof line - n, " | x%d %llx%s%s", it->reg, (unsigned long long)c->x[it->reg], hn ? " " : "", hn ? hn : "");
            continue;
        }
        if (it->kind == 's') { n += snprintf(line + n, sizeof line - n, " | s%d %g", it->reg, sreg(c, it->reg)); continue; }
        uint64_t a = c->x[it->reg] + it->off;
        n += snprintf(line + n, sizeof line - n, " | [x%d+%llx]", it->reg, (unsigned long long)it->off);
        for (int j = 0; j < it->count && n < (int)sizeof line - 32; j++) {
            if (it->fmt == 'f') { float f; memcpy(&f, (void *)(uintptr_t)(a + 4 * j), 4); n += snprintf(line + n, sizeof line - n, " %g", f); }
            else if (it->fmt == 'w') n += snprintf(line + n, sizeof line - n, " %x", *(uint32_t *)(uintptr_t)(a + 4 * j));
            else n += snprintf(line + n, sizeof line - n, " %llx", (unsigned long long)*(uint64_t *)(uintptr_t)(a + 8 * j));
        }
    }
    LOG("%s\n", line);
    pthread_mutex_unlock(&g_lock);
}
