// NVN objects, builders, and state objects (everything except command recording).
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "../kernel.h"
#include "../sampler.h"
#include "interp.h"
#include "rqueue.h"
#include "nvn.h"
#include "linear_texture.h"

void renderer_sync(void);

static uint64_t next_debug_id = 1;

// ================================================================== device
// NVNdeviceInfo -> value
static uint32_t device_integer(uint32_t which) {
    static const uint32_t v[] = {
        [0] = 53, [1] = 207,           // API version
        [2] = 14, [3] = 65536, [4] = 256, [5] = 8, [6] = 16, [7] = 4, [8] = 16, [9] = 32,
        [10] = 16, [11] = 4, [12] = 4, [13] = 4, [14] = 16, [15] = 32, [16] = 32,
        [17] = 256, [18] = 256,        // reserved texture / sampler descriptors
        [19] = 4, [20] = 8, [21] = 0x10000, [22] = 0x1000,
        [23] = 256, [24] = 256, [25] = 4096, [26] = 0x20000, [27] = 16,
        [28] = 1536, [29] = 1024, [30] = 64, [31] = 1536,
        [32] = 0x7FFFFFFF, [33] = 65535, [34] = 65535,
        [35] = 8, [36] = 1048576, [37] = 4096, [38] = 16, [39] = 0x10000,
        [40] = 1, [41] = 1, [42] = 1,  // min/max filtering, stencil8, ASTC
        [43] = 0x40000, [44] = 15, [45] = 2048,
        [46] = 1, [47] = 5, [48] = 1, [49] = 15,
        [50] = 256, [51] = 1, [52] = 0, [53] = 0, [54] = 0, [55] = 256, [56] = 32, [57] = 128,
        [58] = 1, [59] = 1, [60] = 1, [61] = 1, [62] = 1, [63] = 1, [64] = 1,
    };
    // Odyssey 1.4.1 was built against a newer NVN and refuses one older than 53.311 (sead::GameFrameworkNx::
    // initializeGraphicsSystem: no sead::Graphics then, and agl's setup faults on it); 1.0.0 keeps the version it saw
    if (which == 1) {
        static int v141 = -1;
        if (v141 < 0) v141 = loader_main_is_v141();
        if (v141) return 311;
    }
    return which < sizeof v / sizeof *v ? v[which] : 0;
}

NVN(nvnDeviceBuilderSetDefaults) { G(uint32_t, A(0), 0) = 0; }
NVN(nvnDeviceBuilderSetFlags) { G(uint32_t, A(0), 0) = (uint32_t)A(1); }
NVN(nvnDeviceBuilderGetFlags) { RET(G(uint32_t, A(0), 0)); }
NVN(nvnDeviceInitialize) {
    static bool once;
    if (!once) { once = true; backend_init(); }
    LOG("[nvn] device initialized\n");
    RET(1);
}
NVN(nvnDeviceFinalize) { (void)c; }
NVN(nvnDeviceSetDebugLabel) { (void)c; }
NVN(nvnDeviceGetInteger) { *(int32_t *)AP(2) = (int32_t)device_integer((uint32_t)A(1)); }
NVN(nvnDeviceGetCurrentTimestampInNanoseconds) {
    RET(plat_now_ns());
}
NVN(nvnDeviceGetTimestampInNanoseconds) {  // counter.timestamp: Switch GPU ticks (614.4 MHz, as the counters are written)
    RET((uint64_t)((unsigned __int128)G(uint64_t, A(1), 8) * 625 / 384));
}
NVN(nvnDeviceSetIntermediateShaderCache) { (void)c; }
NVN(nvnDeviceGetTextureHandle) { RET((A(1) & 0xFFFFF) | ((A(2) & 0xFFF) << 20)); }
NVN(nvnDeviceGetTexelFetchHandle) { RET(A(1) & 0xFFFFF); }
NVN(nvnDeviceGetImageHandle) { RET(A(1) & 0xFFFFF); }
NVN(nvnDeviceGetSeparateTextureHandle) { RET(A(1) & 0xFFFFF); }
NVN(nvnDeviceGetSeparateSamplerHandle) { RET((A(1) & 0xFFF) << 20); }
NVN(nvnDeviceInstallDebugCallback) { (void)c; }
NVN(nvnDeviceGenerateDebugDomainId) { RET(1); }
static uint32_t window_origin, depth_mode;
RT_ON_RESET(nvn_device_reset) { window_origin = depth_mode = 0; }  // (the next boot sets its own)
NVN(nvnDeviceSetWindowOriginMode) { window_origin = (uint32_t)A(1); LOG("[nvn] window origin mode %u\n", window_origin); }
uint32_t nvn_window_origin(void) { return window_origin; }
uint32_t nvn_depth_mode(void) { return depth_mode; }
NVN(nvnDeviceGetWindowOriginMode) { RET(window_origin); }
NVN(nvnDeviceSetDepthMode) { depth_mode = (uint32_t)A(1); LOG("[nvn] depth mode %u\n", depth_mode); }
NVN(nvnDeviceGetDepthMode) { RET(depth_mode); }
NVN(nvnDeviceRegisterFastClearColor) { RET(1); }
NVN(nvnDeviceRegisterFastClearColori) { RET(1); }
NVN(nvnDeviceRegisterFastClearColorui) { RET(1); }
NVN(nvnDeviceRegisterFastClearDepth) { RET(1); }
NVN(nvnDeviceApplyDeferredFinalizes) { (void)c; }
NVN(nvnDeviceFinalizeCommandHandle) { (void)c; }
NVN(nvnDeviceIsExternalDebuggerAttached) { RET(0); }
NVN(nvnDeviceWalkDebugDatabase) { (void)c; }

// ================================================================== queue
// QueueBuilder layout: device@0 flags@8 cmd@0xC compute@0x10 control@0x14 mem@0x18 memsize@0x20
//                      flush@0x28 priority@0x2C
NVN(nvnQueueBuilderSetDevice) { G(uint64_t, A(0), 0) = A(1); }
NVN(nvnQueueBuilderSetDefaults) {
    memset(AP(0), 0, 0x30);
    G(uint32_t, A(0), 0x0C) = 0x10000;
    G(uint32_t, A(0), 0x10) = 0x40000;
    G(uint32_t, A(0), 0x14) = 0x4000;
    G(uint32_t, A(0), 0x28) = 0x8000;
    G(uint32_t, A(0), 0x2C) = 1;
}
NVN(nvnQueueBuilderSetFlags) { G(uint32_t, A(0), 8) = (uint32_t)A(1); }
NVN(nvnQueueBuilderSetCommandMemorySize) { G(uint32_t, A(0), 0x0C) = (uint32_t)A(1); }
NVN(nvnQueueBuilderSetComputeMemorySize) { G(uint32_t, A(0), 0x10) = (uint32_t)A(1); }
NVN(nvnQueueBuilderSetControlMemorySize) { G(uint32_t, A(0), 0x14) = (uint32_t)A(1); }
NVN(nvnQueueBuilderSetQueueMemory) { G(uint64_t, A(0), 0x18) = A(1); G(uint64_t, A(0), 0x20) = A(2); }
NVN(nvnQueueBuilderSetCommandFlushThreshold) { G(uint32_t, A(0), 0x28) = (uint32_t)A(1); }
NVN(nvnQueueBuilderSetQueuePriority) { G(uint32_t, A(0), 0x2C) = (uint32_t)A(1); }
NVN(nvnQueueBuilderGetDevice) { RET(G(uint64_t, A(0), 0)); }
NVN(nvnQueueBuilderGetFlags) { RET(G(uint32_t, A(0), 8)); }
NVN(nvnQueueBuilderGetCommandMemorySize) { *(int32_t *)AP(1) = G(int32_t, A(0), 0x0C); RET(1); }
NVN(nvnQueueBuilderGetComputeMemorySize) { *(int32_t *)AP(1) = G(int32_t, A(0), 0x10); RET(1); }
NVN(nvnQueueBuilderGetControlMemorySize) { *(int32_t *)AP(1) = G(int32_t, A(0), 0x14); RET(1); }
NVN(nvnQueueBuilderGetCommandFlushThreshold) { *(int32_t *)AP(1) = G(int32_t, A(0), 0x28); RET(1); }
NVN(nvnQueueBuilderGetQueuePriority) { RET(G(uint32_t, A(0), 0x2C)); }
NVN(nvnQueueBuilderGetMemorySize) { RET(0x10000); }  // required queue memory
NVN(nvnQueueBuilderGetQueueMemorySize) { RET(G(uint64_t, A(0), 0x20)); }
NVN(nvnQueueBuilderGetMemory) { RET(G(uint64_t, A(0), 0x18)); }

NVN(nvnQueueInitialize) { RET(1); }
NVN(nvnQueueFinalize) { (void)c; }
NVN(nvnQueueSetDebugLabel) { (void)c; }
NVN(nvnQueueGetError) { RET(0); }
NVN(nvnQueueGetTotalCommandMemoryUsed) { RET(0); }
NVN(nvnQueueGetTotalControlMemoryUsed) { RET(0); }
NVN(nvnQueueGetTotalComputeMemoryUsed) { RET(0); }
NVN(nvnQueueResetMemoryUsageCounts) { (void)c; }
// (On the Switch a submit just hands the commands to the GPU; here the renderer works through them on this thread,
// up to hundreds of ms while shaders build: the core's other threads - the audio thread on core 1 - run meanwhile.)
NVN(nvnQueueSubmitCommands) {
    int n = (int)A(1);
    const uint64_t *handles = AP(2);
    if (rq_enabled()) {  // (translated on the render thread: rqueue.c)
        for (int i = 0; i < n; i++) rq_push(RQ_EXEC, NULL, handles[i]);
        return;
    }
    kernel_block_begin();
    for (int i = 0; i < n; i++) backend_execute(handles[i]);
}
NVN(nvnQueueFlush) { (void)c; }
NVN(nvnQueueFinish) { (void)c; rq_drain(); renderer_sync(); }

static NvnSync *sync_of(uint64_t guest) {
    NvnSync *s = HOST(NvnSync, guest);
    if (!s) { s = calloc(1, sizeof *s); SET_HOST(guest, s); }
    return s;
}
uint64_t renderer_fence_seq(void);
void renderer_wait_seq(uint64_t seq);
NVN(nvnQueueFenceSync) {
    NvnSync *s = sync_of(A(1));
    if (rq_enabled()) { s->signaled = 0; s->ticket = rq_push(RQ_FENCE, s, 0); return; }  // (its submission: known there)
    s->signaled = 1;
    s->seq = renderer_fence_seq();
}
void nvn_sync_set_seq(uint64_t guest, uint64_t seq) { NvnSync *s = sync_of(guest); s->signaled = 1; s->seq = seq; }
NVN(nvnQueueWaitSync) { RET(1); }

// ================================================================== sync / event
NVN(nvnSyncInitialize) { SET_HOST(A(0), calloc(1, sizeof(NvnSync))); RET(1); }
NVN(nvnSyncFinalize) { free(HOST(NvnSync, A(0))); SET_HOST(A(0), 0); }
NVN(nvnSyncSetDebugLabel) { (void)c; }
void renderer_flush(void);
NVN(nvnSyncWait) {  // NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED
    static int flush = -1;
    static uint64_t n;
    if (flush < 0) flush = getenv("ODYSSEY_SYNC_FLUSH") ? 1 : 0;
    if (getenv("ODYSSEY_LOG_SYNC") && n++ % 60 == 0) LOG("[nvn] SyncWait #%llu (lr %llx)\n", (unsigned long long)n, c->x[30]);
    static int whole = -1;  // ODYSSEY_SYNC_ALL=1: old behaviour, wait for all submitted work
    if (whole < 0) whole = getenv("ODYSSEY_SYNC_ALL") ? 1 : 0;
    rq_drain();  // (the render thread first: the fence's lists, or a command list's fence, may still be queued)
    if (flush) renderer_flush();
    else if (whole) renderer_sync();
    else renderer_wait_seq(sync_of(A(0))->seq);  // only the submission that holds this fence
    RET(0);
}
NVN(nvnSyncCreateGLSync) { RET(0); }
NVN(nvnSyncInitializeFromFencedGLSync) { RET(0); }

// EventBuilder: pool@0 offset@8 ; Event: storage address (u32 value lives in guest memory)
NVN(nvnEventBuilderSetDefaults) { G(uint64_t, A(0), 0) = 0; G(uint64_t, A(0), 8) = 0; }
NVN(nvnEventBuilderSetStorage) { G(uint64_t, A(0), 0) = A(1); G(uint64_t, A(0), 8) = A(2); }
NVN(nvnEventBuilderGetStorage) { RET(G(uint64_t, A(0), 0)); }
NVN(nvnEventBuilderGetMemoryPool) { RET(G(uint64_t, A(0), 0)); }
NVN(nvnEventBuilderGetMemoryOffset) { RET(G(uint64_t, A(0), 8)); }
static uint64_t event_addr(uint64_t ev) { return G(uint64_t, ev, 0); }
NVN(nvnEventInitialize) {
    uint64_t pool = G(uint64_t, A(1), 0), off = G(uint64_t, A(1), 8);
    G(uint64_t, A(0), 0) = HOST(NvnPool, pool)->storage + off;
    G(uint64_t, A(0), 8) = pool;
    G(uint64_t, A(0), 16) = off;
    RET(1);
}
NVN(nvnEventFinalize) { (void)c; }
NVN(nvnEventGetValue) { RET(*(uint32_t *)(uintptr_t)event_addr(A(0))); }
NVN(nvnEventSignal) { *(uint32_t *)(uintptr_t)event_addr(A(0)) = (uint32_t)A(2); }
NVN(nvnEventGetMemoryPool) { RET(G(uint64_t, A(0), 8)); }
NVN(nvnEventGetMemoryOffset) { RET(G(uint64_t, A(0), 16)); }

// ================================================================== memory pools
// Builder: device@0 flags@8 storage@0x10 size@0x18
NVN(nvnMemoryPoolBuilderSetDevice) { G(uint64_t, A(0), 0) = A(1); }
NVN(nvnMemoryPoolBuilderSetDefaults) { memset(AP(0), 0, 0x20); G(uint32_t, A(0), 8) = 0x9; }
NVN(nvnMemoryPoolBuilderSetFlags) { G(uint32_t, A(0), 8) = (uint32_t)A(1); }
NVN(nvnMemoryPoolBuilderSetStorage) { G(uint64_t, A(0), 0x10) = A(1); G(uint64_t, A(0), 0x18) = A(2); }
NVN(nvnMemoryPoolBuilderGetDevice) { RET(G(uint64_t, A(0), 0)); }
NVN(nvnMemoryPoolBuilderGetFlags) { RET(G(uint32_t, A(0), 8)); }
NVN(nvnMemoryPoolBuilderGetMemory) { RET(G(uint64_t, A(0), 0x10)); }
NVN(nvnMemoryPoolBuilderGetSize) { RET(G(uint64_t, A(0), 0x18)); }

NVN(nvnMemoryPoolInitialize) {
    NvnPool *p = calloc(1, sizeof *p);
    p->storage = G(uint64_t, A(1), 0x10);
    p->size = G(uint64_t, A(1), 0x18);
    p->flags = G(uint32_t, A(1), 8);
    SET_HOST(A(0), p);
    if (!p->storage || (p->flags & 0x200)) {  // (NVN_MEMORY_POOL_FLAGS_VIRTUAL: no storage of its own)
        static int n;
        if (n++ < 20) LOG("[nvn] memory pool %llx: storage %llx size %#llx flags %#x\n", (unsigned long long)A(0),
                          (unsigned long long)p->storage, (unsigned long long)p->size, p->flags);
    }
    RET(1);
}
// (frame interpolation: a replay on its own thread must not run while a host object it may use goes - interp.c)
NVN(nvnMemoryPoolFinalize) { rq_drain(); interp_free_begin(); free(HOST(NvnPool, A(0))); SET_HOST(A(0), 0); interp_free_end(); }
NVN(nvnMemoryPoolSetDebugLabel) { (void)c; }
// al::getF32FromLinearTextureF16, immediately after its indirect MemoryPoolMap
// call. Both builds keep nc/comp/y/x/offset/row-elements in x19..x24.
// Match the loaded main as well as LR: another game's caller must never cause
// its registers to be interpreted as Odyssey's texture coordinates.
static bool fog_f16_map_reader(uint64_t lr) {
    if (lr == 0x70007ac640ull) return loader_main_is_v141();
    if (lr != 0x7000a2b86cull) return false;
    static const uint8_t v100[8] = {0x3c, 0xa1, 0x2d, 0xfa, 0xaf, 0x9c, 0x82, 0xda};
    const Module *main = loader_module("main");
    return main && !memcmp(main->build_id, v100, sizeof v100);
}

static void paint_sync_rows(uint64_t td, bool refresh, unsigned reader) {
    if (td < GUEST_LO || td >= GUEST_HI - 0x50) return;
    uint64_t pool = G(uint64_t, td, 0), off = G(uint32_t, td, 8);
    if (!pool) return;
    NvnPool *p = HOST(NvnPool, pool);
    if (!p || off >= p->size) return;
    uint64_t capacity = p->size - off;
    if (capacity > 4 * 1024 * 1024) capacity = 4 * 1024 * 1024;
    void *snapshot = malloc(capacity);
    RqCpuTexel q = {.addr = p->storage + off, .components = 1, .component_bytes = 4,
        .snapshot = snapshot, .snapshot_capacity = capacity, .guest_capacity = p->size - off,
        .guest_pitch = G(uint32_t, td, 0x4c), .refresh_snapshot = refresh};
#ifndef __APPLE__
    if (snapshot) {
        if (rq_enabled()) rq_wait(rq_push(RQ_CPU_TEXEL, &q, 0));
        else { extern void renderer_cpu_texel(RqCpuTexel *); renderer_cpu_texel(&q); }
    }
#endif
    bool stored = q.valid && nvn_linear_store_rows((void *)(uintptr_t)q.addr, q.guest_capacity, q.guest_pitch,
        snapshot, capacity, (uint64_t)q.snapshot_width * 4, q.snapshot_height);
    if (stored) p->paint_cpu_shared = true;
    static unsigned counts[4];
    static const char *names[] = {"attack", "label mask", "label color", "median"};
    if (reader < 4 && ++counts[reader] <= 4) LOG("[paint-sync] %s input %llx %ux%u pitch %llu synchronized %d\n",
        names[reader], (unsigned long long)q.addr, q.snapshot_width, q.snapshot_height, (unsigned long long)q.guest_pitch, stored);
    free(snapshot);
}
NVN(nvnMemoryPoolMap) {
    NvnPool *p = HOST(NvnPool, A(0));
    // The 1.0.0 RGBA8 reader calculates its row index after Map. The paint mask
    // is GPU-rendered; return only the requested, validated packed texel.
    static int paint_feedback = -1;
    if (paint_feedback < 0) {
        const char *e = getenv("ODYSSEY_PAINT_CPU_READBACK");
        paint_feedback = !e || atoi(e) != 0;
    }
    static int paint_trace = -1;
    if (paint_trace < 0) paint_trace = getenv("ODYSSEY_LOG_PAINT_READS") != NULL;
    // These PaintObj loops bypass getColorFromLinearTexture. attackSensor
    // samples a whole player footprint, rather than a single coordinate.
    uint64_t paint_lr = c->x[30];
    if (paint_feedback && (paint_lr == 0x70002dc49cull || paint_lr == 0x70002da5e0ull || paint_lr == 0x70002d99a0ull)) {
        static const uint8_t v100[8] = {0x3c, 0xa1, 0x2d, 0xfa, 0xaf, 0x9c, 0x82, 0xda};
        const Module *main = loader_module("main");
        if (main && !memcmp(main->build_id, v100, sizeof v100)) {
            if (paint_lr == 0x70002dc49cull) paint_sync_rows(c->x[22], true, 3);
            else if (paint_lr == 0x70002da5e0ull) paint_sync_rows(c->x[21], false, 0);
            else if (c->x[26] >= GUEST_LO && c->x[26] < GUEST_HI - 0x79b0) {
                // Prepare both buffers before labeling changes either on the CPU.
                // Do not overwrite CPU edits at its inner per-pixel Map calls.
                paint_sync_rows(G(uint64_t, c->x[26], 0x79a8), true, 1);
                paint_sync_rows(G(uint64_t, c->x[26], 0x7968), true, 2);
            }
        }
    }
    if (c->x[30] == 0x7000a2b7ccull && (paint_trace || paint_feedback)) {
        static const uint8_t v100[8] = {0x3c, 0xa1, 0x2d, 0xfa, 0xaf, 0x9c, 0x82, 0xda};
        const Module *main = loader_module("main");
        uint64_t td = c->x[21];
        if (main && !memcmp(main->build_id, v100, sizeof v100) && td >= GUEST_LO && td < GUEST_HI - 0x50) {
            uint32_t pitch = G(uint32_t, td, 0x4c), x = c->x[20], y = c->x[19];
            uint64_t off = (uint32_t)c->x[22];
            int64_t byte = (int64_t)off + (int64_t)(int32_t)((pitch >> 2) * y + x) * 4;
            bool valid = byte >= 0 && (uint64_t)byte < p->size && p->size - (uint64_t)byte >= 4;
            uint32_t rgba = 0;
            if (valid) memcpy(&rgba, (void *)(uintptr_t)(p->storage + byte), 4);
            static uint64_t count;
            ++count;
            RqCpuTexel q = {.addr = p->storage + off, .x = x, .y = y, .components = 1, .component_bytes = 4};
#ifndef __APPLE__
            if (valid && (paint_feedback || count <= 64 || !(count % 2000))) {
                if (rq_enabled()) rq_wait(rq_push(RQ_CPU_TEXEL, &q, 0));
                else { extern void renderer_cpu_texel(RqCpuTexel *); renderer_cpu_texel(&q); }
            }
#endif
            if (paint_feedback && q.valid) memcpy((void *)(uintptr_t)(p->storage + byte), &q.value, 4);
            if (paint_feedback && count == 1) LOG("[paint-sync] texel readback enabled; first sample valid %d\n", q.valid);
            if (paint_trace && (count <= 64 || !(count % 2000)))
                LOG("[paint-map] #%llu storage %llx size %llx offset %llx xy %u %u pitch %u byte %lld cpu %08x valid %d gpu %08x gpu-valid %d applied %d\n",
                    (unsigned long long)count, (unsigned long long)p->storage, (unsigned long long)p->size,
                    (unsigned long long)off, x, y, pitch, (long long)byte, rgba, valid, q.value, q.valid, paint_feedback && q.valid);
            if (paint_trace && count == 1 && off < p->size) {
                extern void nvn_log_overlapping_textures(const NvnTexture *, uint64_t, uint64_t);
                nvn_log_overlapping_textures(NULL, p->storage + off, p->size - off);
            }
        }
    }
    // The lifted game's direct calls bypass dispatch overrides. Observe the NVN
    // boundary instead, where the F16 reader has already calculated its row pitch.
    static int trace = -1;
    if (trace < 0) trace = getenv("ODYSSEY_LOG_LINREAD") != NULL;
    static uint64_t count;
    static int feedback = -1;
    // On by default (the reactive Cap Kingdom fog needs it); ODYSSEY_FOG_CPU_READBACK=0 switches it off.
    if (feedback < 0) { const char *e = getenv("ODYSSEY_FOG_CPU_READBACK"); feedback = !e || atoi(e) != 0; }
    if ((trace || feedback) && fog_f16_map_reader(c->x[30])) {
        uint32_t nc = c->x[19], comp = c->x[20], y = c->x[21], x = c->x[22];
        uint64_t off = (uint32_t)c->x[23];
        // Match the reader's 32-bit madd/mul before its signed halfword indexing.
        int64_t index = (int32_t)(((uint32_t)c->x[24] * y + x) * nc);
        int64_t byte = (int64_t)off + index * 2 + (int32_t)comp * 2;
        uint16_t value = 0;
        bool valid = byte >= 0 && (uint64_t)byte < p->size && p->size - (uint64_t)byte >= 2;
#ifndef __APPLE__
        if (feedback && valid) {
            RqCpuTexel q = {.addr = p->storage + off, .x = x, .y = y, .component = comp, .components = nc};
            if (rq_enabled() && !rq_on_thread()) rq_wait(rq_push(RQ_CPU_TEXEL, &q, 0));
            else { extern void renderer_cpu_texel(RqCpuTexel *); renderer_cpu_texel(&q); }
            if (q.valid) memcpy((void *)(uintptr_t)(p->storage + byte), &q.value, 2);
        }
#endif
        if (valid) memcpy(&value, (void *)(uintptr_t)(p->storage + byte), 2);
        ++count;
        if (trace && (count <= 64 || !(count % 2000)))
        LOG("[linread-map] #%llu storage %llx size %llx offset %llx xy %u %u nc %u comp %u row-elements %u byte %lld half %04x valid %d\n",
            (unsigned long long)count, (unsigned long long)p->storage, (unsigned long long)p->size,
            (unsigned long long)off, x, y, nc, comp, (unsigned)c->x[24], (long long)byte, value, valid);
    }
    RET(p->storage);
}
NVN(nvnMemoryPoolGetBufferAddress) { RET(HOST(NvnPool, A(0))->storage); }  // GPU address == CPU address
NVN(nvnMemoryPoolGetSize) { RET(HOST(NvnPool, A(0))->size); }
NVN(nvnMemoryPoolGetFlags) { RET(HOST(NvnPool, A(0))->flags); }
NVN(nvnMemoryPoolFlushMappedRange) {
    NvnPool *p = HOST(NvnPool, A(0));
#ifndef __APPLE__
    if (p->paint_cpu_shared && A(1) <= p->size && A(2) <= p->size - A(1)) {
        RqPaintWrite w = {p->storage + A(1), A(2)};
        if (rq_enabled() && !rq_on_thread()) rq_wait(rq_push(RQ_PAINT_WRITE, &w, 0));
        else { extern void renderer_paint_cpu_written(uint64_t, uint64_t); renderer_paint_cpu_written(w.addr, w.size); }
    }
#endif
    backend_pool_written(p->storage + A(1), A(2));
}
NVN(nvnMemoryPoolInvalidateMappedRange) { (void)c; }
// Virtual pools (Odyssey: one of 100 MB for render targets, mapped once at boot) have no storage of their own; a
// request maps part of a physical pool in (NVNmappingRequest: physicalPool@0 physicalOffset@8 virtualOffset@0x10
// size@0x18 storageClass@0x20, 40 bytes). Addresses here are plain guest memory, so a virtual pool becomes an alias of
// the physical memory behind it: storage = physical storage + physicalOffset - virtualOffset. That holds while all
// requests of a pool agree on that difference (one linear mapping); others are only logged. Before this, textures
// in the pool had their offset as address (0xc98800): the first CPU upload of one crashed.
NVN(nvnMemoryPoolMapVirtual) {  // (pool, numRequests, requests)
    NvnPool *p = HOST(NvnPool, A(0));
    int n = (int)A(1);
    static int logged;
    for (int i = 0; i < n; i++) {
        uint64_t r = A(2) + (uint64_t)i * 40, phys = G(uint64_t, r, 0), size = G(uint64_t, r, 0x18);
        int64_t poff = G(int64_t, r, 8), voff = G(int64_t, r, 0x10);
        NvnPool *pp = phys ? HOST(NvnPool, phys) : NULL;
        uint64_t base = pp && pp->storage ? pp->storage + poff - voff : 0;
        if (logged++ < 20)
            LOG("[nvn] MapVirtual pool %llx: virtual %#llx..%#llx -> pool %llx (storage %llx) + %#llx, class %#x\n",
                (unsigned long long)A(0), (unsigned long long)voff, (unsigned long long)(voff + size),
                (unsigned long long)phys, (unsigned long long)(pp ? pp->storage : 0), (unsigned long long)poff,
                G(uint32_t, r, 0x20));
        if (!p || !base) continue;  // (unmapping: physicalPool NULL)
        if (!p->storage) p->storage = base;
        else if (p->storage != base)
            LOG("[nvn] MapVirtual pool %llx: request %d maps at another offset (storage %llx, wants %llx): ignored\n",
                (unsigned long long)A(0), i, (unsigned long long)p->storage, (unsigned long long)base);
    }
    RET(1);
}

// ================================================================== buffers
// Builder: device@0 pool@8 offset@0x10 size@0x18. Buffer: address@0 pool@8 offset@0x10 size@0x18 id@0x20
NVN(nvnBufferBuilderSetDevice) { G(uint64_t, A(0), 0) = A(1); }
NVN(nvnBufferBuilderSetDefaults) { memset(AP(0) + 8, 0, 0x18); }
NVN(nvnBufferBuilderSetStorage) {
    G(uint64_t, A(0), 8) = A(1); G(uint64_t, A(0), 0x10) = A(2); G(uint64_t, A(0), 0x18) = A(3);
}
NVN(nvnBufferBuilderGetDevice) { RET(G(uint64_t, A(0), 0)); }
NVN(nvnBufferBuilderGetMemoryPool) { RET(G(uint64_t, A(0), 8)); }
NVN(nvnBufferBuilderGetMemoryOffset) { RET(G(uint64_t, A(0), 0x10)); }
NVN(nvnBufferBuilderGetSize) { RET(G(uint64_t, A(0), 0x18)); }
NVN(nvnBufferInitialize) {
    uint64_t pool = G(uint64_t, A(1), 8), off = G(uint64_t, A(1), 0x10);
    G(uint64_t, A(0), 0) = HOST(NvnPool, pool)->storage + off;
    G(uint64_t, A(0), 8) = pool;
    G(uint64_t, A(0), 0x10) = off;
    G(uint64_t, A(0), 0x18) = G(uint64_t, A(1), 0x18);
    G(uint64_t, A(0), 0x20) = next_debug_id++;
    RET(1);
}
NVN(nvnBufferFinalize) { (void)c; }
NVN(nvnBufferSetDebugLabel) { (void)c; }
NVN(nvnBufferMap) { RET(G(uint64_t, A(0), 0)); }
NVN(nvnBufferGetAddress) { RET(G(uint64_t, A(0), 0)); }
NVN(nvnBufferGetMemoryPool) { RET(G(uint64_t, A(0), 8)); }
NVN(nvnBufferGetMemoryOffset) { RET(G(uint64_t, A(0), 0x10)); }
NVN(nvnBufferGetSize) { RET(G(uint64_t, A(0), 0x18)); }
NVN(nvnBufferGetDebugID) { RET(G(uint64_t, A(0), 0x20)); }
NVN(nvnBufferFlushMappedRange) { backend_pool_written(G(uint64_t, A(0), 0) + A(1), A(2)); }
NVN(nvnBufferInvalidateMappedRange) { (void)c; }

// ================================================================== textures
// TextureBuilder: device@0 flags@8 target@0xC width@0x10 height@0x14 depth@0x18 levels@0x1C
//   format@0x20 samples@0x24 swizzle@0x28 dsmode@0x2C stride@0x30 pool@0x38 offset@0x40
//   packaged@0x48 layout@0x50 glname@0x58
#define TB(T, off) G(T, A(0), off)
NVN(nvnTextureBuilderSetDevice) { TB(uint64_t, 0) = A(1); }
NVN(nvnTextureBuilderSetDefaults) {
    memset(AP(0) + 8, 0, 0x58);
    TB(uint32_t, 0xC) = 1;  // 2D
    TB(uint32_t, 0x10) = TB(uint32_t, 0x14) = TB(uint32_t, 0x18) = 1;
    TB(uint32_t, 0x1C) = 1;
    TB(uint8_t, 0x28) = 2; TB(uint8_t, 0x29) = 3; TB(uint8_t, 0x2A) = 4; TB(uint8_t, 0x2B) = 5;
}
NVN(nvnTextureBuilderSetFlags) { TB(uint32_t, 8) = (uint32_t)A(1); }
NVN(nvnTextureBuilderSetTarget) { TB(uint32_t, 0xC) = (uint32_t)A(1); }
NVN(nvnTextureBuilderSetWidth) { TB(uint32_t, 0x10) = (uint32_t)A(1); }
NVN(nvnTextureBuilderSetHeight) { TB(uint32_t, 0x14) = (uint32_t)A(1); }
NVN(nvnTextureBuilderSetDepth) { TB(uint32_t, 0x18) = (uint32_t)A(1); }
NVN(nvnTextureBuilderSetSize1D) { TB(uint32_t, 0x10) = (uint32_t)A(1); TB(uint32_t, 0x14) = 1; TB(uint32_t, 0x18) = 1; }
NVN(nvnTextureBuilderSetSize2D) { TB(uint32_t, 0x10) = (uint32_t)A(1); TB(uint32_t, 0x14) = (uint32_t)A(2); TB(uint32_t, 0x18) = 1; }
NVN(nvnTextureBuilderSetSize3D) {
    TB(uint32_t, 0x10) = (uint32_t)A(1); TB(uint32_t, 0x14) = (uint32_t)A(2); TB(uint32_t, 0x18) = (uint32_t)A(3);
}
NVN(nvnTextureBuilderSetLevels) { TB(uint32_t, 0x1C) = (uint32_t)A(1); }
NVN(nvnTextureBuilderSetFormat) { TB(uint32_t, 0x20) = (uint32_t)A(1); }
NVN(nvnTextureBuilderSetSamples) { TB(uint32_t, 0x24) = (uint32_t)A(1); }
NVN(nvnTextureBuilderSetSwizzle) {
    TB(uint8_t, 0x28) = (uint8_t)A(1); TB(uint8_t, 0x29) = (uint8_t)A(2);
    TB(uint8_t, 0x2A) = (uint8_t)A(3); TB(uint8_t, 0x2B) = (uint8_t)A(4);
}
NVN(nvnTextureBuilderSetDepthStencilMode) { TB(uint32_t, 0x2C) = (uint32_t)A(1); }
NVN(nvnTextureBuilderSetStride) { TB(uint64_t, 0x30) = A(1); }
NVN(nvnTextureBuilderSetStorage) { TB(uint64_t, 0x38) = A(1); TB(uint64_t, 0x40) = A(2); }
NVN(nvnTextureBuilderSetPackagedTextureData) { TB(uint64_t, 0x48) = A(1); }
NVN(nvnTextureBuilderSetPackagedTextureLayout) { TB(uint64_t, 0x50) = A(1) ? *(uint64_t *)AP(1) : 0; }
NVN(nvnTextureBuilderSetGLTextureName) { TB(uint32_t, 0x58) = (uint32_t)A(1); }
NVN(nvnTextureBuilderGetDevice) { RET(TB(uint64_t, 0)); }
NVN(nvnTextureBuilderGetFlags) { RET(TB(uint32_t, 8)); }
NVN(nvnTextureBuilderGetTarget) { RET(TB(uint32_t, 0xC)); }
NVN(nvnTextureBuilderGetWidth) { RET(TB(uint32_t, 0x10)); }
NVN(nvnTextureBuilderGetHeight) { RET(TB(uint32_t, 0x14)); }
NVN(nvnTextureBuilderGetDepth) { RET(TB(uint32_t, 0x18)); }
NVN(nvnTextureBuilderGetLevels) { RET(TB(uint32_t, 0x1C)); }
NVN(nvnTextureBuilderGetFormat) { RET(TB(uint32_t, 0x20)); }
NVN(nvnTextureBuilderGetSamples) { RET(TB(uint32_t, 0x24)); }
NVN(nvnTextureBuilderGetSwizzle) {
    for (int i = 0; i < 4; i++) *(uint32_t *)AP(1 + i) = TB(uint8_t, 0x28 + i);
}
NVN(nvnTextureBuilderGetDepthStencilMode) { RET(TB(uint32_t, 0x2C)); }
NVN(nvnTextureBuilderGetStride) { RET(TB(uint64_t, 0x30)); }
NVN(nvnTextureBuilderGetMemoryPool) { RET(TB(uint64_t, 0x38)); }
NVN(nvnTextureBuilderGetMemoryOffset) { RET(TB(uint64_t, 0x40)); }
NVN(nvnTextureBuilderGetPackagedTextureData) { RET(TB(uint64_t, 0x48)); }
NVN(nvnTextureBuilderGetPackagedTextureLayout) { RET(0); }
NVN(nvnTextureBuilderGetGLTextureName) { RET(TB(uint32_t, 0x58)); }
NVN(nvnTextureBuilderGetSparseTileLayout) { RET(0); }
NVN(nvnTextureBuilderGetZCullStorageSize) { RET(0); }
NVN(nvnTextureBuilderGetStorageClass) { RET(0); }
NVN(nvnTextureBuilderGetRawStorageClass) { RET(0); }

static bool target_is_array(uint32_t t) { return t == 3 || t == 4 || t == 6 || t == 8 || t == 9; }

static void builder_layout(uint64_t b, TexLayout *t) {
    uint32_t target = G(uint32_t, b, 0xC);
    t->fmt = nvn_format_info(G(uint32_t, b, 0x20));
    t->width = G(uint32_t, b, 0x10);
    t->height = target == 0 || target == 3 ? 1 : G(uint32_t, b, 0x14);
    if (target == 3) {  // 1D array: height is the layer count
        t->layers = G(uint32_t, b, 0x14);
    }
    t->depth = target == 2 ? G(uint32_t, b, 0x18) : 1;
    if (target_is_array(target) && target != 3) t->layers = G(uint32_t, b, 0x18);
    if (target == 8) t->layers = 6;
    if (!t->layers) t->layers = 1;
    t->levels = G(uint32_t, b, 0x1C) ? G(uint32_t, b, 0x1C) : 1;
    tegra_layout_init(t);
    // Packaged textures (BNTX) were swizzled offline with their own block height, which can differ
    // from the driver default: NVNpackagedTextureLayout's low word is log2(GOBs per block).
    uint64_t pl = G(uint64_t, b, 0x50);
    if (pl && (uint32_t)pl <= 5) t->block_height_log2 = (uint32_t)pl;
    if (pl && getenv("ODYSSEY_LOG_PACKAGED")) {
        static int n;
        if (n++ < 4000) LOG("[nvn] packaged layout %016llx fmt %#x %ux%u lv %u -> default bh %u\n", pl, G(uint32_t, b, 0x20), t->width, t->height, t->levels, t->block_height_log2);
    }
}

static bool is_linear(uint64_t b) { return nvn_flags_linear(G(uint32_t, b, 8)) || G(uint32_t, b, 0xC) == 10; }

static uint64_t builder_pitch(uint64_t b, const TexLayout *t) {
    uint64_t row = ((uint64_t)t->width + t->fmt->bw - 1) / t->fmt->bw * t->fmt->bpb;
    return nvn_linear_pitch(G(uint32_t, b, 8), row, G(uint64_t, b, 0x30));
}

static uint64_t builder_storage_size(uint64_t b) {
    if (G(uint32_t, b, 0xC) == 10) {  // buffer texture
        return (uint64_t)G(uint32_t, b, 0x10) * nvn_format_info(G(uint32_t, b, 0x20))->bpb;
    }
    TexLayout t = {0};
    builder_layout(b, &t);
    if (is_linear(b)) return builder_pitch(b, &t) * t.height;
    return tegra_total_size(&t);
}
NVN(nvnTextureBuilderGetStorageSize) { RET(builder_storage_size(A(0))); }
NVN(nvnTextureBuilderGetStorageAlignment) { RET(is_linear(A(0)) ? nvn_linear_alignment(G(uint32_t, A(0), 8)) : 512); }

static NvnTexture *texture_from_builder(uint64_t b) {
    NvnTexture *t = calloc(1, sizeof *t);
    t->target = G(uint32_t, b, 0xC);
    t->flags = G(uint32_t, b, 8);
    t->width = G(uint32_t, b, 0x10);
    t->height = G(uint32_t, b, 0x14);
    t->depth = G(uint32_t, b, 0x18);
    t->levels = G(uint32_t, b, 0x1C) ? G(uint32_t, b, 0x1C) : 1;
    t->format = G(uint32_t, b, 0x20);
    t->samples = G(uint32_t, b, 0x24);
    memcpy(t->swizzle, (void *)(uintptr_t)(b + 0x28), 4);
    t->ds_mode = G(uint32_t, b, 0x2C);
    t->stride = G(uint64_t, b, 0x30);
    uint64_t pool = G(uint64_t, b, 0x38);
    t->pool = pool ? HOST(NvnPool, pool) : NULL;
    t->pool_offset = G(uint64_t, b, 0x40);
    t->addr = t->pool ? t->pool->storage + t->pool_offset : 0;
    builder_layout(b, &t->layout);
    if (nvn_flags_linear(t->flags)) t->stride = builder_pitch(b, &t->layout);
    t->debug_id = next_debug_id++;
    return t;
}

// Host textures alive (nvnTextureInitialize .. nvnTextureFinalize). Their pointer sits in the guest object's first word,
// and the game copies texture objects (Odyssey's agl: TextureData by value) - harmless with the real driver, whose
// texture object is plain data, but every copy here points at the same host texture. Finalizing a copy used to free
// it under the others (use after free, a double free that corrupted the heap). So a host texture is never freed:
// finalizing any copy releases its GPU resources (made again if another copy is drawn with), and pointers that aren't
// ours (stale or garbage) are left alone. (agl initializes most textures in a temporary on the stack and finalizes the
// long-lived copy: releasing only on the original's finalize leaked ~50 images a frame, VRAM over budget, device lost.)
static pthread_mutex_t g_tex_lock = PTHREAD_MUTEX_INITIALIZER;
static NvnTexture **g_tex_set;  // every host texture (open addressing; they are never freed)
static size_t g_tex_cap, g_tex_used;

static size_t tex_slot(NvnTexture *t) { return (size_t)(((uintptr_t)t >> 4) * 0x9E3779B97F4A7C15ull) & (g_tex_cap - 1); }

static void tex_set_add(NvnTexture *t) {
    pthread_mutex_lock(&g_tex_lock);
    if ((g_tex_used + 1) * 2 > g_tex_cap) {  // grow
        NvnTexture **old = g_tex_set;
        size_t ocap = g_tex_cap;
        g_tex_cap = g_tex_cap ? g_tex_cap * 2 : 4096;
        g_tex_set = calloc(g_tex_cap, sizeof *g_tex_set);
        g_tex_used = 0;
        for (size_t i = 0; i < ocap; i++)
            if (old[i]) {
                size_t h = tex_slot(old[i]);
                while (g_tex_set[h]) h = (h + 1) & (g_tex_cap - 1);
                g_tex_set[h] = old[i];
                g_tex_used++;
            }
        free(old);
    }
    size_t h = tex_slot(t);
    while (g_tex_set[h]) h = (h + 1) & (g_tex_cap - 1);
    g_tex_set[h] = t;
    g_tex_used++;
    pthread_mutex_unlock(&g_tex_lock);
}

static bool tex_set_has(NvnTexture *t) {
    bool found = false;
    pthread_mutex_lock(&g_tex_lock);
    if (g_tex_cap)
        for (size_t h = tex_slot(t); g_tex_set[h]; h = (h + 1) & (g_tex_cap - 1))
            if (g_tex_set[h] == t) { found = true; break; }
    pthread_mutex_unlock(&g_tex_lock);
    return found;
}

// Host textures by description. agl initializes most textures anew every frame (texture objects on the stack, the
// same render targets: ~1700 initializations a second); each was a new host texture, never freed (copies may still
// point to it: see above), with its own GPU image made and released every frame. An initialization with the
// description of an existing host texture (texel address, format, size, levels, layout, swizzle...: the bytes from
// layout to addr) now gets that one back, GPU image and contents included, as the same memory under a new texture
// object has them on the Switch. GPU resources go TEX_RELEASE_FRAMES presents after the finalizations caught up with
// the initializations, unless it is initialized again before (agl: initialized in a temporary, finalized as a copy).
#define TEX_DESC_END (offsetof(NvnTexture, addr) + sizeof(uint64_t))
#define TEX_RELEASE_FRAMES 4
#define TEX_DESC_BUCKETS 65536
static NvnTexture *g_tex_desc[TEX_DESC_BUCKETS];
static NvnTexture **g_tex_release;  // queued for release (live <= 0)
static size_t g_tex_nrelease, g_tex_cap_release;
static uint64_t g_presents;

static uint64_t tex_desc_hash(const NvnTexture *t) {
    const uint8_t *p = (const uint8_t *)t;
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < TEX_DESC_END; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

// The host texture with n's description (n freed), or n itself, new. One more initialization of it.
static NvnTexture *tex_by_desc(NvnTexture *n) {
    static int off = -1;  // ODYSSEY_NO_TEX_DEDUP=1 (debug): every initialization its own host texture, as before
    if (off < 0) off = getenv("ODYSSEY_NO_TEX_DEDUP") != NULL;
    if (off) { n->live = 1; tex_set_add(n); return n; }
    uint64_t h = tex_desc_hash(n);
    pthread_mutex_lock(&g_tex_lock);
    NvnTexture **head = &g_tex_desc[h & (TEX_DESC_BUCKETS - 1)], *t = *head;
    while (t && !(t->desc_hash == h && !memcmp(t, n, TEX_DESC_END))) t = t->desc_next;
    if (!t) {
        t = n;
        t->desc_hash = h;
        t->desc_next = *head;
        *head = t;
    }
    t->live++;
    t->release_at = 0;
    pthread_mutex_unlock(&g_tex_lock);
    if (t != n) free(n);
    else tex_set_add(t);
    return t;
}

static void tex_finalized(NvnTexture *t) {
    pthread_mutex_lock(&g_tex_lock);
    if (--t->live <= 0) {
        t->live = 0;
        t->release_at = g_presents + TEX_RELEASE_FRAMES;
        if (!t->queued) {
            if (g_tex_nrelease == g_tex_cap_release) {
                g_tex_cap_release = g_tex_cap_release ? g_tex_cap_release * 2 : 256;
                g_tex_release = realloc(g_tex_release, g_tex_cap_release * sizeof *g_tex_release);
            }
            g_tex_release[g_tex_nrelease++] = t;
            t->queued = true;
        }
    }
    pthread_mutex_unlock(&g_tex_lock);
}

// Per present: GPU resources of the textures whose time came. (Released outside g_tex_lock: the renderer takes its
// lock first and then this one, debug logging. A texture initialized again meanwhile just gets a new image.)
static void tex_process_releases(void) {
    NvnTexture *due[256];
    int n = 0;
    pthread_mutex_lock(&g_tex_lock);
    g_presents++;
    size_t k = 0;
    for (size_t i = 0; i < g_tex_nrelease; i++) {
        NvnTexture *t = g_tex_release[i];
        if (t->live > 0 || !t->release_at) { t->queued = false; continue; }  // (initialized again)
        if (g_presents >= t->release_at && n < 256) { due[n++] = t; t->queued = false; t->release_at = 0; continue; }
        g_tex_release[k++] = t;
    }
    g_tex_nrelease = k;
    pthread_mutex_unlock(&g_tex_lock);
    for (int i = 0; i < n; i++) backend_texture_destroyed(due[i]);
}

// (debug, ODYSSEY_TEX_TRACE) the most recently initialized host texture with that storage address
NvnTexture *tex_trace_find(uint64_t addr) {
    NvnTexture *best = NULL;
    pthread_mutex_lock(&g_tex_lock);
    for (size_t i = 0; i < g_tex_cap; i++) {
        NvnTexture *t = g_tex_set[i];
        if (t && t->addr == addr && t->backend && (!best || t->debug_id > best->debug_id)) best = t;
    }
    pthread_mutex_unlock(&g_tex_lock);
    return best;
}

// (debug) every live texture whose storage overlaps [addr, addr + size), logged
void nvn_log_overlapping_textures(const NvnTexture *self, uint64_t addr, uint64_t size) {
    pthread_mutex_lock(&g_tex_lock);
    for (size_t i = 0; i < g_tex_cap; i++) {
        NvnTexture *t = g_tex_set[i];
        if (!t || t == self || !t->addr || !t->guest_obj) continue;
        uint64_t sz = tegra_total_size(&t->layout);
        if (t->addr < addr + size && addr < t->addr + sz)
            LOG("[nvn]   overlaps %p: %ux%ux%u fmt %#x target %u levels %u addr %llx size %#llx flags %#x '%s'\n", (void *)t,
                t->width, t->height, t->depth, t->format, t->target, t->levels, (unsigned long long)t->addr,
                (unsigned long long)sz, t->flags, t->label);
    }
    pthread_mutex_unlock(&g_tex_lock);
}

NVN(nvnTextureInitialize) {
    if (getenv("ODYSSEY_LOG_TEXLIFE")) {  // (debug) an object initialized again while it still holds a live texture
        NvnTexture *old = HOST(NvnTexture, A(0));
        static uint64_t reinit, total;
        total++;
        if (old && tex_set_has(old) && old->guest_obj == A(0) && reinit++ < 20)
            LOG("[nvn] texture object %llx initialized again (old %p %ux%u fmt %#x) lr %llx\n", (unsigned long long)A(0), (void *)old,
                old->width, old->height, old->format, (unsigned long long)c->x[30]);
        if ((total & 4095) == 0) LOG("[nvn] %llu texture inits, %llu over live ones\n", (unsigned long long)total, (unsigned long long)reinit);
    }
    NvnTexture *t = tex_by_desc(texture_from_builder(A(1)));
    t->guest_obj = A(0);
    if (t->flags & 2)
        LOG("[nvn] video texture %p: %ux%u fmt %#x flags %#x target %u addr %llx stride %llu\n", t, t->width, t->height,
            t->format, t->flags, t->target, t->addr, t->stride);
    if (t->target == 2 && getenv("ODYSSEY_LOG_TEXELS")) {  // (3D textures: color grading maps)
        static int n;
        LOG("[nvn] 3D texture %p: %ux%ux%u fmt %#x flags %#x addr %llx (guest %llx, lr %llx)\n", (void *)t, t->width, t->height, t->depth,
            t->format, t->flags, (unsigned long long)t->addr, (unsigned long long)A(0), (unsigned long long)c->x[30]);
        if (n++ < 400) rt_print_guest_chain(c);
    }
    SET_HOST(A(0), t);
    RET(1);
}
NVN(nvnTextureFinalize) {
    NvnTexture *t = HOST(NvnTexture, A(0));
    if (getenv("ODYSSEY_LOG_TEXLIFE")) {
        static uint64_t fin, copies;
        fin++;
        if (t && tex_set_has(t) && t->guest_obj != A(0) && copies++ < 20)
            LOG("[nvn] finalize of a copy %llx (texture %p made in %llx, %ux%u fmt %#x) lr %llx\n", (unsigned long long)A(0), (void *)t,
                (unsigned long long)t->guest_obj, t->width, t->height, t->format, (unsigned long long)c->x[30]);
        if ((fin & 4095) == 0) LOG("[nvn] %llu texture finalizes, %llu of copies\n", (unsigned long long)fin, (unsigned long long)copies);
    }
    if (t && tex_set_has(t)) {
        tex_finalized(t);  // (GPU resources a few presents later; the host texture stays for copies: see above)
        if (t->guest_obj == A(0)) t->guest_obj = 0;
    } else if (t) {
        static int nlog;
        if (nlog++ < 8) LOG("[nvn] finalize of a texture object that isn't one (guest %llx, host %p): left alone\n",
                            (unsigned long long)A(0), (void *)t);
    }
    SET_HOST(A(0), 0);
}
NVN(nvnTextureSetDebugLabel) {
    NvnTexture *t = HOST(NvnTexture, A(0));
    if (t && A(1)) strncpy(t->label, (const char *)AP(1), sizeof t->label - 1);
}
#define TEX HOST(NvnTexture, A(0))
NVN(nvnTextureGetFlags) { RET(TEX->flags); }
NVN(nvnTextureGetTarget) { RET(TEX->target); }
NVN(nvnTextureGetWidth) { RET(TEX->width); }
NVN(nvnTextureGetHeight) { RET(TEX->height); }
NVN(nvnTextureGetDepth) { RET(TEX->depth); }
NVN(nvnTextureGetLevels) { RET(TEX->levels); }
NVN(nvnTextureGetFormat) { RET(TEX->format); }
NVN(nvnTextureGetSamples) { RET(TEX->samples); }
NVN(nvnTextureGetSwizzle) { for (int i = 0; i < 4; i++) *(uint32_t *)AP(1 + i) = TEX->swizzle[i]; }
NVN(nvnTextureGetDepthStencilMode) { RET(TEX->ds_mode); }
NVN(nvnTextureGetStride) { RET(TEX->stride); }
NVN(nvnTextureGetTextureAddress) { RET(TEX->addr); }
NVN(nvnTextureGetMemoryPool) { RET(0); }
NVN(nvnTextureGetMemoryOffset) { RET(TEX->pool_offset); }
NVN(nvnTextureGetStorageSize) { RET(nvn_flags_linear(TEX->flags) ? TEX->stride * TEX->height : tegra_total_size(&TEX->layout)); }
NVN(nvnTextureGetDebugID) { RET(TEX->debug_id); }
NVN(nvnTextureGetViewOffset) { RET(0); }
NVN(nvnTextureGetStorageClass) { RET(0); }
NVN(nvnTextureGetRawStorageClass) { RET(0); }
NVN(nvnTextureGetSparseTileLayout) { RET(0); }
NVN(nvnTextureGetZCullStorageSize) { RET(0); }
NVN(nvnTextureCompare) { RET(A(0) == A(1)); }
NVN(nvnTextureFlushTexels) { (void)c; }
NVN(nvnTextureInvalidateTexels) { (void)c; }

// Texture views (our layout of the 0x28-byte guest struct)
#define TV(T, off) G(T, A(0), off)
NVN(nvnTextureViewSetDefaults) { memset(AP(0), 0, 0x28); }
NVN(nvnTextureViewSetLevels) { TV(uint32_t, 0) |= 1; TV(uint16_t, 4) = (uint16_t)A(1); TV(uint16_t, 6) = (uint16_t)A(2); }
NVN(nvnTextureViewSetLayers) { TV(uint32_t, 0) |= 2; TV(uint16_t, 8) = (uint16_t)A(1); TV(uint16_t, 10) = (uint16_t)A(2); }
NVN(nvnTextureViewSetFormat) { TV(uint32_t, 0) |= 4; TV(uint32_t, 12) = (uint32_t)A(1); }
NVN(nvnTextureViewSetSwizzle) {
    NvnTextureView *v = AP(0);
    v->setmask |= 8;
    v->swizzle[0] = (uint8_t)A(1); v->swizzle[1] = (uint8_t)A(2); v->swizzle[2] = (uint8_t)A(3); v->swizzle[3] = (uint8_t)A(4);
}
NVN(nvnTextureViewSetDepthStencilMode) { NvnTextureView *v = AP(0); v->setmask |= 16; v->ds_mode = (uint32_t)A(1); }
NVN(nvnTextureViewSetTarget) { NvnTextureView *v = AP(0); v->setmask |= 32; v->target = (uint32_t)A(1); }
NVN(nvnTextureViewGetLevels) { *(int32_t *)AP(1) = TV(uint16_t, 4); *(int32_t *)AP(2) = TV(uint16_t, 6); RET(TV(uint32_t, 0) & 1); }
NVN(nvnTextureViewGetLayers) { *(int32_t *)AP(1) = TV(uint16_t, 8); *(int32_t *)AP(2) = TV(uint16_t, 10); RET(!!(TV(uint32_t, 0) & 2)); }
NVN(nvnTextureViewGetFormat) { *(uint32_t *)AP(1) = TV(uint32_t, 12); RET(!!(TV(uint32_t, 0) & 4)); }
NVN(nvnTextureViewGetSwizzle) { NvnTextureView *v = AP(0); for (int i = 0; i < 4; i++) *(uint32_t *)AP(1 + i) = v->swizzle[i]; RET(!!(v->setmask & 8)); }
NVN(nvnTextureViewGetDepthStencilMode) { NvnTextureView *v = AP(0); *(uint32_t *)AP(1) = v->ds_mode; RET(!!(v->setmask & 16)); }
NVN(nvnTextureViewGetTarget) { NvnTextureView *v = AP(0); *(uint32_t *)AP(1) = v->target; RET(!!(v->setmask & 32)); }
NVN(nvnTextureViewCompare) { RET(memcmp(AP(0), AP(1), 0x1C) == 0); }

NVN(nvnTextureWriteTexels) {
    // (texture, view, region, data) -- linear data into block-linear storage
    NvnTexture *t = HOST(NvnTexture, A(0));
    const int32_t *r = AP(2);  // xoffset, yoffset, zoffset, width, height, depth
    LevelLayout l = tegra_level(&t->layout, 0);
    if (getenv("ODYSSEY_LOG_TEXELS")) {
        const uint32_t *d = AP(3);
        LOG("[nvn] WriteTexels %p %ux%ux%u fmt %#x target %u addr %llx view %llx region %d,%d,%d %dx%dx%d data %08x %08x %08x %08x\n",
            (void *)t, t->width, t->height, t->depth, t->format, t->target, (unsigned long long)t->addr, (unsigned long long)A(1),
            r ? r[0] : -1, r ? r[1] : -1, r ? r[2] : -1, r ? r[3] : -1, r ? r[4] : -1, r ? r[5] : -1, d ? d[0] : 0, d ? d[1] : 0,
            d ? d[9] : 0, d ? d[100] : 0);
    }
    if (r && r[0] == 0 && r[1] == 0 && (uint32_t)r[3] == t->width && (uint32_t)r[4] == t->height) {
        tegra_swizzle(AP(3), (uint8_t *)(uintptr_t)t->addr + l.offset, l.wb, l.hb, r[5] > 0 ? (uint32_t)r[5] : 1,
                      t->layout.fmt->bpb, l.block_height_log2, l.block_depth_log2);
        backend_pool_written(t->addr, tegra_total_size(&t->layout));
    } else {
        LOG("[nvn] partial WriteTexels not implemented\n");
    }
}
NVN(nvnTextureWriteTexelsStrided) { LOG("[nvn] WriteTexelsStrided not implemented\n"); (void)c; }
NVN(nvnTextureReadTexels) { LOG("[nvn] ReadTexels not implemented\n"); (void)c; }
NVN(nvnTextureReadTexelsStrided) { (void)c; }

// ================================================================== samplers
// SamplerBuilder: device@0 then an NvnSamplerDesc at 8
#define SD ((NvnSamplerDesc *)(AP(0) + 8))
NVN(nvnSamplerBuilderSetDevice) { G(uint64_t, A(0), 0) = A(1); }
NVN(nvnSamplerBuilderSetDefaults) {
    NvnSamplerDesc *d = SD;
    memset(d, 0, sizeof *d);
    d->min_filter = 0; d->mag_filter = 0;
    d->wrap[0] = d->wrap[1] = d->wrap[2] = 1;  // REPEAT
    d->lod_min = 0; d->lod_max = 1000.0f; d->max_aniso = 1.0f;
    d->compare_func = 4;
}
NVN(nvnSamplerBuilderSetMinMagFilter) { SD->min_filter = (uint32_t)A(1); SD->mag_filter = (uint32_t)A(2); }
NVN(nvnSamplerBuilderSetWrapMode) { SD->wrap[0] = (uint32_t)A(1); SD->wrap[1] = (uint32_t)A(2); SD->wrap[2] = (uint32_t)A(3); }
NVN(nvnSamplerBuilderSetLodClamp) { SD->lod_min = AF(0); SD->lod_max = AF(1); }
NVN(nvnSamplerBuilderSetLodBias) { SD->lod_bias = AF(0); }
NVN(nvnSamplerBuilderSetCompare) { SD->compare_mode = (uint32_t)A(1); SD->compare_func = (uint32_t)A(2); }
NVN(nvnSamplerBuilderSetBorderColor) { memcpy(SD->border, AP(1), 16); }
NVN(nvnSamplerBuilderSetBorderColori) { memcpy(SD->border, AP(1), 16); }
NVN(nvnSamplerBuilderSetBorderColorui) { memcpy(SD->border, AP(1), 16); }
NVN(nvnSamplerBuilderSetMaxAnisotropy) { SD->max_aniso = AF(0); }
NVN(nvnSamplerBuilderSetReductionFilter) { SD->reduction = (uint32_t)A(1); }
NVN(nvnSamplerBuilderSetLodSnap) { (void)c; }
NVN(nvnSamplerBuilderGetDevice) { RET(G(uint64_t, A(0), 0)); }
NVN(nvnSamplerBuilderGetMinMagFilter) { *(uint32_t *)AP(1) = SD->min_filter; *(uint32_t *)AP(2) = SD->mag_filter; }
NVN(nvnSamplerBuilderGetWrapMode) { for (int i = 0; i < 3; i++) *(uint32_t *)AP(1 + i) = SD->wrap[i]; }
NVN(nvnSamplerBuilderGetLodClamp) { *(float *)AP(1) = SD->lod_min; *(float *)AP(2) = SD->lod_max; }
NVN(nvnSamplerBuilderGetLodBias) { RETF(SD->lod_bias); }
NVN(nvnSamplerBuilderGetCompare) { *(uint32_t *)AP(1) = SD->compare_mode; *(uint32_t *)AP(2) = SD->compare_func; }
NVN(nvnSamplerBuilderGetBorderColor) { memcpy(AP(1), SD->border, 16); }
NVN(nvnSamplerBuilderGetBorderColori) { memcpy(AP(1), SD->border, 16); }
NVN(nvnSamplerBuilderGetBorderColorui) { memcpy(AP(1), SD->border, 16); }
NVN(nvnSamplerBuilderGetMaxAnisotropy) { RETF(SD->max_aniso); }
NVN(nvnSamplerBuilderGetReductionFilter) { RET(SD->reduction); }
NVN(nvnSamplerBuilderGetLodSnap) { RETF(0); }

// A sampler keeps its NvnSamplerDesc in the guest object (at 8, as the builder), not behind a host pointer: agl copies
// NVNsampler objects (memcpy into its sampler table), hashes their bytes and compares the copies with nvnSamplerCompare.
// A copy's host pointer outlived the original's Finalize; once malloc reused that memory for another sampler, two
// different samplers compared equal and agl handed out the other one's sampler id (the tonemap's 8x8x8 colour map then
// sampled with NEAREST: hard colour steps).
#define NVN_SAMPLER_SIZE 0x60
_Static_assert(8 + sizeof(NvnSamplerDesc) <= NVN_SAMPLER_SIZE, "NvnSamplerDesc must fit in NVNsampler");
NVN(nvnSamplerInitialize) {
    NvnSamplerDesc d = *(NvnSamplerDesc *)(AP(1) + 8);
    d.backend = NULL;
    memset(AP(0), 0, NVN_SAMPLER_SIZE);  // (the rest zero: agl hashes the whole object)
    *(NvnSamplerDesc *)(AP(0) + 8) = d;
    RET(1);
}
NVN(nvnSamplerFinalize) { memset(AP(0), 0, NVN_SAMPLER_SIZE); }
NVN(nvnSamplerSetDebugLabel) { (void)c; }
#define SMP ((NvnSamplerDesc *)(AP(0) + 8))
NVN(nvnSamplerGetMinMagFilter) { *(uint32_t *)AP(1) = SMP->min_filter; *(uint32_t *)AP(2) = SMP->mag_filter; }
NVN(nvnSamplerGetWrapMode) { for (int i = 0; i < 3; i++) *(uint32_t *)AP(1 + i) = SMP->wrap[i]; }
NVN(nvnSamplerGetLodClamp) { *(float *)AP(1) = SMP->lod_min; *(float *)AP(2) = SMP->lod_max; }
NVN(nvnSamplerGetLodBias) { RETF(SMP->lod_bias); }
NVN(nvnSamplerGetCompare) { *(uint32_t *)AP(1) = SMP->compare_mode; *(uint32_t *)AP(2) = SMP->compare_func; }
NVN(nvnSamplerGetBorderColor) { memcpy(AP(1), SMP->border, 16); }
NVN(nvnSamplerGetBorderColori) { memcpy(AP(1), SMP->border, 16); }
NVN(nvnSamplerGetBorderColorui) { memcpy(AP(1), SMP->border, 16); }
NVN(nvnSamplerGetMaxAnisotropy) { RETF(SMP->max_aniso); }
NVN(nvnSamplerGetReductionFilter) { RET(SMP->reduction); }
NVN(nvnSamplerCompare) { RET(memcmp(AP(0) + 8, AP(1) + 8, offsetof(NvnSamplerDesc, backend)) == 0); }
NVN(nvnSamplerGetDebugID) { RET(A(0)); }

// ================================================================== descriptor pools
NvnDescPool *g_texture_pools[64], *g_sampler_pools[64];
RT_ON_RESET(desc_pools_reset) { memset(g_texture_pools, 0, sizeof g_texture_pools); memset(g_sampler_pools, 0, sizeof g_sampler_pools); }

static NvnDescPool *desc_pool_init(uint64_t guest, uint64_t pool, uint64_t off, int count, size_t entry, NvnDescPool **reg,
                                   uint64_t lr) {
    if (getenv("ODYSSEY_LOG_POOLS")) {
        extern uint64_t renderer_present_count(void);
        LOG("[nvn] %s pool %llx initialized: memory %llx+%#llx, %d entries (frame %llu, lr %llx)\n",
            reg == g_sampler_pools ? "sampler" : "texture", (unsigned long long)guest, (unsigned long long)HOST(NvnPool, pool)->storage,
            (unsigned long long)off, count, (unsigned long long)renderer_present_count(), (unsigned long long)lr);
    }
    NvnDescPool *d = calloc(1, sizeof *d);
    d->addr = HOST(NvnPool, pool)->storage + off;
    d->count = count;
    d->entries = calloc(count, entry);
    SET_HOST(guest, d);
    G(uint64_t, guest, 8) = pool;
    G(uint64_t, guest, 16) = off;
    G(uint32_t, guest, 24) = (uint32_t)count;
    for (int i = 0; i < 64; i++) if (!reg[i]) { reg[i] = d; break; }
    return d;
}

NVN(nvnTexturePoolInitialize) {
    desc_pool_init(A(0), A(1), A(2), (int)A(3), sizeof(NvnTexEntry), g_texture_pools, c->x[30]);
    RET(1);
}
NVN(nvnTexturePoolFinalize) { (void)c; }
NVN(nvnTexturePoolSetDebugLabel) { (void)c; }
NVN(nvnTexturePoolGetMemoryPool) { RET(G(uint64_t, A(0), 8)); }
NVN(nvnTexturePoolGetMemoryOffset) { RET(G(uint64_t, A(0), 16)); }
NVN(nvnTexturePoolGetSize) { RET(G(uint32_t, A(0), 24)); }
static void register_texture(Cpu *c) {
    NvnDescPool *d = HOST(NvnDescPool, A(0));
    int id = (int)A(1);
    static int log_pools = -1;
    if (log_pools < 0) log_pools = getenv("ODYSSEY_LOG_POOLS") != NULL;
    static _Atomic unsigned long long nreg;
    if (log_pools && ++nreg % 1000 == 0) {
        extern uint64_t renderer_present_count(void);
        LOG("[nvn] %llu texture registrations (frame %llu, lr %llx)\n", nreg, (unsigned long long)renderer_present_count(),
            (unsigned long long)c->x[30]);
    }
    if (id < 0 || id >= d->count) return;
    NvnTexEntry *e = &((NvnTexEntry *)d->entries)[id];
    e->tex = HOST(NvnTexture, A(2));
    e->has_view = A(3) != 0;
    if (e->has_view) memcpy(&e->view, AP(3), sizeof e->view);
    static int log_swz = -1;  // ODYSSEY_LOG_SWIZZLE=1: the component swizzle of two-channel (BC5, RG8) textures registered
    if (log_swz < 0) log_swz = getenv("ODYSSEY_LOG_SWIZZLE") != NULL;
    if (log_swz && e->tex && (e->tex->format == 0x4B || e->tex->format == 0x4C || e->tex->format == 0x0D)) {
        static int n;
        if (n++ < 300)
            LOG("[swz] id %d tex %p %ux%u f%#x swz %u%u%u%u view %d mask %#x swz %u%u%u%u fmt %#x (lr %llx)\n", id, (void *)e->tex,
                e->tex->width, e->tex->height, e->tex->format, e->tex->swizzle[0], e->tex->swizzle[1], e->tex->swizzle[2],
                e->tex->swizzle[3], e->has_view, e->has_view ? e->view.setmask : 0, e->view.swizzle[0], e->view.swizzle[1],
                e->view.swizzle[2], e->view.swizzle[3], e->has_view ? e->view.format : 0, (unsigned long long)c->x[30]);
    }
}
NVN(nvnTexturePoolRegisterTexture) { register_texture(c); }
NVN(nvnTexturePoolRegisterImage) { register_texture(c); }

NVN(nvnSamplerPoolInitialize) {
    desc_pool_init(A(0), A(1), A(2), (int)A(3), sizeof(NvnSamplerDesc), g_sampler_pools, c->x[30]);
    RET(1);
}
NVN(nvnSamplerPoolFinalize) { (void)c; }
NVN(nvnSamplerPoolSetDebugLabel) { (void)c; }
NVN(nvnSamplerPoolGetMemoryPool) { RET(G(uint64_t, A(0), 8)); }
NVN(nvnSamplerPoolGetMemoryOffset) { RET(G(uint64_t, A(0), 16)); }
NVN(nvnSamplerPoolGetSize) { RET(G(uint32_t, A(0), 24)); }
static void log_sampler_registration(Cpu *c, int id, const NvnSamplerDesc *sd, const char *how) {
    static char ids[256];  // ODYSSEY_LOG_SAMPLER_IDS=<id>[,<id>...] | all: who registers these sampler ids, with what
    static int init;
    if (!init) { init = 1; const char *e = getenv("ODYSSEY_LOG_SAMPLER_IDS"); snprintf(ids, sizeof ids, ",%s,", e ? e : ""); }
    if (ids[1] == ',') return;
    char key[16];
    snprintf(key, sizeof key, ",%d,", id);
    if (strcmp(ids, ",all,") && !strstr(ids, key)) return;
    extern uint64_t renderer_present_count(void);
    KThread *t = kernel_current_thread();
    uint64_t lr2 = c->x[29] ? *(uint64_t *)(uintptr_t)(c->x[29] + 8) : 0;  // (the caller's caller, through the frame chain)
    LOG("[nvn] sampler %d registered (%s, frame %llu, lr %llx < %llx, thread %llu core %d prio %d): min %u mag %u wrap %u %u %u "
        "lod %.2f..%.2f aniso %.0f\n", id, how, (unsigned long long)renderer_present_count(), (unsigned long long)c->x[30],
        (unsigned long long)lr2, t ? (unsigned long long)t->id : 0ull, t ? t->core : -1, t ? t->prio : -1, sd->min_filter,
        sd->mag_filter, sd->wrap[0], sd->wrap[1], sd->wrap[2], sd->lod_min, sd->lod_max, sd->max_aniso);
}
NVN(nvnSamplerPoolRegisterSampler) {
    NvnDescPool *d = HOST(NvnDescPool, A(0));
    int id = (int)A(1);
    if (id >= 0 && id < d->count) ((NvnSamplerDesc *)d->entries)[id] = *(NvnSamplerDesc *)(AP(2) + 8);
    if (id >= 0 && id < d->count) log_sampler_registration(c, id, (NvnSamplerDesc *)(AP(2) + 8), "sampler");
}
NVN(nvnSamplerPoolRegisterSamplerBuilder) {
    NvnDescPool *d = HOST(NvnDescPool, A(0));
    int id = (int)A(1);
    if (id >= 0 && id < d->count) ((NvnSamplerDesc *)d->entries)[id] = *(NvnSamplerDesc *)(AP(2) + 8);
    if (id >= 0 && id < d->count) log_sampler_registration(c, id, (NvnSamplerDesc *)(AP(2) + 8), "builder");
}

NvnDescPool *nvn_find_pool(NvnDescPool **reg, uint64_t guest_pool) {
    return guest_pool ? HOST(NvnDescPool, guest_pool) : NULL;
}

// ================================================================== programs
NVN(nvnProgramInitialize) { SET_HOST(A(0), calloc(1, sizeof(NvnProgram))); RET(1); }
NVN(nvnProgramFinalize) { rq_drain(); interp_free_begin(); free(HOST(NvnProgram, A(0))); SET_HOST(A(0), 0); interp_free_end(); }
NVN(nvnProgramSetDebugLabel) { (void)c; }
NVN(nvnProgramSetSampleShading) { (void)c; }
NVN(nvnProgramSetSubroutineLinkage) { RET(1); }
// Who sets a program up, by the caller of the function calling nvnProgramSetShaders (the frame record x29 points to),
// looked up once per return address. Bulk set-ups - every variation of agl's shader archives at boot
// (agl::ShaderProgram::setUpAllVariation, ~15,000) and every shader of an effect resource as it loads
// (nn::vfx::detail::ShaderManager::Initialize, ~10,000) - are mostly never drawn; a model material's variation
// (nn::g3d::ShaderSelector::UpdateVariation), layout and font shaders and agl's on-first-use ones are, soon.
static bool setup_is_bulk(Cpu *c) {
    static struct { uint64_t ret; bool bulk; } seen[64];
    static int nseen;
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    uint64_t fp = c->x[29];
    if (fp < GUEST_LO || fp >= GUEST_HI || (fp & 7)) return false;
    uint64_t ret = ((uint64_t *)(uintptr_t)fp)[1];
    pthread_mutex_lock(&lock);
    for (int i = 0; i < nseen; i++)
        if (seen[i].ret == ret) { bool b = seen[i].bulk; pthread_mutex_unlock(&lock); return b; }
    uint64_t off;
    const char *s = loader_symbol_at(ret, &off);
    bool bulk = s && (strstr(s, "setUpAllVariation") || strstr(s, "3vfx"));
    if (nseen < 64) { seen[nseen].ret = ret; seen[nseen].bulk = bulk; nseen++; }
    pthread_mutex_unlock(&lock);
    return bulk;
}

static void set_shaders(Cpu *c, int stride) {
    NvnProgram *p = HOST(NvnProgram, A(0));
    int n = (int)A(1);
    const uint8_t *data = AP(2);  // NVNshaderData[n] = {u64 data (GPU addr), const void* control}
    memset(p->stages, 0, sizeof p->stages);
    p->stage_mask = 0;
    p->backend = NULL;
    for (int i = 0; i < n; i++) {
        uint64_t code = *(uint64_t *)(data + i * stride);
        const uint8_t *control = (const uint8_t *)(uintptr_t) * (uint64_t *)(data + i * stride + 8);
        // stage from the Maxwell shader program header (after the 0x30-byte NVN header)
        uint32_t sph0 = *(uint32_t *)(uintptr_t)(code + 0x30);
        uint32_t stage;
        switch ((sph0 >> 10) & 0xF) {
        case 1: stage = 0; break;  // vertex
        case 5: stage = 1; break;  // fragment
        case 4: stage = 2; break;  // geometry
        case 2: stage = 3; break;  // tess control
        case 3: stage = 4; break;  // tess eval
        default: stage = 5; break; // compute (no SPH)
        }
        if (*(uint32_t *)(uintptr_t)code != 0x12345678) stage = 5;
        p->stages[stage].stage = stage;
        p->stages[stage].code_addr = code;
        p->stages[stage].control = control;
        p->stages[stage].const_offset = *(const uint32_t *)(control + 0x700);
        p->stages[stage].const_size = *(const uint32_t *)(control + 0x6FC);
        p->stage_mask |= 1u << stage;
    }
    if ((p->stage_mask & 3) == 3) {
        static int logp = -1;  // (ODYSSEY_LOG_PROGS=1: when programs are set up, against their first draw: exec.c)
        if (logp < 0) logp = getenv("ODYSSEY_LOG_PROGS") != NULL;
        if (logp) {
            extern uint64_t renderer_present_count(void);
            LOG("[prog] set vs=%llx fs=%llx frame %llu\n", (unsigned long long)p->stages[0].code_addr,
                (unsigned long long)p->stages[1].code_addr, (unsigned long long)renderer_present_count());
            if (getenv("ODYSSEY_LOG_PROGS")[0] == '2') {  // =2: and who sets it up (the guest call chain)
                char chain[1024];
                int n = 0;
                uint64_t off, fp = c->x[29], ret = c->x[30];
                for (int i = 0; i < 6 && ret && n < (int)sizeof chain - 200; i++) {
                    const char *s = loader_symbol_at(ret, &off);
                    n += snprintf(chain + n, sizeof chain - n, " < %s", s ? s : "?");
                    if (fp < GUEST_LO || fp >= GUEST_HI || (fp & 7)) break;
                    uint64_t next = ((uint64_t *)(uintptr_t)fp)[0];
                    ret = ((uint64_t *)(uintptr_t)fp)[1];
                    if (next <= fp) break;
                    fp = next;
                }
                LOG("[prog] by%s\n", chain);
            }
        }
        extern void renderer_program_hint(NvnProgram *p, bool bulk);  // (compiled now, ahead of its first draw)
        renderer_program_hint(p, setup_is_bulk(c));
    }
    RET(1);
}

NVN(nvnProgramSetShaders) { set_shaders(c, 16); }    // NVNshaderData {data, control}
NVN(nvnProgramSetShadersExt) { set_shaders(c, 24); } // NVNshaderDataExt {data, control, debug hash}

// ================================================================== window
// WindowBuilder: device@0 native@8 num@0x10 textures@0x18 interval@0x20 active@0x24
NVN(nvnWindowBuilderSetDevice) { G(uint64_t, A(0), 0) = A(1); }
NVN(nvnWindowBuilderSetDefaults) { memset(AP(0) + 8, 0, 0x20); G(uint32_t, A(0), 0x20) = 1; }
NVN(nvnWindowBuilderSetNativeWindow) { G(uint64_t, A(0), 8) = A(1); }
NVN(nvnWindowBuilderSetTextures) { G(uint32_t, A(0), 0x10) = (uint32_t)A(1); G(uint64_t, A(0), 0x18) = A(2); }
NVN(nvnWindowBuilderSetPresentInterval) { G(uint32_t, A(0), 0x20) = (uint32_t)A(1); }
NVN(nvnWindowBuilderSetNumActiveTextures) { G(uint32_t, A(0), 0x24) = (uint32_t)A(1); }
NVN(nvnWindowBuilderGetDevice) { RET(G(uint64_t, A(0), 0)); }
NVN(nvnWindowBuilderGetNativeWindow) { RET(G(uint64_t, A(0), 8)); }
NVN(nvnWindowBuilderGetNumTextures) { RET(G(uint32_t, A(0), 0x10)); }
NVN(nvnWindowBuilderGetTexture) {
    int i = (int)A(1);
    RET(i < (int)G(uint32_t, A(0), 0x10) ? ((uint64_t *)(uintptr_t)G(uint64_t, A(0), 0x18))[i] : 0);
}
NVN(nvnWindowBuilderGetPresentInterval) { RET(G(uint32_t, A(0), 0x20)); }
NVN(nvnWindowBuilderGetNumActiveTextures) { RET(G(uint32_t, A(0), 0x24)); }

NVN(nvnWindowInitialize) {
    NvnWindow *w = calloc(1, sizeof *w);
    w->num = (int)G(uint32_t, A(1), 0x10);
    if (w->num > 8) w->num = 8;
    memcpy(w->textures, (void *)(uintptr_t)G(uint64_t, A(1), 0x18), w->num * 8);
    w->interval = (int)G(uint32_t, A(1), 0x20);
    w->active = G(uint32_t, A(1), 0x24) ? (int)G(uint32_t, A(1), 0x24) : w->num;
    SET_HOST(A(0), w);
    // (the game's last draw of a frame copies its picture into one of these, through an sRGB view: dithered there,
    // vulkan/exec.c draw_ rt_kind 3)
    for (int i = 0; i < w->num; i++) {
        NvnTexture *t = w->textures[i] ? HOST(NvnTexture, w->textures[i]) : NULL;
        if (t) t->window = true;
    }
    LOG("[nvn] window with %d textures\n", w->num);
    RET(1);
}
NVN(nvnWindowFinalize) { (void)c; }
NVN(nvnWindowSetDebugLabel) { (void)c; }
#define WIN HOST(NvnWindow, A(0))
NVN(nvnWindowGetNativeWindow) { RET(0); }
NVN(nvnWindowGetNumTextures) { RET(WIN->num); }
NVN(nvnWindowGetNumActiveTextures) { RET(WIN->active); }
NVN(nvnWindowSetNumActiveTextures) { WIN->active = (int)A(1); }
NVN(nvnWindowGetPresentInterval) { RET(WIN->interval); }
NVN(nvnWindowSetPresentInterval) { WIN->interval = (int)A(1); }
NVN(nvnWindowSetCrop) { int *cr = WIN->crop; cr[0] = (int)A(1); cr[1] = (int)A(2); cr[2] = (int)A(3); cr[3] = (int)A(4); }
NVN(nvnWindowGetCrop) { memcpy(AP(1), WIN->crop, 16); }
NVN(nvnWindowAcquireTexture) {
    NvnWindow *w = WIN;
    if (A(1)) sync_of(A(1))->signaled = 1;
    *(int32_t *)AP(2) = w->next;
    w->next = (w->next + 1) % (w->active ? w->active : 1);
    RET(0);  // NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_SUCCESS
}
NVN(nvnQueueAcquireTexture) {
    NvnWindow *w = HOST(NvnWindow, A(1));
    *(int32_t *)AP(2) = w->next;
    w->next = (w->next + 1) % (w->active ? w->active : 1);
    RET(0);
}
// The window's present interval (Odyssey: 1) paces the game: on the Switch the frame after a present can't start
// before the display took it, so a game runs at 60 fps / interval. Here the present waits for that many vsyncs of
// the 60 Hz display timer (runtime/hle/vi.c) since the last one; a game that already waits for the vsync itself
// (Link's Awakening) finds them passed. Counted as blocked time (the frame pacing statistics: vulkan/exec.c).
// ODYSSEY_PRESENT_INTERVAL=<n> overrides the game's interval (0: no limit).
// The frame is done before that wait: backend_frame_done gets it with the time its present is due (frame
// interpolation makes the frame's in-between image while this thread waits).
NVN(nvnQueuePresentTexture) {
    NvnWindow *w = HOST(NvnWindow, A(1));
    static int forced = -2;
    if (forced == -2) forced = getenv("ODYSSEY_PRESENT_INTERVAL") ? atoi(getenv("ODYSSEY_PRESENT_INTERVAL")) : -1;
    int interval = forced >= 0 ? forced : w->interval;
    int i = (int)A(2);
    NvnTexture *tex = i >= 0 && i < w->num ? HOST(NvnTexture, w->textures[i]) : NULL;
    // ODYSSEY_LOG_GAME_TIME=1: the game thread's time from the end of one present to the next present (its frame without
    // the present's vsync wait: logic, recording, and with the render thread its waits for it), averaged over 10 s
    static int gt_log = -1;
    static uint64_t gt_out, gt_sum, gt_n;
    if (gt_log < 0) gt_log = getenv("ODYSSEY_LOG_GAME_TIME") != NULL;
    if (gt_log && gt_out && plat_now_ns() - gt_out < 100000000ull) {
        gt_sum += plat_now_ns() - gt_out;
        if (++gt_n == 600) { LOG("[game-time] %.2f ms a frame outside its present\n", gt_sum / 600.0 / 1e6); gt_sum = gt_n = 0; }
    }
    {   // ODYSSEY_SAMPLE_AT=<frame>: the game thread sampled from there (runtime/sampler.c, Linux)
        extern uint64_t renderer_present_count(void);
        sampler_frame(renderer_present_count());
    }
    bool rq = rq_enabled();
    if (rq) rq_present_throttle();  // (the render thread one frame behind at most)
    if (interval > 0) {
        extern uint64_t vi_vsync_count(void), vi_wait_vsync(uint64_t target), vi_vsync_time(uint64_t count);
        extern __thread uint64_t g_blocked_ns;
        uint64_t v = vi_vsync_count(), target = w->last_vsync + (uint64_t)interval;
        bool wait = w->last_vsync && v < target;
        if (tex && rq) rq_push(RQ_FRAME, tex, wait ? vi_vsync_time(target) : plat_now_ns());
        else if (tex) backend_frame_done(tex, wait ? vi_vsync_time(target) : plat_now_ns());
        if (wait) {
            uint64_t t0 = plat_now_ns();
            kernel_block_begin();  // (the core's other threads run until the vsync: kernel.c core gates)
            v = vi_wait_vsync(target);
            g_blocked_ns += plat_now_ns() - t0;
        }
        w->last_vsync = v;
    } else if (tex && rq) rq_push(RQ_FRAME, tex, plat_now_ns());
    else if (tex) backend_frame_done(tex, plat_now_ns());
    if (tex && rq) rq_push(RQ_PRESENT, tex, 0);
    else if (tex) backend_present(tex);
    tex_process_releases();
    if (gt_log) gt_out = plat_now_ns();
}

// ================================================================== small state objects
// BlendState (8): target, srcC, dstC, srcA, dstA, eqC, eqA, advanced
NVN(nvnBlendStateSetDefaults) {
    uint8_t *b = AP(0);
    b[0] = 0; b[1] = 2; b[2] = 1; b[3] = 2; b[4] = 1; b[5] = 1; b[6] = 1; b[7] = 0;
}
NVN(nvnBlendStateSetBlendTarget) { G(uint8_t, A(0), 0) = (uint8_t)A(1); }
NVN(nvnBlendStateSetBlendFunc) {
    uint8_t *b = AP(0);
    b[1] = (uint8_t)A(1); b[2] = (uint8_t)A(2); b[3] = (uint8_t)A(3); b[4] = (uint8_t)A(4);
}
NVN(nvnBlendStateSetBlendEquation) { G(uint8_t, A(0), 5) = (uint8_t)A(1); G(uint8_t, A(0), 6) = (uint8_t)A(2); }
NVN(nvnBlendStateSetAdvancedMode) { G(uint8_t, A(0), 7) = (uint8_t)A(1); }
NVN(nvnBlendStateSetAdvancedOverlap) { (void)c; }
NVN(nvnBlendStateSetAdvancedPremultipliedSrc) { (void)c; }
NVN(nvnBlendStateSetAdvancedNormalizedDst) { (void)c; }
NVN(nvnBlendStateGetBlendTarget) { RET(G(uint8_t, A(0), 0)); }
NVN(nvnBlendStateGetBlendFunc) {
    uint8_t *b = AP(0);
    for (int i = 0; i < 4; i++) *(uint32_t *)AP(1 + i) = b[1 + i];
}
NVN(nvnBlendStateGetBlendEquation) { *(uint32_t *)AP(1) = G(uint8_t, A(0), 5); *(uint32_t *)AP(2) = G(uint8_t, A(0), 6); }
NVN(nvnBlendStateGetAdvancedMode) { RET(G(uint8_t, A(0), 7)); }
NVN(nvnBlendStateGetAdvancedOverlap) { RET(0); }
NVN(nvnBlendStateGetAdvancedPremultipliedSrc) { RET(1); }
NVN(nvnBlendStateGetAdvancedNormalizedDst) { RET(1); }

// ChannelMaskState (4): 4 bits per target
// (an NVNboolean argument is its register's low byte: the rest can be junk - Odyssey passes 0x01010100 for false)
NVN(nvnChannelMaskStateSetDefaults) { G(uint32_t, A(0), 0) = 0xFFFFFFFF; }
NVN(nvnChannelMaskStateSetChannelMask) {
    int t = (int)A(1) & 7;
    uint32_t m = ((uint8_t)A(2) ? 1 : 0) | ((uint8_t)A(3) ? 2 : 0) | ((uint8_t)A(4) ? 4 : 0) | ((uint8_t)A(5) ? 8 : 0);
    G(uint32_t, A(0), 0) = (G(uint32_t, A(0), 0) & ~(0xFu << (t * 4))) | (m << (t * 4));
}
NVN(nvnChannelMaskStateGetChannelMask) {
    uint32_t m = G(uint32_t, A(0), 0) >> (((int)A(1) & 7) * 4);
    for (int i = 0; i < 4; i++) *(uint8_t *)AP(2 + i) = (m >> i) & 1;
}

// ColorState (4): blend enable mask, logic op, alpha test
NVN(nvnColorStateSetDefaults) { G(uint8_t, A(0), 0) = 0; G(uint8_t, A(0), 1) = 3; G(uint8_t, A(0), 2) = 8; }
NVN(nvnColorStateSetBlendEnable) {
    uint8_t m = G(uint8_t, A(0), 0), bit = (uint8_t)(1u << ((int)A(1) & 7));
    G(uint8_t, A(0), 0) = (uint8_t)A(2) ? (m | bit) : (m & ~bit);
}
NVN(nvnColorStateSetLogicOp) { G(uint8_t, A(0), 1) = (uint8_t)A(1); }
NVN(nvnColorStateSetAlphaTest) { G(uint8_t, A(0), 2) = (uint8_t)A(1); }
NVN(nvnColorStateGetBlendEnable) { RET((G(uint8_t, A(0), 0) >> ((int)A(1) & 7)) & 1); }
NVN(nvnColorStateGetLogicOp) { RET(G(uint8_t, A(0), 1)); }
NVN(nvnColorStateGetAlphaTest) { RET(G(uint8_t, A(0), 2)); }

// DepthStencilState (8): [0]=depthTest [1]=depthWrite [2]=depthFunc [3]=stencilTest
//   [4]=front func|back func<<4 [5..7]=ops (front fail,zfail,zpass / back) in 4-bit fields
NVN(nvnDepthStencilStateSetDefaults) {
    uint8_t *d = AP(0);
    d[0] = 0; d[1] = 1; d[2] = 2; d[3] = 0; d[4] = 0x88;
    d[5] = 0x11; d[6] = 0x11; d[7] = 0x11;
}
NVN(nvnDepthStencilStateSetDepthTestEnable) { G(uint8_t, A(0), 0) = (uint8_t)A(1); }
NVN(nvnDepthStencilStateSetDepthWriteEnable) { G(uint8_t, A(0), 1) = (uint8_t)A(1); }
NVN(nvnDepthStencilStateSetDepthFunc) { G(uint8_t, A(0), 2) = (uint8_t)A(1); }
NVN(nvnDepthStencilStateSetStencilTestEnable) { G(uint8_t, A(0), 3) = (uint8_t)A(1); }
NVN(nvnDepthStencilStateSetStencilFunc) {
    uint8_t *d = AP(0);
    int face = (int)A(1);  // 1 front, 2 back, 3 both
    if (face & 1) d[4] = (d[4] & 0xF0) | (A(2) & 0xF);
    if (face & 2) d[4] = (d[4] & 0x0F) | (uint8_t)((A(2) & 0xF) << 4);
}
NVN(nvnDepthStencilStateSetStencilOp) {
    uint8_t *d = AP(0);
    int face = (int)A(1);
    uint32_t ops = (uint32_t)((A(2) & 0xF) | (A(3) & 0xF) << 4 | (A(4) & 0xF) << 8);
    uint32_t packed = d[5] | d[6] << 8 | d[7] << 16;
    if (face & 1) packed = (packed & ~0xFFFu) | ops;
    if (face & 2) packed = (packed & 0xFFF) | ops << 12;
    d[5] = (uint8_t)packed; d[6] = (uint8_t)(packed >> 8); d[7] = (uint8_t)(packed >> 16);
}
NVN(nvnDepthStencilStateGetDepthTestEnable) { RET(G(uint8_t, A(0), 0)); }
NVN(nvnDepthStencilStateGetDepthWriteEnable) { RET(G(uint8_t, A(0), 1)); }
NVN(nvnDepthStencilStateGetDepthFunc) { RET(G(uint8_t, A(0), 2)); }
NVN(nvnDepthStencilStateGetStencilTestEnable) { RET(G(uint8_t, A(0), 3)); }
NVN(nvnDepthStencilStateGetStencilFunc) { RET((A(1) & 2) ? G(uint8_t, A(0), 4) >> 4 : G(uint8_t, A(0), 4) & 0xF); }
NVN(nvnDepthStencilStateGetStencilOp) {
    uint8_t *d = AP(0);
    uint32_t packed = d[5] | d[6] << 8 | d[7] << 16;
    if (A(1) & 2) packed >>= 12;
    for (int i = 0; i < 3; i++) *(uint32_t *)AP(2 + i) = (packed >> (4 * i)) & 0xF;
}

// MultisampleState (0x18): enable@0 samples@4 raster@8 a2c@0xC a2cdither@0xD cov2color@0xE output@0xF
NVN(nvnMultisampleStateSetDefaults) { memset(AP(0), 0, 0x18); G(uint8_t, A(0), 0) = 1; G(uint8_t, A(0), 0xD) = 1; }
NVN(nvnMultisampleStateSetMultisampleEnable) { G(uint8_t, A(0), 0) = (uint8_t)A(1); }
NVN(nvnMultisampleStateSetSamples) { G(uint32_t, A(0), 4) = (uint32_t)A(1); }
NVN(nvnMultisampleStateSetRasterSamples) { G(uint32_t, A(0), 8) = (uint32_t)A(1); }
NVN(nvnMultisampleStateSetAlphaToCoverageEnable) { G(uint8_t, A(0), 0xC) = (uint8_t)A(1); }
NVN(nvnMultisampleStateSetAlphaToCoverageDither) { G(uint8_t, A(0), 0xD) = (uint8_t)A(1); }
NVN(nvnMultisampleStateSetCoverageToColorEnable) { G(uint8_t, A(0), 0xE) = (uint8_t)A(1); }
NVN(nvnMultisampleStateSetCoverageToColorOutput) { G(uint8_t, A(0), 0xF) = (uint8_t)A(1); }
NVN(nvnMultisampleStateSetCoverageModulationMode) { G(uint8_t, A(0), 0x10) = (uint8_t)A(1); }
NVN(nvnMultisampleStateSetSampleLocations) { (void)c; }
NVN(nvnMultisampleStateSetSampleLocationsEnable) { (void)c; }
NVN(nvnMultisampleStateSetSampleLocationsGridEnable) { (void)c; }
NVN(nvnMultisampleStateGetMultisampleEnable) { RET(G(uint8_t, A(0), 0)); }
NVN(nvnMultisampleStateGetSamples) { RET(G(uint32_t, A(0), 4)); }
NVN(nvnMultisampleStateGetRasterSamples) { RET(G(uint32_t, A(0), 8)); }
NVN(nvnMultisampleStateGetAlphaToCoverageEnable) { RET(G(uint8_t, A(0), 0xC)); }
NVN(nvnMultisampleStateGetAlphaToCoverageDither) { RET(G(uint8_t, A(0), 0xD)); }
NVN(nvnMultisampleStateGetCoverageToColorEnable) { RET(G(uint8_t, A(0), 0xE)); }
NVN(nvnMultisampleStateGetCoverageToColorOutput) { RET(G(uint8_t, A(0), 0xF)); }
NVN(nvnMultisampleStateGetCoverageModulationMode) { RET(G(uint8_t, A(0), 0x10)); }
NVN(nvnMultisampleStateGetSampleLocationsEnable) { RET(0); }
NVN(nvnMultisampleStateGetSampleLocationsGrid) { (void)c; }
NVN(nvnMultisampleStateGetSampleLocationsGridEnable) { RET(0); }

// PolygonState (4): cull, front face, polygon mode, offset enables
NVN(nvnPolygonStateSetDefaults) { uint8_t *p = AP(0); p[0] = 0; p[1] = 1; p[2] = 2; p[3] = 0; }
NVN(nvnPolygonStateSetCullFace) { G(uint8_t, A(0), 0) = (uint8_t)A(1); }
NVN(nvnPolygonStateSetFrontFace) { G(uint8_t, A(0), 1) = (uint8_t)A(1); }
NVN(nvnPolygonStateSetPolygonMode) { G(uint8_t, A(0), 2) = (uint8_t)A(1); }
NVN(nvnPolygonStateSetPolygonOffsetEnables) { G(uint8_t, A(0), 3) = (uint8_t)A(1); }
NVN(nvnPolygonStateGetCullFace) { RET(G(uint8_t, A(0), 0)); }
NVN(nvnPolygonStateGetFrontFace) { RET(G(uint8_t, A(0), 1)); }
NVN(nvnPolygonStateGetPolygonMode) { RET(G(uint8_t, A(0), 2)); }
NVN(nvnPolygonStateGetPolygonOffsetEnables) { RET(G(uint8_t, A(0), 3)); }

// VertexAttribState (4): format bits 0-7, stream bits 8-12, relative offset bits 13-31
NVN(nvnVertexAttribStateSetDefaults) { G(uint32_t, A(0), 0) = 0; }
NVN(nvnVertexAttribStateSetFormat) {
    uint32_t v = G(uint32_t, A(0), 0);
    G(uint32_t, A(0), 0) = (v & 0x1F00) | (uint32_t)(A(1) & 0xFF) | (uint32_t)(A(2) << 13);
}
NVN(nvnVertexAttribStateSetStreamIndex) {
    G(uint32_t, A(0), 0) = (G(uint32_t, A(0), 0) & ~0x1F00u) | (uint32_t)((A(1) & 0x1F) << 8);
}
NVN(nvnVertexAttribStateGetFormat) {
    uint32_t v = G(uint32_t, A(0), 0);
    *(uint32_t *)AP(1) = v & 0xFF;
    *(int64_t *)AP(2) = v >> 13;
}
NVN(nvnVertexAttribStateGetStreamIndex) { RET((G(uint32_t, A(0), 0) >> 8) & 0x1F); }

// VertexStreamState (8): stride u32, divisor u32
NVN(nvnVertexStreamStateSetDefaults) { G(uint64_t, A(0), 0) = 0; }
NVN(nvnVertexStreamStateSetStride) { G(uint32_t, A(0), 0) = (uint32_t)A(1); }
NVN(nvnVertexStreamStateSetDivisor) { G(uint32_t, A(0), 4) = (uint32_t)A(1); }
NVN(nvnVertexStreamStateGetStride) { RET(G(uint32_t, A(0), 0)); }
NVN(nvnVertexStreamStateGetDivisor) { RET(G(uint32_t, A(0), 4)); }
