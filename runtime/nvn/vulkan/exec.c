// Executes recorded NVN command lists with Vulkan (port of renderer.m; same structure, same behaviour).
#include <SDL3/SDL_cpuinfo.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_timer.h>
#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#include "../../kernel.h"
#include "../../paths.h"
#include "../../odyssey_pose.h"
#include "shaders.h"
#include "vk.h"
#include "dlss_probe.h"
#include "../interp.h"
#include "../rqueue.h"
#include "../renderer.h"
#include "../linear_texture.h"
#include "../../romfs_read.h"
#include "../../settings.h"
#include "../../ui/performance.h"

static uint64_t stat_draws, stat_skipped, stat_passes, stat_dispatches, stat_wait_ns, stat_flushes, stat_feedback_copies;
static uint64_t stat_upload_bytes;
// ODYSSEY_VK_PROF=1: render-thread time per category in the fps log (exec = whole command lists, draw = draw(),
// sets = descriptor sets incl. cbuf copies, vtx = index scan + vertex copies, submit = vkQueueSubmit2)
static int prof = -1;
static uint64_t prof_exec, prof_draw, prof_sets, prof_vtx, prof_submit, prof_pre, prof_key, prof_dyn, prof_tex, prof_up, prof_gpu;
static VkQueryPool prof_qp;
static uint64_t cum_exec, cum_wait, cum_draws, cum_submits;  // (profiling) never reset: hitch log
static uint32_t prof_end_line[4096];  // passes ended, by the exec.c line that ended them (0: submit / present)
uint64_t prof_views_created;  // textures.c vk_image_view cache misses
uint64_t prof_up_ns = ~0ull;  // textures.c upload checks (~0: profiling off)
#define PROF_T0() uint64_t prof_t0_ = prof ? SDL_GetTicksNS() : 0
#define PROF_ADD(v) do { if (prof) v += SDL_GetTicksNS() - prof_t0_; } while (0)
static _Atomic int stat_compiles, stat_links;  // (links: pipelines made from libraries, g_vk.gpl)
static _Atomic int stat_bin_loads, stat_bin_saves, stat_bin_refused, stat_bin_absent;  // (g_vk.pbin: libraries from
// saved data / saved / data the driver refused / none saved)
// ODYSSEY_LOG_SHADER_TIME=<ms>: shader / pipeline build steps that took longer (where first-use hitches go)
static double shader_time_ms(void) {
    static double v = -2;
    if (v == -2) v = getenv("ODYSSEY_LOG_SHADER_TIME") ? atof(getenv("ODYSSEY_LOG_SHADER_TIME")) : -1;
    return v;
}
static void shader_time_log(const char *what, uint64_t addr, uint64_t t0) {
    double lim = shader_time_ms(), ms = (plat_now_ns() - t0) / 1e6;
    if (lim >= 0 && ms >= lim)
        LOG("[shtime] %s %llx: %.1f ms (ram %llu MB)\n", what, (unsigned long long)addr, ms,
            (unsigned long long)(plat_private_bytes() >> 20));
}
static uint64_t op_counts[CMD_COUNT];
bool g_debug_frame;
static char stats_text[2048];
static bool log_linear(void);
void vk_collect_dead(void);

// ================================================================== submissions
// A submission = optional upload command buffer + the main one, signalling the timeline with its number.
#define SUBS 8
typedef struct {
    VkCommandBuffer main, upload;
    bool main_begun, upload_begun, ts;  // ts: GPU timestamps written (profiling)
    uint64_t seq;
    VkDescriptorPool pools[16];
    int npools, cur_pool;
} Sub;
static Sub subs[SUBS];
static int cur_sub;
static void ss_forget(void);
static uint64_t submit_seq;  // last submitted
static pthread_mutex_t seq_lock = PTHREAD_MUTEX_INITIALIZER;
static bool in_pass;

uint64_t vk_completed_seq(void) {
    uint64_t v = 0;
    vkGetSemaphoreCounterValue(g_vk.dev, g_vk.timeline, &v);
    return v;
}

// (ODYSSEY_VK_PROF) waits on the GPU by cause: the caller names it in wait_why just before waiting
static __thread const char *wait_why;
static struct { const char *why; uint64_t ns, n; } wait_tab[12];
static pthread_mutex_t wait_tab_lock = PTHREAD_MUTEX_INITIALIZER;
static void wait_account(uint64_t ns) {
    const char *why = wait_why ? wait_why : "other";
    wait_why = NULL;
    pthread_mutex_lock(&wait_tab_lock);
    for (int i = 0; i < 12; i++)
        if (!wait_tab[i].why || wait_tab[i].why == why) { wait_tab[i].why = why; wait_tab[i].ns += ns; wait_tab[i].n++; break; }
    pthread_mutex_unlock(&wait_tab_lock);
}

static void cq_resolve_done(uint64_t done);

void vk_wait_seq(uint64_t seq) {
    if (!seq) { wait_why = NULL; return; }
    kernel_block_begin();  // (a guest thread waiting for the GPU: its core's other threads run meanwhile)
    VkSemaphoreWaitInfo wi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, .pSemaphores = &g_vk.timeline,
                              .pValues = &seq};
    uint64_t t0 = SDL_GetTicksNS();
    VkResult r = vkWaitSemaphores(g_vk.dev, &wi, 10ull * 1000000000ull);
    stat_wait_ns += SDL_GetTicksNS() - t0;
    cum_wait += SDL_GetTicksNS() - t0;
    wait_account(SDL_GetTicksNS() - t0);
    if (r == VK_TIMEOUT) LOG("[vk] GPU did not finish submission %llu within 10 s (device lost?)\n", (unsigned long long)seq);
    else if (r == VK_SUCCESS) cq_resolve_done(seq);  // (the game reads its timing counters after waiting)
    else {
        extern void vk_devlost_dump(void);
        vk_devlost_dump();
        vk_device_fault_report();
        FATAL("vkWaitSemaphores: %d (device lost)\n", r);
    }
}

uint64_t vk_pending_seq(void) { return submit_seq + 1; }

// One thread at a time in the renderer: Odyssey submits command lists, finalizes textures and presents from several
// threads at once (simpler games do it all from one), and the command buffer being recorded, the descriptor and
// view caches and the deferred-destruction list are shared - a texture finalized on a loading thread had its views
// destroyed under the list being recorded (an invalid command buffer, then device lost). Recursive: the entry points
// call each other. Waiting for the GPU (vk_wait_seq) happens outside it.
static pthread_mutex_t g_render_lock;
static void render_lock_init(void) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_render_lock, &a);
}
// The queue: submissions and presents come from the render lock's holder and from frame interpolation's presenter
// (device.c), which presents without the render lock; ImGui's font upload submits too (menu_render, first frame).
static pthread_mutex_t g_queue_lock;
static void queue_lock_init(void) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_queue_lock, &a);
}
// Asynchronous submission: the render thread hands each finished submission to a submitter thread, which makes the
// vkQueueSubmit2 call. On a Steam Deck the amdgpu kernel driver takes ~4.3 ms in every submission that references the
// imported guest heap (it re-checks the 3.1 GB of user memory page by page), 45% of the render thread's time; on its
// own thread that wait overlaps the recording of the next submission. Order is kept (one queue, one thread), the
// timeline values are assigned when handed over (waiting for a value whose signal isn't submitted yet is allowed for
// timeline semaphores), and anything else using the queue (vk_queue_lock: presents, ImGui, a swapchain re-creation)
// first waits until everything handed over has been submitted. Linux by default (ODYSSEY_VK_ASYNC_SUBMIT=0/1).
typedef struct {
    VkCommandBufferSubmitInfo cbs[2];
    VkSemaphoreSubmitInfo sig[2], wt;
    uint32_t ncb, nsig, nwt;
} SubJob;
#define SQ 4
static SubJob sq[SQ];
static unsigned sq_head, sq_tail;  // (head: submitted; tail: handed over)
static pthread_mutex_t sq_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sq_cv = PTHREAD_COND_INITIALIZER, sq_done_cv = PTHREAD_COND_INITIALIZER;
static int sq_on = -1;
static __thread bool t_submitter;
static __thread int t_queue_depth;
static void queue_submit_now(const SubJob *j);

static void *submitter(void *a) {
    (void)a;
    t_submitter = true;
    for (;;) {
        pthread_mutex_lock(&sq_mx);
        while (sq_head == sq_tail) pthread_cond_wait(&sq_cv, &sq_mx);
        SubJob j = sq[sq_head % SQ];
        pthread_mutex_unlock(&sq_mx);
        queue_submit_now(&j);
        pthread_mutex_lock(&sq_mx);
        sq_head++;
        pthread_cond_broadcast(&sq_done_cv);
        pthread_mutex_unlock(&sq_mx);
    }
    return NULL;
}
static bool async_submit(void) {
    if (sq_on < 0) {
        const char *e = getenv("ODYSSEY_VK_ASYNC_SUBMIT");
#ifdef __linux__
        sq_on = !e || atoi(e) != 0;
#else
        sq_on = e && atoi(e) != 0;
#endif
        if (sq_on) {
            pthread_t t;
            if (pthread_create(&t, NULL, submitter, NULL)) sq_on = 0;
            else pthread_detach(t);
            LOG("[vk] submissions on their own thread: %s\n", sq_on ? "yes" : "no");
        }
    }
    return sq_on;
}
static void sq_drain(void) {  // everything handed over submitted
    if (sq_on <= 0 || t_submitter) return;
    pthread_mutex_lock(&sq_mx);
    while (sq_head != sq_tail) pthread_cond_wait(&sq_done_cv, &sq_mx);
    pthread_mutex_unlock(&sq_mx);
}

void vk_queue_lock(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, queue_lock_init);
    if (!t_queue_depth) sq_drain();  // (not while this thread holds the lock: the submitter would wait for it)
    pthread_mutex_lock(&g_queue_lock);
    t_queue_depth++;
}
void vk_queue_unlock(void) { t_queue_depth--; pthread_mutex_unlock(&g_queue_lock); }

// ODYSSEY_LOG_LOCKS=1 (research: what holds up a present): each hold over 0.3 ms, with the holder's role
__thread const char *g_lock_role;  // (the interpolation thread and the presenter name themselves; else "game")
static __thread int lock_depth;
static __thread uint64_t lock_t0;
static int log_locks = -1;
void vk_render_lock(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, render_lock_init);
    if (pthread_mutex_trylock(&g_render_lock) != 0) {
        kernel_block_begin();  // (busy: a guest thread lets its core's other threads run while it waits - audio on core 1)
        pthread_mutex_lock(&g_render_lock);
    }
    if (log_locks < 0) log_locks = getenv("ODYSSEY_LOG_LOCKS") != NULL;
    if (log_locks && lock_depth++ == 0) lock_t0 = plat_now_ns();
}
void vk_render_unlock(void) {
    if (log_locks > 0 && --lock_depth == 0) {
        uint64_t now = plat_now_ns();
        if (now - lock_t0 > 300000) LOG("[lockhold] %.3f %.3f %s\n", lock_t0 / 1e6, now / 1e6, g_lock_role ? g_lock_role : "game");
    }
    pthread_mutex_unlock(&g_render_lock);
}

// ---- the game's GPU timing counters (nvnCommandBufferReportCounter, two per frame)
// The game times its GPU work with these and drops to 30 fps when frames look slow. They used to get the host clock
// at translation time, so our command translation counted as GPU time: busy scenes (outdoors, ~400 draws) looked slow
// and the game locked itself at 30 even on fast GPUs, at any resolution. Now each counter also records a GPU
// timestamp at its place in the command stream; once that submission is done (the game waits on its fence before
// reading - on the Switch the GPU writes them too) the GPU time replaces the provisional host value, in the host
// clock's terms (offset calibrated at each resolve: a timestamp can't be later than the moment its submission is seen
// done). ODYSSEY_VK_COUNTER=cpu: host clock only; =frozen: one value per presented frame (test); =legacy: the host
// clock in ns instead of GPU ticks, as before (test: reproduces the locked 30).
#define CQ_MAX 64
typedef struct { VkQueryPool pool; uint32_t n; uint64_t seq; uint64_t *out[CQ_MAX]; } CounterQ;
static CounterQ cq[SUBS];
static pthread_mutex_t cq_lock = PTHREAD_MUTEX_INITIALIZER;
static int64_t cq_offset;  // host ns - GPU ns (the smallest seen)
// Counter timestamps are in the Switch GPU's ticks (614.4 MHz): the game converts the difference to ns itself
// (x 625 / 384). Writing ns made every GPU time look 1.63x as long.
static uint64_t ns_to_gpu_ticks(uint64_t ns) { return (uint64_t)((unsigned __int128)ns * 384 / 625); }
static bool cq_calibrated;
static int counter_mode = -1;  // 0 cpu, 1 frozen, 2 gpu
static uint64_t stat_counters_gpu;

static int vk_counter_mode(void) {
    if (counter_mode < 0) {
        const char *e = getenv("ODYSSEY_VK_COUNTER");
        counter_mode = e && !strcmp(e, "cpu") ? 0 : e && !strcmp(e, "frozen") ? 1 : e && !strcmp(e, "legacy") ? 3 : 2;
        if (counter_mode == 2 && !g_vk.props.limits.timestampComputeAndGraphics) counter_mode = 0;
    }
    return counter_mode;
}

// ---- frame pacing: the game's own 30 fps mode when we can't hold 60
// The game advances its world one fixed step per frame in its 60 fps mode, and two steps per frame in its 30 fps mode
// (measured: Link walks twice as far per frame there) - so a 60-mode frame that takes 20 ms plays in slow motion, while
// the 30 mode plays at full speed. The game picks its mode from these counters (GPU time per frame), as on the Switch;
// but our frames can be slow for reasons its GPU time doesn't show (command translation, a slower CPU). So each
// presented frame's busy time is measured (present to present, minus the time the presenting thread was blocked in
// kernel waits and sleeps: idle - the game waits for vsync through its own threads); when
// frames don't fit 60 (12 of the last 60 over 16.2 ms), the counters report a frame of at least PACE_SLOW_NS: the game
// takes its 30 fps mode, at full speed. Back to the real times after 2 s with headroom (busy under 13 ms on average,
// at most 2 of the last 60 frames over). The game itself also tries 60 again now and then.
// ODYSSEY_PACE=0: off.
#define PACE_SLOW_NS 20000000ull
extern __thread uint64_t g_blocked_ns;  // kernel.c: this thread's time blocked in waits / sleeps
static bool pace_slow;
static uint64_t stat_pace_over;

// (Measured and counted only: ODYSSEY_LOG_PACE, the stats. Moving the GPU timestamps so that the game picks a 30 fps
// mode - pace_slow - was for Link's Awakening; Odyssey reads those timers for its dynamic resolution.)
void vk_pace_frame(void) {
    static uint64_t last, last_wait;
    uint64_t now = plat_now_ns(), w = g_blocked_ns;
    uint64_t p = now - last, wt = w - last_wait;
    bool first = !last;
    last = now, last_wait = w;
    if (first || p > 100000000ull) return;  // (loading, a hitch: not a pace)
    double busy = (p > wt ? p - wt : 0) / 1e6;
    static int log = -1;  // ODYSSEY_LOG_PACE=1: each frame's period, vsync wait and busy time
    if (log < 0) log = getenv("ODYSSEY_LOG_PACE") != NULL;
    if (log) LOG("[pace] period %.2f blocked %.2f busy %.2f ms\n", p / 1e6, wt / 1e6, busy);
    stat_pace_over += busy > 16.2;
}

// a pair's end counter (the 16 bytes after its start) is at least PACE_SLOW_NS after it while pace_slow
static uint64_t pace_end_counter(const uint64_t *start_entry, const uint64_t *entry, uint64_t value) {
    if (!pace_slow || !start_entry || (const uint8_t *)entry - (const uint8_t *)start_entry != 16) return value;
    uint64_t min = start_entry[1] + ns_to_gpu_ticks(PACE_SLOW_NS);
    return value < min ? min : value;
}

static void cq_resolve(int i) {  // (cq_lock held) submission slot i is done: its counters get their GPU times
    CounterQ *q = &cq[i];
    if (!q->n) return;
    uint64_t t[CQ_MAX];
    if (vkGetQueryPoolResults(g_vk.dev, q->pool, 0, q->n, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
        double per = g_vk.props.limits.timestampPeriod;
        int64_t cand = (int64_t)plat_now_ns() - (int64_t)((double)t[q->n - 1] * per);
        if (!cq_calibrated || cand < cq_offset) cq_offset = cand, cq_calibrated = true;
        static int log = -1;  // ODYSSEY_LOG_COUNTER=1: the host (provisional) and GPU times of each counter
        if (log < 0) log = getenv("ODYSSEY_LOG_COUNTER") != NULL;
        for (uint32_t k = 0; k < q->n; k++) {
            uint64_t gpu = ns_to_gpu_ticks((uint64_t)((int64_t)((double)t[k] * per) + cq_offset));
            if (log) LOG("[counter] seq %llu #%u at %llx host %.3f gpu %.3f ms\n", (unsigned long long)q->seq, k,
                         (unsigned long long)(uintptr_t)q->out[k], q->out[k][1] * 625.0 / 384 / 1e6, gpu * 625.0 / 384 / 1e6);
            q->out[k][1] = pace_end_counter(k ? q->out[k - 1] : NULL, q->out[k], gpu);
        }
        stat_counters_gpu += q->n;
    }
    q->n = 0;
}

static void cq_resolve_done(uint64_t done) {  // every slot whose submission is done
    pthread_mutex_lock(&cq_lock);
    for (int i = 0; i < SUBS; i++) if (cq[i].n && cq[i].seq <= done) cq_resolve(i);
    pthread_mutex_unlock(&cq_lock);
}

static Sub *sub(void) {
    Sub *s = &subs[cur_sub];
    if (s->seq != submit_seq + 1) {  // slot reused: its previous submission must be done
        wait_why = "ring slot";
        vk_wait_seq(s->seq);
        cq_resolve_done(s->seq);
        if (s->ts) {  // (profiling) GPU time of that submission
            uint64_t t[2];
            if (vkGetQueryPoolResults(g_vk.dev, prof_qp, (uint32_t)cur_sub * 2, 2, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && t[1] > t[0]) {
                prof_gpu += (uint64_t)((double)(t[1] - t[0]) * g_vk.props.limits.timestampPeriod);
            }
            s->ts = false;
        }
        for (int i = 0; i < s->npools; i++) vkResetDescriptorPool(g_vk.dev, s->pools[i], 0);
        s->cur_pool = 0;
        s->seq = submit_seq + 1;
        s->main_begun = s->upload_begun = false;
    }
    return s;
}

static void begin_cb(VkCommandBuffer *cb, bool *begun) {
    if (*begun) return;
    if (!*cb) {
        VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = g_vk.pool,
                                          .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        VK_CHECK(vkAllocateCommandBuffers(g_vk.dev, &ai, cb));
    }
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VK_CHECK(vkBeginCommandBuffer(*cb, &bi));
    *begun = true;
}

static void gp_mark(VkCommandBuffer cb, const char *fmt, ...);
VkCommandBuffer vk_cmd(void) {
    Sub *s = sub();
    if (!s->main_begun) {
        begin_cb(&s->main, &s->main_begun);
        ss_forget();  // (a new command buffer starts with no state)
        if (vk_counter_mode() == 2) {  // this slot's timing counter queries (reset outside any render pass)
            CounterQ *q = &cq[cur_sub];
            if (!q->pool) {
                VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                            .queryCount = CQ_MAX};
                if (vkCreateQueryPool(g_vk.dev, &qi, NULL, &q->pool) != VK_SUCCESS) q->pool = VK_NULL_HANDLE;
            }
            pthread_mutex_lock(&cq_lock);
            q->n = 0;  // (resolved when the slot was freed: sub())
            q->seq = s->seq;
            pthread_mutex_unlock(&cq_lock);
            if (q->pool) vkCmdResetQueryPool(s->main, q->pool, 0, CQ_MAX);
        }
        gp_mark(s->main, "(submission start; includes GPU idle before it)");
        if (prof && prof_qp) {
            vkCmdResetQueryPool(s->main, prof_qp, (uint32_t)cur_sub * 2, 2);
            vkCmdWriteTimestamp2(s->main, VK_PIPELINE_STAGE_2_NONE, prof_qp, (uint32_t)cur_sub * 2);
            s->ts = true;
        }
    }
    return s->main;
}

VkCommandBuffer vk_upload_cmd(void) {
    Sub *s = sub();
    if (!s->upload_begun) {
        begin_cb(&s->upload, &s->upload_begun);
        // after everything submitted before it: a submission's commands may start while the previous one's still run,
        // and these copies overwrite what those may read (textures being replaced, vertex cache pages)
        vk_full_barrier(s->upload);
        vk_checkpoint(s->upload, "upload command buffer of submission %llu", (unsigned long long)vk_pending_seq());
    }
    return s->upload;
}

void vk_full_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}

static void ring_retire(void);
void vc_flush(void);

// ---- ODYSSEY_GPUPROF_AT=<frame>: GPU time per pass / dispatch / copy of that frame, logged as "[gpuprof]".
// A timestamp (ALL_COMMANDS: after everything before it) at each mark; a row = the work since the previous mark.
bool g_gpuprof_frame;
static VkQueryPool gp_qp;
static int gp_n;
static char gp_label[512][112];
static uint32_t gp_draws[512];
// ODYSSEY_VK_DEVLOST_TRACE=1: the last 64 GPU operations, dumped on device loss (with ODYSSEY_SYNC_SUBMIT=1 the
// dump ends with the command list that faulted).
static char dl_ring[64][128];
static unsigned dl_n;
static int dl_on = -1;
static void dl_note(const char *fmt, ...) {
    if (dl_on < 0) dl_on = getenv("ODYSSEY_VK_DEVLOST_TRACE") ? atoi(getenv("ODYSSEY_VK_DEVLOST_TRACE")) : 0;
    if (!dl_on) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(dl_ring[dl_n++ & 63], sizeof dl_ring[0], fmt, ap);
    va_end(ap);
}
void vk_dl_note_upload(void *img, uint32_t w, uint32_t h, uint32_t lv) { dl_note("  upload %p %ux%u lv%u", img, w, h, lv); }

// ODYSSEY_VK_CHECKPOINTS=1 (NVIDIA, VK_NV_device_diagnostic_checkpoints): a named marker before each draw / dispatch /
// copy; after a device loss the queue reports the last markers each pipeline stage reached - the faulting command.
static char cp_desc[16384][128];
static uint64_t cp_n;
void vk_checkpoint(VkCommandBuffer cb, const char *fmt, ...) {
    if (!g_vk.checkpoints) return;
    static PFN_vkCmdSetCheckpointNV set;
    if (!set) set = (PFN_vkCmdSetCheckpointNV)vkGetDeviceProcAddr(g_vk.dev, "vkCmdSetCheckpointNV");
    if (!set) return;
    uint64_t id = ++cp_n;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cp_desc[id & 16383], sizeof cp_desc[0], fmt, ap);
    va_end(ap);
    set(cb, (const void *)(uintptr_t)id);
}
void vk_checkpoints_report(void) {
    if (!g_vk.checkpoints) return;
    PFN_vkGetQueueCheckpointDataNV get = (PFN_vkGetQueueCheckpointDataNV)vkGetDeviceProcAddr(g_vk.dev, "vkGetQueueCheckpointDataNV");
    if (!get) return;
    uint32_t n = 0;
    get(g_vk.queue, &n, NULL);
    VkCheckpointDataNV *d = calloc(n ? n : 1, sizeof *d);
    for (uint32_t i = 0; i < n; i++) d[i].sType = VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV;
    get(g_vk.queue, &n, d);
    LOG("[vk] device lost: last checkpoints reached (%u, of %llu recorded):\n", n, (unsigned long long)cp_n);
    for (uint32_t i = 0; i < n; i++) {
        uint64_t m = (uint64_t)(uintptr_t)d[i].pCheckpointMarker;
        LOG("[vk]   stage %#x: #%llu %s\n", (unsigned)d[i].stage, (unsigned long long)m,
            m && m + 16384 > cp_n ? cp_desc[m & 16383] : "(too old)");
    }
    // the commands in flight (between the earliest and latest stage reached) and the next ones recorded
    uint64_t last = 0, first = UINT64_MAX;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t m = (uint64_t)(uintptr_t)d[i].pCheckpointMarker;
        if (m > last) last = m;
        if (m && m < first) first = m;
    }
    for (uint64_t m = first + 1; first != UINT64_MAX && m < last && m + 16384 > cp_n; m++)
        LOG("[vk]   in flight: #%llu %s\n", (unsigned long long)m, cp_desc[m & 16383]);
    for (uint64_t m = last + 1; m <= cp_n && m <= last + 6; m++) LOG("[vk]   next: #%llu %s\n", (unsigned long long)m, cp_desc[m & 16383]);
    free(d);
}
void vk_devlost_dump(void) {
    if (!dl_on) return;
    LOG("[vk] last GPU operations before the device loss (oldest first):\n");
    for (unsigned i = dl_n > 64 ? dl_n - 64 : 0; i < dl_n; i++) LOG("[vk]   %s\n", dl_ring[i & 63]);
}
static void gp_mark(VkCommandBuffer cb, const char *fmt, ...) {
    if (!g_gpuprof_frame || gp_n >= 511) return;
    if (!gp_qp) {
        VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 512};
        if (vkCreateQueryPool(g_vk.dev, &qi, NULL, &gp_qp) != VK_SUCCESS) { g_gpuprof_frame = false; return; }
    }
    if (!gp_n) vkCmdResetQueryPool(cb, gp_qp, 0, 512);
    vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, gp_qp, (uint32_t)gp_n);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(gp_label[gp_n], sizeof gp_label[0], fmt, ap);
    va_end(ap);
    gp_draws[gp_n] = 0;
    gp_n++;
}
void renderer_flush(void);
void vk_gpuprof_report(void) {
    if (!gp_n) return;
    renderer_flush();
    uint64_t t[512];
    if (vkGetQueryPoolResults(g_vk.dev, gp_qp, 0, (uint32_t)gp_n, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS) {
        double per = g_vk.props.limits.timestampPeriod / 1e6, total = (t[gp_n - 1] - t[0]) * per;
        LOG("[gpuprof] %d marks, %.2f ms first to last (rows: the work after each mark, until the next)\n", gp_n, total);
        for (int i = 0; i + 1 < gp_n; i++) {
            double ms = (t[i + 1] - t[i]) * per;
            LOG("[gpuprof] %6.3f ms %5.1f%%  %s%s", ms, 100 * ms / (total > 0 ? total : 1), gp_label[i], gp_draws[i] ? "" : "\n");
            if (gp_draws[i]) LOG(", %u draws\n", gp_draws[i]);
        }
    }
    gp_n = 0;
}

// Submit the pending work (if any). wait/signal: binary semaphores for presentation (may be null).
static void queue_submit_now(const SubJob *j) {
    VkSubmitInfo2 si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = j->ncb, .pCommandBufferInfos = j->cbs,
                        .signalSemaphoreInfoCount = j->nsig, .pSignalSemaphoreInfos = j->sig,
                        .waitSemaphoreInfoCount = j->nwt, .pWaitSemaphoreInfos = &j->wt};
    vk_queue_lock();
    VkResult sr = vkQueueSubmit2(g_vk.queue, 1, &si, VK_NULL_HANDLE);
    vk_queue_unlock();
    if (sr == VK_ERROR_DEVICE_LOST) vk_devlost_dump();
    VK_CHECK(sr);
}

void vk_submit_extra(VkSemaphore wait, VkSemaphore signal, VkCommandBuffer extra) {
    (void)extra;
    vk_end_pass();
    Sub *s = sub();
    if (!s->main_begun && !s->upload_begun && !wait && !signal) return;
    VkCommandBufferSubmitInfo cbs[2];
    int n = 0;
    vc_flush();
    if (s->upload_begun) {
        vk_full_barrier(s->upload);  // uploads complete before anything in the main buffer reads them
        VK_CHECK(vkEndCommandBuffer(s->upload));
        cbs[n++] = (VkCommandBufferSubmitInfo){.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = s->upload};
    }
    vk_cmd();  // (an empty main buffer still carries the semaphores)
    if (s->ts) vkCmdWriteTimestamp2(s->main, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, prof_qp, (uint32_t)cur_sub * 2 + 1);
    VK_CHECK(vkEndCommandBuffer(s->main));
    cbs[n++] = (VkCommandBufferSubmitInfo){.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = s->main};
    SubJob j = {.ncb = (uint32_t)n, .nsig = signal ? 2 : 1, .nwt = wait ? 1 : 0};
    for (int i = 0; i < n; i++) j.cbs[i] = cbs[i];
    j.sig[0] = (VkSemaphoreSubmitInfo){.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = g_vk.timeline,
                                       .value = submit_seq + 1, .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    j.sig[1] = (VkSemaphoreSubmitInfo){.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = signal,
                                       .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    j.wt = (VkSemaphoreSubmitInfo){.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = wait,
                                   .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT};
    {
        PROF_T0();
        dl_note("submit %llu", (unsigned long long)submit_seq + 1);
        if (async_submit()) {
            pthread_mutex_lock(&sq_mx);
            while (sq_tail - sq_head >= SQ) pthread_cond_wait(&sq_done_cv, &sq_mx);
            sq[sq_tail % SQ] = j;
            sq_tail++;
            pthread_cond_signal(&sq_cv);
            pthread_mutex_unlock(&sq_mx);
        } else {
            queue_submit_now(&j);
        }
        PROF_ADD(prof_submit);
    }
    pthread_mutex_lock(&seq_lock);
    submit_seq++;
    pthread_mutex_unlock(&seq_lock);
    cur_sub = (cur_sub + 1) % SUBS;
    stat_flushes++;
    cum_submits++;
    ring_retire();
    vk_collect_dead();
}

void renderer_submit(void) {
    vk_render_lock();
    vk_submit_extra(VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE);
    vk_render_unlock();
}

void renderer_flush(void) {
    renderer_submit();
    if (!wait_why) wait_why = "flush";
    vk_wait_seq(submit_seq);
}

void renderer_sync(void) {
    pthread_mutex_lock(&seq_lock);
    uint64_t s = submit_seq;
    pthread_mutex_unlock(&seq_lock);
    wait_why = "QueueFinish / sync all";
    vk_wait_seq(s);
}

uint64_t renderer_fence_seq(void) {
    pthread_mutex_lock(&seq_lock);
    uint64_t q = submit_seq + (subs[cur_sub].main_begun || subs[cur_sub].upload_begun ? 1 : 0);
    pthread_mutex_unlock(&seq_lock);
    return q;
}

void renderer_wait_seq(uint64_t seq) {
    if (!seq) return;
    pthread_mutex_lock(&seq_lock);
    uint64_t s = submit_seq;
    pthread_mutex_unlock(&seq_lock);
    wait_why = seq > s ? "SyncWait (fence not submitted yet)" : "SyncWait";
    vk_wait_seq(seq > s ? s : seq);  // not submitted yet (other thread): everything submitted so far
}

VkDescriptorSet vk_alloc_set(VkDescriptorSetLayout layout) {
    Sub *s = sub();
    for (;;) {
        if (s->cur_pool == s->npools) {
            if (s->npools == 16) FATAL("descriptor pools exhausted\n");
            VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 32768}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4096},
                                            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 32768},
                                            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1024}, {VK_DESCRIPTOR_TYPE_SAMPLER, 1024},
                                            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1024}};
            VkDescriptorPoolCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 8192,
                                             .poolSizeCount = 5, .pPoolSizes = sizes};
            VK_CHECK(vkCreateDescriptorPool(g_vk.dev, &ci, NULL, &s->pools[s->npools++]));
        }
        VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                          .descriptorPool = s->pools[s->cur_pool], .descriptorSetCount = 1, .pSetLayouts = &layout};
        VkDescriptorSet set;
        VkResult r = vkAllocateDescriptorSets(g_vk.dev, &ai, &set);
        if (r == VK_SUCCESS) return set;
        s->cur_pool++;
    }
}

// ================================================================== transient memory ring
// 64 MB host-visible chunks (device-local when the GPU exposes resizable BAR); a chunk is reused once every
// submission that allocated from it has completed.
#define CHUNK_SIZE (64ull << 20)
typedef struct { VkBuffer buf; VmaAllocation mem; uint8_t *ptr; uint64_t size, used, seq; } Chunk;
static Chunk *chunks;
static int nchunks, active = -1;

static int new_chunk(uint64_t size) {
    chunks = realloc(chunks, (nchunks + 1) * sizeof *chunks);
    Chunk *c = &chunks[nchunks];
    memset(c, 0, sizeof *c);
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                             .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
    if (getenv("ODYSSEY_VK_RING_SYSMEM")) ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;  // (test: a GPU without resizable BAR)
    VmaAllocationInfo info;
    VK_CHECK(vmaCreateBuffer(g_vk.vma, &bi, &ai, &c->buf, &c->mem, &info));
    c->ptr = info.pMappedData;
    c->size = size;
    if (!nchunks) {  // where the ring lives: device-local (resizable BAR) or system memory read across PCIe
        VkMemoryPropertyFlags f;
        vmaGetMemoryTypeProperties(g_vk.vma, info.memoryType, &f);
        LOG("[vk] transient ring: memory type %u (%s%s%s)\n", info.memoryType, f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT ? "device-local " : "system memory ",
            f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT ? "host-visible " : "", f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT ? "cached" : "");
    }
    return nchunks++;
}

static void ring_retire(void) {}  // chunks are recycled lazily in vk_transient

VkSpan vk_transient(uint64_t size, uint64_t align) {
    if (align < 16) align = 16;
    uint64_t pending = vk_pending_seq();
    if (active >= 0) {
        Chunk *c = &chunks[active];
        uint64_t off = (c->used + align - 1) & ~(align - 1);
        if (off + size <= c->size) {
            c->used = off + size;
            c->seq = pending;
            stat_upload_bytes += size;
            return (VkSpan){c->buf, off, c->ptr + off};
        }
    }
    uint64_t done = vk_completed_seq();
    int pick = -1;
    for (int i = 0; i < nchunks; i++)
        if (i != active && chunks[i].seq <= done && chunks[i].size >= size) { pick = i; break; }
    if (pick < 0) pick = new_chunk(size > CHUNK_SIZE ? size : CHUNK_SIZE);
    active = pick;
    chunks[pick].used = size;
    chunks[pick].seq = pending;
    stat_upload_bytes += size;
    return (VkSpan){chunks[pick].buf, 0, chunks[pick].ptr};
}

// GPU -> CPU copies: the ring is write-combined (often device-local over the BAR), where CPU reads crawl (a
// 1280x720 screenshot took 7 s). Readbacks use one host-cached buffer instead. Every user waits for the GPU and
// reads the data before the next readback (render thread), so a single grow-only buffer suffices.
VkSpan vk_readback(uint64_t size) {
    static VkBuffer buf;
    static VmaAllocation mem;
    static uint8_t *ptr;
    static uint64_t cap;
    if (size > cap) {
        if (buf) { vk_wait_seq(submit_seq); vmaDestroyBuffer(g_vk.vma, buf, mem); }
        cap = (size + (16 << 20) - 1) & ~((16ull << 20) - 1);
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = cap, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                      .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
        VmaAllocationInfo info;
        VK_CHECK(vmaCreateBuffer(g_vk.vma, &bi, &ai, &buf, &mem, &info));
        ptr = info.pMappedData;
    }
    return (VkSpan){buf, 0, ptr};
}

VkSpan vk_transient_copy(const void *data, uint64_t size, uint64_t align) {
    VkSpan s = vk_transient(size ? size : 16, align);
    if (size) memcpy(s.ptr, data, size);
    return s;
}

// Per-draw constants that usually repeat the previous draw's (scale table, tint, driver constants): reuse that copy
// while it's identical, within one submission. slot: one per kind of data.
enum { DEDUPE_RSCALE_V, DEDUPE_RSCALE_F, DEDUPE_TINT, DEDUPE_DRV_V, DEDUPE_DRV_F, DEDUPE_DRV_C, DEDUPE_SLOTS };
static VkSpan copy_dedupe(int slot, const void *data, uint64_t size, uint64_t align) {
    static struct { uint64_t seq, size; uint8_t data[0x800]; VkSpan span; } last[DEDUPE_SLOTS];
    uint64_t seq = vk_pending_seq();
    if (size > sizeof last[0].data) return vk_transient_copy(data, size, align);
    if (last[slot].seq == seq && last[slot].size == size && !memcmp(last[slot].data, data, size)) return last[slot].span;
    VkSpan s = vk_transient_copy(data, size, align);
    last[slot].seq = seq; last[slot].size = size; memcpy(last[slot].data, data, size); last[slot].span = s;
    return s;
}

// Guest memory copied once per submission: draws binding the same range share the copy.
#define GCOPY_TAB 4096
static struct { uint64_t addr, size, seq; VkSpan span; } gcopy[GCOPY_TAB];
static uint64_t gcopy_gen;  // bumped to drop every shared copy (guest memory written mid-list)
static void guest_copy_flush(void) { gcopy_gen++; }
static VkSpan guest_copy(uint64_t addr, uint64_t size) {
    uint64_t seq = vk_pending_seq() + (gcopy_gen << 40), h = (addr * 0x9E3779B97F4A7C15ull) ^ (size * 0xC2B2AE3D27D4EB4Full);
    uint32_t slot = (uint32_t)(h ^ (h >> 32)) & (GCOPY_TAB - 1);
    static int nocache = -1;  // ODYSSEY_NO_CB_CACHE=1: every draw its own copy (test)
    if (nocache < 0) nocache = getenv("ODYSSEY_NO_CB_CACHE") != NULL;
    if (!nocache && gcopy[slot].seq == seq && gcopy[slot].addr == addr && gcopy[slot].size == size) return gcopy[slot].span;
    VkSpan s = vk_transient_copy((const void *)(uintptr_t)addr, size, 256);
    gcopy[slot].addr = addr; gcopy[slot].size = size; gcopy[slot].seq = seq; gcopy[slot].span = s;
    return s;
}

// ================================================================== worker threads (shader / pipeline builds)
// First-use compiles run off the render thread; draws needing them are skipped until ready (as on Metal).
// ODYSSEY_SYNC_SHADERS=1 builds on first use, blocking (deterministic tests).
// Two queues: [0] builds a draw waits for, [1] programs compiled ahead (renderer_program_hint) - those on at most half
// the workers and at low thread priority, so the game and the urgent builds keep the CPU.
typedef struct Job { void (*fn)(void *); void *arg; struct Job *next; } Job;
typedef struct { Job *head, *tail; } JobQ;
static JobQ jobs_q[2];
static int jobs_low_running, jobs_low_max = 1;
static pthread_mutex_t jobs_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t jobs_cond = PTHREAD_COND_INITIALIZER;

static _Atomic int dbg_qlen[4], dbg_progs;  // (ODYSSEY_LOG_MEM: queued jobs, programs made)
static JobQ tjobs_q[2];                      // (the translator's, below)
static void jobq_push(JobQ *q, Job *j) {
    atomic_fetch_add(&dbg_qlen[q == &jobs_q[0] ? 0 : q == &jobs_q[1] ? 1 : q == &tjobs_q[0] ? 2 : 3], 1);
    j->next = NULL;
    if (q->tail) q->tail->next = j; else q->head = j;
    q->tail = j;
}
static Job *jobq_pop(JobQ *q) {
    Job *j = q->head;
    if (j) atomic_fetch_sub(&dbg_qlen[q == &jobs_q[0] ? 0 : q == &jobs_q[1] ? 1 : q == &tjobs_q[0] ? 2 : 3], 1);
    if (j && !(q->head = j->next)) q->tail = NULL;
    return j;
}

static void *worker(void *a) {
    (void)a;
    for (;;) {
        pthread_mutex_lock(&jobs_lock);
        Job *j;
        bool low = false;
        for (;;) {
            if ((j = jobq_pop(&jobs_q[0]))) break;
            if (jobs_low_running < jobs_low_max && (j = jobq_pop(&jobs_q[1]))) { low = true; jobs_low_running++; break; }
            pthread_cond_wait(&jobs_cond, &jobs_lock);
        }
        pthread_mutex_unlock(&jobs_lock);
        if (low) SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_LOW);
        j->fn(j->arg);
        free(j);
        if (low) {
            SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_NORMAL);
            pthread_mutex_lock(&jobs_lock);
            jobs_low_running--;
            pthread_cond_signal(&jobs_cond);
            pthread_mutex_unlock(&jobs_lock);
        }
    }
    return NULL;
}

// Odyssey draws some targets once only (its color grading maps when a scene starts, layout captures): a draw skipped
// while its shaders build left them empty for good (black cutscene). Those come as a scene starts, so there it builds
// on first use, blocking, while a scene warms up or between scenes (hle/odyssey_warp.c odyssey_scene_warming). In
// play a blocking build stalled the frame for the driver's pipeline compile (100-150 ms each, hitches whenever
// something new showed up): there, as LA always, builds run off-thread and their draws wait a few frames.
// ODYSSEY_SYNC_SHADERS=0/1 overrides.
static bool builds_block(void) {  // (the scene state: warming up, or ODYSSEY_SYNC_SHADERS)
    static int v = -2;
    if (v == -2) { const char *e = getenv("ODYSSEY_SYNC_SHADERS"); v = e ? atoi(e) != 0 : -1; }
    if (v >= 0) return v;
    if (g_settings.shader_wait) return true;  // (menu, Video: While a shader loads = Wait)
    if (!game_is_odyssey()) return false;
    extern bool odyssey_scene_warming(void);
    return odyssey_scene_warming();
}
// ... and whether the draw at hand waits: while a scene warms up only the draws whose output may be drawn once
// (draw_: full-screen passes, 3D / layered targets - the color grading map, environment probes - and small ones) wait;
// the others are skipped until their build is done, as in play. A new player's first frame of the Sand Kingdom stood
// 2.4 s: the game sets up a stage's model shaders ~11 frames before their first draw, 450 libraries (27 s of
// compiles) were built for it right then. Mostly hidden by the fade-in. ODYSSEY_WARMUP_WAIT=all: every draw waits.
static bool g_draw_relaxed;
static bool sync_shaders(void) { return !g_draw_relaxed && builds_block(); }

// Ready to use? While builds block (sync_shaders), a build started off-thread before waits to its end instead of the
// draw being skipped.
// Full-screen passes (lighting, composite, tonemap: a few vertices over the whole target): skipped, the whole picture
// went black for a frame; waiting for the build stalled the game 50-200 ms (the driver's compile, on first sight of
// a new area: the hitches in play). Now such a frame is just not shown (the window keeps the previous one: the
// present takes renderer_take_frame_incomplete) while the game, its sound and the build go on.
// ODYSSEY_FULLSCREEN_WAIT=1: wait as before.
static bool g_draw_must_wait, g_draw_fullscreen;
static _Atomic bool g_frame_incomplete;
bool renderer_take_frame_incomplete(void) { return atomic_exchange(&g_frame_incomplete, false); }
typedef struct Prog Prog;
static void remember_frame_hold(Prog *p);
uint64_t renderer_present_count(void);
static bool built(_Atomic int *state, Prog *p) {
    int s = atomic_load(state);
    static int fs_wait = -1;
    if (fs_wait < 0) fs_wait = getenv("ODYSSEY_FULLSCREEN_WAIT") != NULL;
    if (s == 0 && g_draw_must_wait && !fs_wait && !sync_shaders()) {
        atomic_store(&g_frame_incomplete, true);
        if (p) remember_frame_hold(p);
        return false;
    }
    if (s == 0 && p && g_draw_fullscreen && !g_draw_must_wait && !sync_shaders() && shader_time_ms() >= 0)
        LOG("[shtime] frame %llu can present while a clipped or blended draw is building\n",
            (unsigned long long)renderer_present_count());
    if (s == 0 && (sync_shaders() || g_draw_must_wait)) {
        uint64_t t0 = plat_now_ns();
        kernel_block_begin();  // (the core's other threads run meanwhile: loading)
        while ((s = atomic_load(state)) == 0 && plat_now_ns() - t0 < 10000000000ull) SDL_DelayNS(200000);
        if (shader_time_ms() >= 0 && (plat_now_ns() - t0) / 1e6 >= shader_time_ms())
            LOG("[shtime] draw waited %.1f ms for its build (%s)\n", (plat_now_ns() - t0) / 1e6,
                g_draw_must_wait && !sync_shaders() ? "full-screen pass" : "scene warm-up");
    }
    return s == 1;
}

// Shader translation: one dedicated thread (the translator isn't re-entrant - translate.c's RSC2 string ring - and
// translating on the game thread cost up to ~90 ms per frame on entering an area). Compiles go on to the workers.
static pthread_mutex_t translate_lock = PTHREAD_MUTEX_INITIALIZER;  // (compute shaders translate on the game thread)
static JobQ tjobs_q[2];  // (as jobs_q: [0] urgent, [1] ahead)
static pthread_mutex_t tjobs_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t tjobs_cond = PTHREAD_COND_INITIALIZER;
static void *translator(void *a) {
    (void)a;
    for (;;) {
        pthread_mutex_lock(&tjobs_lock);
        Job *j;
        bool low = false;
        while (!(j = jobq_pop(&tjobs_q[0])) && !(low = (j = jobq_pop(&tjobs_q[1])) != NULL))
            pthread_cond_wait(&tjobs_cond, &tjobs_lock);
        pthread_mutex_unlock(&tjobs_lock);
        // Programs set up ahead translate at low priority, as the workers build them (a Steam Deck's 4 cores: two
        // translators at normal priority for minutes after a stage load, every program it set up, took the game
        // thread from 24 to 38 ms a frame). Urgent ones - a draw waits - stay at normal priority.
        if (low) SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_LOW);
        j->fn(j->arg);
        free(j);
        if (low) SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_NORMAL);
    }
    return NULL;
}
static bool sync_shaders(void);
// (several: the translator is re-entrant since its string ring is per thread - a stage start sets up hundreds of
// programs, translated one after another on one thread they kept the scene's first draws waiting; with the shader
// precompile their libraries are ready, so the translation was what was left. ODYSSEY_TRANSLATE_THREADS=<n>)
static void translator_start(void) {
    int n = SDL_GetNumLogicalCPUCores() / 4;
    if (n < 2) n = 2;
    if (n > 4) n = 4;
    if (getenv("ODYSSEY_TRANSLATE_THREADS")) n = atoi(getenv("ODYSSEY_TRANSLATE_THREADS")) > 0 ? atoi(getenv("ODYSSEY_TRANSLATE_THREADS")) : 1;
    for (int i = 0; i < n; i++) { pthread_t t; pthread_create(&t, NULL, translator, NULL); }
}
static void workers_start(void) {
    // first entry into an area builds dozens of pipelines at once: use most of the CPU (draws wait for them)
    int n = SDL_GetNumLogicalCPUCores() - 4;
    if (n < 4) n = 4;
    if (n > 16) n = 16;
    if (getenv("ODYSSEY_SHADER_THREADS")) n = atoi(getenv("ODYSSEY_SHADER_THREADS")) > 0 ? atoi(getenv("ODYSSEY_SHADER_THREADS")) : 1;
    jobs_low_max = n > 1 ? n / 2 : 1;
    for (int i = 0; i < n; i++) { pthread_t t; pthread_create(&t, NULL, worker, NULL); }
}

// q: 0 urgent, 1 ahead of use. Never runs the job right here (run_job: in blocking mode).
static void queue_translate(void (*fn)(void *), void *arg, int q) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, translator_start);
    Job *j = malloc(sizeof *j);
    j->fn = fn; j->arg = arg;
    pthread_mutex_lock(&tjobs_lock);
    jobq_push(&tjobs_q[q], j);
    pthread_cond_signal(&tjobs_cond);
    pthread_mutex_unlock(&tjobs_lock);
}

static void queue_job(void (*fn)(void *), void *arg, int q) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, workers_start);
    Job *j = malloc(sizeof *j);
    j->fn = fn; j->arg = arg;
    pthread_mutex_lock(&jobs_lock);
    jobq_push(&jobs_q[q], j);
    pthread_cond_signal(&jobs_cond);
    pthread_mutex_unlock(&jobs_lock);
}

static void run_job(void (*fn)(void *), void *arg) {
    if (sync_shaders()) { fn(arg); return; }
    queue_job(fn, arg, 0);
}

// ================================================================== state
typedef struct {
    NvnTexture *color[8];
    NvnTextureView cview[8];
    uint8_t has_cview[8];
    int ncolor;
    NvnTexture *depth;
    NvnTextureView dview;
    uint8_t has_dview;
} RtState;

typedef struct { uint64_t addr, size; } Range;

static struct {
    RtState rt;
    bool rt_dirty;
    NvnProgram *program;
    uint8_t blend[8][8];
    uint32_t channel_mask;
    uint8_t color_state[4], ds[8], ms[0x18], poly[4];
    float poly_offset[3];
    uint32_t attrib[16];
    int nattrib;
    uint32_t stream[16][2];
    int nstream;
    Range vbuf[16], ubuf[6][18], sbuf[6][16];
    uint64_t tex[6][32], img[6][8];
    NvnDescPool *tpool, *spool;
    float viewport[16][4];
    int32_t scissor[16][4];
    float depth_range[16][2];
    float blend_color[4];
    uint32_t stencil_ref[2], stencil_mask[2], stencil_vmask[2];
    bool prim_restart;
    uint32_t restart_index;
    uint64_t copy_row_stride, copy_image_stride;
} st;

uint32_t nvn_window_origin(void);
uint32_t nvn_depth_mode(void);
bool renderer_origin_lower_left(void) { return nvn_window_origin() == 0; }

// (ODYSSEY_VK_PROF) called once per presented frame: frames over 50 ms are logged with where their time went.
// present_ns: time in backend_present's acquire/blit/present.
void vk_prof_frame(uint64_t frame, uint64_t present_ns) {
    if (!prof) return;
    static uint64_t last_t, l_exec, l_wait, l_draws, l_subs;
    uint64_t now = SDL_GetTicksNS();
    if (last_t && now - last_t > 50000000) {
        uint64_t dt = now - last_t, ex = cum_exec - l_exec, wt = cum_wait - l_wait;
        LOG("[hitch] frame %llu: %.0f ms = backend exec %.1f (of which GPU waits %.1f) + present %.1f + other %.1f; %llu draws, "
            "%llu submits, compiles %d pending\n", (unsigned long long)frame, dt / 1e6, ex / 1e6, wt / 1e6, present_ns / 1e6,
            ((double)dt - ex - present_ns) / 1e6, (unsigned long long)(cum_draws - l_draws), (unsigned long long)(cum_submits - l_subs),
            atomic_load(&stat_compiles));
    }
    last_t = now; l_exec = cum_exec; l_wait = cum_wait; l_draws = cum_draws; l_subs = cum_submits;
}

const char *renderer_stats(void) {
    static int dumps;
    if (dumps++ < 3) {
        LOG("[vk] command counts:");
        for (int i = 0; i < CMD_COUNT; i++) if (op_counts[i]) LOG(" %d:%llu", i, (unsigned long long)op_counts[i]);
        LOG("\n");
    }
    snprintf(stats_text, sizeof stats_text,
             "%llu draws, %llu skipped, %llu passes, %llu dispatches, %llu flushes, wait %.0fms, compile %d, link %d, fbcopy %llu, upload %.1fMB",
             (unsigned long long)stat_draws, (unsigned long long)stat_skipped, (unsigned long long)stat_passes,
             (unsigned long long)stat_dispatches, (unsigned long long)stat_flushes, (double)stat_wait_ns / 1e6,
             atomic_exchange(&stat_compiles, 0), atomic_exchange(&stat_links, 0), (unsigned long long)stat_feedback_copies, (double)stat_upload_bytes / 1048576.0);
    stat_feedback_copies = stat_upload_bytes = 0;
    {
        extern void vk_alias_stats(void);
        vk_alias_stats();
    }
    {   // GPU memory: what this process uses of each device-local heap's budget, and live images / views
        VkPhysicalDeviceMemoryProperties const *mp;
        vmaGetMemoryProperties(g_vk.vma, &mp);
        VmaBudget bud[VK_MAX_MEMORY_HEAPS];
        vmaGetHeapBudgets(g_vk.vma, bud);
        uint64_t vram = 0, vram_budget = 0;
        size_t n = strlen(stats_text);
        for (uint32_t h = 0; h < mp->memoryHeapCount && n < sizeof stats_text - 64; h++) {
            if (!(mp->memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)) continue;
            vram += bud[h].usage;
            vram_budget += bud[h].budget;
            snprintf(stats_text + n, sizeof stats_text - n, ", vram%u %.0f/%.0fMB (%u allocs)", h, bud[h].usage / 1048576.0,
                     bud[h].budget / 1048576.0, bud[h].statistics.blockCount);
            n = strlen(stats_text);
        }
        vk_overlay_memory(vram, vram_budget);
        extern int vk_live_textures(void);
        snprintf(stats_text + n, sizeof stats_text - n, ", %d textures, ram %llu MB", vk_live_textures(),
                 (unsigned long long)(plat_private_bytes() >> 20));
        if (g_vk.pbin) {  // (libraries created from saved data / compiled and saved)
            n = strlen(stats_text);
            extern uint32_t vk_libs_shared_take(void);
            snprintf(stats_text + n, sizeof stats_text - n, ", binaries %d loaded %d saved %d refused %d absent, libraries %u shared",
                     atomic_exchange(&stat_bin_loads, 0), atomic_exchange(&stat_bin_saves, 0), atomic_exchange(&stat_bin_refused, 0),
                     atomic_exchange(&stat_bin_absent, 0), vk_libs_shared_take());
        }
        // ODYSSEY_LOG_MEM=1: where that memory is (the guest's window, the process's heaps: malloc and drivers), build
        // queues, programs, command records. ODYSSEY_HEAP_HIST=<frame>: the heaps' blocks by size, once.
        static int log_mem = -1, hist_at = -2;
        if (log_mem < 0) log_mem = getenv("ODYSSEY_LOG_MEM") != NULL;
        if (hist_at == -2) hist_at = getenv("ODYSSEY_HEAP_HIST") ? atoi(getenv("ODYSSEY_HEAP_HIST")) : -1;
        if (log_mem) {
            extern uint64_t nvn_record_bytes(void);
            n = strlen(stats_text);
            snprintf(stats_text + n, sizeof stats_text - n,
                     " (guest %llu, heaps %llu) [jobs %d/%d, translate %d/%d, programs %d, records %llu MB]",
                     (unsigned long long)(plat_guest_bytes() >> 20), (unsigned long long)(plat_heap_bytes() >> 20),
                     atomic_load(&dbg_qlen[0]), atomic_load(&dbg_qlen[1]), atomic_load(&dbg_qlen[2]),
                     atomic_load(&dbg_qlen[3]), atomic_load(&dbg_progs), (unsigned long long)(nvn_record_bytes() >> 20));
            for (uint32_t h = 0; h < mp->memoryHeapCount; h++) {  // (every heap: system memory ones too)
                n = strlen(stats_text);
                if (n >= sizeof stats_text - 64) break;
                snprintf(stats_text + n, sizeof stats_text - n, " heap%u%s %.0f MB in %u blocks, %u allocs", h,
                         mp->memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT ? "(gpu)" : "(sys)",
                         bud[h].statistics.blockBytes / 1048576.0, bud[h].statistics.blockCount,
                         bud[h].statistics.allocationCount);
            }
        }
        extern uint64_t renderer_present_count(void);
        if (hist_at >= 0 && renderer_present_count() >= (uint64_t)hist_at) {
            plat_heap_histogram();
            hist_at = -1;
        }
    }
    if (prof) {
        prof_up = prof_up_ns;
        prof_up_ns = 0;
        size_t n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | gpu %.0f cpu exec %.0f draw %.0f (pre %.0f key %.0f dyn %.0f sets %.0f tex %.0f (upload %.0f) vtx %.0f) submit %.0f ms",
                 prof_gpu / 1e6, prof_exec / 1e6, prof_draw / 1e6, prof_pre / 1e6, prof_key / 1e6, prof_dyn / 1e6, prof_sets / 1e6,
                 prof_tex / 1e6, prof_up / 1e6, prof_vtx / 1e6, prof_submit / 1e6);
        extern uint64_t prof_tex_checks, prof_tex_samples, prof_tex_full, prof_tex_full_bytes, prof_tex_dyn;
        n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | tex checks %llu samples %llu full %llu (%.1f MB, dynamic %llu)",
                 (unsigned long long)prof_tex_checks, (unsigned long long)prof_tex_samples, (unsigned long long)prof_tex_full,
                 prof_tex_full_bytes / 1048576.0, (unsigned long long)prof_tex_dyn);
        n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | pass ends:");
        for (int l = 0; l < 4096; l++)
            if (prof_end_line[l] > 20) { n = strlen(stats_text); snprintf(stats_text + n, sizeof stats_text - n, " %d:%u", l, prof_end_line[l]); }
        memset(prof_end_line, 0, sizeof prof_end_line);
        n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | views created %llu | counters gpu %llu | pace %s, %llu frames over", (unsigned long long)prof_views_created,
                 (unsigned long long)stat_counters_gpu, pace_slow ? "30" : "60", (unsigned long long)stat_pace_over);
        stat_counters_gpu = stat_pace_over = 0;
        {
            extern uint64_t prof_vc_hits, prof_vc_bypass, prof_vc_copied, prof_vc_queries, prof_vc_scan_pages, prof_vc_scan_ns;
            n = strlen(stats_text);
            snprintf(stats_text + n, sizeof stats_text - n,  " | vcache %llu hit %llu in place %.1f MB copied, %llu queries %.2f ms, %llu pages written",
                     (unsigned long long)prof_vc_hits, (unsigned long long)prof_vc_bypass, prof_vc_copied / 1048576.0,
                     (unsigned long long)prof_vc_queries, prof_vc_scan_ns / 1e6, (unsigned long long)prof_vc_scan_pages);
            prof_vc_hits = prof_vc_bypass = prof_vc_copied = prof_vc_queries = prof_vc_scan_pages = prof_vc_scan_ns = 0;
            extern uint64_t prof_tex_watch_skips;
            n = strlen(stats_text);
            snprintf(stats_text + n, sizeof stats_text - n, " | tex unwritten %llu", (unsigned long long)prof_tex_watch_skips);
            prof_tex_watch_skips = 0;
        }
        pthread_mutex_lock(&wait_tab_lock);
        for (int i = 0; i < 12 && wait_tab[i].why; i++) {
            n = strlen(stats_text);
            snprintf(stats_text + n, sizeof stats_text - n, "%s%s %llux %.0f ms", i ? "," : " | waits:", wait_tab[i].why,
                     (unsigned long long)wait_tab[i].n, wait_tab[i].ns / 1e6);
        }
        memset(wait_tab, 0, sizeof wait_tab);
        pthread_mutex_unlock(&wait_tab_lock);
        prof_views_created = 0;
        prof_tex_checks = prof_tex_samples = prof_tex_full = prof_tex_full_bytes = prof_tex_dyn = 0;
        prof_exec = prof_draw = prof_sets = prof_vtx = prof_submit = prof_pre = prof_key = prof_dyn = prof_tex = prof_up = prof_gpu = 0;
    }
    stat_draws = stat_skipped = stat_passes = stat_dispatches = stat_flushes = stat_wait_ns = 0;
    return stats_text;
}

static void st_defaults(void) {
    memset(&st, 0, sizeof st);
    for (int i = 0; i < 16; i++) {
        st.viewport[i][2] = 1280; st.viewport[i][3] = 720;
        st.scissor[i][2] = 16384; st.scissor[i][3] = 16384;
        st.depth_range[i][1] = 1.0f;
    }
    st.channel_mask = 0xFFFFFFFF;
    st.stencil_mask[0] = st.stencil_mask[1] = 0xFF;
    st.stencil_vmask[0] = st.stencil_vmask[1] = 0xFF;
    st.ds[1] = 1; st.ds[2] = 2;
    st.poly[1] = 1; st.poly[2] = 2;
    for (int i = 0; i < 8; i++) { st.blend[i][1] = 2; st.blend[i][2] = 1; st.blend[i][3] = 2; st.blend[i][4] = 1; st.blend[i][5] = 1; st.blend[i][6] = 1; }
}

void vk_exec_init(void) {
    prof = getenv("ODYSSEY_VK_PROF") != NULL;
    if (prof) {
        prof_up_ns = 0;
        VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = SUBS * 2};
        if (vkCreateQueryPool(g_vk.dev, &qi, NULL, &prof_qp) != VK_SUCCESS) prof_qp = VK_NULL_HANDLE;
    }
    st_defaults();
}

void renderer_texture_scale(NvnTexture *t, float *sx, float *sy);
bool renderer_texture_presented(NvnTexture *t);
bool renderer_texture_first_write(NvnTexture *t);
bool renderer_texture_level_fresh(NvnTexture *t);
bool renderer_texture_frame_fresh(NvnTexture *t);
void renderer_texture_mark_presented(NvnTexture *t);
void renderer_save_texture_tex(TexBackend *b, const char *path);
static float pass_sx = 1, pass_sy = 1;
static float vp_sx = 1, vp_sy = 1, vp_ox = 0, vp_oy = 0;
static float tex_sc[2][64][2];
static uint32_t tex_wh[2][64][2];

// ================================================================== render passes
static VkFormat pass_color[8], pass_depth, pass_stencil;
static uint32_t pass_ncolor, pass_samples = 1, pass_w, pass_h;
static bool pass_color_drawn;
static int debug_pass_no;

// Redundant-state filter: what draw() last set in the current command buffer, so unchanged pipeline binds, dynamic
// state and vertex / index buffer binds are not recorded again (a draw used to issue ~20 vkCmd* calls; most repeat the
// previous draw's). Forgotten (ss_forget) whenever something else may have recorded state: a new command buffer, a new
// render pass (present, ImGui, post-processing and copies all run between game passes) and the clear pipeline.
// Descriptor sets are not filtered: every draw writes new ones (sharing identical sets within a submission was tried:
// a third matched, but comparing them cost more than allocating and writing them on NVIDIA).
// ODYSSEY_VK_STATE_FILTER=0: every draw sets everything, as before.
enum { SS_PIPE = 1 << 0, SS_DT = 1 << 1, SS_DW = 1 << 2, SS_DOP = 1 << 3, SS_STEN = 1 << 4, SS_SFACE0 = 1 << 5, SS_SFACE1 = 1 << 6,
       SS_BC = 1 << 7, SS_CULL = 1 << 8, SS_FF = 1 << 9, SS_TOPO = 1 << 10, SS_PRE = 1 << 11, SS_DBE = 1 << 12, SS_DB = 1 << 13,
       SS_VP = 1 << 14, SS_SC = 1 << 15, SS_IB = 1 << 16, SS_AK = 1 << 17 };
static struct {
    uint32_t known;  // SS_* bits: the value below is what the command buffer has
    uint32_t vb_known;  // per vertex binding
    VkPipeline pipe;
    VkBool32 dt, dw, sten, pre, dbe;
    VkCompareOp dop;
    uint32_t sface[2][7];  // fail, pass, depth fail, compare, compare mask, write mask, reference
    float bc[4], db[3];
    VkCullModeFlags cull;
    VkFrontFace ff;
    VkPrimitiveTopology topo;
    VkViewport vp;
    VkRect2D sc;
    VkBuffer ib; VkDeviceSize io; VkIndexType it;
    VkBuffer vb[17]; VkDeviceSize vo[17];
    uint32_t ak[4];  // vertex attribute kinds (push constant: translate.c vattr)
} ss;
static int ss_on = -1;
static void ss_forget(void) { ss.known = 0; ss.vb_known = 0; }
static bool ss_enabled(void) {
    if (ss_on < 0) { const char *e = getenv("ODYSSEY_VK_STATE_FILTER"); ss_on = !e || atoi(e) != 0; }
    return ss_on;
}
// true: the state must be recorded (and is remembered as known)
static inline bool ss_need(uint32_t bit, bool same) {
    if (ss_on && (ss.known & bit) && same) return false;
    ss.known |= bit;
    return true;
}

// Debug frames: a float depth target's range, logged, and saved as grey (min black, max white) next to the pass dumps.
static void debug_depth_dump(TexBackend *b, const char *path) {
    if (!b || !b->image || (b->format != VK_FORMAT_D32_SFLOAT && b->format != VK_FORMAT_D32_SFLOAT_S8_UINT)) return;
    uint32_t w = b->width, h = b->height;
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    VkSpan out = vk_readback((uint64_t)w * h * 4);
    VkBufferImageCopy cp = {.bufferOffset = out.off, .imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
    vkCmdCopyImageToBuffer(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, out.buf, 1, &cp);
    renderer_flush();
    const float *d = out.ptr;
    float mn = 1e30f, mx = -1e30f;
    uint64_t ones = 0, zeros = 0, n = (uint64_t)w * h;
    for (uint64_t i = 0; i < n; i++) {
        float v = d[i];
        if (v < mn) mn = v;
        if (v > mx) mx = v;
        ones += v == 1.0f;
        zeros += v == 0.0f;
    }
    LOG("[vk] %s: depth %ux%u min %.9g max %.9g, %.1f%% at 1.0, %.1f%% at 0.0\n", path, w, h, mn, mx, 100.0 * ones / n, 100.0 * zeros / n);
    uint8_t *rgba = malloc(n * 4);
    float span = mx > mn ? mx - mn : 1.0f;
    for (uint64_t i = 0; i < n; i++) {
        uint8_t g = (uint8_t)((d[i] - mn) / span * 255.0f + 0.5f);
        rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = g;
        rgba[i * 4 + 3] = 255;
    }
    vk_save_png(path, rgba, w, h);
    free(rgba);
}

// Debug frames: level 0 of a small color image as the GPU has it (any type, 4-byte texels): its first words and how
// many are non-zero.
static void debug_tex_readback(TexBackend *b, const char *what) {
    if (!b || !b->image || (b->aspect & ~VK_IMAGE_ASPECT_COLOR_BIT)) return;
    uint32_t w = b->width, h = b->height, d = b->depth ? b->depth : 1, layers = b->layers ? b->layers : 1;
    uint64_t n = (uint64_t)w * h * d * layers;
    if (n > 65536) return;
    vk_end_pass();
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    VkSpan out = vk_readback(n * 16);  // (room for 16-byte texels)
    VkBufferImageCopy cp = {.bufferOffset = out.off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layers}, .imageExtent = {w, h, d}};
    vkCmdCopyImageToBuffer(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, out.buf, 1, &cp);
    renderer_flush();
    const uint32_t *p = out.ptr;
    uint64_t nz = 0;
    for (uint64_t i = 0; i < n; i++) nz += p[i] != 0;
    LOG("[draw]   %s image %ux%ux%u x%u fmt %d: %llu/%llu words non-zero:", what, w, h, d, layers, (int)b->format,
        (unsigned long long)nz, (unsigned long long)n);
    for (int i = 0; i < 12 && i < (int)n; i++) LOG(" %08x", p[i]);
    LOG("\n");
}

// Debug frames, ODYSSEY_RAW_AT_FS=<fragment program address>: at the first draw of that program, every fragment
// texture and color target 0 (before and after the draw) as raw level-0 texels into build/raw_*.bin, for analysis
// offline (header: "RAWT", width, height, VkFormat, bytes per texel; then the rows).
static uint32_t vk_texel_bytes(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8_UNORM: return 1;
    case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_R16_UNORM: return 2;
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: case VK_FORMAT_R16G16_SFLOAT: case VK_FORMAT_R16G16_SNORM:
    case VK_FORMAT_R32_SFLOAT: return 4;
    case VK_FORMAT_R16G16B16A16_SFLOAT: return 8;
    case VK_FORMAT_R32G32B32A32_SFLOAT: return 16;
    default: return 0;
    }
}

static void debug_raw_dump(TexBackend *b, const char *path) {
    bool depth = b && (b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT);  // (depth: the depth aspect, 4 bytes a texel)
    uint32_t bpt = !b ? 0 : depth ? 4 : vk_texel_bytes(b->format);
    if (!bpt || !b->image || (!depth && (b->aspect & ~VK_IMAGE_ASPECT_COLOR_BIT))) {
        LOG("[vk] raw %s: not dumped (format %d)\n", path, b ? (int)b->format : -1);
        return;
    }
    for (uint32_t lv = 0; lv < (b->levels ? b->levels : 1); lv++) {  // (level 0: <path>, the others: <path>.lvN)
        uint32_t w = b->width >> lv ? b->width >> lv : 1, h = b->height >> lv ? b->height >> lv : 1;
        uint32_t dd = b->depth > 1 ? (b->depth >> lv ? b->depth >> lv : 1) : 1;  // (3D: all slices, height dd times)
        uint64_t size = (uint64_t)w * h * dd * bpt;
        vk_end_pass();
        st.rt_dirty = true;  // (the next draw begins the pass again)
        VkCommandBuffer cb = vk_cmd();
        vk_full_barrier(cb);
        VkSpan out = vk_readback(size);
        VkBufferImageCopy cp = {.bufferOffset = out.off,
                                .imageSubresource = {depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, lv, 0, 1},
                                .imageExtent = {w, h, dd}};
        vkCmdCopyImageToBuffer(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, out.buf, 1, &cp);
        renderer_flush();
        char p[300];
        if (lv) snprintf(p, sizeof p, "%s.lv%u", path, lv);
        else snprintf(p, sizeof p, "%s", path);
        FILE *f = fopen(p, "wb");
        if (!f) return;
        uint32_t hdr[5] = {0x54574152u, w, h * dd, (uint32_t)b->format, bpt};
        fwrite(hdr, 4, 5, f);
        fwrite(out.ptr, 1, size, f);
        fclose(f);
        LOG("[vk] raw %s: %ux%u format %d (levels %u, layers %u)\n", p, w, h, (int)b->format, b->levels, b->layers);
    }
}

// Debug frames, ODYSSEY_WATCH_MAX=<value>: after every draw into an R11G11B10F target, the target read back; the
// draws that first put a channel above <value> (and each later one that doubles the maximum) are logged with their
// programs and the pixel (which draw makes the absurdly bright spots).
static float uf_bits(uint32_t v, int mbits) {
    uint32_t e = (v >> mbits) & 31, m = v & ((1u << mbits) - 1);
    if (e == 31) return INFINITY;
    return e ? ldexpf(1.0f + (float)m / (float)(1u << mbits), (int)e - 15) : ldexpf((float)m / (float)(1u << mbits), -14);
}
static void debug_watch_max(void) {
    static float limit = -1;
    if (limit < 0) { const char *e = getenv("ODYSSEY_WATCH_MAX"); limit = e ? (float)atof(e) : 0; }
    if (limit <= 0 || !st.rt.ncolor || !st.rt.color[0] || !st.rt.color[0]->backend) return;
    TexBackend *b = st.rt.color[0]->backend;
    if (b->format != VK_FORMAT_B10G11R11_UFLOAT_PACK32 || !b->image) return;
    vk_end_pass();
    st.rt_dirty = true;
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    uint64_t n = (uint64_t)b->width * b->height;
    VkSpan out = vk_readback(n * 4);
    VkBufferImageCopy cp = {.bufferOffset = out.off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                            .imageExtent = {b->width, b->height, 1}};
    vkCmdCopyImageToBuffer(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, out.buf, 1, &cp);
    renderer_flush();
    const uint32_t *p = out.ptr;
    float mx = 0;
    uint64_t at = 0, over = 0;
    for (uint64_t i = 0; i < n; i++) {
        float r = uf_bits(p[i] & 0x7FF, 6), g = uf_bits((p[i] >> 11) & 0x7FF, 6), bl = uf_bits((p[i] >> 22) & 0x3FF, 5);
        float c = r > g ? (r > bl ? r : bl) : (g > bl ? g : bl);
        if (c > limit) over++;
        if (c > mx) mx = c, at = i;
    }
    static const TexBackend *last_b;
    static float last_mx;
    if (b != last_b) last_mx = 0, last_b = b;
    if (mx > limit && mx > 2 * last_mx)
        LOG("[watch] %ux%u max %.1f at (%llu,%llu), %llu texels over %g, after vs=%llx fs=%llx\n", b->width, b->height, mx,
            (unsigned long long)(at % b->width), (unsigned long long)(at / b->width), (unsigned long long)over, limit,
            (unsigned long long)st.program->stages[0].code_addr, (unsigned long long)st.program->stages[1].code_addr);
    if (mx > last_mx) last_mx = mx;
}

// ODYSSEY_TEX_TRACE=<texel address hex>:<from frame>:<to frame>: after every draw, copy and clear in that window,
// level 0 / layer 0 of the textures with that storage address read back; each change of its checksum is logged with
// what ran (which operation overwrites a texture).
NvnTexture *tex_trace_find(uint64_t addr);  // (objects.c)
void debug_tex_trace(const char *what) {
    static unsigned long long addr = ~0ull, from, to;
    if (addr == ~0ull) {
        const char *e = getenv("ODYSSEY_TEX_TRACE");
        addr = 0;
        if (e) sscanf(e, "%llx:%llu:%llu", &addr, &from, &to);
    }
    uint64_t f = renderer_present_count();
    if (!addr || f < from || f > to) return;
    static int per_op = -1;  // (without ODYSSEY_TEX_TRACE_OPS=1 once per frame, at present: then narrow the window)
    if (per_op < 0) per_op = getenv("ODYSSEY_TEX_TRACE_OPS") != NULL;
    if (!per_op && strcmp(what, "present")) return;
    NvnTexture *t = tex_trace_find(addr);
    TexBackend *b = t ? t->backend : NULL;
    uint32_t bpt = b ? vk_texel_bytes(b->format) : 0;
    if (!bpt || !b->image) return;
    uint64_t size = (uint64_t)b->width * b->height * bpt;
    bool reopen = in_pass;
    vk_end_pass();
    if (reopen) st.rt_dirty = true;
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    VkSpan out = vk_readback(size);
    VkBufferImageCopy cp = {.bufferOffset = out.off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                            .imageExtent = {b->width, b->height, 1}};
    vkCmdCopyImageToBuffer(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, out.buf, 1, &cp);
    renderer_flush();
    uint64_t h = 0xcbf29ce484222325ull;
    const uint8_t *p = out.ptr;
    for (uint64_t i = 0; i < size; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    static uint64_t last;
    if (h != last) {
        uint64_t sat = 0;  // (RGBA8: texels at alpha 255, the RGBM symptom; R11G11B10F: the largest channel)
        float mx = 0;
        if (b->format == VK_FORMAT_B10G11R11_UFLOAT_PACK32)
            for (uint64_t i = 0; i < size / 4; i++) {
                uint32_t v = ((const uint32_t *)p)[i];
                float r = uf_bits(v & 0x7FF, 6), g = uf_bits((v >> 11) & 0x7FF, 6), bl = uf_bits((v >> 22) & 0x3FF, 5);
                if (r > mx) mx = r;
                if (g > mx) mx = g;
                if (bl > mx) mx = bl;
            }
        else if (bpt == 4) for (uint64_t i = 3; i < size; i += 4) sat += p[i] == 255;
        LOG("[textrace] frame %llu %s: %p level 0 layer 0 changed (%016llx, %llu texels at alpha 255, max %.1f) vs=%llx fs=%llx\n",
            (unsigned long long)f, what, (void *)t, (unsigned long long)h, (unsigned long long)sat, mx,
            st.program ? (unsigned long long)st.program->stages[0].code_addr : 0ull,
            st.program ? (unsigned long long)st.program->stages[1].code_addr : 0ull);
        last = h;
    }
}

static int raw_state;  // 1: the textures and the target before are written, the target after is due
static char raw_prefix[32] = "build/raw_";  // (the n-th dump of a run: build/raw<n>_...)
static void debug_raw_after(void) {
    if (raw_state != 1) return;
    char path[64];
    snprintf(path, sizeof path, "%starget_after.bin", raw_prefix);
    if (st.rt.ncolor && st.rt.color[0] && st.rt.color[0]->backend) debug_raw_dump(st.rt.color[0]->backend, path);
    raw_state = 2;
}

// ODYSSEY_PROBE=grid: the same over an 8 x 6 grid of points spread over whatever color target 0 is (the centres of
// its cells), 8 bytes each; one line per draw that changed any: the cells (column,row) with old -> new. For finding
// which draw paints something on screen without knowing where it will be.
enum { PG_W = 8, PG_H = 6, PG_N = PG_W * PG_H };
static void debug_probe_grid(void) {
    if (!st.rt.ncolor || !st.rt.color[0] || !st.rt.color[0]->backend) return;
    extern __thread bool g_twin_redirect;
    TexBackend *b = g_twin_redirect ? vk_tex(st.rt.color[0], true) : st.rt.color[0]->backend;
    if ((b->aspect & ~VK_IMAGE_ASPECT_COLOR_BIT) || b->width < PG_W || b->height < PG_H) return;
    vk_end_pass();
    st.rt_dirty = true;
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    VkSpan out = vk_readback(PG_N * 16);
    VkBufferImageCopy cp[PG_N];
    for (int i = 0; i < PG_N; i++)
        cp[i] = (VkBufferImageCopy){.bufferOffset = out.off + (VkDeviceSize)i * 16, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                    .imageOffset = {(int32_t)((2 * (i % PG_W) + 1) * b->width / (2 * PG_W)),
                                                    (int32_t)((2 * (i / PG_W) + 1) * b->height / (2 * PG_H)), 0},
                                    .imageExtent = {1, 1, 1}};
    vkCmdCopyImageToBuffer(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, out.buf, PG_N, cp);
    renderer_flush();
    static uint64_t last[PG_N];
    static const TexBackend *last_b;
    char line[2400];
    int n = 0, changed = 0;
    for (int i = 0; i < PG_N; i++) {
        uint64_t v = *(const uint64_t *)((const uint8_t *)out.ptr + i * 16);
        uint64_t was = b == last_b ? last[i] : 0;
        if (v != was && n < (int)sizeof line - 64) {
            n += snprintf(line + n, sizeof line - n, " (%d,%d)%llx>%llx", i % PG_W, i / PG_W, (unsigned long long)was, (unsigned long long)v);
            changed++;
        }
        last[i] = v;
    }
    if (changed)
        LOG("[probe] %ux%u after vs=%llx fs=%llx: %d cells%s\n", b->width, b->height,
            (unsigned long long)st.program->stages[0].code_addr, (unsigned long long)st.program->stages[1].code_addr, changed,
            b == last_b ? line : " (new target)");
    last_b = b;
}

// Debug frames, ODYSSEY_PROBE=x,y[,target]: after every draw, one texel of that color target read back; a change is
// logged with the draw's programs (which draw paints a broken spot).
// Debug frames, ODYSSEY_PROBE_RG16F=1: after every draw into an RG16F colour target (format 0x11: the fluid surfaces'
// wave maps), the whole target read back; its largest |value| per channel and where, and how many texels aren't 0
static float dbg_h2f(uint16_t h) {
    uint32_t s = (uint32_t)(h & 0x8000) << 16, e = (h >> 10) & 0x1F, m = h & 0x3FF, u;
    if (!e) { float d = m * 5.9604644775390625e-8f; return s ? -d : d; }
    u = e == 31 ? (s | 0x7F800000u | (m << 13)) : (s | ((e + 112) << 23) | (m << 13));
    float f;
    memcpy(&f, &u, 4);
    return f;
}
static void debug_rg16f_max(void) {
    static int on = -1;
    if (on < 0) on = getenv("ODYSSEY_PROBE_RG16F") != NULL;
    if (!on || !st.rt.ncolor || !st.rt.color[0] || st.rt.color[0]->format != 0x11 || !st.rt.color[0]->backend) return;
    TexBackend *b = st.rt.color[0]->backend;
    if ((b->aspect & ~VK_IMAGE_ASPECT_COLOR_BIT) || b->width > 1024 || b->height > 1024) return;
    vk_end_pass();
    st.rt_dirty = true;
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    uint64_t n = (uint64_t)b->width * b->height;
    VkSpan out = vk_readback(n * 4);
    VkBufferImageCopy cp = {.bufferOffset = out.off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                            .imageExtent = {b->width, b->height, 1}};
    vkCmdCopyImageToBuffer(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, out.buf, 1, &cp);
    renderer_flush();
    const uint16_t *px = out.ptr;
    float mx[2] = {0, 0};
    uint64_t at[2] = {0, 0}, nonzero = 0;
    for (uint64_t i = 0; i < n; i++)
        for (int c = 0; c < 2; c++) {
            float v = fabsf(dbg_h2f(px[i * 2 + c]));
            if (v > mx[c]) mx[c] = v, at[c] = i;
            if (c == 0 && (px[i * 2] | px[i * 2 + 1])) nonzero++;
        }
    LOG("[rg16f] flags %#x stride %llu addr %llx\n", st.rt.color[0]->flags, (unsigned long long)st.rt.color[0]->stride,
        (unsigned long long)st.rt.color[0]->addr);
    LOG("[rg16f] %ux%u %p after vs=%llx fs=%llx: max |r| %g (r %g g %g there) at (%llu,%llu), |g| %g at (%llu,%llu), %llu texels not 0\n",
        b->width, b->height, (void *)st.rt.color[0], (unsigned long long)st.program->stages[0].code_addr,
        (unsigned long long)st.program->stages[1].code_addr, mx[0], dbg_h2f(px[at[0] * 2]), dbg_h2f(px[at[0] * 2 + 1]),
        (unsigned long long)(at[0] % b->width),
        (unsigned long long)(at[0] / b->width), mx[1], (unsigned long long)(at[1] % b->width),
        (unsigned long long)(at[1] / b->width), (unsigned long long)nonzero);
}

static void debug_probe(void) {
    static int px = -1, py, att, grid;
    if (px == -1) {
        const char *e = getenv("ODYSSEY_PROBE");
        px = -2;
        if (e && !strcmp(e, "grid")) grid = 1;
        else if (e) sscanf(e, "%d,%d,%d", &px, &py, &att);
    }
    if (grid) { debug_probe_grid(); return; }
    if (px < 0 || att >= st.rt.ncolor || !st.rt.color[att] || !st.rt.color[att]->backend) return;
    extern __thread bool g_twin_redirect;  // (a replay draws into twins)
    TexBackend *b = g_twin_redirect ? vk_tex(st.rt.color[att], true) : st.rt.color[att]->backend;
    if ((uint32_t)px >= b->width || (uint32_t)py >= b->height || (b->aspect & ~VK_IMAGE_ASPECT_COLOR_BIT)) return;
    vk_end_pass();
    st.rt_dirty = true;  // (the next draw begins the pass again)
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    VkSpan out = vk_readback(16);
    VkBufferImageCopy cp = {.bufferOffset = out.off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                            .imageOffset = {px, py, 0}, .imageExtent = {1, 1, 1}};
    vkCmdCopyImageToBuffer(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, out.buf, 1, &cp);
    renderer_flush();
    static uint32_t last;
    static const TexBackend *last_b;
    uint32_t v = *(const uint32_t *)out.ptr;
    if (b != last_b || v != last)
        LOG("[probe] %ux%u target %d (%d,%d): %08x -> %08x after vs=%llx fs=%llx\n", b->width, b->height, att, px, py,
            b == last_b ? last : 0, v, (unsigned long long)st.program->stages[0].code_addr,
            (unsigned long long)st.program->stages[1].code_addr);
    last = v, last_b = b;
}

static NvnTexture *dbg_color[8], *dbg_depth;  // debug frames: the open pass's attachments (begin_pass)
static int dbg_ncolor, dbg_draws;
static void vk_end_pass_at(int line);
void vk_end_pass(void) { vk_end_pass_at(0); }
#define vk_end_pass() vk_end_pass_at(__LINE__)
static NvnTexture *pass_lin[8];  // the open pass's linear colour targets (a pitch, in the guest heap): written back
static int pass_nlin;
static void linear_writeback(void);
static void vk_end_pass_at(int line) {
    if (!in_pass) return;
    if (prof && line < 4096) prof_end_line[line]++;
    vkCmdEndRendering(vk_cmd());
    in_pass = false;
    if (pass_nlin) linear_writeback();
    // (the pass's own attachments, from begin_pass: st.rt may already hold the next pass's targets; none while probing:
    // every draw ends its pass then)
    static int probing = -1;  // (ODYSSEY_DEBUG_NO_DUMPS=1: the log only, e.g. over many frames)
    if (probing < 0) probing = getenv("ODYSSEY_PROBE") || getenv("ODYSSEY_DEBUG_NO_DUMPS");
    static int depth_dumps = -1;  // (ODYSSEY_DEBUG_DEPTH: depth-only passes too, as pass_NNN_d.png)
    if (depth_dumps < 0) depth_dumps = getenv("ODYSSEY_DEBUG_DEPTH") != NULL;
    bool color = dbg_ncolor && dbg_color[0] && dbg_color[0]->backend, depth_only = !color && depth_dumps && dbg_depth && dbg_depth->backend;
    if (g_debug_frame && !probing && (color || depth_only)) {
        char path[256];
        static uint64_t debug_pass_frame = ~0ull;
        static int multi = -1;
        if (multi < 0) multi = getenv("ODYSSEY_DEBUG_FRAMES") != NULL;
        if (multi && renderer_present_count() != debug_pass_frame) { debug_pass_frame = renderer_present_count(); debug_pass_no = 0; }
        if (multi) snprintf(path, sizeof path, "build/pass_%llu_%03d.png", (unsigned long long)debug_pass_frame, debug_pass_no);
        else snprintf(path, sizeof path, "build/pass_%03d.png", debug_pass_no);
        LOG("[vk] end of pass %d (%d draws)\n", debug_pass_no, dbg_draws);
        if (color) renderer_save_texture_tex(dbg_color[0]->backend, path);
        for (int i = 1; i < dbg_ncolor; i++)  // (the other targets of an MRT pass: pass_NNN_cI.png)
            if (dbg_color[i] && dbg_color[i]->backend) {
                size_t n = strlen(path) - 4;
                snprintf(path + n, sizeof path - n, "_c%d.png", i);
                renderer_save_texture_tex(dbg_color[i]->backend, path);
                path[n] = 0, strcat(path, ".png");
            }
        if (dbg_depth && dbg_depth->backend && getenv("ODYSSEY_DEBUG_DEPTH")) {  // pass_NNN_d.png
            size_t n = strlen(path) - 4;
            snprintf(path + n, sizeof path - n, "_d.png");
            debug_depth_dump(dbg_depth->backend, path);
        }
        debug_pass_no++;
    }
}

static uint32_t level_extent(uint32_t v, uint32_t lv) { v >>= lv; return v ? v : 1; }

static void begin_pass(void) {
    if (in_pass && !st.rt_dirty) return;
    vk_end_pass();
    if (dl_on == 2) renderer_flush();  // (ODYSSEY_VK_DEVLOST_TRACE=2: each pass its own submission)
    st.rt_dirty = false;
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);  // passes, copies and dispatches see each other's results
    {
        NvnTexture *t0n = st.rt.ncolor ? st.rt.color[0] : NULL;
        dl_note("pass %d color %ux%u fmt %#x depth %p", st.rt.ncolor, t0n ? t0n->width : 0, t0n ? t0n->height : 0, t0n ? t0n->format : 0,
                (void *)st.rt.depth);
    }
    if (g_gpuprof_frame) {
        NvnTexture *t0 = st.rt.ncolor ? st.rt.color[0] : NULL;
        gp_mark(cb, "pass: %d color %ux%u fmt %#x, depth %s", st.rt.ncolor, t0 ? t0->width : 0, t0 ? t0->height : 0,
                t0 ? t0->format : 0, st.rt.depth ? "yes" : "no");
    }
    VkRenderingAttachmentInfo ca[8], da = {0}, sa = {0};
    uint32_t w = 0, minw = UINT32_MAX, minh = UINT32_MAX;  // w: first attachment seen
    pass_ncolor = (uint32_t)st.rt.ncolor;
    pass_samples = 1;
    for (int i = 0; i < st.rt.ncolor; i++) {
        NvnTexture *t = st.rt.color[i];
        ca[i] = (VkRenderingAttachmentInfo){.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        pass_color[i] = VK_FORMAT_UNDEFINED;
        if (!t) continue;
        renderer_mark_gpu_owned(t);
        if (((t->flags & 0x10) || t->stride) && log_linear()) {  // (a linear texture drawn into: the CPU may read it - PaintObj)
            static int n;
            if (n++ < 200) LOG("[linear] draw into linear %p addr %llx %ux%u f%#x stride %llu (frame %llu)\n", (void *)t,
                               (unsigned long long)t->addr, t->width, t->height, t->format, (unsigned long long)t->stride,
                               (unsigned long long)renderer_present_count());
        }
        if ((t->target == 8 || t->target == 9) && getenv("ODYSSEY_LOG_CUBES")) {  // (who draws into cube maps)
            static int n;
            const NvnTextureView *cv = &st.rt.cview[i];
            if (n++ < 20000)
                LOG("[cube] draw target %p addr %llx %ux%u f%#x: view %s level %u layer %u (frame %llu)\n", (void *)t,
                    (unsigned long long)t->addr, t->width, t->height, t->format, st.rt.has_cview[i] ? "yes" : "no",
                    (cv->setmask & 1) ? cv->base_level : 0, (cv->setmask & 2) ? cv->min_layer : 0,
                    (unsigned long long)renderer_present_count());
        }
        VkImageView v = vk_view(t, &st.rt.cview[i], st.rt.has_cview[i], true, TEXTYPE_2D);
        TexBackend *b = t->backend;
        if (!v) continue;
        ca[i].imageView = v;
        ca[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        ca[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        bool fresh = (int)renderer_texture_level_fresh(t) | (int)renderer_texture_frame_fresh(t);  // both: each marks its state
        if (renderer_texture_first_write(t) || fresh) {
            ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            ca[i].clearValue.color.float32[3] = fresh ? 0.0f : 1.0f;
        }
        pass_color[i] = st.rt.has_cview[i] && (st.rt.cview[i].setmask & 4) && vk_format(t->format) == b->format
                      ? vk_format(st.rt.cview[i].format) : b->format;
        uint32_t lv = st.rt.has_cview[i] && (st.rt.cview[i].setmask & 1) ? st.rt.cview[i].base_level : 0;
        uint32_t ew = level_extent(b->width, lv), eh = level_extent(b->height, lv);
        if (!w) { renderer_texture_scale(t, &pass_sx, &pass_sy); w = ew; }
        if (ew < minw) minw = ew;
        if (eh < minh) minh = eh;
        pass_samples = b->samples;
    }
    pass_depth = pass_stencil = VK_FORMAT_UNDEFINED;
    bool has_d = false, has_s = false;
    if (st.rt.depth) {
        renderer_mark_gpu_owned(st.rt.depth);
        VkImageView v = vk_view(st.rt.depth, &st.rt.dview, st.rt.has_dview, true, TEXTYPE_2D);
        TexBackend *b = st.rt.depth->backend;
        if (v && renderer_format_is_depth(st.rt.depth->format)) {
            VkRenderingAttachmentInfo a = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = v,
                                           .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                           .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
            if (b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) { da = a; has_d = true; pass_depth = b->format; }
            if (b->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) { sa = a; has_s = true; pass_stencil = b->format; }
            uint32_t lv = st.rt.has_dview && (st.rt.dview.setmask & 1) ? st.rt.dview.base_level : 0;
            uint32_t ew = level_extent(b->width, lv), eh = level_extent(b->height, lv);
            if (!w) { renderer_texture_scale(st.rt.depth, &pass_sx, &pass_sy); w = ew; }
            if (ew < minw) minw = ew;
            if (eh < minh) minh = eh;
            if (!pass_ncolor) pass_samples = b->samples;
        }
    }
    if (!w) { pass_sx = pass_sy = 1; minw = minh = 16; }
    {   // (output resolution) every attachment of a pass needs the same scale: one at another scale clips the render area
        // to the smaller one - the scene in a corner of the image (output_scale_texture misses a target)
        static int nmixed;
        float asx, asy;
        for (int i = 0; i < st.rt.ncolor + 1 && nmixed < 20; i++) {
            NvnTexture *t = i < st.rt.ncolor ? st.rt.color[i] : st.rt.depth;
            if (!t || !t->backend) continue;
            renderer_texture_scale(t, &asx, &asy);
            if (asx != pass_sx || asy != pass_sy)
                LOG("[scale] pass mixes scales: %s %d %ux%u f%#x flags %#x target %u at %.3f, pass at %.3f\n",
                    i < st.rt.ncolor ? "color" : "depth", i, t->width, t->height, t->format, t->flags, t->target, asx, pass_sx), nmixed++;
        }
    }
    pass_w = minw; pass_h = minh;
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {minw, minh}}, .layerCount = 1,
                          .colorAttachmentCount = pass_ncolor, .pColorAttachments = ca,
                          .pDepthAttachment = has_d ? &da : NULL, .pStencilAttachment = has_s ? &sa : NULL};
    vk_checkpoint(cb, "pass %d color, %ux%u, depth %p", pass_ncolor, minw, minh, (void *)st.rt.depth);
    vkCmdBeginRendering(cb, &ri);
    ss_forget();  // (whatever ran between the passes may have set its own state)
    pass_nlin = 0;
    for (int i = 0; i < st.rt.ncolor && i < 8; i++) {
        NvnTexture *t = st.rt.color[i];
        if (t && (t->flags & 0x10) && t->stride && t->addr >= HEAP_BASE && t->addr < HEAP_BASE + HEAP_MAX)
            pass_lin[pass_nlin++] = t;
    }
    if (g_debug_frame) {
        dbg_ncolor = st.rt.ncolor < 8 ? st.rt.ncolor : 8;
        for (int i = 0; i < dbg_ncolor; i++) dbg_color[i] = st.rt.color[i];
        dbg_depth = st.rt.depth;
        dbg_draws = 0;
    }
    in_pass = true;
    pass_color_drawn = false;
    vp_sx = pass_sx; vp_sy = pass_sy; vp_ox = 0; vp_oy = 0;
    stat_passes++;
}

static void pass_size(int *w, int *h) {
    *w = *h = 0;
    for (int i = 0; i < st.rt.ncolor && !*w; i++)
        if (st.rt.color[i]) { *w = (int)st.rt.color[i]->width; *h = (int)st.rt.color[i]->height; }
    if (!*w && st.rt.depth) { *w = (int)st.rt.depth->width; *h = (int)st.rt.depth->height; }
    *w = (int)lroundf(*w * pass_sx);
    *h = (int)lroundf(*h * pass_sy);
}

static VkRect2D scissor_rect(void) {
    int w, h;
    pass_size(&w, &h);
    if (!w) return (VkRect2D){{0, 0}, {pass_w, pass_h}};
    int x = st.scissor[0][0], y = st.scissor[0][1], sw = st.scissor[0][2], sh = st.scissor[0][3];
    if (vp_sx != 1 || vp_sy != 1 || vp_ox != 0 || vp_oy != 0) {
        double x0 = floor(x * (double)vp_sx + vp_ox), y0 = floor(y * (double)vp_sy + vp_oy);
        double x1 = ceil(((double)x + sw) * vp_sx + vp_ox), y1 = ceil(((double)y + sh) * vp_sy + vp_oy);
        if (sw >= 16384) x1 = 1e9;
        if (sh >= 16384) y1 = 1e9;
        if (x1 > 1e9) x1 = 1e9;
        if (y1 > 1e9) y1 = 1e9;
        x = (int)x0; y = (int)y0; sw = (int)(x1 - x0); sh = (int)(y1 - y0);
    }
    if (x < 0) { sw += x; x = 0; }
    if (y < 0) { sh += y; y = 0; }
    if (w > (int)pass_w) w = (int)pass_w;
    if (h > (int)pass_h) h = (int)pass_h;
    if (x >= w || y >= h) { x = y = 0; sw = sh = 0; }
    if (x + sw > w) sw = w - x;
    if (y + sh > h) sh = h - y;
    if (sw < 0) sw = 0;
    if (sh < 0) sh = 0;
    return (VkRect2D){{x, y}, {(uint32_t)sw, (uint32_t)sh}};
}

static void apply_scissor(void) {
    VkRect2D r = scissor_rect();
    if (!ss_need(SS_SC, !memcmp(&r, &ss.sc, sizeof r))) return;
    ss.sc = r;
    vkCmdSetScissor(vk_cmd(), 0, 1, &r);
}

// Metal convention (y up in NDC, origin top-left) through a negative-height viewport.
static void set_viewport(float x, float y, float w, float h, float zn, float zf) {
    if (w <= 0) w = 1;
    if (h == 0) h = 1;
    VkViewport vp = {x, y + h, w, -h, fminf(fmaxf(zn, 0), 1), fminf(fmaxf(zf, 0), 1)};
    if (!ss_need(SS_VP, !memcmp(&vp, &ss.vp, sizeof vp))) return;
    ss.vp = vp;
    vkCmdSetViewport(vk_cmd(), 0, 1, &vp);
}

// ================================================================== clears
static VkPipelineLayout pc_layout(void) {  // push constants only (clears)
    static VkPipelineLayout l;
    if (!l) {
        VkPushConstantRange r = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
        VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .pushConstantRangeCount = 1,
                                         .pPushConstantRanges = &r};
        VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &l));
    }
    return l;
}

static const VkDynamicState DYN[] = {
    VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
    VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
    VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,
    VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
    VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE,
    VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE};
#define NDYN (sizeof DYN / sizeof DYN[0])

// Fixed-function state that isn't part of a pipeline is set explicitly on every bind (it persists across pipelines).
static void dyn_defaults(VkCommandBuffer cb) {
    vkCmdSetDepthBias(cb, 0, 0, 0);
    vkCmdSetDepthBiasEnable(cb, VK_FALSE);
    float bc[4] = {0, 0, 0, 0};
    vkCmdSetBlendConstants(cb, bc);
    vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0xFF);
    vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0xFF);
    vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
    vkCmdSetCullMode(cb, VK_CULL_MODE_NONE);
    vkCmdSetFrontFace(cb, VK_FRONT_FACE_COUNTER_CLOCKWISE);
    vkCmdSetPrimitiveTopology(cb, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    vkCmdSetDepthTestEnable(cb, VK_FALSE);
    vkCmdSetDepthWriteEnable(cb, VK_FALSE);
    vkCmdSetDepthCompareOp(cb, VK_COMPARE_OP_ALWAYS);
    vkCmdSetStencilTestEnable(cb, VK_FALSE);
    vkCmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP,
                      VK_COMPARE_OP_ALWAYS);
    vkCmdSetPrimitiveRestartEnable(cb, VK_FALSE);
}

typedef struct { uint64_t key; VkPipeline pipe; } PipeEnt;
#define CLEAR_TAB 256
static PipeEnt clear_tab[CLEAR_TAB];

static VkPipeline clear_pipeline(int index, int kind, uint32_t mask, bool depth, bool stencil) {
    uint64_t key = 0xcbf29ce484222325ull;
#define MIX(v) do { uint64_t v_ = (uint64_t)(v); for (int b_ = 0; b_ < 8; b_++) { key ^= (v_ >> (b_ * 8)) & 0xFF; key *= 0x100000001B3ull; } } while (0)
    MIX(index); MIX(kind); MIX(mask); MIX(depth); MIX(stencil); MIX(pass_ncolor); MIX(pass_depth); MIX(pass_stencil); MIX(pass_samples);
    for (uint32_t i = 0; i < pass_ncolor; i++) MIX(pass_color[i]);
    uint32_t slot = (uint32_t)key & (CLEAR_TAB - 1);
    for (int p = 0; p < CLEAR_TAB; p++, slot = (slot + 1) & (CLEAR_TAB - 1)) {
        if (clear_tab[slot].key == key) return clear_tab[slot].pipe;
        if (!clear_tab[slot].key) break;
    }
    char fsname[32];
    if (index >= 0) snprintf(fsname, sizeof fsname, "clear.frag.%c%d", kind == 1 ? 'i' : kind == 2 ? 'u' : 'f', index);
    else snprintf(fsname, sizeof fsname, "clear.frag.none");
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vk_builtin_shader("clear.vert"), .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = vk_builtin_shader(fsname), .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = (VkSampleCountFlagBits)pass_samples};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState cba[8] = {0};
    for (uint32_t i = 0; i < pass_ncolor; i++)
        if ((int)i == index) cba[i].colorWriteMask = mask & 0xF;
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .attachmentCount = pass_ncolor, .pAttachments = cba};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = NDYN, .pDynamicStates = DYN};
    VkPipelineRenderingCreateInfo rinfo = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = pass_ncolor,
                                           .pColorAttachmentFormats = pass_color, .depthAttachmentFormat = pass_depth,
                                           .stencilAttachmentFormat = pass_stencil};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &rinfo, .stageCount = 2,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = pc_layout()};
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &p) != VK_SUCCESS) LOG("[vk] clear pipeline failed\n");
    clear_tab[slot].key = key;
    clear_tab[slot].pipe = p;
    return p;
}

static void clear(int index, int kind, const void *color, uint32_t mask, bool depth, float dval, bool stencil, uint32_t sval,
                  uint32_t smask) {
    begin_pass();
    VkCommandBuffer cb = vk_cmd();
    vp_sx = pass_sx; vp_sy = pass_sy; vp_ox = 0; vp_oy = 0;
    VkRect2D rect = scissor_rect();
    if (!rect.extent.width || !rect.extent.height) return;
    if (index >= 0 && (index >= (int)pass_ncolor || pass_color[index] == VK_FORMAT_UNDEFINED)) return;
    bool simple = (index < 0 || (mask & 0xF) == 0xF) && (!stencil || (smask & 0xFF) == 0xFF);
    if (simple) {
        VkClearAttachment a[2];
        uint32_t n = 0;
        if (index >= 0) {
            a[n] = (VkClearAttachment){.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .colorAttachment = (uint32_t)index};
            memcpy(a[n].clearValue.color.float32, color, 16);
            n++;
        }
        VkImageAspectFlags dsa = (depth && pass_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | (stencil && pass_stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        if (dsa) a[n++] = (VkClearAttachment){.aspectMask = dsa, .clearValue.depthStencil = {dval, sval}};
        VkClearRect cr = {rect, 0, 1};
        if (n) vkCmdClearAttachments(cb, n, a, 1, &cr);
        return;
    }
    VkPipeline p = clear_pipeline(index, kind, mask, depth, stencil);
    if (!p) return;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    ss_forget();  // (its own pipeline and state: the next draw sets everything again)
    dyn_defaults(cb);
    if (depth) { vkCmdSetDepthTestEnable(cb, VK_TRUE); vkCmdSetDepthWriteEnable(cb, VK_TRUE); }
    if (stencil) {
        vkCmdSetStencilTestEnable(cb, VK_TRUE);
        vkCmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_REPLACE,
                          VK_COMPARE_OP_ALWAYS);
        vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, smask);
        vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, sval);
    }
    int w, h;
    pass_size(&w, &h);
    VkViewport vp = {0, 0, (float)(w ? w : (int)pass_w), (float)(h ? h : (int)pass_h), 0, 1};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &rect);
    float pc[8] = {0};
    if (color) memcpy(pc, color, 16);
    pc[4] = dval;
    vkCmdPushConstants(cb, pc_layout(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32, pc);
    vkCmdDraw(cb, 3, 1, 0, 0);
}

// ================================================================== programs
#define MAX_BIND 96
struct Prog {
    ShaderInfo *vi, *fi;
    uint64_t code[2], cmax[2];  // translation inputs (copied: translated on the translator thread)
    ShaderKey vkey, fkey;
    VkShaderModule vs, fs;
    VkBindingInfo binds[MAX_BIND];
    int nbinds;
    VkDescriptorSetLayout set_layout[2];
    VkPipelineLayout layout;
    bool vs_uses_vid;
    VkPipeline lib_pr[2], lib_fs;  // (g_vk.gpl) the compiled shaders as pipeline libraries: vertex (line / fill), fragment
    bool no_gpl;                   // libraries failed: whole pipelines
    // Its build (whoever gets to a step first does it: a queued job, or a draw that needs it now):
    _Atomic int step;       // PSTEP_*
    _Atomic bool urgent;    // a draw waits for it: its next step goes to the urgent queues
    _Atomic int urged;      // (the step an urgent job was queued for + 1)
    bool ahead;             // set up ahead of its first draw (renderer_program_hint): failures logged once one needs it
    bool fail_told, drawn, cache_tried, frame_hold_recorded;
    bool huge;              // a shader the drivers took minutes and GBs for: see translate_program
    uint64_t spv_key[2];    // its SPIR-V (the disk cache's names, shaders.c): the key of its saved libraries (pbin_path)
    uint64_t code_sum[2];   // (ahead) the start of both shaders then: different when translated = the memory was reused
    _Atomic int state;  // 0 building, 1 ready, 2 failed
};
// translation, then SPIR-V + modules (compile), then (g_vk.gpl) the libraries; programs set up ahead stop before
// the libraries until a draw needs them (PSTEP_LIBS: see compile_program)
enum { PSTEP_TRANSLATE, PSTEP_TRANSLATING, PSTEP_COMPILE, PSTEP_COMPILING, PSTEP_LIBS, PSTEP_LIBSING, PSTEP_DONE };
static void make_shader_libs(Prog *p, bool cached_only);
static void huge_link_test(Prog *p);
static __thread bool t_build_here;  // (blocking builds) a step goes on with the next one itself

static VkPipelineLayout make_layout(Prog *p, int nsets, VkShaderStageFlags s0, VkShaderStageFlags s1) {
    p->set_layout[0] = vk_set_layout(p->binds, p->nbinds, 0, s0);
    if (nsets > 1) p->set_layout[1] = vk_set_layout(p->binds, p->nbinds, 1, s1);
    // graphics: the vertex attribute kinds (GLSL contract in shader.h), the same range in every layout so the values
    // stay across pipeline binds
    VkPushConstantRange pc = {VK_SHADER_STAGE_VERTEX_BIT, 0, 16};
    VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = (uint32_t)nsets,
                                     .pSetLayouts = p->set_layout, .pushConstantRangeCount = nsets > 1, .pPushConstantRanges = &pc};
    VkPipelineLayout l;
    VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &l));
    return l;
}

static void save_glsl(const char *src, uint64_t addr, const char *suffix) {
    if (!getenv("ODYSSEY_SAVE_MSL")) return;
    mkdir("build/glsl", 0755);
    char path[256];
    snprintf(path, sizeof path, "build/glsl/%llx_%s.glsl", (unsigned long long)addr, suffix);
    FILE *f = fopen(path, "w");
    if (f) { fputs(src, f); fclose(f); }
}

static void prog_failed(Prog *p) {
    atomic_store(&p->step, PSTEP_DONE);
    atomic_store(&p->state, 2);
}

// A program's shader modules, made when its libraries or pipelines are built - not when it is set up. The game sets
// up ~15,000 programs ahead (every material of every model a stage loads, every shader of an effect archive), most
// never drawn; their modules alone held ~4.7 GB in the NVIDIA driver (2026-11-13, Cascade: 11.2 GB in RAM with them
// made at set-up, 6.5 GB without setting programs up at all). The SPIR-V is in the cache on disk (cache/spirv), read
// back here; if a write to it ever failed, modules are made at set-up as before. ODYSSEY_EAGER_MODULES=1: as before.
extern _Atomic bool vk_spirv_cache_failed;
static pthread_mutex_t prog_mod_lock = PTHREAD_MUTEX_INITIALIZER;
static bool prog_modules(Prog *p) {
    pthread_mutex_lock(&prog_mod_lock);
    if ((!p->vs || !p->fs) && p->spv_key[0] && p->spv_key[1]) {
        size_t nv = 0, nf = 0;
        uint32_t *sv = vk_spirv_by_key(p->spv_key[0], &nv), *sf = sv ? vk_spirv_by_key(p->spv_key[1], &nf) : NULL;
        if (sv && sf) {
            if (!p->vs) p->vs = vk_module(sv, nv);
            if (!p->fs) p->fs = vk_module(sf, nf);
        } else {
            static int n;
            if (n++ < 10) LOG("[shader] SPIR-V of vs=%llx fs=%llx not in the cache any more: program unavailable\n",
                              (unsigned long long)p->code[0], (unsigned long long)p->code[1]);
        }
        free(sv);
        free(sf);
    }
    bool ok = p->vs && p->fs;
    pthread_mutex_unlock(&prog_mod_lock);
    return ok;
}
static bool lazy_modules(void) {
    static int eager = -1;
    if (eager < 0) eager = getenv("ODYSSEY_EAGER_MODULES") || getenv("ODYSSEY_VK_NO_SPIRV_CACHE") || getenv("ODYSSEY_SHADER_SALT");
    return !eager && !atomic_load(&vk_spirv_cache_failed);
}

static bool preload_room(void);
static void compile_program(void *arg) {
    Prog *p = arg;
    int e = PSTEP_COMPILE;
    if (!atomic_compare_exchange_strong(&p->step, &e, PSTEP_COMPILING)) return;  // (done by someone else)
    size_t nv = 0, nf = 0;
    uint64_t t0 = plat_now_ns();
    uint32_t *sv = vk_glsl_to_spirv(p->vi->msl, STAGE_VERTEX, "vs", &nv);
    p->spv_key[0] = vk_spirv_last_key;
    uint32_t *sf = sv ? vk_glsl_to_spirv(p->fi->msl, STAGE_FRAGMENT, "fs", &nf) : NULL;
    p->spv_key[1] = sf ? vk_spirv_last_key : 0;
    shader_time_log("spirv vs+fs", p->code[1], t0);
    // the sources aren't needed any more (hundreds of programs are set up ahead: tens of KB each)
    free(p->vi->msl); p->vi->msl = NULL;
    free(p->fi->msl); p->fi->msl = NULL;
    if (!sv || !sf) { free(sv); free(sf); prog_failed(p); return; }
    p->nbinds = vk_reflect(sv, nv, p->binds, MAX_BIND);
    p->nbinds += vk_reflect(sf, nf, p->binds + p->nbinds, MAX_BIND - p->nbinds);
    bool lazy = lazy_modules();
    if (!lazy) {
        p->vs = vk_module(sv, nv);
        p->fs = vk_module(sf, nf);
    }
    free(sv);
    free(sf);
    p->layout = make_layout(p, 2, VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT);
    atomic_fetch_add(&stat_compiles, 1);
    if (!lazy && (!p->vs || !p->fs)) { prog_failed(p); return; }
    if (p->huge) {
        static int n;
        if (n++ < 8) LOG("[shader] huge program vs=%llx fs=%llx (SPIR-V %zu + %zu KB): compiled unoptimized when drawn\n",
                         (unsigned long long)p->code[0], (unsigned long long)p->code[1], nv / 256, nf / 256);
    }
    // Set up ahead and no draw waits for it: the libraries (the driver's compile) wait for its first draw. NVIDIA keeps
    // ~17 MB per pair of libraries never linked, and also when they are destroyed (2026-10-27, driver 610.88): ~800
    // programs set up ahead in a stage load, half of them drawn, cost +5 GB. Translation and SPIR-V are cheap; the modules
    // aren't (prog_modules: made at the first build).
    static int huge_ahead = -1;  // (ODYSSEY_HUGE_AHEAD=1, tests: built ahead anyway, to time its compile)
    if (huge_ahead < 0) huge_ahead = getenv("ODYSSEY_HUGE_AHEAD") != NULL;
    // (menu, Video: Preload area shaders - the libraries of everything a stage sets up, right away: from the saved
    // binaries a few ms each on the low-priority workers, so nothing is left to load at a first draw)
    bool preload = g_settings.shader_preload && !settings_low_memory() && !g_vk.headless && preload_room();
    if (g_vk.gpl && p->ahead && ((!atomic_load(&p->urgent) && !preload) || (p->huge && !p->drawn && !huge_ahead))) {
        atomic_store(&p->step, PSTEP_LIBS);
        return;
    }
    if (g_vk.gpl) {
        atomic_store(&p->step, PSTEP_LIBSING);
        make_shader_libs(p, false);
    }
    if (p->huge && huge_ahead) huge_link_test(p);
    atomic_store(&p->step, PSTEP_DONE);
    atomic_store(&p->state, 1);
}

// Preload area shaders, within a memory budget: the driver keeps its libraries for good (NVIDIA: ~17 MB a program,
// also once destroyed), and most programs a stage sets up are never drawn - with it on, a few stages took the process
// to 48 GB in RAM (61 GB committed; 10 GB without it, 2026-11-12). It stops once the process holds a quarter of the
// machine's RAM or 8 GB, whichever is less (ODYSSEY_PRELOAD_MEM_MB=<MB> sets it); from then on a program's libraries
// are built at its first draw (from the driver's cache: 1-3 ms), as without the setting.
static bool preload_room(void) {
    static uint64_t budget, checked_ns;
    static _Atomic bool full;
    if (!budget) {
        const char *e = getenv("ODYSSEY_PRELOAD_MEM_MB");
        uint64_t ram = plat_total_ram(), b = ram / 4;
        if (!b || b > (8ull << 30)) b = 8ull << 30;
        budget = e ? strtoull(e, NULL, 10) << 20 : b;
    }
    uint64_t now = plat_now_ns();
    if (now - checked_ns < 200000000ull) return !atomic_load(&full);  // (looked at 5 times a second)
    checked_ns = now;
    uint64_t used = plat_resident_bytes();
    bool over = used && used > budget;
    if (over && !atomic_exchange(&full, true))
        LOG("[shader] preloading stopped: the game holds %llu MB in RAM (budget %llu MB); libraries build at first draws\n",
            (unsigned long long)(used >> 20), (unsigned long long)(budget >> 20));
    if (!over) atomic_store(&full, false);
    return !over;
}

// The libraries of a program set up ahead, for its first draw (a job, or the draw itself while builds block)
static void build_libs(void *arg) {
    Prog *p = arg;
    int e = PSTEP_LIBS;
    if (!atomic_compare_exchange_strong(&p->step, &e, PSTEP_LIBSING)) return;
    make_shader_libs(p, false);
    atomic_store(&p->step, PSTEP_DONE);
    atomic_store(&p->state, 1);  // (no libraries: whole pipelines, from its modules)
}

// ... or right in the draw when the driver has them already (pipeline cache, its own shader cache: 1-3 ms, no compile)
static bool libs_from_cache(Prog *p) {
    if (!g_vk.cache_control || p->cache_tried) return false;
    p->cache_tried = true;
    int e = PSTEP_LIBS;
    if (!atomic_compare_exchange_strong(&p->step, &e, PSTEP_LIBSING)) return false;
    make_shader_libs(p, true);
    bool ok = p->lib_pr[1] && p->lib_fs;
    if (ok) atomic_store(&p->state, 1);
    atomic_store(&p->step, ok ? PSTEP_DONE : PSTEP_LIBS);
    return ok;
}

static uint64_t hmix(uint64_t h, const void *p, size_t n);
static uint64_t code_sum(uint64_t code) { return hmix(1469598103934665603ull, (const void *)(uintptr_t)(code + 0x80), 64); }

// A failed translation is logged when a draw needs the program (not for the thousands compiled ahead, most never drawn).
static void prog_tell_failure(Prog *p) {
    if (p->fail_told || !p->vi) return;
    p->fail_told = true;
    if (!p->vi->msl && p->vi->error[0]) LOG("[shader] VS translate failed: %s (code %llx)\n", p->vi->error, (unsigned long long)p->code[0]);
    else if (p->fi && !p->fi->msl && p->fi->error[0])
        LOG("[shader] FS translate failed: %s (code %llx)\n", p->fi->error, (unsigned long long)p->code[1]);
}

static void translate_program(void *arg) {
    Prog *p = arg;
    int e = PSTEP_TRANSLATE;
    if (!atomic_compare_exchange_strong(&p->step, &e, PSTEP_TRANSLATING)) return;
    if (p->ahead && (code_sum(p->code[0]) != p->code_sum[0] || code_sum(p->code[1]) != p->code_sum[1])) {
        static int n;
        if (n++ < 10) LOG("[prog] shader code at %llx / %llx changed since it was set up (translating what is there now)\n",
                          (unsigned long long)p->code[0], (unsigned long long)p->code[1]);
    }
    maxwell_gmem_base = HEAP_BASE;
    maxwell_gmem_size = HEAP_MAX;
    uint64_t t0 = plat_now_ns();
    p->vi = maxwell_translate((const uint8_t *)(uintptr_t)p->code[0], p->cmax[0], STAGE_VERTEX, &p->vkey);
    if (p->vi->msl) {
        p->fkey.vs_out_mask = p->vi->out_mask;
        p->fi = maxwell_translate((const uint8_t *)(uintptr_t)p->code[1], p->cmax[1], STAGE_FRAGMENT, &p->fkey);
    }
    shader_time_log("translate vs+fs", p->code[1], t0);
    {   // ODYSSEY_LOG_GLSL=<code address, hex>: the GLSL a vertex or fragment shader at that address becomes
        static uint64_t wants[8];
        static int nwant = -1;
        if (nwant < 0) {  // (several: comma separated)
            nwant = 0;
            const char *e = getenv("ODYSSEY_LOG_GLSL");
            while (e && *e && nwant < 8) {
                char *end;
                wants[nwant++] = strtoull(e, &end, 16);
                e = *end == ',' ? end + 1 : NULL;
            }
        }
        for (int k = 0; k < nwant; k++) {
            uint64_t want = wants[k];
            if (!want || (p->code[0] != want && p->code[1] != want)) continue;
            const ShaderInfo *si = p->code[0] == want ? p->vi : p->fi;
            LOG("[glsl] %s %llx:\n%s\n", p->code[0] == want ? "vs" : "fs", (unsigned long long)want, si && si->msl ? si->msl : "(none)");
            wants[k] = 0;  // (once)
        }
    }
    if (!p->vi->msl) {
        if (atomic_load(&p->urgent)) prog_tell_failure(p);
        prog_failed(p);
        return;
    }
    if (!p->fi->msl) {
        if (atomic_load(&p->urgent)) prog_tell_failure(p);
        if (getenv("ODYSSEY_SAVE_MSL")) {  // the raw words, to look at what failed: build/glsl/<addr>_fs.hex
            char path[256];
            mkdir("build/glsl", 0755);
            snprintf(path, sizeof path, "build/glsl/%llx_fs.hex", (unsigned long long)p->code[1]);
            FILE *f = fopen(path, "w");
            const uint64_t *w = (const uint64_t *)(uintptr_t)(p->code[1] + 0x80);  // (after the 0x80-byte header)
            for (size_t i = 0; f && i < 512 && i * 8 + 0x80 < p->cmax[1]; i++) fprintf(f, "%04zx %016llx\n", i * 8, (unsigned long long)w[i]);
            if (f) fclose(f);
        }
        prog_failed(p);
        return;
    }
    p->vs_uses_vid = strstr(p->vi->msl, "gl_VertexIndex") != NULL;
    // A huge shader: Odyssey has a fragment shader of ~35,700 instructions (~380 KB of code: 3 MB of GLSL, 6.8 MB of
    // SPIR-V; every other one is under 1 MB of SPIR-V). NVIDIA's and AMD's compilers worked on it for minutes and took
    // ~11 GB (it was in players' lists of programs drawn in a scene's first frames: built at every boot). So it is
    // compiled without the driver's optimizations, only once a draw needs it, never blocking (its draws are skipped
    // until it is ready). ODYSSEY_HUGE_SHADER=<GLSL bytes>: the limit (0: none).
    {
        static long lim = -1;
        if (lim < 0) lim = getenv("ODYSSEY_HUGE_SHADER") ? atol(getenv("ODYSSEY_HUGE_SHADER")) : 1536 * 1024;
        p->huge = lim > 0 && (strlen(p->vi->msl) > (size_t)lim || strlen(p->fi->msl) > (size_t)lim);
    }
    save_glsl(p->vi->msl, p->code[0], "vs");
    save_glsl(p->fi->msl, p->code[1], "fs");
    atomic_store(&p->step, PSTEP_COMPILE);
    if (t_build_here && !p->huge) compile_program(p);
    else queue_job(compile_program, p, atomic_load(&p->urgent) ? 0 : 1);
}

// Its next step to the urgent queues (once per step: a step queued twice runs once anyway, the step claim). A step
// running now goes on to the next one urgently itself (it reads p->urgent when done).
static void prog_urge(Prog *p) {
    atomic_store(&p->urgent, true);
    int s = atomic_load(&p->step);
    if ((s != PSTEP_TRANSLATE && s != PSTEP_COMPILE && s != PSTEP_LIBS) || atomic_exchange(&p->urged, s + 1) == s + 1) return;
    if (s == PSTEP_TRANSLATE) queue_translate(translate_program, p, 0);
    else if (s == PSTEP_COMPILE) queue_job(compile_program, p, 0);
    else queue_job(build_libs, p, 0);
}

// Only restore libraries on the render thread when the draw has opted into waiting.
// Even a driver cache hit can take longer than a frame.
static void prog_need(Prog *p) {
    if (!sync_shaders() || p->huge) { prog_urge(p); return; }
    if (atomic_load(&p->step) == PSTEP_LIBS && libs_from_cache(p)) return;
    atomic_store(&p->urgent, true);
    t_build_here = true;
    if (atomic_load(&p->step) == PSTEP_TRANSLATE) translate_program(p);  // (a huge one goes on off-thread from there)
    if (atomic_load(&p->step) == PSTEP_COMPILE && !p->huge) compile_program(p);
    if (atomic_load(&p->step) == PSTEP_LIBS && !p->huge) build_libs(p);
    t_build_here = false;
}

#define PROG_TAB 65536  // (open addressing, never shrinks: a full table skips new programs instead of hanging)
static struct { uint64_t key; Prog *prog; } prog_tab[PROG_TAB];
static pthread_mutex_t prog_lock = PTHREAD_MUTEX_INITIALIZER;  // (draws: render thread; set-ups: the game's threads)
static _Atomic int stat_ahead, stat_ahead_drawn;

// The program for these shaders and keys, created (and its build queued) when new: for a draw (while builds block, built
// right here), set up by the game ahead of its first draw (renderer_program_hint: low priority), or bound by a
// submission about to run (prefetch_programs: urgent, never right here).
enum { PF_DRAW, PF_AHEAD, PF_AHEAD_WARM, PF_PREFETCH };  // (warm: drawn in a scene's first frames before: urgent)
static Prog *program_for(NvnProgram *np, const ShaderKey *vkey, const ShaderKey *fkey, int how) {
    bool ahead = how == PF_AHEAD || how == PF_AHEAD_WARM;
    if (!(np->stage_mask & 1) || !(np->stage_mask & 2)) return NULL;
    uint64_t h = 0xcbf29ce484222325ull;
    for (int i = 0; i < 2; i++) { uint64_t a = np->stages[i].code_addr; for (int k = 0; k < 8; k++) { h ^= (a >> (k * 8)) & 0xFF; h *= 0x100000001B3ull; } }
    const uint8_t *vb = (const uint8_t *)vkey;
    for (size_t i = 0; i < sizeof *vkey; i++) { h ^= vb[i]; h *= 0x100000001B3ull; }
    for (int i = 0; i < 8; i++) { h ^= fkey->rt_kind[i]; h *= 0x100000001B3ull; }
    if (!h) h = 1;
    pthread_mutex_lock(&prog_lock);
    uint32_t slot = (uint32_t)(h ^ (h >> 31)) & (PROG_TAB - 1), probes = 0;
    while (prog_tab[slot].key && prog_tab[slot].key != h && ++probes < PROG_TAB) slot = (slot + 1) & (PROG_TAB - 1);
    if (prog_tab[slot].key == h) {
        Prog *p = prog_tab[slot].prog;
        pthread_mutex_unlock(&prog_lock);
        return p;
    }
    if (prog_tab[slot].key) {
        pthread_mutex_unlock(&prog_lock);
        static int full;
        if (!full++) LOG("[vk] program table full (%d)\n", PROG_TAB);
        return NULL;
    }
    Prog *p = calloc(1, sizeof *p);
    atomic_fetch_add(&dbg_progs, 1);
    for (int i = 0; i < 2; i++) {
        p->code[i] = np->stages[i].code_addr;
        p->cmax[i] = np->stages[i].const_offset ? np->stages[i].const_offset : 0x10000;
    }
    p->vkey = *vkey;
    p->fkey = *fkey;
    p->ahead = ahead;
    if (ahead) for (int i = 0; i < 2; i++) p->code_sum[i] = code_sum(p->code[i]);
    if (how != PF_AHEAD) atomic_store(&p->urgent, true);
    prog_tab[slot].key = h;
    prog_tab[slot].prog = p;
    pthread_mutex_unlock(&prog_lock);
    static int logp = -1;
    if (logp < 0) logp = getenv("ODYSSEY_LOG_PROGS") != NULL;
    if (logp && !ahead) {
        uint32_t rk = 0;
        for (int i = 0; i < 8; i++) if (fkey->rt_kind[i]) rk |= 1u << i;
        LOG("[prog] draw vs=%llx fs=%llx frame %llu rk %x (not set up ahead)\n", (unsigned long long)p->code[0],
            (unsigned long long)p->code[1], (unsigned long long)renderer_present_count(), rk);
    }
    if (ahead) atomic_fetch_add(&stat_ahead, 1);
    if (how == PF_AHEAD) queue_translate(translate_program, p, 1);
    else if (how == PF_DRAW && sync_shaders()) prog_need(p);
    else prog_urge(p);
    return p;
}

// What a draw puts in the shader keys besides its render target kinds (draw_)
static void base_keys(ShaderKey *vkey, ShaderKey *fkey) {
    memset(vkey, 0, sizeof *vkey);
    memset(fkey, 0, sizeof *fkey);
    vkey->glsl = fkey->glsl = 1;
    vkey->flip_y = fkey->flip_y = nvn_window_origin() == 0;
    vkey->depth_minus_one = nvn_depth_mode() == 0;
}

// Programs drawn before: a hash of the start of both shaders' code each (a collision only compiles one more program),
// and whether it was drawn while builds block or held a frame during gameplay. What this player's runs drew:
// cache/drawn_programs.bin (bit 0 of each hash = warm); what a run
// through every kingdom drew, shipped with the program: known_programs.txt (tools/known_programs.py; hashes only, no
// game data), so a first start gets it too. Bulk set-ups of these are compiled ahead, warm ones with their libraries
// right away (renderer_program_hint).
#define DRAWN_TAB 32768
static uint64_t drawn_tab[DRAWN_TAB];  // (hash | 1)
static bool drawn_warm[DRAWN_TAB];
static int drawn_n, drawn_new;
static pthread_mutex_t drawn_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t hash_words(uint64_t h, const void *p, size_t n);

static uint64_t prog_code_hash(uint64_t vs, uint64_t vmax, uint64_t fs, uint64_t fmax) {
    uint64_t h = hash_words(0x9e3779b97f4a7c15ull, (const void *)(uintptr_t)vs, vmax < 2048 ? vmax : 2048);
    h = hash_words(h, (const void *)(uintptr_t)fs, fmax < 2048 ? fmax : 2048);
    return h | 1;
}

static int drawn_slot(uint64_t h) {  // (drawn_lock held) its slot, or a free one; -1: full
    uint32_t slot = (uint32_t)(h ^ (h >> 31)) & (DRAWN_TAB - 1), probes = 0;
    while (drawn_tab[slot] && drawn_tab[slot] != h && ++probes < DRAWN_TAB) slot = (slot + 1) & (DRAWN_TAB - 1);
    return drawn_tab[slot] == h || (!drawn_tab[slot] && drawn_n < DRAWN_TAB / 2) ? (int)slot : -1;
}

static bool drawn_insert(uint64_t h, bool warm) {  // (drawn_lock held) true: new to the table, or now warm
    int s = drawn_slot(h | 1);
    if (s < 0 || (drawn_tab[s] && (drawn_warm[s] || !warm))) return false;
    if (!drawn_tab[s]) { drawn_tab[s] = h | 1; drawn_n++; }
    drawn_warm[s] |= warm;
    return true;
}

static void drawn_load(void) {  // (drawn_lock held, once)
    static bool loaded;
    if (loaded) return;
    loaded = true;
    char path[1100];
    if (rt_resource("known_programs.txt", "runtime/nvn/vulkan/known_programs.txt", path, sizeof path)) {
        FILE *f = fopen(path, "r");
        char line[128];
        unsigned long long h;
        int w;
        while (f && fgets(line, sizeof line, f))
            if (line[0] != '#' && sscanf(line, "%llx %d", &h, &w) == 2) drawn_insert(h, w != 0);
        if (f) fclose(f);
    }
    int shipped = drawn_n;
    FILE *f = fopen("cache/drawn_programs.bin", "rb");
    uint64_t h;
    while (f && fread(&h, 8, 1, f) == 1) if (h) drawn_insert(h, h & 1);
    if (f) fclose(f);
    LOG("[vk] programs drawn before: %d shipped, %d more from earlier runs (compiled ahead when the game sets them up)\n",
        shipped, drawn_n - shipped);
}

static bool drawn_has(uint64_t h, bool *warm) {
    pthread_mutex_lock(&drawn_lock);
    drawn_load();
    int s = drawn_slot(h | 1);
    bool r = s >= 0 && drawn_tab[s];
    *warm = r && drawn_warm[s];
    pthread_mutex_unlock(&drawn_lock);
    return r;
}

void vk_drawn_programs_save(void) {
    pthread_mutex_lock(&drawn_lock);
    if (drawn_new) {
        mkdir("cache", 0755);
        FILE *f = fopen("cache/drawn_programs.bin.tmp", "wb");
        bool ok = f != NULL;
        for (int i = 0; ok && i < DRAWN_TAB; i++) {
            uint64_t h = drawn_tab[i] ? (drawn_tab[i] & ~1ull) | drawn_warm[i] : 0;
            if (h) ok = fwrite(&h, 8, 1, f) == 1;
        }
        if (f) fclose(f);
        if (ok) { remove("cache/drawn_programs.bin"); rename("cache/drawn_programs.bin.tmp", "cache/drawn_programs.bin"); drawn_new = 0; }
    }
    pthread_mutex_unlock(&drawn_lock);
}

static void drawn_add(Prog *p, bool held) {
    uint64_t h = prog_code_hash(p->code[0], p->cmax[0], p->code[1], p->cmax[1]);
    pthread_mutex_lock(&drawn_lock);
    drawn_load();
    bool changed = drawn_insert(h, held || builds_block());
    bool save = changed && ++drawn_new >= 100 && !held;  // (also now and then: a killed run keeps it)
    pthread_mutex_unlock(&drawn_lock);
    if (save) vk_drawn_programs_save();
    if (changed && held) {
        extern _Atomic unsigned g_pcache_dirty;
        atomic_fetch_add(&g_pcache_dirty, 1);
    }
}

static void remember_frame_hold(Prog *p) {
    if (p->frame_hold_recorded) return;
    p->frame_hold_recorded = true;
    if (shader_time_ms() >= 0)
        LOG("[shtime] frame %llu held for vs=%llx fs=%llx (program step %d)\n",
            (unsigned long long)renderer_present_count(), (unsigned long long)p->code[0],
            (unsigned long long)p->code[1], atomic_load(&p->step));
    drawn_add(p, true);
}

// Programs set up ahead: the game sets up its programs (nvnProgramSetShaders, objects.c) before it draws them, a model's
// material variations as the model loads (mostly while a stage loads, a few frames before their first draw), layout
// and font shaders at boot. Each is translated and turned into SPIR-V modules right then in the background, with the
// keys a draw will use (the vertex formats no longer matter: translate.c vattr; float render targets assumed); the
// driver's compile (its libraries) waits for the first draw (compile_program: memory), which takes them from the
// driver's cache when it has them, else moves them to the front (prog_need) - except for programs drawn warm before
// (drawn_tab: in a scene's first frames, which wait for them), built whole right away and urgently: a stage loading
// compiles the programs its first frames need. Bulk set-ups (bulk: every variation of agl's shader archives, every
// shader of an effect resource - ~25,000 at boot, a few hundred ever drawn) only when drawn before.
// A model's material set up as a stage loads that was drawn before (in any run, any scene) is built whole right away
// too: a new player's first frame of the Sand Kingdom (2026-11-02, cold caches) stood 2.4 s for the 450 libraries of
// such programs (all drawn then), the loading screen before it 10 s long. ODYSSEY_PREWARM_KNOWN=0: as before.
// ODYSSEY_PREWARM=0: programs build when first drawn; =2: bulk ones too.
static _Atomic uint64_t prog_setups;  // programs the game set up so far (backend_execute: when to prefetch)
void renderer_program_hint(NvnProgram *np, bool bulk) {
    atomic_fetch_add(&prog_setups, 1);
    static int on = -1, known_whole = -1;
    if (on < 0) { const char *e = getenv("ODYSSEY_PREWARM"); on = e ? atoi(e) : 1; }
    if (known_whole < 0) { const char *e = getenv("ODYSSEY_PREWARM_KNOWN"); known_whole = e ? atoi(e) != 0 : 1; }
    if (!on || !g_vk.dev || (np->stage_mask & 3) != 3) return;
    // Low memory (settings: Memory use): nothing set up ahead - each program is translated and built at its first draw
    // (from the driver's cache when it has it: a few ms; 6.5 GB in RAM instead of 9.4, Cascade, NVIDIA)
    if (settings_low_memory() && !getenv("ODYSSEY_PREWARM")) return;
    bool warm = false;
    bool known = drawn_has(prog_code_hash(np->stages[0].code_addr, np->stages[0].const_offset ? np->stages[0].const_offset : 0x10000,
                                          np->stages[1].code_addr, np->stages[1].const_offset ? np->stages[1].const_offset : 0x10000),
                           &warm);
    if (bulk && !known && on < 2) return;
    ShaderKey vkey, fkey;
    base_keys(&vkey, &fkey);
    program_for(np, &vkey, &fkey, warm || (known && !bulk && known_whole) ? PF_AHEAD_WARM : PF_AHEAD);
}

static Prog *get_program(NvnProgram *np, ShaderKey *vkey, ShaderKey *fkey) {
    Prog *p = program_for(np, vkey, fkey, PF_DRAW);
    if (!p) return NULL;
    if (!p->drawn) {
        p->drawn = true;
        drawn_add(p, false);
        static int logp = -1;  // (ODYSSEY_LOG_PROGS: how far it got before its first draw)
        if (logp < 0) logp = getenv("ODYSSEY_LOG_PROGS") != NULL;
        if (logp && p->ahead) {
            int n = atomic_fetch_add(&stat_ahead_drawn, 1) + 1;
            static int ready, modules;
            ready += atomic_load(&p->state) == 1;
            modules += atomic_load(&p->step) == PSTEP_LIBS;
            LOG("[prog] first draw vs=%llx fs=%llx frame %llu: %s (set up ahead: %d, drawn %d, built %d, to its libraries %d)\n",
                (unsigned long long)p->code[0], (unsigned long long)p->code[1], (unsigned long long)renderer_present_count(),
                atomic_load(&p->state) == 1 ? "built" : atomic_load(&p->step) == PSTEP_LIBS ? "modules" : "building",
                atomic_load(&stat_ahead), n, ready, modules);
        }
    }
    if (atomic_load(&p->state) == 0) prog_need(p);
    return p;
}

// ================================================================== vertex formats / fixed function tables
// How the vertex shader receives an attribute: 0 float, 1 signed int, 2 unsigned int (the raw integer), 3 / 4 an
// unsigned / signed 32-bit integer the hardware converts to float (NVN's *32_UI2F / *32_I2F: Vulkan has no 32-bit
// SCALED formats, so the shader converts; runtime/nvn/maxwell/translate.c).
static int attr_kind(uint32_t fmt) {
    switch (fmt) {
    case 0x03: case 0x08: case 0x0B: case 0x0F: case 0x14: case 0x17: case 0x1B: case 0x20: case 0x23:
    case 0x27: case 0x2C: case 0x2F: case 0x3E: case 0x6F: case 0x74: case 0x77:
        return 2;
    case 0x04: case 0x09: case 0x0C: case 0x10: case 0x15: case 0x18: case 0x1C: case 0x21: case 0x24:
    case 0x28: case 0x2D: case 0x30: case 0x6A: case 0x70: case 0x75: case 0x78:
        return 1;
    case 0x55: case 0x5B: case 0x61: case 0x67:  // R / RG / RGB / RGBA 32 UI2F
        return 3;
    case 0x56: case 0x5C: case 0x62: case 0x68:  // ... 32 I2F
        return 4;
    default: return 0;
    }
}

static VkFormat vertex_format(uint32_t f) {
    switch (f) {
    case 0x01: return VK_FORMAT_R8_UNORM;
    case 0x02: return VK_FORMAT_R8_SNORM;
    case 0x03: return VK_FORMAT_R8_UINT;
    case 0x04: return VK_FORMAT_R8_SINT;
    case 0x05: return VK_FORMAT_R16_SFLOAT;
    case 0x06: return VK_FORMAT_R16_UNORM;
    case 0x07: return VK_FORMAT_R16_SNORM;
    case 0x08: return VK_FORMAT_R16_UINT;
    case 0x09: return VK_FORMAT_R16_SINT;
    case 0x0A: return VK_FORMAT_R32_SFLOAT;
    case 0x0B: return VK_FORMAT_R32_UINT;
    case 0x0C: return VK_FORMAT_R32_SINT;
    case 0x0D: return VK_FORMAT_R8G8_UNORM;
    case 0x0E: return VK_FORMAT_R8G8_SNORM;
    case 0x0F: return VK_FORMAT_R8G8_UINT;
    case 0x10: return VK_FORMAT_R8G8_SINT;
    case 0x11: return VK_FORMAT_R16G16_SFLOAT;
    case 0x12: return VK_FORMAT_R16G16_UNORM;
    case 0x13: return VK_FORMAT_R16G16_SNORM;
    case 0x14: return VK_FORMAT_R16G16_UINT;
    case 0x15: return VK_FORMAT_R16G16_SINT;
    case 0x16: return VK_FORMAT_R32G32_SFLOAT;
    case 0x17: return VK_FORMAT_R32G32_UINT;
    case 0x18: return VK_FORMAT_R32G32_SINT;
    case 0x19: return VK_FORMAT_R8G8B8_UNORM;
    case 0x1A: return VK_FORMAT_R8G8B8_SNORM;
    case 0x1B: return VK_FORMAT_R8G8B8_UINT;
    case 0x1C: return VK_FORMAT_R8G8B8_SINT;
    case 0x1D: return VK_FORMAT_R16G16B16_SFLOAT;
    case 0x1E: return VK_FORMAT_R16G16B16_UNORM;
    case 0x1F: return VK_FORMAT_R16G16B16_SNORM;
    case 0x20: return VK_FORMAT_R16G16B16_UINT;
    case 0x21: return VK_FORMAT_R16G16B16_SINT;
    case 0x22: return VK_FORMAT_R32G32B32_SFLOAT;
    case 0x23: return VK_FORMAT_R32G32B32_UINT;
    case 0x24: return VK_FORMAT_R32G32B32_SINT;
    case 0x25: return VK_FORMAT_R8G8B8A8_UNORM;
    case 0x26: return VK_FORMAT_R8G8B8A8_SNORM;
    case 0x27: return VK_FORMAT_R8G8B8A8_UINT;
    case 0x28: return VK_FORMAT_R8G8B8A8_SINT;
    case 0x29: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case 0x2A: return VK_FORMAT_R16G16B16A16_UNORM;
    case 0x2B: return VK_FORMAT_R16G16B16A16_SNORM;
    case 0x2C: return VK_FORMAT_R16G16B16A16_UINT;
    case 0x2D: return VK_FORMAT_R16G16B16A16_SINT;
    case 0x2E: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case 0x2F: return VK_FORMAT_R32G32B32A32_UINT;
    case 0x30: return VK_FORMAT_R32G32B32A32_SINT;
    case 0x3D: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case 0x3E: return VK_FORMAT_A2B10G10R10_UINT_PACK32;
    case 0x69: return VK_FORMAT_A2B10G10R10_SNORM_PACK32;
    case 0x6A: return VK_FORMAT_A2B10G10R10_SINT_PACK32;
    case 0x6B: return VK_FORMAT_A2B10G10R10_USCALED_PACK32;
    case 0x6C: return VK_FORMAT_A2B10G10R10_SSCALED_PACK32;
    case 0x3F: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case 0x40: return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
    // integer to float (UI2F / I2F): SCALED formats; the 32-bit ones read as integers, the shader converts (attr_kind)
    case 0x51: return VK_FORMAT_R8_USCALED;
    case 0x52: return VK_FORMAT_R8_SSCALED;
    case 0x53: return VK_FORMAT_R16_USCALED;
    case 0x54: return VK_FORMAT_R16_SSCALED;
    case 0x55: return VK_FORMAT_R32_UINT;
    case 0x56: return VK_FORMAT_R32_SINT;
    case 0x57: return VK_FORMAT_R8G8_USCALED;
    case 0x58: return VK_FORMAT_R8G8_SSCALED;
    case 0x59: return VK_FORMAT_R16G16_USCALED;
    case 0x5A: return VK_FORMAT_R16G16_SSCALED;
    case 0x5B: return VK_FORMAT_R32G32_UINT;
    case 0x5C: return VK_FORMAT_R32G32_SINT;
    case 0x5D: return VK_FORMAT_R8G8B8_USCALED;
    case 0x5E: return VK_FORMAT_R8G8B8_SSCALED;
    case 0x5F: return VK_FORMAT_R16G16B16_USCALED;
    case 0x60: return VK_FORMAT_R16G16B16_SSCALED;
    case 0x61: return VK_FORMAT_R32G32B32_UINT;
    case 0x62: return VK_FORMAT_R32G32B32_SINT;
    case 0x63: return VK_FORMAT_R8G8B8A8_USCALED;
    case 0x64: return VK_FORMAT_R8G8B8A8_SSCALED;
    case 0x65: return VK_FORMAT_R16G16B16A16_USCALED;
    case 0x66: return VK_FORMAT_R16G16B16A16_SSCALED;
    case 0x67: return VK_FORMAT_R32G32B32A32_UINT;
    case 0x68: return VK_FORMAT_R32G32B32A32_SINT;
    // RGBX: the RGBA layout (the shader doesn't read the X)
    case 0x6D: return VK_FORMAT_R8G8B8A8_UNORM;
    case 0x6E: return VK_FORMAT_R8G8B8A8_SNORM;
    case 0x6F: return VK_FORMAT_R8G8B8A8_UINT;
    case 0x70: return VK_FORMAT_R8G8B8A8_SINT;
    case 0x71: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case 0x72: return VK_FORMAT_R16G16B16A16_UNORM;
    case 0x73: return VK_FORMAT_R16G16B16A16_SNORM;
    case 0x74: return VK_FORMAT_R16G16B16A16_UINT;
    case 0x75: return VK_FORMAT_R16G16B16A16_SINT;
    case 0x76: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case 0x77: return VK_FORMAT_R32G32B32A32_UINT;
    case 0x78: return VK_FORMAT_R32G32B32A32_SINT;
    case 0x98: case 0x99: return VK_FORMAT_B8G8R8A8_UNORM;
    default: {
        static uint8_t told[256];
        if (f < 256 && !told[f]++) LOG("[vk] vertex format %#x not mapped: read as 4 floats\n", f);
        return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
    }
}

// 3-component 8/16-bit vertex formats aren't universally supported: read 4 components instead.
// (GLSL contract, runtime/nvn/maxwell/shader.h) what an NVN vertex format is bound as - a float format keeping the value -
// and the kind the vertex shader turns it back with (push constant): integers of 8 / 16 bits as SCALED (exact), of 32
// bits as SFLOAT (the bits; NVN's *32_UI2F / *32_I2F the shader converts). Programs so compile without the formats.
static VkFormat vertex_input_format(uint32_t fmt, uint32_t *kind) {
    VkFormat f = vertex_format(fmt);
    *kind = 0;
    switch (f) {
    case VK_FORMAT_R8_UINT: *kind = 2; return VK_FORMAT_R8_USCALED;
    case VK_FORMAT_R8_SINT: *kind = 1; return VK_FORMAT_R8_SSCALED;
    case VK_FORMAT_R16_UINT: *kind = 2; return VK_FORMAT_R16_USCALED;
    case VK_FORMAT_R16_SINT: *kind = 1; return VK_FORMAT_R16_SSCALED;
    case VK_FORMAT_R8G8_UINT: *kind = 2; return VK_FORMAT_R8G8_USCALED;
    case VK_FORMAT_R8G8_SINT: *kind = 1; return VK_FORMAT_R8G8_SSCALED;
    case VK_FORMAT_R16G16_UINT: *kind = 2; return VK_FORMAT_R16G16_USCALED;
    case VK_FORMAT_R16G16_SINT: *kind = 1; return VK_FORMAT_R16G16_SSCALED;
    case VK_FORMAT_R8G8B8_UINT: *kind = 2; return VK_FORMAT_R8G8B8_USCALED;
    case VK_FORMAT_R8G8B8_SINT: *kind = 1; return VK_FORMAT_R8G8B8_SSCALED;
    case VK_FORMAT_R16G16B16_UINT: *kind = 2; return VK_FORMAT_R16G16B16_USCALED;
    case VK_FORMAT_R16G16B16_SINT: *kind = 1; return VK_FORMAT_R16G16B16_SSCALED;
    case VK_FORMAT_R8G8B8A8_UINT: *kind = 2; return VK_FORMAT_R8G8B8A8_USCALED;
    case VK_FORMAT_R8G8B8A8_SINT: *kind = 1; return VK_FORMAT_R8G8B8A8_SSCALED;
    case VK_FORMAT_R16G16B16A16_UINT: *kind = 2; return VK_FORMAT_R16G16B16A16_USCALED;
    case VK_FORMAT_R16G16B16A16_SINT: *kind = 1; return VK_FORMAT_R16G16B16A16_SSCALED;
    case VK_FORMAT_A2B10G10R10_UINT_PACK32: *kind = 2; return VK_FORMAT_A2B10G10R10_USCALED_PACK32;
    case VK_FORMAT_A2B10G10R10_SINT_PACK32: *kind = 1; return VK_FORMAT_A2B10G10R10_SSCALED_PACK32;
    case VK_FORMAT_R32_UINT: case VK_FORMAT_R32_SINT: f = VK_FORMAT_R32_SFLOAT; break;
    case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32_SINT: f = VK_FORMAT_R32G32_SFLOAT; break;
    case VK_FORMAT_R32G32B32_UINT: case VK_FORMAT_R32G32B32_SINT: f = VK_FORMAT_R32G32B32_SFLOAT; break;
    case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT: f = VK_FORMAT_R32G32B32A32_SFLOAT; break;
    default: return f;
    }
    int ak = attr_kind(fmt);
    *kind = ak == 3 || ak == 4 ? (uint32_t)ak : 0;
    return f;
}

static VkFormat vertex_format_supported(VkFormat f) {
    static uint8_t known[256];  // 1 ok, 2 not
    uint32_t i = f < 256 ? f : 0;
    if (!known[i]) {
        VkFormatProperties p;
        vkGetPhysicalDeviceFormatProperties(g_vk.phys, f, &p);
        known[i] = (p.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) ? 1 : 2;
    }
    if (known[i] == 1) return f;
    { static uint8_t told[256]; if (!told[i]++) LOG("[vk] vertex format %d unsupported: read as 4 components\n", f); }
    switch (f) {
    case VK_FORMAT_R8G8B8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_R8G8B8_SNORM: return VK_FORMAT_R8G8B8A8_SNORM;
    case VK_FORMAT_R8G8B8_UINT: return VK_FORMAT_R8G8B8A8_UINT;
    case VK_FORMAT_R8G8B8_SINT: return VK_FORMAT_R8G8B8A8_SINT;
    case VK_FORMAT_R16G16B16_SFLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case VK_FORMAT_R16G16B16_UNORM: return VK_FORMAT_R16G16B16A16_UNORM;
    case VK_FORMAT_R16G16B16_SNORM: return VK_FORMAT_R16G16B16A16_SNORM;
    case VK_FORMAT_R16G16B16_UINT: return VK_FORMAT_R16G16B16A16_UINT;
    case VK_FORMAT_R16G16B16_SINT: return VK_FORMAT_R16G16B16A16_SINT;
    case VK_FORMAT_R8G8B8_USCALED: return VK_FORMAT_R8G8B8A8_USCALED;
    case VK_FORMAT_R8G8B8_SSCALED: return VK_FORMAT_R8G8B8A8_SSCALED;
    case VK_FORMAT_R16G16B16_USCALED: return VK_FORMAT_R16G16B16A16_USCALED;
    case VK_FORMAT_R16G16B16_SSCALED: return VK_FORMAT_R16G16B16A16_SSCALED;
    default: return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
}

static VkBlendFactor blend_factor(uint8_t f) {
    switch (f) {
    case 1: return VK_BLEND_FACTOR_ZERO;
    case 2: return VK_BLEND_FACTOR_ONE;
    case 3: return VK_BLEND_FACTOR_SRC_COLOR;
    case 4: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 5: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 7: return VK_BLEND_FACTOR_DST_ALPHA;
    case 8: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 9: return VK_BLEND_FACTOR_DST_COLOR;
    case 10: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 11: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 0x10: return VK_BLEND_FACTOR_SRC1_COLOR;
    case 0x11: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    case 0x12: return VK_BLEND_FACTOR_SRC1_ALPHA;
    case 0x13: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    case 0x61: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 0x62: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 0x63: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 0x64: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default: {
        static uint8_t warned[256];
        if (!warned[f]++) LOG("[vk] unknown blend factor %#x\n", f);
        return VK_BLEND_FACTOR_ONE;
    }
    }
}

static VkBlendOp blend_op(uint8_t e) {
    switch (e) {
    case 2: return VK_BLEND_OP_SUBTRACT;
    case 3: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 4: return VK_BLEND_OP_MIN;
    case 5: return VK_BLEND_OP_MAX;
    default: return VK_BLEND_OP_ADD;
    }
}

static VkCompareOp compare_fn(uint8_t f) {
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

static VkStencilOp stencil_op(uint8_t o) {
    switch (o) {
    case 2: return VK_STENCIL_OP_ZERO;
    case 3: return VK_STENCIL_OP_REPLACE;
    case 4: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case 5: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case 6: return VK_STENCIL_OP_INVERT;
    case 7: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 8: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default: return VK_STENCIL_OP_KEEP;
    }
}

static int rt_kind(uint32_t f) {
    switch (f) {
    case 0x03: case 0x08: case 0x0B: case 0x0F: case 0x14: case 0x17: case 0x27: case 0x2C: case 0x2F: case 0x3E: return 2;
    case 0x04: case 0x09: case 0x0C: case 0x10: case 0x15: case 0x18: case 0x28: case 0x2D: case 0x30: return 1;
    default: return 0;
    }
}

// ================================================================== graphics pipelines
// Everything a pipeline bakes in, captured by value so it can be built on a worker thread.
#define ZERO_BINDING 16
typedef struct {
    Prog *prog;
    VkFormat color[8], depth, stencil;
    uint32_t ncolor, samples;
    uint32_t attr_fmt[32], attr_off[32], attr_bind[32];
    uint32_t attr_mask;
    uint32_t stride[17], divisor[17], used_streams;
    uint8_t blend[8][8];
    uint8_t blend_on, a2c, topo_class, poly_fill;
    uint32_t write_mask[8];
} PipeKey;

typedef struct { PipeKey k; VkPipeline pipe; _Atomic int state; bool recorded, first_scene; } Pipe;  // state: 0 building, 1 ready, 2 failed
#define PIPE_TAB 65536  // (as PROG_TAB)
static struct { uint64_t key; Pipe *pipe; } pipe_tab[PIPE_TAB];

// 8 bytes a step (PipeKey is ~700 bytes, hashed per draw); in-memory tables only - hmix values are compared with
// stored constants (fragment-shader hashes) and must not change.
static uint64_t hash_words(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (; n >= 8; n -= 8, b += 8) { uint64_t w; memcpy(&w, b, 8); h = (h ^ w) * 0x9E3779B97F4A7C15ull; h ^= h >> 29; }
    for (; n; n--, b++) { h = (h ^ *b) * 0x100000001B3ull; }
    return h ^ (h >> 32);
}

static uint64_t hmix(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001B3ull; }
    return h;
}

static VkPrimitiveTopology topo_class_base(uint8_t c) {
    return c == 0 ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST : c == 1 ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

static void build_pipeline(void *arg) {
    Pipe *pp = arg;
    PipeKey *k = &pp->k;
    prog_modules(k->prog);  // (made here when its set-up didn't: a null module fails this pipeline as any error does)
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = k->prog->vs, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = k->prog->fs, .pName = "main"}};
    VkVertexInputAttributeDescription va[32];
    VkVertexInputBindingDescription vb[17];
    VkVertexInputBindingDivisorDescriptionEXT vd[17];
    uint32_t na = 0, nb = 0, nd = 0;
    for (int i = 0; i < 32; i++)
        if (k->attr_mask & (1u << i)) va[na++] = (VkVertexInputAttributeDescription){(uint32_t)i, k->attr_bind[i], k->attr_fmt[i], k->attr_off[i]};
    for (uint32_t s = 0; s < 17; s++) {
        if (!(k->used_streams & (1u << s))) continue;
        bool inst = k->divisor[s] != 0;
        vb[nb++] = (VkVertexInputBindingDescription){s, k->stride[s], inst ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX};
        if (inst && k->divisor[s] > 1 && g_vk.divisor) vd[nd++] = (VkVertexInputBindingDivisorDescriptionEXT){s, k->divisor[s]};
    }
    VkPipelineVertexInputDivisorStateCreateInfoEXT dvi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT,
                                                          .vertexBindingDivisorCount = nd, .pVertexBindingDivisors = vd};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, .pNext = nd ? &dvi : NULL,
                                               .vertexBindingDescriptionCount = nb, .pVertexBindingDescriptions = vb,
                                               .vertexAttributeDescriptionCount = na, .pVertexAttributeDescriptions = va};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = topo_class_base(k->topo_class)};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = k->poly_fill ? VK_POLYGON_MODE_FILL : VK_POLYGON_MODE_LINE, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = (VkSampleCountFlagBits)k->samples, .alphaToCoverageEnable = k->a2c};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState cba[8] = {0};
    for (uint32_t i = 0; i < k->ncolor; i++) {
        cba[i].colorWriteMask = k->write_mask[i];
        if ((k->blend_on >> i) & 1) {
            const uint8_t *b = k->blend[i];
            cba[i].blendEnable = VK_TRUE;
            cba[i].srcColorBlendFactor = blend_factor(b[1]);
            cba[i].dstColorBlendFactor = blend_factor(b[2]);
            cba[i].srcAlphaBlendFactor = blend_factor(b[3]);
            cba[i].dstAlphaBlendFactor = blend_factor(b[4]);
            cba[i].colorBlendOp = blend_op(b[5]);
            cba[i].alphaBlendOp = blend_op(b[6]);
        }
    }
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .attachmentCount = k->ncolor, .pAttachments = cba};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = NDYN, .pDynamicStates = DYN};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = k->ncolor,
                                        .pColorAttachmentFormats = k->color, .depthAttachmentFormat = k->depth,
                                        .stencilAttachmentFormat = k->stencil};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = k->prog->layout};
    if (k->prog->huge) pi.flags |= VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT;
    uint64_t t0 = plat_now_ns();
    VkResult r = vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &pp->pipe);
    shader_time_log("pipeline", k->prog->code[1], t0);
    if (r != VK_SUCCESS) {
        static int n;
        if (n++ < 10) LOG("[vk] pipeline creation failed: %d\n", r);
    }
    extern _Atomic unsigned g_pcache_dirty;
    atomic_fetch_add(&g_pcache_dirty, 1);
    atomic_fetch_add(&stat_compiles, 1);
    atomic_store(&pp->state, r == VK_SUCCESS ? 1 : 2);
}

// ---- pipeline libraries (g_vk.gpl, VK_EXT_graphics_pipeline_library)
// A pipeline in four parts: vertex input (layout of the vertex streams), pre-rasterization (the vertex shader), fragment
// shader, fragment output (target formats, blending). The two shader parts are compiled once per program
// (make_shader_libs, on the worker that built its SPIR-V); the cheap interface parts once per state (tables below); a
// draw's pipeline is those four linked without link-time optimization, which the driver promises to be fast
// (graphicsPipelineLibraryFastLinking): done right in the draw, so a new combination never waits or skips.
// Each dynamic state goes with the part it belongs to (DYN, the whole pipelines, has them all).
static const VkDynamicState DYN_VI[] = {VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE};
static const VkDynamicState DYN_PR[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS,
                                        VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE, VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE};
static const VkDynamicState DYN_FS[] = {
    VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
    VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
    VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP};
static const VkDynamicState DYN_FO[] = {VK_DYNAMIC_STATE_BLEND_CONSTANTS};
#define DYN_INFO(a) {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = sizeof a / sizeof a[0], .pDynamicStates = a}

static __thread VkResult t_lib_result;  // (the last create_lib_in's on this thread)
static VkPipeline create_lib_in(VkPipelineCache cache, VkGraphicsPipelineCreateInfo *pi, VkGraphicsPipelineLibraryFlagsEXT part) {
    const void *next = pi->pNext;
    VkPipelineCreateFlags flags = pi->flags;
    VkGraphicsPipelineLibraryCreateInfoEXT li = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT,
                                                 .pNext = (void *)pi->pNext, .flags = part};
    pi->pNext = &li;
    pi->flags |= VK_PIPELINE_CREATE_LIBRARY_BIT_KHR;
    VkPipeline lib = VK_NULL_HANDLE;
    VkResult r = vkCreateGraphicsPipelines(g_vk.dev, cache, 1, pi, NULL, &lib);
    t_lib_result = r;
    pi->pNext = next;
    pi->flags = flags;
    if (r != VK_SUCCESS) {
        static int n;
        if (r != VK_PIPELINE_COMPILE_REQUIRED && n++ < 10) LOG("[vk] pipeline library (part %#x) failed: %d\n", part, r);
        return VK_NULL_HANDLE;
    }
    if (cache) {
        extern _Atomic unsigned g_pcache_dirty;  // (device.c pcache_autosave)
        atomic_fetch_add(&g_pcache_dirty, 1);
    }
    return lib;
}
static VkPipeline create_lib(VkGraphicsPipelineCreateInfo *pi, VkGraphicsPipelineLibraryFlagsEXT part) {
    return create_lib_in(g_vk.pcache, pi, part);
}

// ---- pipeline binaries (g_vk.pbin, VK_KHR_pipeline_binary): a compiled shader library as data of our own,
// cache/pbin/<key>.bin, created from it later - in another run or another process - without a compile. The key: both
// shaders' SPIR-V (the layout comes from them), the stage, line / fill, unoptimized. The data holds the driver's
// global key: another driver version doesn't take it (then compiled again). Capturing and creating from data go
// without the pipeline cache (the extension's rule), and with the same create info (flags as flags2).
static PFN_vkCreatePipelineBinariesKHR pf_create_bins;
static PFN_vkDestroyPipelineBinaryKHR pf_destroy_bin;
static PFN_vkGetPipelineBinaryDataKHR pf_bin_data;
static PFN_vkReleaseCapturedPipelineDataKHR pf_release_captured;
static PFN_vkGetPipelineKeyKHR pf_pipeline_key;
static VkPipelineBinaryKeyKHR pbin_global = {.sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR};
#define PBIN_MAGIC 0x4e494250u  // "PBIN"
#define PBIN_VERSION 3u
#define PBIN_BLOB_MIN (1u << 20)

// Data many libraries share is saved once, as cache/pbin/blob-<hash>.bin, and kept in memory once read: NVIDIA puts
// the same 2 MB compression dictionary ("_NVDICT_") into every library's binaries (3.4 GB for 1,600 libraries).
static struct { uint64_t hash; void *data; size_t size; } blobs[64];
static int nblobs;
static pthread_mutex_t blob_lock = PTHREAD_MUTEX_INITIALIZER;

static const void *blob_get(uint64_t h, size_t size) {
    pthread_mutex_lock(&blob_lock);
    for (int i = 0; i < nblobs; i++)
        if (blobs[i].hash == h) {
            const void *d = blobs[i].size == size ? blobs[i].data : NULL;
            pthread_mutex_unlock(&blob_lock);
            return d;
        }
    char path[64];
    snprintf(path, sizeof path, "cache/pbin/blob-%016llx.bin", (unsigned long long)h);
    FILE *f = fopen(path, "rb");
    void *d = f && nblobs < 64 ? malloc(size) : NULL;
    if (d && fread(d, 1, size, f) == size) blobs[nblobs++] = (typeof(blobs[0])){h, d, size};
    else free(d), d = NULL;
    if (f) fclose(f);
    pthread_mutex_unlock(&blob_lock);
    return d;
}

static bool blob_put(uint64_t h, const void *data, size_t size) {
    pthread_mutex_lock(&blob_lock);
    bool ok = false;
    for (int i = 0; i < nblobs && !ok; i++) ok = blobs[i].hash == h;
    if (!ok) {
        char path[64], tmp[80];
        snprintf(path, sizeof path, "cache/pbin/blob-%016llx.bin", (unsigned long long)h);
        struct stat st;
        if (stat(path, &st) == 0 && (size_t)st.st_size == size) ok = true;  // (an earlier run's)
        else {
            snprintf(tmp, sizeof tmp, "%s.tmp", path);
            FILE *f = fopen(tmp, "wb");
            ok = f && fwrite(data, 1, size, f) == size;
            if (f) ok = fclose(f) == 0 && ok;
            if (ok) { remove(path); ok = rename(tmp, path) == 0; }
            if (!ok) remove(tmp);
        }
        void *d = ok && nblobs < 64 ? malloc(size) : NULL;
        if (d) { memcpy(d, data, size); blobs[nblobs++] = (typeof(blobs[0])){h, d, size}; }
    }
    pthread_mutex_unlock(&blob_lock);
    return ok;
}

static bool pbin_init(void) {
    static int ok = -1;
    if (ok >= 0) return ok;
    static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&m);
    if (ok < 0) {
        pf_create_bins = (PFN_vkCreatePipelineBinariesKHR)vkGetDeviceProcAddr(g_vk.dev, "vkCreatePipelineBinariesKHR");
        pf_destroy_bin = (PFN_vkDestroyPipelineBinaryKHR)vkGetDeviceProcAddr(g_vk.dev, "vkDestroyPipelineBinaryKHR");
        pf_bin_data = (PFN_vkGetPipelineBinaryDataKHR)vkGetDeviceProcAddr(g_vk.dev, "vkGetPipelineBinaryDataKHR");
        pf_release_captured = (PFN_vkReleaseCapturedPipelineDataKHR)vkGetDeviceProcAddr(g_vk.dev, "vkReleaseCapturedPipelineDataKHR");
        PFN_vkGetPipelineKeyKHR key = (PFN_vkGetPipelineKeyKHR)vkGetDeviceProcAddr(g_vk.dev, "vkGetPipelineKeyKHR");
        pf_pipeline_key = key;
        bool have = pf_create_bins && pf_destroy_bin && pf_bin_data && pf_release_captured && key &&
                    key(g_vk.dev, NULL, &pbin_global) == VK_SUCCESS && pbin_global.keySize <= VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR;
        if (have) {
            mkdir("cache", 0755), mkdir("cache/pbin", 0755);
            // (binaries by library content since 0.2.2: the files before that - by program - are no use, they go once)
            struct stat st;
            if (stat("cache/pbin/content-keys", &st) != 0) {
                DIR *d = opendir("cache/pbin");
                for (struct dirent *e; d && (e = readdir(d)) != NULL;) {
                    if (e->d_name[0] == '.' || !strncmp(e->d_name, "blob-", 5)) continue;  // (the shared dictionaries stay)
                    char path[600];
                    snprintf(path, sizeof path, "cache/pbin/%s", e->d_name);
                    remove(path);
                }
                if (d) closedir(d);
                FILE *f = fopen("cache/pbin/content-keys", "w");
                if (f) fclose(f);
            }
        }
        else LOG("[vk] pipeline binaries: the driver's functions are missing - off\n");
        ok = have;
    }
    pthread_mutex_unlock(&m);
    return ok;
}

// The driver's key of a library's create info (as create_lib_in makes it): saved with its binaries, compared when
// creating from them - a different key means the create info isn't the one they were made from.
static bool lib_key(VkGraphicsPipelineCreateInfo *pi, VkGraphicsPipelineLibraryFlagsEXT part, VkPipelineBinaryKeyKHR *out) {
    const void *next = pi->pNext;
    VkPipelineCreateFlags flags = pi->flags;
    VkGraphicsPipelineLibraryCreateInfoEXT li = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT,
                                                 .pNext = (void *)pi->pNext, .flags = part};
    pi->pNext = &li;
    pi->flags |= VK_PIPELINE_CREATE_LIBRARY_BIT_KHR;
    VkPipelineCreateInfoKHR pc = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_INFO_KHR, .pNext = pi};
    *out = (VkPipelineBinaryKeyKHR){.sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR};
    bool ok = pf_pipeline_key(g_vk.dev, &pc, out) == VK_SUCCESS && out->keySize <= VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR;
    pi->pNext = next;
    pi->flags = flags;
    return ok;
}

// (by the library's content - its shader, the layout, the state - not the program's: one file serves every program with
// that library, as shader_lib shares it in memory; the layout from the program's reflection)
static void slib_key(const Prog *p, bool frag, bool fill, uint64_t k[3]);
static void pbin_path(char *out, size_t n, const Prog *p, bool frag, bool fill, const VkPipelineBinaryKeyKHR *driver_key) {
    uint64_t k[3];
    slib_key(p, frag, fill, k);
    uint64_t hash = hash_words(0x9e3779b97f4a7c15ull ^ 2, k, sizeof k);
    if (driver_key) {
        hash = hash_words(hash, pbin_global.key, pbin_global.keySize);
        hash = hash_words(hash, driver_key->key, driver_key->keySize);
    }
    snprintf(out, n, "cache/pbin/%016llx.bin", (unsigned long long)hash);
}

// The library from its data (VK_NULL_HANDLE: none, or the driver refused it). pi: the create info as for a compile,
// its flags2 (f2) without the capture or fail bits.
static VkPipeline lib_from_binary(const Prog *p, VkGraphicsPipelineCreateInfo *pi, bool frag, bool fill,
                                  VkGraphicsPipelineLibraryFlagsEXT part, bool *driver_miss) {
    VkPipelineBinaryKeyKHR now;
    if (!lib_key(pi, part, &now)) return VK_NULL_HANDLE;
    char path[64];
    pbin_path(path, sizeof path, p, frag, fill, &now);
    FILE *f = fopen(path, "rb");
    bool legacy = !f;
    if (legacy) {
        pbin_path(path, sizeof path, p, frag, fill, NULL);
        f = fopen(path, "rb");
    }
    if (!f) { atomic_fetch_add(&stat_bin_absent, 1); return VK_NULL_HANDLE; }
    uint32_t head[3], n = 0;  // magic, version, count
    VkPipelineBinaryKeyKHR gk = {.sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR};
    VkPipelineBinaryKeyKHR keys[8];
    VkPipelineBinaryDataKHR data[8] = {0};
    VkPipelineBinaryKHR bins[8] = {0};
    bool owned[8] = {0};  // (else a shared blob's)
    VkPipelineBinaryKeyKHR pk = {.sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR};  // (the create info's, when saved)
    bool ok = fread(head, 4, 3, f) == 3 && head[0] == PBIN_MAGIC && head[1] == PBIN_VERSION && (n = head[2]) && n <= 8 &&
              fread(&gk.keySize, 4, 1, f) == 1 && gk.keySize == pbin_global.keySize && gk.keySize <= VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR &&
              fread(gk.key, 1, gk.keySize, f) == gk.keySize && !memcmp(gk.key, pbin_global.key, gk.keySize) &&
              fread(&pk.keySize, 4, 1, f) == 1 && pk.keySize <= VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR &&
              fread(pk.key, 1, pk.keySize, f) == pk.keySize;
    bool same_key = ok && now.keySize == pk.keySize && !memcmp(now.key, pk.key, pk.keySize);
    VkResult rb = VK_SUCCESS;
    for (uint32_t i = 0; ok && same_key && i < n; i++) {  // each: key size, key, data size, 0 + data | 1 + its blob's hash
        uint64_t size = 0, bh = 0;
        uint8_t kind = 0;
        keys[i] = (VkPipelineBinaryKeyKHR){.sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR};
        ok = fread(&keys[i].keySize, 4, 1, f) == 1 && keys[i].keySize <= VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR &&
             fread(keys[i].key, 1, keys[i].keySize, f) == keys[i].keySize && fread(&size, 8, 1, f) == 1 && size &&
             size < (64u << 20) && fread(&kind, 1, 1, f) == 1;
        if (ok && kind == 1) ok = fread(&bh, 8, 1, f) == 1 && (data[i].pData = (void *)blob_get(bh, size));
        else if (ok) ok = kind == 0 && (owned[i] = (data[i].pData = malloc(size)) != NULL) && fread(data[i].pData, 1, size, f) == size;
        data[i].dataSize = size;
    }
    fclose(f);
    VkPipeline lib = VK_NULL_HANDLE;
    if (ok && same_key) {
        VkPipelineBinaryKeysAndDataKHR kd = {.binaryCount = n, .pPipelineBinaryKeys = keys, .pPipelineBinaryData = data};
        VkPipelineBinaryCreateInfoKHR ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_CREATE_INFO_KHR, .pKeysAndDataInfo = &kd};
        VkPipelineBinaryHandlesInfoKHR hi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_HANDLES_INFO_KHR, .pipelineBinaryCount = n,
                                             .pPipelineBinaries = bins};
        if ((rb = pf_create_bins(g_vk.dev, &ci, NULL, &hi)) == VK_SUCCESS) {
            VkPipelineBinaryInfoKHR bi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_INFO_KHR, .pNext = pi->pNext,
                                          .binaryCount = n, .pPipelineBinaries = bins};
            const void *next = pi->pNext;
            pi->pNext = &bi;
            lib = create_lib_in(VK_NULL_HANDLE, pi, part);
            pi->pNext = next;
        }
    }
    if (ok && !lib) {
        static int nlog;
        if (nlog++ < 12) {
            char a[72] = "", b[72] = "";
            for (uint32_t i = 0; i < pk.keySize && i < 32; i++) sprintf(a + 2 * i, "%02x", pk.key[i]);
            for (uint32_t i = 0; i < now.keySize && i < 32; i++) sprintf(b + 2 * i, "%02x", now.key[i]);
            LOG("[vk] pipeline binary %s: binaries %d, library %d, key when saved %s, now %s (%s)\n", path, rb,
                rb == VK_SUCCESS ? t_lib_result : 0, a, b, same_key ? "same" : "DIFFERENT");
        }
    }
    for (uint32_t i = 0; i < n && i < 8; i++) {
        if (bins[i]) pf_destroy_bin(g_vk.dev, bins[i], NULL);
        if (owned[i]) free(data[i].pData);
    }
    if (lib) atomic_fetch_add(&stat_bin_loads, 1);
    else {
        atomic_fetch_add(&stat_bin_refused, 1);
        static int nlog;
        if (nlog++ < 5) LOG("[vk] pipeline binary %s not taken (%s)\n", path,
                             !ok ? "unreadable / other driver" : !same_key ? "pipeline key changed" : "the driver refused it");
        *driver_miss = ok && same_key && rb == VK_SUCCESS && t_lib_result == VK_PIPELINE_COMPILE_REQUIRED;
        if (!legacy || same_key) remove(path);  // a different driver key may still use the legacy file
    }
    return lib;
}

// Saves a library compiled with the capture bit as cache/pbin/<key>.bin, then lets the driver drop the captured data.
static bool lib_save_binary(const Prog *p, VkPipeline lib, bool frag, bool fill, const VkPipelineBinaryKeyKHR *pk) {
    VkPipelineBinaryCreateInfoKHR ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_CREATE_INFO_KHR, .pipeline = lib};
    VkPipelineBinaryKHR bins[8] = {0};
    VkPipelineBinaryHandlesInfoKHR hi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_HANDLES_INFO_KHR};
    bool ok = pf_create_bins(g_vk.dev, &ci, NULL, &hi) == VK_SUCCESS && hi.pipelineBinaryCount && hi.pipelineBinaryCount <= 8;
    uint32_t n = ok ? hi.pipelineBinaryCount : 0;
    if (ok) {
        hi.pPipelineBinaries = bins;
        ok = pf_create_bins(g_vk.dev, &ci, NULL, &hi) == VK_SUCCESS;
    }
    char path[64], tmp[80];
    pbin_path(path, sizeof path, p, frag, fill, pk);
    snprintf(tmp, sizeof tmp, "%s.%llx.tmp", path, (unsigned long long)(uintptr_t)lib);
    FILE *f = ok ? fopen(tmp, "wb") : NULL;
    if (f) {
        uint32_t head[3] = {PBIN_MAGIC, PBIN_VERSION, n};
        ok = fwrite(head, 4, 3, f) == 3 && fwrite(&pbin_global.keySize, 4, 1, f) == 1 &&
             fwrite(pbin_global.key, 1, pbin_global.keySize, f) == pbin_global.keySize &&
             fwrite(&pk->keySize, 4, 1, f) == 1 && fwrite(pk->key, 1, pk->keySize, f) == pk->keySize;
        for (uint32_t i = 0; ok && i < n; i++) {
            VkPipelineBinaryDataInfoKHR di = {.sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_DATA_INFO_KHR, .pipelineBinary = bins[i]};
            VkPipelineBinaryKeyKHR k = {.sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR};
            size_t size = 0;
            void *buf = NULL;
            ok = pf_bin_data(g_vk.dev, &di, &k, &size, NULL) == VK_SUCCESS && size && (buf = malloc(size)) &&
                 pf_bin_data(g_vk.dev, &di, &k, &size, buf) == VK_SUCCESS;
            uint64_t s64 = size, bh = 0;
            uint8_t kind = 0;
            if (ok && size >= PBIN_BLOB_MIN) {  // (shared data: saved once; a blob already known is found by comparing)
                pthread_mutex_lock(&blob_lock);
                for (int b = 0; b < nblobs && !bh; b++)
                    if (blobs[b].size == size && !memcmp(blobs[b].data, buf, size)) bh = blobs[b].hash;
                pthread_mutex_unlock(&blob_lock);
                if (!bh) bh = hash_words(0x2545f4914f6cdd1dull, buf, size & ~(size_t)7) ^ size;
                kind = blob_put(bh, buf, size);
            }
            ok = ok && fwrite(&k.keySize, 4, 1, f) == 1 && fwrite(k.key, 1, k.keySize, f) == k.keySize &&
                 fwrite(&s64, 8, 1, f) == 1 && fwrite(&kind, 1, 1, f) == 1 &&
                 (kind ? fwrite(&bh, 8, 1, f) == 1 : fwrite(buf, 1, size, f) == size);
            free(buf);
        }
        ok = fclose(f) == 0 && ok;
        if (ok) ok = SDL_RenamePath(tmp, path);
        if (!ok) remove(tmp);
    }
    for (uint32_t i = 0; i < n; i++) if (bins[i]) pf_destroy_bin(g_vk.dev, bins[i], NULL);
    VkReleaseCapturedPipelineDataInfoKHR ri = {.sType = VK_STRUCTURE_TYPE_RELEASE_CAPTURED_PIPELINE_DATA_INFO_KHR, .pipeline = lib};
    pf_release_captured(g_vk.dev, &ri, NULL);
    if (ok) atomic_fetch_add(&stat_bin_saves, 1);
    else {
        static int nlog;
        if (nlog++ < 8) LOG("[vk] pipeline binary %s not saved (%u binaries)\n", path, n);
    }
    return ok;
}

// Libraries the driver's pipeline cache had (from runs before the pipeline binaries, or saved when a binary wasn't):
// taken from there in 20-60 ms (NVIDIA, 4K Cascade) every run, where a binary takes 1-3 ms - and a full-screen pass
// holds the picture meanwhile (the same few hitches at the same places every session). Each one taken from the cache
// is compiled once more in the background with the capture bit and saved as a binary (a low-priority worker; the
// driver's own shader cache makes that 3-8 ms each; at most 640 a run). ODYSSEY_PBIN_MIGRATE=0: off.
static VkPipeline shader_lib_make(Prog *p, bool frag, bool fill, bool cached_only, bool capture_only);
static void slib_key(const Prog *p, bool frag, bool fill, uint64_t k[3]);
typedef struct { Prog *p; bool frag, fill; } PbinMigrate;
static void pbin_migrate_job(void *arg) {
    PbinMigrate *m = arg;
    VkPipeline lib = shader_lib_make(m->p, m->frag, m->fill, false, true);
    if (lib) vkDestroyPipeline(g_vk.dev, lib, NULL);
    free(m);
}
static void pbin_migrate(Prog *p, bool frag, bool fill) {
    static int on = -1;
    if (on < 0) on = !getenv("ODYSSEY_PBIN_MIGRATE") || atoi(getenv("ODYSSEY_PBIN_MIGRATE")) != 0;
    if (!on || g_vk.headless) return;
    uint64_t k[3];
    slib_key(p, frag, fill, k);
    uint64_t h = hash_words(0x51ed270b27f3a6c1ull, k, sizeof k) | 1;
    static uint64_t seen[640];
    static int nseen;
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    bool dup = false;
    for (int i = 0; i < nseen; i++) dup |= seen[i] == h;
    bool go = !dup && nseen < 640;
    if (go) seen[nseen++] = h;
    pthread_mutex_unlock(&lock);
    if (!go) return;
    PbinMigrate *m = malloc(sizeof *m);
    *m = (PbinMigrate){p, frag, fill};
    queue_job(pbin_migrate_job, m, 1);
}

// cached_only: only if the driver has it compiled already (otherwise VK_NULL_HANDLE right away)
bool g_pcache_had_data;  // (device.c) cache/vk_pipelines.bin had data at the start
static VkPipeline shader_lib_make(Prog *p, bool frag, bool fill, bool cached_only, bool capture_only) {
    if (!prog_modules(p)) return VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                             .stage = frag ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT,
                                             .module = frag ? p->fs : p->vs, .pName = "main"};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};  // (view mask 0)
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = fill ? VK_POLYGON_MODE_FILL : VK_POLYGON_MODE_LINE, .lineWidth = 1};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineDynamicStateCreateInfo dpr = DYN_INFO(DYN_PR), dfs = DYN_INFO(DYN_FS);
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 1,
                                       .pStages = &stage, .layout = p->layout};
    // (input assembly: ignored by the spec in a pre-rasterization library, but NVIDIA's driver reads it there in some
    // cases - left NULL, a new player's first library compile crashed it reading NULL + 0x18, 2026-11-04)
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    if (frag) { pi.pDepthStencilState = &ds; pi.pDynamicState = &dfs; }
    else { pi.pViewportState = &vs; pi.pRasterizationState = &rs; pi.pDynamicState = &dpr; pi.pInputAssemblyState = &ia; }
    VkGraphicsPipelineLibraryFlagsEXT part = frag ? VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT
                                                  : VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT;
    VkPipelineCreateFlags base = p->huge ? VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT : 0;
    pi.flags = base | (cached_only ? VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT : 0);
    uint64_t t0 = plat_now_ns();
    // its saved data (pipeline binaries), with the flags as flags2: the capture bit has no 32-bit form
    bool pbin = g_vk.pbin && p->spv_key[0] && p->spv_key[1] && pbin_init();
    VkPipelineCreateFlags2CreateInfoKHR f2 = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR,
                                              .pNext = pi.pNext, .flags = base | VK_PIPELINE_CREATE_LIBRARY_BIT_KHR};
    // (not on the render thread at a first draw - a file read and the driver's work, 1-50 ms: hundreds at a stage's start
    // stalled it; a worker does it while the draw waits a frame - except for a full-screen pass, whose wait holds the
    // whole picture)
    bool driver_miss = false;
    if (capture_only && !pbin) return VK_NULL_HANDLE;
    if (pbin && !capture_only && (!cached_only || g_draw_must_wait)) {
        pi.pNext = &f2;
        VkPipeline lib = lib_from_binary(p, &pi, frag, fill, part, &driver_miss);
        pi.pNext = f2.pNext;
        if (lib) {
            shader_time_log(frag ? "binary library fs" : "binary library vs", p->code[1], t0);
            return lib;
        }
    }
    static int log_libs = -1;  // ODYSSEY_LOG_LIBS=1: each library compile as it starts (one that never ends shows)
    if (log_libs < 0) log_libs = getenv("ODYSSEY_LOG_LIBS") != NULL;
    if (log_libs && !cached_only && !capture_only) {
        uint64_t lay = 0;  // (the layout's content: sets, bindings, types - in any order)
        for (int i = 0; i < p->nbinds; i++)
            lay += (((uint64_t)p->binds[i].set << 40 | (uint64_t)p->binds[i].binding << 8 | (uint64_t)p->binds[i].type) + 1) * 0x9E3779B97F4A7C15ull;
        LOG("[lib] start %s vs=%llx fs=%llx spv=%016llx lay=%016llx (ram %llu MB)\n", frag ? "fs" : "vs", (unsigned long long)p->code[0],
            (unsigned long long)p->code[1], (unsigned long long)p->spv_key[frag ? 1 : 0], (unsigned long long)lay,
            (unsigned long long)(plat_private_bytes() >> 20));
    }
    VkPipeline lib = VK_NULL_HANDLE;
    if (driver_miss && !cached_only) {
        // This driver returned COMPILE_REQUIRED for a matching binary. Persist its fallback in VkPipelineCache
        // so the next launch can use the cache lookup instead of recapturing a binary the driver will refuse again.
        lib = create_lib(&pi, part);
    } else if (pbin && !cached_only) {
        // in the pipeline cache from runs before the binaries: taken from there (not saved); otherwise compiled with
        // the capture bit (without the pipeline cache) and saved
        // (only with a pipeline cache from an earlier run: a new player's first compiles crashed NVIDIA's driver in that
        // lookup - an empty cache, FAIL_ON_PIPELINE_COMPILE_REQUIRED, 2026-11-04, depending on how the code was laid out)
        if (g_pcache_had_data && !g_vk.headless && !capture_only) {
            pi.flags = base | VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;
            lib = create_lib(&pi, part);
            pi.flags = base;
            if (lib) pbin_migrate(p, frag, fill);  // (saved as a binary in the background, for the next runs)
        }
        if (!lib) {
            VkPipelineBinaryKeyKHR pk;  // (as the create info will be for its creation from the binaries: no capture bit)
            pi.pNext = &f2;
            bool keyed = lib_key(&pi, part, &pk);
            f2.flags |= VK_PIPELINE_CREATE_2_CAPTURE_DATA_BIT_KHR;
            lib = create_lib_in(VK_NULL_HANDLE, &pi, part);
            pi.pNext = f2.pNext;
            if (lib && keyed) {
                if (!lib_save_binary(p, lib, frag, fill, &pk) && !capture_only) {
                    // (not saved: into the pipeline cache as well, so the next run isn't a compile again)
                    VkPipeline again = create_lib(&pi, part);
                    if (again) vkDestroyPipeline(g_vk.dev, again, NULL);
                }
            } else if (lib) {
                VkReleaseCapturedPipelineDataInfoKHR ri = {.sType = VK_STRUCTURE_TYPE_RELEASE_CAPTURED_PIPELINE_DATA_INFO_KHR, .pipeline = lib};
                pf_release_captured(g_vk.dev, &ri, NULL);
            }
        }
    } else lib = create_lib(&pi, part);
    if (log_libs && !cached_only && !capture_only)
        LOG("[lib] done %s vs=%llx fs=%llx %.1f ms (ram %llu MB)\n", frag ? "fs" : "vs", (unsigned long long)p->code[0],
            (unsigned long long)p->code[1], (plat_now_ns() - t0) / 1e6, (unsigned long long)(plat_private_bytes() >> 20));
    if (capture_only) {
        static _Atomic int nmig;
        if (log_libs || atomic_fetch_add(&nmig, 1) < 3)
            LOG("[vk] library %s vs=%llx fs=%llx saved as a pipeline binary in the background (%.1f ms)\n", frag ? "fs" : "vs",
                (unsigned long long)p->code[0], (unsigned long long)p->code[1], (plat_now_ns() - t0) / 1e6);
        return lib;
    }
    shader_time_log(cached_only ? (frag ? "cached library fs" : "cached library vs") : frag ? "library fs" : "library vs",
                    p->code[1], t0);
    return lib;
}

// Libraries by content: one shader (its SPIR-V) with an identically defined layout and the same state (fill, huge) in
// several programs - material variants sharing a vertex shader, a fragment shader drawn with several vertex shaders -
// was compiled once per program: a quarter of a first session's compiles (Cap Kingdom, 100 s: 1924 compiles, 1461
// distinct). A library made once now serves every program with that content (libraries are never destroyed; layouts
// with the same sets, bindings and types are compatible). ODYSSEY_SHARED_LIBS=0: one per program, as before.
#define SLBITS 14
static struct { uint64_t k[3]; VkPipeline lib; bool busy; } slib_tab[1 << SLBITS];
static uint32_t slib_n;
static pthread_mutex_t slib_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t slib_cv = PTHREAD_COND_INITIALIZER;
static _Atomic uint32_t stat_lib_shared;
uint32_t vk_libs_shared_take(void) { return atomic_exchange(&stat_lib_shared, 0); }  // (the stats line)
static void slib_key(const Prog *p, bool frag, bool fill, uint64_t k[3]) {
    uint64_t lay = 0;  // (the layout's content in any order: every binding's set, number and type)
    for (int i = 0; i < p->nbinds; i++)
        lay += (((uint64_t)p->binds[i].set << 40 | (uint64_t)p->binds[i].binding << 8 | (uint64_t)p->binds[i].type) + 1) * 0x9E3779B97F4A7C15ull;
    k[0] = p->spv_key[frag ? 1 : 0];
    k[1] = lay ^ (uint64_t)p->nbinds << 56;
    k[2] = (uint64_t)frag | (uint64_t)fill << 1 | (uint64_t)p->huge << 2;
}
static int slib_find(const uint64_t k[3], bool add) {  // (slib_lock) the entry's index, -1: none (or no room)
    uint32_t h = (uint32_t)(((k[0] ^ k[1] * 0x9E3779B97F4A7C15ull ^ k[2] * 0xff51afd7ed558ccdull) * 0x9E3779B97F4A7C15ull) >> (64 - SLBITS));
    for (int i = 0; i < 64; i++, h = (h + 1) & ((1 << SLBITS) - 1)) {
        if (!slib_tab[h].lib && !slib_tab[h].busy && !slib_tab[h].k[0]) {
            if (!add || slib_n >= (3u << SLBITS) / 4) return -1;
            memcpy(slib_tab[h].k, k, sizeof slib_tab[h].k);
            slib_n++;
            return (int)h;
        }
        if (!memcmp(slib_tab[h].k, k, sizeof slib_tab[h].k)) return (int)h;
    }
    return -1;
}
// The library with that content when one was made - or, while another thread makes it, once it has (cached_only: not
// waited for); else VK_NULL_HANDLE and *mine: this thread makes it (slib_done)
static VkPipeline slib_claim(const uint64_t k[3], bool cached_only, bool *mine) {
    *mine = false;
    pthread_mutex_lock(&slib_lock);
    int e = slib_find(k, true);
    VkPipeline r = VK_NULL_HANDLE;
    if (e >= 0) {
        while (slib_tab[e].busy && !slib_tab[e].lib && !cached_only) pthread_cond_wait(&slib_cv, &slib_lock);
        r = slib_tab[e].lib;
        if (!r && !slib_tab[e].busy) slib_tab[e].busy = true, *mine = true;
    }
    pthread_mutex_unlock(&slib_lock);
    return r;
}
static void slib_done(const uint64_t k[3], VkPipeline lib) {
    pthread_mutex_lock(&slib_lock);
    int e = slib_find(k, false);
    if (e >= 0) slib_tab[e].lib = lib, slib_tab[e].busy = false;  // (none made: the next one tries)
    pthread_cond_broadcast(&slib_cv);
    pthread_mutex_unlock(&slib_lock);
}
static VkPipeline shader_lib(Prog *p, bool frag, bool fill, bool cached_only) {
    static int on = -1;
    if (on < 0) on = !getenv("ODYSSEY_SHARED_LIBS") || atoi(getenv("ODYSSEY_SHARED_LIBS")) != 0;
    uint64_t k[3];
    bool mine = false;
    if (on && !g_vk.headless && p->spv_key[frag ? 1 : 0]) {  // (the preparation's process destroys what it made)
        slib_key(p, frag, fill, k);
        VkPipeline s = slib_claim(k, cached_only, &mine);
        if (s) { atomic_fetch_add(&stat_lib_shared, 1); return s; }
    }
    VkPipeline lib = shader_lib_make(p, frag, fill, cached_only, false);
    if (mine) slib_done(k, lib);
    return lib;
}

// The two it lacks (a library made once is kept: destroying it gives NVIDIA's memory back only in part, see
// compile_program). Not cached_only: no libraries = whole pipelines from its modules (no_gpl).
static void make_shader_libs(Prog *p, bool cached_only) {
    if (!p->lib_pr[1]) p->lib_pr[1] = shader_lib(p, false, true, cached_only);
    if (!p->lib_fs) p->lib_fs = shader_lib(p, true, true, cached_only);
    if (!cached_only) p->no_gpl = !p->lib_pr[1] || !p->lib_fs;
}

// ---- shader preparation: translate the game archives and prepare their pipeline libraries before a fresh install boots.
typedef struct { uint64_t spv[2]; } PrepItem;
typedef struct { PrepItem program; uint32_t local[3], huge; } FullPrepItem;
#define PRECOMPILE_LIST "cache/precompile-v2.list"
#define PRECOMPILE_NEXT "cache/precompile-v2.next"

// The preparation's process: SuperOdysseyRecomp --prepare-shaders <list> (runtime/main.c). The game's GPU and device
// (device.c vk_headless_init), each listed program's SPIR-V from the disk cache, its libraries compiled and saved as
// pipeline binaries (shader_lib), then gone (with what the driver kept).
static FullPrepItem *pm_items;
#define PREPARED_COMPUTE "cache/prepared-compute.bin"
static int pm_n, pm_base;           // (pm_base: where this round started - the precompile goes in rounds)
static _Atomic int pm_next, pm_done, pm_exited, pm_compute_built, pm_failed;
static long long pm_mem_limit;      // (the precompile: a round ends once the process holds this much - NVIDIA keeps memory
                                    // for every library compiled and destroyed unlinked; the next round's process starts clean)

static VkPipeline create_compute_pipeline(VkShaderModule module, VkPipelineLayout layout, const uint32_t local[3]);

static bool compute_local_supported(const uint32_t local[3]) {
    for (int i = 0; i < 3; i++)
        if (!local[i] || local[i] > g_vk.props.limits.maxComputeWorkGroupSize[i]) return false;
    return (uint64_t)local[0] * local[1] * local[2] <= g_vk.props.limits.maxComputeWorkGroupInvocations;
}

static bool compute_split_x(const uint32_t local[3]) {
    return local[0] == 1536 && local[1] == 1 && local[2] == 1;
}

static bool compute_bindings_supported(const Prog *p) {
    if (g_vk.storage_write_without_format) return true;
    for (int i = 0; i < p->nbinds; i++)
        if (p->binds[i].type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) return false;
    return true;
}

static bool pm_build_compute(Prog *p, const uint32_t *spv, size_t size, const uint32_t local[3]) {
    if (!compute_local_supported(local)) return false;
    p->nbinds = vk_reflect(spv, size, p->binds, MAX_BIND);
    if (!compute_bindings_supported(p)) return false;
    VkShaderModule module = vk_module(spv, size);
    if (!module) return false;
    p->layout = make_layout(p, 1, VK_SHADER_STAGE_COMPUTE_BIT, 0);
    VkPipeline pipeline = create_compute_pipeline(module, p->layout, local);
    if (pipeline) vkDestroyPipeline(g_vk.dev, pipeline, NULL);
    vkDestroyShaderModule(g_vk.dev, module, NULL);
    vkDestroyPipelineLayout(g_vk.dev, p->layout, NULL);
    vkDestroyDescriptorSetLayout(g_vk.dev, p->set_layout[0], NULL);
    return pipeline != VK_NULL_HANDLE;
}

static void *pm_worker(void *arg) {
    (void)arg;
    for (bool first = true;; first = false) {  // (each worker takes one at least: every round gets somewhere)
        if (!first && pm_mem_limit && (long long)plat_private_bytes() > pm_mem_limit) break;
        int i = atomic_fetch_add(&pm_next, 1);
        if (i >= pm_n) break;
        Prog *p = calloc(1, sizeof *p);
        const FullPrepItem *item = &pm_items[i];
        p->spv_key[0] = item->program.spv[0];
        p->spv_key[1] = item->program.spv[1];
        p->huge = item->huge != 0;
        size_t nv = 0, nf = 0;
        uint32_t *sv = vk_spirv_by_key(p->spv_key[0], &nv);
        uint32_t *sf = sv && p->spv_key[1] ? vk_spirv_by_key(p->spv_key[1], &nf) : NULL;
        bool ok = false;
        if (item->local[0]) {
            ok = sv && pm_build_compute(p, sv, nv, item->local);
            if (ok) atomic_fetch_add(&pm_compute_built, 1);
        } else if (sf && g_vk.gpl) {
            p->nbinds = vk_reflect(sv, nv, p->binds, MAX_BIND);
            p->nbinds += vk_reflect(sf, nf, p->binds + p->nbinds, MAX_BIND - p->nbinds);
            // A cache file can outlive the driver or have a different pipeline key.
            // Restore each library during explicit preparation, so unusable files get rebuilt here.
            p->vs = vk_module(sv, nv);
            p->fs = vk_module(sf, nf);
            if (p->vs && p->fs) {
                p->layout = make_layout(p, 2, VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT);
                VkPipeline libs[2] = {shader_lib(p, false, true, false), shader_lib(p, true, true, false)};
                ok = libs[0] && libs[1];
                for (int k = 0; k < 2; k++) if (libs[k]) vkDestroyPipeline(g_vk.dev, libs[k], NULL);
                vkDestroyPipelineLayout(g_vk.dev, p->layout, NULL);
                for (int k = 0; k < 2; k++) vkDestroyDescriptorSetLayout(g_vk.dev, p->set_layout[k], NULL);
            }
            if (p->vs) vkDestroyShaderModule(g_vk.dev, p->vs, NULL);
            if (p->fs) vkDestroyShaderModule(g_vk.dev, p->fs, NULL);
        }
        if (!ok) atomic_fetch_add(&pm_failed, 1);
        free(sv);
        free(sf);
        free(p);
        atomic_fetch_add(&pm_done, 1);
    }
    atomic_fetch_add(&pm_exited, 1);
    return NULL;
}

// The game's RomFS contains NintendoWare BNSH and agl BAHS programs, some inside Yaz0/SARC containers.
typedef struct { const uint8_t *code; uint32_t size; } PcStage;
typedef struct { PcStage stages[2]; uint32_t local[3]; } PcProg;

enum { PC_SEEN_SIZE = 1 << 18, PC_ASSET_LIMIT = 512u << 20 };
static PcProg *pc_progs;
static uint64_t pc_seen[PC_SEEN_SIZE];
static int pc_n, pc_cap, pc_bnsh, pc_bahs, pc_compute;
static _Atomic int pc_next, pc_done, pc_items_n, pc_compute_translated;
static FullPrepItem *pc_items;
static long pc_huge_limit;

static uint32_t pc_le32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t pc_le64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static bool pc_span(size_t size, uint64_t off, uint64_t len) { return off <= size && len <= size - off; }

static bool pc_add(PcStage first, PcStage second, const uint32_t local[3]) {
    if (!first.size || (!second.size && !local)) return true;
    uint64_t h = hash_words(first.size, first.code, first.size);
    h = hash_words(h ^ second.size, second.code, second.size);
    if (local) h = hash_words(h, local, 3 * sizeof *local);
    h |= 1;
    uint32_t slot = (uint32_t)(h >> 32) & (PC_SEEN_SIZE - 1);
    while (pc_seen[slot] && pc_seen[slot] != h) slot = (slot + 1) & (PC_SEEN_SIZE - 1);
    if (pc_seen[slot]) return true;
    if (pc_n == pc_cap) {
        int capacity = pc_cap ? pc_cap * 2 : 4096;
        if (capacity >= PC_SEEN_SIZE) return false;
        PcProg *more = realloc(pc_progs, sizeof *pc_progs * (size_t)capacity);
        if (!more) return false;
        pc_progs = more;
        pc_cap = capacity;
    }
    uint8_t *code = malloc((size_t)first.size + second.size);
    if (!code) return false;
    memcpy(code, first.code, first.size);
    if (second.size) memcpy(code + first.size, second.code, second.size);
    PcProg *prog = &pc_progs[pc_n++];
    *prog = (PcProg){.stages = {{code, first.size}, {code + first.size, second.size}}};
    if (local) { memcpy(prog->local, local, sizeof prog->local); pc_compute++; }
    pc_seen[slot] = h;
    return true;
}

static bool pc_read_stage(const uint8_t *code, uint32_t size, const uint8_t *ctl, uint32_t ctlsz,
                          uint32_t stage, PcStage *out, uint32_t local[3]) {
    if (ctlsz < 0x704 || size < 0x80 || pc_le32(ctl) != 0x98761234) return false;
    uint32_t end = pc_le32(ctl + 0x700);
    if (!end || end > size) end = size;
    if (stage == 3) {
        if (ctlsz < 0x754 || pc_le32(code) == 0x12345678) return false;
        for (int i = 0; i < 3; i++) {
            local[i] = pc_le32(ctl + 0x748 + 4 * i);
            if (!local[i]) return false;
        }
        if (pc_le32(code) == 0x12345679) {
            if (end <= 0x108) return false;
            code += 0x100;
            end -= 0x100;
        }
    } else if (end < 0x50 || pc_le32(code) != 0x12345678 ||
               ((pc_le32(code + 0x30) >> 10) & 15) != (stage == 0 ? 1 : 5)) return false;
    if (end < 8) return false;
    *out = (PcStage){code, end};
    return true;
}

static bool pc_bnsh_stage(const uint8_t *bn, size_t size, uint64_t rec, uint32_t stage,
                          PcStage *out, uint32_t local[3]) {
    if (!rec || !pc_span(size, rec, 0x20)) return false;
    uint64_t ctl = pc_le64(bn + rec + 8), code = pc_le64(bn + rec + 0x10);
    uint32_t len = pc_le32(bn + rec + 0x18), ctlsz = pc_le32(bn + rec + 0x1c);
    return pc_span(size, ctl, ctlsz) && pc_span(size, code, len) &&
           pc_read_stage(bn + code, len, bn + ctl, ctlsz, stage, out, local);
}

static bool pc_scan_bnsh(const uint8_t *bn, size_t size) {
    if (size < 0x80) return true;
    uint32_t grsc = (uint32_t)bn[0x16] | (uint32_t)bn[0x17] << 8;
    if (!pc_span(size, grsc, 0x28) || memcmp(bn + grsc, "grsc", 4)) return true;
    uint32_t count = pc_le32(bn + grsc + 0x1c);
    uint64_t vars = pc_le64(bn + grsc + 0x20);
    if (!pc_span(size, vars, (uint64_t)count * 64)) return true;
    pc_bnsh++;
    for (uint32_t v = 0; v < count; v++) {
        uint64_t prog = pc_le64(bn + vars + (uint64_t)v * 64 + 0x10);
        if (!prog || !pc_span(size, prog, 0x38)) continue;
        uint64_t vs = pc_le64(bn + prog + 8), fs = pc_le64(bn + prog + 0x28), cs = pc_le64(bn + prog + 0x30);
        PcStage vertex, fragment, compute;
        uint32_t local[3];
        if (cs && pc_bnsh_stage(bn, size, cs, 3, &compute, local) && !pc_add(compute, (PcStage){0}, local)) return false;
        if (pc_le64(bn + prog + 0x10) || pc_le64(bn + prog + 0x18) || pc_le64(bn + prog + 0x20)) continue;
        if (pc_bnsh_stage(bn, size, vs, 0, &vertex, NULL) &&
            pc_bnsh_stage(bn, size, fs, 1, &fragment, NULL) && !pc_add(vertex, fragment, NULL)) return false;
    }
    return true;
}

static bool pc_bahs_stage(const uint8_t *ba, uint32_t region, uint32_t region_size, size_t entry,
                          uint32_t kind, PcStage *out, uint32_t local[3]) {
    uint32_t offset = pc_le32(ba + entry), ctlsz = pc_le32(ba + entry + 8);
    int64_t ctl = (int64_t)entry + (int32_t)pc_le32(ba + entry + 12);
    if (pc_le32(ba + entry + 4) || offset % 0x100 || ctlsz < 0x708 || ctlsz > 0x900 ||
        ctl < 0 || !pc_span(region, (uint64_t)ctl, ctlsz) || pc_le32(ba + ctl) != 0x98761234) return false;
    uint32_t len = pc_le32(ba + ctl + 0x704);
    return pc_span(region_size, offset, len) &&
           pc_read_stage(ba + region + offset, len, ba + ctl, ctlsz, kind, out, local);
}

static bool pc_scan_bahs(const uint8_t *ba, size_t size) {
    if (size < 0x20) return true;
    uint32_t region_size = pc_le32(ba + 0x18), region = pc_le32(ba + 0x1c);
    if (!pc_span(size, region, region_size)) return true;

    // Find the binary table by its first NVN entry. The program table that follows groups the
    // ordinal vertex and fragment entries into variations.
    for (size_t table = 0x20; table + 0x28 <= region; table += 4) {
        size_t bytes = pc_le32(ba + table);
        uint32_t count = pc_le32(ba + table + 4), kind = pc_le32(ba + table + 12);
        PcStage first;
        uint32_t local[3];
        if (!count || bytes < 8 + (uint64_t)count * 0x20 || !pc_span(region, table, bytes + 8) ||
            (kind > 1 && kind != 3) || !pc_bahs_stage(ba, region, region_size, table + 0x18, kind, &first, local)) continue;
        size_t programs = table + bytes, pbytes = pc_le32(ba + programs);
        uint32_t nprog = pc_le32(ba + programs + 4);
        if (!nprog || pbytes < 8 + (uint64_t)nprog * 16 || !pc_span(region, programs, pbytes)) continue;

        size_t at = table + 8;
        uint32_t records = 0;
        for (; records < count && pc_span(programs, at, 0x20); records++) {
            uint32_t len = pc_le32(ba + at);
            if (len < 0x20 || !pc_span(programs, at, len)) break;
            at += len;
        }
        if (records != count || at != programs) continue;
        size_t end = programs + pbytes;
        size_t row = programs + 8;
        uint32_t prev = 0;
        uint32_t rows = 0;
        for (; rows < nprog && pc_span(end, row, 16); rows++) {
            uint32_t len = pc_le32(ba + row), base = pc_le32(ba + row + 12);
            if (len < 16 || !pc_span(end, row, len) || base < prev || base > count || (!rows && base)) break;
            prev = base;
            row += len;
        }
        if (rows != nprog || row != end) continue;

        PcStage *vs = calloc(count, sizeof *vs), *fs = calloc(count, sizeof *fs);
        bool ok = vs && fs;
        at = table + 8;
        row = programs + 8;
        uint32_t base = 0;
        for (uint32_t i = 0; ok && i < nprog; i++) {
            row += pc_le32(ba + row);
            uint32_t limit = i + 1 == nprog ? count : pc_le32(ba + row + 12);
            uint32_t nv = 0, nf = 0;
            for (; ok && base < limit; base++, at += pc_le32(ba + at)) {
                kind = pc_le32(ba + at + 4);
                PcStage stage = {0};
                if (kind == 0) {
                    pc_bahs_stage(ba, region, region_size, at + 0x10, kind, &stage, NULL);
                    vs[nv++] = stage;
                } else if (kind == 1) {
                    pc_bahs_stage(ba, region, region_size, at + 0x10, kind, &stage, NULL);
                    fs[nf++] = stage;
                } else if (kind == 3 && pc_bahs_stage(ba, region, region_size, at + 0x10, kind, &stage, local)) {
                    ok = pc_add(stage, (PcStage){0}, local);
                }
            }
            for (uint32_t v = 0; ok && v < nv && v < nf; v++)
                if (vs[v].size && fs[v].size) ok = pc_add(vs[v], fs[v], NULL);
        }
        free(vs);
        free(fs);
        pc_bahs++;
        return ok;
    }
    return true;
}

static bool pc_scan_asset(const uint8_t *data, size_t size, void *arg) {
    unsigned depth = *(const unsigned *)arg;
    if (size < 4) return true;
    unsigned next = depth + 1;
    if (!memcmp(data, "Yaz0", 4)) {
        if (depth == 8 || size < 16 || data[4] >= PC_ASSET_LIMIT >> 24) return false;
        size_t n;
        uint8_t *decoded = yaz0_decode(data, size, &n);
        if (!decoded) return false;
        bool ok = pc_scan_asset(decoded, n, &next);
        free(decoded);
        return ok;
    }
    if (!memcmp(data, "SARC", 4)) return depth < 8 && sarc_visit_files(data, size, pc_scan_asset, &next);
    if (!memcmp(data, "BAHS", 4)) return pc_scan_bahs(data, size);
    const uint8_t *end = data + size - 3;
    while (data < end) {
        data = memchr(data, 'B', (size_t)(end - data));
        if (!data) break;
        size_t remaining = (size_t)(end - data) + 3;
        if (!memcmp(data, "BNSH", 4) && !pc_scan_bnsh(data, remaining)) return false;
        if (!memcmp(data, "BAHS", 4) && !pc_scan_bahs(data, remaining)) return false;
        data++;
    }
    return true;
}

static void *pc_worker(void *arg) {
    (void)arg;
    ShaderKey vkey, fkey;
    memset(&vkey, 0, sizeof vkey);
    memset(&fkey, 0, sizeof fkey);
    vkey.glsl = fkey.glsl = 1;
    vkey.flip_y = fkey.flip_y = false;   // (Odyssey: nvnDeviceSetWindowOriginMode 1 - as base_keys has it in the game)
    vkey.depth_minus_one = true;         // (depth mode left at 0)
    for (int i; (i = atomic_fetch_add(&pc_next, 1)) < pc_n; atomic_fetch_add(&pc_done, 1)) {
        const PcProg *pp = &pc_progs[i];
        const PcStage *stages = pp->stages;
        if (pp->local[0]) {
            ShaderKey key = {.glsl = 1, .split_compute_x = compute_split_x(pp->local)};
            ShaderInfo *cs = maxwell_translate(stages[0].code, stages[0].size, STAGE_COMPUTE, &key);
            if (cs && cs->msl) {
                size_t size;
                uint32_t *spv = vk_glsl_to_spirv(cs->msl, STAGE_COMPUTE, "cs", &size);
                if (spv) {
                    FullPrepItem *item = &pc_items[atomic_fetch_add(&pc_items_n, 1)];
                    item->program.spv[0] = vk_spirv_last_key;
                    memcpy(item->local, pp->local, sizeof item->local);
                    if (key.split_compute_x) item->local[0] /= 3;
                    atomic_fetch_add(&pc_compute_translated, 1);
                    free(spv);
                }
            } else if (cs) {
                LOG("[precompile] compute translation failed: %s\n", cs->error);
            }
            if (cs) maxwell_free(cs);
            continue;
        }
        ShaderKey fk = fkey;
        ShaderInfo *vi = maxwell_translate(stages[0].code, stages[0].size, STAGE_VERTEX, &vkey), *fi = NULL;
        if (vi && vi->msl) {
            fk.vs_out_mask = vi->out_mask;
            fi = maxwell_translate(stages[1].code, stages[1].size, STAGE_FRAGMENT, &fk);
        }
        if (vi && vi->msl && fi && fi->msl) {
            bool huge = pc_huge_limit > 0 &&
                        (strlen(vi->msl) > (size_t)pc_huge_limit || strlen(fi->msl) > (size_t)pc_huge_limit);
            size_t nv = 0, nf = 0;
            uint32_t *sv = vk_glsl_to_spirv(vi->msl, STAGE_VERTEX, "vs", &nv);
            uint64_t kv = vk_spirv_last_key;
            uint32_t *sf = sv ? vk_glsl_to_spirv(fi->msl, STAGE_FRAGMENT, "fs", &nf) : NULL;
            uint64_t kf = vk_spirv_last_key;
            if (sv && sf) pc_items[atomic_fetch_add(&pc_items_n, 1)] = (FullPrepItem){.program = {{kv, kf}}, .huge = huge};
            free(sv);
            free(sf);
        } else {
            const ShaderInfo *failed = !vi || !vi->msl ? vi : fi;
            if (failed) LOG("[precompile] graphics translation failed: %s\n", failed->error);
        }
        if (vi) maxwell_free(vi);
        if (fi) maxwell_free(fi);
    }
    return NULL;
}

static int pc_compare_items(const void *a, const void *b) {
    const FullPrepItem *first = a, *second = b;
    bool first_compute = first->local[0] != 0, second_compute = second->local[0] != 0;
    if (first_compute != second_compute) return first_compute ? -1 : 1;
    return memcmp(first, second, sizeof *first);
}

// Translate the static programs from the installed game before preparing their Vulkan binaries.
static bool precompile_collect(const char *out, int nt) {
    uint64_t t0 = plat_now_ns();
    unsigned depth = 0;
    LOG("[precompile] scanning extracted/romfs.bin for shader archives\n");
    bool ok = romfs_visit_files("extracted/romfs.bin", pc_scan_asset, &depth);
    static const char *const modules[] = {"rtld", "main", "sdk", "subsdk0"};
    for (size_t i = 0; ok && i < sizeof modules / sizeof *modules; i++) {
        char path[64];
        snprintf(path, sizeof path, "extracted/exefs/%s", modules[i]);
        struct stat st;
        if (stat(path, &st) == 0) ok = nso_visit_segments(path, pc_scan_asset, &depth);
    }
    LOG("[precompile] %d graphics and %d compute programs from %d BNSH and %d BAHS archives in %.1f s\n",
        pc_n - pc_compute, pc_compute, pc_bnsh, pc_bahs, (plat_now_ns() - t0) / 1e9);
    if (!ok || !pc_n) {
        LOG("[precompile] %s\n", ok ? "no programs found" : "could not finish scanning game files");
        goto done;
    }
    pc_items = calloc((size_t)pc_n, sizeof *pc_items);
    if (!pc_items) { ok = false; goto done; }
    const char *huge = getenv("ODYSSEY_HUGE_SHADER");
    pc_huge_limit = huge ? atol(huge) : 1536 * 1024;
    maxwell_gmem_base = HEAP_BASE;
    maxwell_gmem_size = HEAP_MAX;
    pthread_t th[16];
    int workers = 0;
    while (workers < nt && !pthread_create(&th[workers], NULL, pc_worker, NULL)) workers++;
    if (!workers) pc_worker(NULL);
    for (int last = -1; atomic_load(&pc_done) < pc_n; SDL_Delay(100)) {
        int dn = atomic_load(&pc_done);
        FILE *pf;
        if (dn != last && (pf = fopen("cache/prepare.progress", "w"))) { fprintf(pf, "%d %d 0\n", dn, pc_n); fclose(pf); last = dn; }
    }
    for (int i = 0; i < workers; i++) pthread_join(th[i], NULL);
    int ni = atomic_load(&pc_items_n);
    int nc = atomic_load(&pc_compute_translated);
    qsort(pc_items, (size_t)ni, sizeof *pc_items, pc_compare_items);
    int unique = 0;
    for (int i = 0; i < ni; i++)
        if (!unique || pc_compare_items(&pc_items[i], &pc_items[unique - 1]))
            pc_items[unique++] = pc_items[i];
    FILE *f = unique ? fopen(PRECOMPILE_LIST ".tmp", "wb") : NULL;
    ok = f && fwrite(pc_items, sizeof *pc_items, (size_t)unique, f) == (size_t)unique;
    if (f) ok = fclose(f) == 0 && ok;
    if (ok) { remove(out); ok = rename(PRECOMPILE_LIST ".tmp", out) == 0; }
    LOG("[precompile] translated %d of %d graphics and %d of %d compute programs to %d unique entries in %.1f s\n",
        ni - nc, pc_n - pc_compute, nc, pc_compute, unique, (plat_now_ns() - t0) / 1e9);
done:
    free(pc_items);
    for (int i = 0; i < pc_n; i++) free((void *)pc_progs[i].stages[0].code);
    free(pc_progs);
    return ok && pc_n;
}

static bool save_prepared_compute(void) {
    // The preparation list starts with compute programs. Keep their exact workgroup sizes for the next boot.
    int count = 0;
    while (count < pm_n && pm_items[count].local[0]) count++;
    FILE *f = fopen(PREPARED_COMPUTE ".tmp", "wb");
    if (!f) return false;
    bool ok = fwrite(pm_items, sizeof *pm_items, (size_t)count, f) == (size_t)count;
    ok = fclose(f) == 0 && ok;
    if (ok && SDL_RenamePath(PREPARED_COMPUTE ".tmp", PREPARED_COMPUTE)) return true;
    remove(PREPARED_COMPUTE ".tmp");
    return false;
}

// The preparation's process: list "all" = the precompile (device.c starts it at the first start and after an update):
// the archives are translated once, then compiled in rounds - a round's process exits with 2
// once it holds pm_mem_limit (the next round goes on from the saved offset), with 0 when the list is done, with 3
// when there was nothing to precompile. Any other list: the programs the game left in it, in one go.
static int prepare_recorded_pipelines(void);

int vk_prepare_main(const char *list) {
    if (!strcmp(list, "recorded")) return prepare_recorded_pipelines();
    extern void vk_headless_init(void);
    int nt = SDL_GetNumLogicalCPUCores() - 1;  // (the player waits for it: before the game starts)
    if (nt < 1) nt = 1;
    if (nt > 16) nt = 16;
    bool all = !strcmp(list, "all");
    if (all) {
        struct stat st;
        if (stat(PRECOMPILE_LIST, &st) != 0 && !precompile_collect(PRECOMPILE_LIST, nt)) return 3;
        list = PRECOMPILE_LIST;
        const char *e = getenv("ODYSSEY_PRECOMPILE_MEM_MB");  // (a round's memory; default 3 GB)
        pm_mem_limit = (e ? atoll(e) : 3072) << 20;
    }
    FILE *f = fopen(list, "rb");
    if (!f) { LOG("[prep] no %s\n", list); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    size_t record = all ? sizeof *pm_items : sizeof(PrepItem);
    pm_n = sz > 0 ? (int)((size_t)sz / record) : 0;
    pm_items = calloc((size_t)pm_n, sizeof *pm_items);
    if (!pm_items || !pm_n) { fclose(f); free(pm_items); return 1; }
    int read = 0;
    while (read < pm_n && fread(&pm_items[read], record, 1, f) == 1) read++;
    fclose(f);
    if (read != pm_n) { free(pm_items); return 1; }
    if (all && (f = fopen(PRECOMPILE_NEXT, "r"))) {
        if (fscanf(f, "%d", &pm_base) != 1 || pm_base < 0 || pm_base > pm_n) pm_base = 0;
        fclose(f);
    }
    atomic_store(&pm_next, pm_base);
    uint64_t t0 = plat_now_ns();
    vk_headless_init();
    if (!g_vk.gpl && !all) { LOG("[prep] no pipeline libraries on this GPU: nothing to do\n"); return 0; }
    // (no pipeline binaries: the libraries go into the game's pipeline cache instead - device.c vk_pcache_load_headless)
    bool bins = g_vk.pbin && pbin_init();
    extern void vk_pcache_load_headless(void);
    extern void vk_pipeline_cache_save(void);
    if (all || !bins) vk_pcache_load_headless();
    if (!bins) {  // (one round: the cache holds what is compiled, so the process's memory is the cache - no NVIDIA here)
        LOG("[prep] no pipeline binaries on this GPU: compiled into the pipeline cache\n");
        pm_mem_limit = 0;
    }
    pthread_t th[16];
    int workers = 0;
    while (workers < nt && !pthread_create(&th[workers], NULL, pm_worker, NULL)) workers++;
    if (!workers) pm_worker(NULL);
    for (int last = -1; atomic_load(&pm_exited) < workers; SDL_Delay(100)) {  // (the game's window shows it)
        int d = pm_base + atomic_load(&pm_done);
        if (d != last && (f = fopen("cache/prepare.progress", "w"))) { fprintf(f, "%d %d 1\n", d, pm_n); fclose(f); last = d; }
    }
    for (int i = 0; i < workers; i++) pthread_join(th[i], NULL);
    int next = pm_base + atomic_load(&pm_done);  // (taken and finished: a worker stops before taking past the limit)
    if (all || !bins) vk_pipeline_cache_save();
    LOG("[prep] %d programs (%d..%d of %d) in %.1f s, %d compute pipelines, %u libraries restored, %u rebuilt and saved, %d unavailable, %llu MB\n",
        next - pm_base, pm_base, next, pm_n, (double)(plat_now_ns() - t0) / 1e9, atomic_load(&pm_compute_built),
        atomic_load(&stat_bin_loads), atomic_load(&stat_bin_saves), atomic_load(&pm_failed), (unsigned long long)(plat_private_bytes() >> 20));
    bool compute_saved = !all || next < pm_n || save_prepared_compute();
    free(pm_items);
    if (!compute_saved) {
        LOG("[prep] could not save prepared compute programs\n");
        return 1;
    }
    if (!all) return 0;
    if (next < pm_n) {
        f = fopen(PRECOMPILE_NEXT, "w");
        if (!f) return 1;
        bool wrote = fprintf(f, "%d\n", next) > 0;
        if (fclose(f) != 0 || !wrote) return 1;
        return 2;
    }
    remove(PRECOMPILE_NEXT);
    remove(PRECOMPILE_LIST);
    return 0;
}

// the interface parts, by the state they bake in (render thread; collisions of the 64-bit keys are ignored as for
// whole pipelines)
#define LIB_TAB 8192
static struct { uint64_t key; VkPipeline lib; } vi_tab[LIB_TAB], fo_tab[LIB_TAB];

static VkPipeline lib_lookup(void *tab_, uint64_t h, bool *found) {
    struct { uint64_t key; VkPipeline lib; } *tab = tab_;
    uint32_t slot = (uint32_t)(h ^ (h >> 29)) & (LIB_TAB - 1), probes = 0;
    while (tab[slot].key && tab[slot].key != h && ++probes < LIB_TAB) slot = (slot + 1) & (LIB_TAB - 1);
    *found = tab[slot].key == h;
    return *found ? tab[slot].lib : (VkPipeline)(uintptr_t)(slot + 1u);  // (not found: the free slot, + 1)
}

static VkPipeline vertex_input_lib(const PipeKey *k) {
    uint64_t h = hash_words(0x5bd1e995ull, k->attr_fmt, offsetof(PipeKey, blend) - offsetof(PipeKey, attr_fmt));
    h = hmix(h, &k->topo_class, 1);
    if (!h) h = 1;
    bool found;
    VkPipeline v = lib_lookup(vi_tab, h, &found);
    if (found) return v;
    uint32_t slot = (uint32_t)(uintptr_t)v - 1u;
    if (vi_tab[slot].key) return VK_NULL_HANDLE;  // (table full)
    VkVertexInputAttributeDescription va[32];
    VkVertexInputBindingDescription vb[17];
    VkVertexInputBindingDivisorDescriptionEXT vd[17];
    uint32_t na = 0, nb = 0, nd = 0;
    for (int i = 0; i < 32; i++)
        if (k->attr_mask & (1u << i)) va[na++] = (VkVertexInputAttributeDescription){(uint32_t)i, k->attr_bind[i], k->attr_fmt[i], k->attr_off[i]};
    for (uint32_t s = 0; s < 17; s++) {
        if (!(k->used_streams & (1u << s))) continue;
        bool inst = k->divisor[s] != 0;
        vb[nb++] = (VkVertexInputBindingDescription){s, k->stride[s], inst ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX};
        if (inst && k->divisor[s] > 1 && g_vk.divisor) vd[nd++] = (VkVertexInputBindingDivisorDescriptionEXT){s, k->divisor[s]};
    }
    VkPipelineVertexInputDivisorStateCreateInfoEXT dvi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT,
                                                          .vertexBindingDivisorCount = nd, .pVertexBindingDivisors = vd};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, .pNext = nd ? &dvi : NULL,
                                               .vertexBindingDescriptionCount = nb, .pVertexBindingDescriptions = vb,
                                               .vertexAttributeDescriptionCount = na, .pVertexAttributeDescriptions = va};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = topo_class_base(k->topo_class)};
    VkPipelineDynamicStateCreateInfo dy = DYN_INFO(DYN_VI);
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pVertexInputState = &vi,
                                       .pInputAssemblyState = &ia, .pDynamicState = &dy};
    VkPipeline lib = create_lib(&pi, VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT);
    if (lib) { vi_tab[slot].key = h; vi_tab[slot].lib = lib; }
    return lib;
}

static VkPipeline fragment_output_lib(const PipeKey *k) {
    uint64_t h = hash_words(0x27d4eb2dull, k->color, offsetof(PipeKey, attr_fmt) - offsetof(PipeKey, color));
    h = hash_words(h, k->blend, sizeof k->blend);
    h = hmix(h, &k->blend_on, 2);  // (blend_on, a2c)
    h = hash_words(h, k->write_mask, sizeof k->write_mask);
    if (!h) h = 1;
    bool found;
    VkPipeline v = lib_lookup(fo_tab, h, &found);
    if (found) return v;
    uint32_t slot = (uint32_t)(uintptr_t)v - 1u;
    if (fo_tab[slot].key) return VK_NULL_HANDLE;
    VkPipelineColorBlendAttachmentState cba[8] = {0};
    for (uint32_t i = 0; i < k->ncolor; i++) {
        cba[i].colorWriteMask = k->write_mask[i];
        if ((k->blend_on >> i) & 1) {
            const uint8_t *b = k->blend[i];
            cba[i].blendEnable = VK_TRUE;
            cba[i].srcColorBlendFactor = blend_factor(b[1]);
            cba[i].dstColorBlendFactor = blend_factor(b[2]);
            cba[i].srcAlphaBlendFactor = blend_factor(b[3]);
            cba[i].dstAlphaBlendFactor = blend_factor(b[4]);
            cba[i].colorBlendOp = blend_op(b[5]);
            cba[i].alphaBlendOp = blend_op(b[6]);
        }
    }
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .attachmentCount = k->ncolor, .pAttachments = cba};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = (VkSampleCountFlagBits)k->samples, .alphaToCoverageEnable = k->a2c};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = k->ncolor,
                                        .pColorAttachmentFormats = k->color, .depthAttachmentFormat = k->depth,
                                        .stencilAttachmentFormat = k->stencil};
    VkPipelineDynamicStateCreateInfo dy = DYN_INFO(DYN_FO);
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .pColorBlendState = &cb,
                                       .pMultisampleState = &ms, .pDynamicState = &dy};
    VkPipeline lib = create_lib(&pi, VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT);
    if (lib) { fo_tab[slot].key = h; fo_tab[slot].lib = lib; }
    return lib;
}

// The draw's pipeline from the four parts (render thread). false: no libraries for it - build it whole (build_pipeline).
static bool link_pipeline(Pipe *pp) {
    PipeKey *k = &pp->k;
    Prog *p = k->prog;
    if (p->no_gpl || !p->lib_fs) return false;
    if (!p->lib_pr[k->poly_fill]) {  // (line mode: rare, compiled when first needed)
        p->lib_pr[k->poly_fill] = shader_lib(p, false, k->poly_fill, false);
        if (!p->lib_pr[k->poly_fill]) return false;
    }
    VkPipeline libs[4] = {vertex_input_lib(k), p->lib_pr[k->poly_fill], p->lib_fs, fragment_output_lib(k)};
    if (!libs[0] || !libs[3]) return false;
    VkPipelineLibraryCreateInfoKHR lci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR, .libraryCount = 4, .pLibraries = libs};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &lci, .layout = p->layout};
    uint64_t t0 = plat_now_ns();
    VkResult r = vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &pp->pipe);
    shader_time_log("link", p->code[1], t0);
    if (r != VK_SUCCESS) {
        static int n;
        if (n++ < 10) LOG("[vk] pipeline link failed: %d\n", r);
        return false;
    }
    atomic_fetch_add(&stat_links, 1);
    extern _Atomic unsigned g_pcache_dirty;
    atomic_fetch_add(&g_pcache_dirty, 1);
    atomic_store(&pp->state, 1);
    return true;
}

// Persist the baked draw state and SPIR-V identities. Guest addresses are only valid during the run that recorded them.
enum { PIPE_STATE_OFFSET = offsetof(PipeKey, color), PIPE_RECIPE_LIMIT = PIPE_TAB / 4 };
typedef struct {
    uint64_t spv[2];
    uint32_t huge, reserved;
    uint8_t state[sizeof(PipeKey) - PIPE_STATE_OFFSET];
} PipeRecipe;
typedef struct {
    uint32_t version, size, count, reserved;
    uint8_t uuid[VK_UUID_SIZE];
    uint64_t checksum;
} PipeRecipeHeader;

static PipeRecipe pipe_recipes[PIPE_RECIPE_LIMIT];
static uint64_t pipe_recipe_seen[PIPE_TAB];
static uint32_t pipe_recipe_count, pipe_recipe_saved;
static pthread_mutex_t pipe_recipe_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t pipe_recipe_io = PTHREAD_MUTEX_INITIALIZER;
static bool pipe_prewarm_enabled;
enum { FIRST_SCENE_PIPELINES = 1024 };
#define FIRST_SCENE_CACHE "cache/first-scene-pipelines.bin"
static uint64_t first_scene_keys[FIRST_SCENE_PIPELINES];
static uint32_t first_scene_count;
static _Atomic bool first_scene_ready;
static bool first_scene_saved;
static struct { uint64_t hash; Prog *prog; } warm_programs[PIPE_TAB];
#define COMPUTE_WARM_SLOTS 4096
static struct { uint64_t hash; VkPipeline pipe; Prog *prog; } compute_warm[COMPUTE_WARM_SLOTS];

static PipeRecipe pipe_recipe(const PipeKey *k) {
    PipeRecipe r;
    memset(&r, 0, sizeof r);
    memcpy(r.spv, k->prog->spv_key, sizeof r.spv);
    r.huge = k->prog->huge;
    memcpy(r.state, (const uint8_t *)k + PIPE_STATE_OFFSET, sizeof r.state);
    return r;
}

static uint64_t pipe_recipe_hash(const PipeRecipe *r) {
    uint64_t hash = hash_words(0xcbf29ce484222325ull, r, sizeof *r);
    return hash ? hash : 1;
}

static uint32_t load_first_scene_keys(uint64_t keys[FIRST_SCENE_PIPELINES]) {
    FILE *f = fopen(FIRST_SCENE_CACHE, "rb");
    if (!f) return 0;
    PipeRecipeHeader h;
    bool ok = fread(&h, sizeof h, 1, f) == 1 && h.version == 1 && h.size == sizeof *keys &&
              h.count <= FIRST_SCENE_PIPELINES && !memcmp(h.uuid, g_vk.props.pipelineCacheUUID, sizeof h.uuid) &&
              fread(keys, sizeof *keys, h.count, f) == h.count && fgetc(f) == EOF &&
              hash_words(0, keys, h.count * sizeof *keys) == h.checksum;
    fclose(f);
    return ok ? h.count : 0;
}

static void record_first_scene_pipeline(Pipe *pipe, uint64_t key) {
    if (atomic_load_explicit(&first_scene_ready, memory_order_relaxed) || !game_is_odyssey()) return;
    extern int odyssey_first_scene_warmup(void);
    int phase = odyssey_first_scene_warmup();
    if (phase == 1 && !pipe->first_scene) {
        pipe->first_scene = true;
        if (first_scene_count < FIRST_SCENE_PIPELINES) first_scene_keys[first_scene_count++] = key;
    }
    if (phase == 2 || first_scene_count == FIRST_SCENE_PIPELINES) {
        atomic_store(&first_scene_ready, true);
        extern _Atomic unsigned g_pcache_dirty;
        if (first_scene_count) atomic_fetch_add(&g_pcache_dirty, 1);
    }
}

// The renderer publishes the list once. The existing cache writer persists it off the render thread.
static void save_first_scene_keys(void) {
    if (first_scene_saved || !atomic_load(&first_scene_ready) || !first_scene_count) return;
    uint64_t old[FIRST_SCENE_PIPELINES];
    size_t bytes = first_scene_count * sizeof *first_scene_keys;
    if (load_first_scene_keys(old) == first_scene_count && !memcmp(old, first_scene_keys, bytes)) {
        first_scene_saved = true;
        return;
    }
    PipeRecipeHeader h = {.version = 1, .size = sizeof *first_scene_keys, .count = first_scene_count,
                          .checksum = hash_words(0, first_scene_keys, bytes)};
    memcpy(h.uuid, g_vk.props.pipelineCacheUUID, sizeof h.uuid);
    FILE *f = fopen(FIRST_SCENE_CACHE ".tmp", "wb");
    if (!f) return;
    bool ok = fwrite(&h, sizeof h, 1, f) == 1 && fwrite(first_scene_keys, 1, bytes, f) == bytes;
    ok = fclose(f) == 0 && ok;
    first_scene_saved = ok && SDL_RenamePath(FIRST_SCENE_CACHE ".tmp", FIRST_SCENE_CACHE);
    if (!first_scene_saved) remove(FIRST_SCENE_CACHE ".tmp");
    else LOG("[vk] saved %u first-scene pipeline priorities\n", first_scene_count);
}

static PipeRecipe compute_recipe(uint64_t spv, const uint32_t local[3]) {
    PipeRecipe r = {0};
    r.spv[0] = spv;
    memcpy(r.state, local, 3 * sizeof *local);
    return r;
}

static uint32_t pipe_slot(uint64_t hash) {
    uint32_t slot = (uint32_t)(hash ^ (hash >> 29)) & (PIPE_TAB - 1);
    for (uint32_t i = 0; i < PIPE_TAB; i++, slot = (slot + 1) & (PIPE_TAB - 1))
        if (!pipe_tab[slot].key || pipe_tab[slot].key == hash) return slot;
    return PIPE_TAB;
}

// Caller holds pipe_recipe_lock, or is starting the renderer before the guest can issue draws.
static void pipe_recipe_add(const PipeRecipe *r) {
    if (pipe_recipe_count >= PIPE_RECIPE_LIMIT) return;
    uint64_t hash = pipe_recipe_hash(r);
    uint32_t slot = (uint32_t)(hash ^ (hash >> 29)) & (PIPE_TAB - 1);
    while (pipe_recipe_seen[slot] && pipe_recipe_seen[slot] != hash) slot = (slot + 1) & (PIPE_TAB - 1);
    if (pipe_recipe_seen[slot]) return;
    pipe_recipe_seen[slot] = hash;
    pipe_recipes[pipe_recipe_count++] = *r;
}

static void pipe_recipe_remember(const PipeRecipe *r) {
    pthread_mutex_lock(&pipe_recipe_lock);
    uint32_t before = pipe_recipe_count;
    pipe_recipe_add(r);
    bool changed = pipe_recipe_count != before;
    pthread_mutex_unlock(&pipe_recipe_lock);
    extern _Atomic unsigned g_pcache_dirty;
    if (changed) atomic_fetch_add(&g_pcache_dirty, 1);
}

static uint32_t compute_warm_slot(uint64_t hash) {
    uint32_t slot = (uint32_t)(hash ^ (hash >> 29)) & (COMPUTE_WARM_SLOTS - 1);
    for (uint32_t i = 0; i < COMPUTE_WARM_SLOTS; i++, slot = (slot + 1) & (COMPUTE_WARM_SLOTS - 1))
        if (!compute_warm[slot].hash || compute_warm[slot].hash == hash) return slot;
    return COMPUTE_WARM_SLOTS;
}

static VkPipeline create_compute_pipeline(VkShaderModule module, VkPipelineLayout layout, const uint32_t local[3]) {
    VkSpecializationMapEntry entries[3] = {{0, 0, 4}, {1, 4, 4}, {2, 8, 4}};
    VkSpecializationInfo spec = {3, entries, 3 * sizeof *local, local};
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main",
                                                .pSpecializationInfo = &spec},
                                      .layout = layout};
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkResult result = vkCreateComputePipelines(g_vk.dev, g_vk.pcache, 1, &ci, NULL, &pipeline);
    if (result != VK_SUCCESS) LOG("[compute] pipeline failed: %d\n", result);
    return result == VK_SUCCESS ? pipeline : VK_NULL_HANDLE;
}

static bool prewarm_compute(const PipeRecipe *r, uint64_t hash) {
    uint32_t slot = compute_warm_slot(hash);
    if (slot == COMPUTE_WARM_SLOTS || compute_warm[slot].pipe) return false;
    uint32_t local[3];
    memcpy(local, r->state, sizeof local);
    if (!compute_local_supported(local)) return false;
    size_t n = 0;
    uint32_t *spv = vk_spirv_by_key(r->spv[0], &n);
    if (!spv) return false;
    Prog *p = calloc(1, sizeof *p);
    if (!p) { free(spv); return false; }
    p->nbinds = vk_reflect(spv, n, p->binds, MAX_BIND);
    if (!compute_bindings_supported(p)) { free(spv); free(p); return false; }
    VkShaderModule module = vk_module(spv, n);
    free(spv);
    if (!module) { free(p); return false; }
    p->layout = make_layout(p, 1, VK_SHADER_STAGE_COMPUTE_BIT, 0);
    VkPipeline pipeline = create_compute_pipeline(module, p->layout, local);
    vkDestroyShaderModule(g_vk.dev, module, NULL);
    if (!pipeline) {
        vkDestroyPipelineLayout(g_vk.dev, p->layout, NULL);
        vkDestroyDescriptorSetLayout(g_vk.dev, p->set_layout[0], NULL);
        free(p);
        return false;
    }
    compute_warm[slot].hash = hash;
    compute_warm[slot].pipe = pipeline;
    compute_warm[slot].prog = p;
    return true;
}

static void prewarm_prepared_compute(void) {
    FILE *f = fopen(PREPARED_COMPUTE, "rb");
    if (!f) return;
    uint32_t warmed = 0, read = 0;
    FullPrepItem item;
    while (read < COMPUTE_WARM_SLOTS && fread(&item, sizeof item, 1, f) == 1) {
        read++;
        if (!item.program.spv[0] || item.program.spv[1] || !compute_local_supported(item.local)) continue;
        PipeRecipe recipe = compute_recipe(item.program.spv[0], item.local);
        warmed += prewarm_compute(&recipe, pipe_recipe_hash(&recipe));
    }
    fclose(f);
    LOG("[vk] prewarmed %u of %u prepared compute pipelines\n", warmed, read);
}

void vk_pipeline_recipes_save(void) {
    pthread_mutex_lock(&pipe_recipe_io);
    save_first_scene_keys();
    pthread_mutex_lock(&pipe_recipe_lock);
    uint32_t count = pipe_recipe_count;
    pthread_mutex_unlock(&pipe_recipe_lock);
    // Published recipes are immutable, so the file write can run while the renderer appends new ones.
    if (count == pipe_recipe_saved) { pthread_mutex_unlock(&pipe_recipe_io); return; }
    PipeRecipeHeader h = {.version = 1, .size = sizeof(PipeRecipe), .count = count,
                          .checksum = hash_words(0, pipe_recipes, count * sizeof(PipeRecipe))};
    memcpy(h.uuid, g_vk.props.pipelineCacheUUID, sizeof h.uuid);
    FILE *f = fopen("cache/graphics-pipelines.tmp", "wb");
    if (f) {
        bool ok = fwrite(&h, sizeof h, 1, f) == 1 && fwrite(pipe_recipes, sizeof(PipeRecipe), count, f) == count;
        ok = fclose(f) == 0 && ok;
        if (ok && SDL_RenamePath("cache/graphics-pipelines.tmp", "cache/graphics-pipelines.bin")) pipe_recipe_saved = count;
        else remove("cache/graphics-pipelines.tmp");
    }
    pthread_mutex_unlock(&pipe_recipe_io);
}

static bool pipe_recipe_program(const PipeRecipe *r, Prog *p) {
    size_t nv = 0, nf = 0;
    uint32_t *sv = vk_spirv_by_key(r->spv[0], &nv);
    uint32_t *sf = sv ? vk_spirv_by_key(r->spv[1], &nf) : NULL;
    if (sf) {
        p->spv_key[0] = r->spv[0];
        p->spv_key[1] = r->spv[1];
        p->huge = r->huge != 0;
        p->nbinds = vk_reflect(sv, nv, p->binds, MAX_BIND);
        p->nbinds += vk_reflect(sf, nf, p->binds + p->nbinds, MAX_BIND - p->nbinds);
        p->vs = vk_module(sv, nv);
        p->fs = vk_module(sf, nf);
    }
    free(sv);
    free(sf);
    if (!p->vs || !p->fs) return false;
    p->layout = make_layout(p, 2, VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT);
    return true;
}

static Prog *prewarm_program(const PipeRecipe *r, bool use_libraries) {
    uint64_t hash = hash_words(r->huge, r->spv, sizeof r->spv) | 1;
    uint32_t slot = (uint32_t)(hash ^ (hash >> 29)) & (PIPE_TAB - 1);
    while (warm_programs[slot].hash && warm_programs[slot].hash != hash) slot = (slot + 1) & (PIPE_TAB - 1);
    if (warm_programs[slot].prog) return warm_programs[slot].prog;
    Prog *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    if (!pipe_recipe_program(r, p)) {
        if (p->vs) vkDestroyShaderModule(g_vk.dev, p->vs, NULL);
        if (p->fs) vkDestroyShaderModule(g_vk.dev, p->fs, NULL);
        free(p);
        return NULL;
    }
    if (use_libraries) make_shader_libs(p, false);
    // Keep the program's Vulkan objects alive alongside the prewarmed pipelines, as for programs built in play.
    warm_programs[slot].hash = hash;
    warm_programs[slot].prog = p;
    return p;
}

// A recipe from another GPU (the release's seed, see below) is only used where this GPU can build it: its attachment
// and vertex formats supported here.
static bool recipe_formats_ok(const PipeRecipe *r) {
    PipeKey k;
    memset(&k, 0, sizeof k);
    memcpy((uint8_t *)&k + PIPE_STATE_OFFSET, r->state, sizeof r->state);
    if (k.ncolor > 8) return false;
    VkFormatProperties p;
    for (uint32_t i = 0; i < k.ncolor; i++) {
        if (!k.color[i]) continue;
        vkGetPhysicalDeviceFormatProperties(g_vk.phys, k.color[i], &p);
        if (!(p.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) return false;
    }
    VkFormat ds[2] = {k.depth, k.stencil};
    for (int i = 0; i < 2; i++) {
        if (!ds[i]) continue;
        vkGetPhysicalDeviceFormatProperties(g_vk.phys, ds[i], &p);
        if (!(p.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) return false;
    }
    for (int i = 0; i < 32; i++)
        if ((k.attr_mask & (1u << i)) && vertex_format_supported((VkFormat)k.attr_fmt[i]) != (VkFormat)k.attr_fmt[i]) return false;
    return true;
}

// cache/graphics-pipelines.bin: the pipelines this PC's runs recorded, for this GPU (its pipeline cache UUID). A file
// whose UUID is all zero is the release's seed (data/pipeline-seed.bin, copied in by the setup: the pipeline states
// recorded while playing on another PC): its recipes are kept where this GPU supports their formats.
static PipeRecipe *load_pipeline_recipes(PipeRecipeHeader *h) {
    FILE *f = fopen("cache/graphics-pipelines.bin", "rb");
    if (!f) return NULL;
    PipeRecipe *recipes = NULL;
    static const uint8_t seed_uuid[VK_UUID_SIZE];
    bool seed = false;
    if (fread(h, sizeof *h, 1, f) == 1 && h->version == 1 && h->size == sizeof(PipeRecipe) &&
        h->count && h->count <= PIPE_RECIPE_LIMIT &&
        (!memcmp(h->uuid, g_vk.props.pipelineCacheUUID, sizeof h->uuid) || (seed = !memcmp(h->uuid, seed_uuid, sizeof h->uuid)))) {
        recipes = malloc(h->count * sizeof *recipes);
        if (recipes && (fread(recipes, sizeof *recipes, h->count, f) != h->count || fgetc(f) != EOF ||
                        hash_words(0, recipes, h->count * sizeof *recipes) != h->checksum)) {
            free(recipes);
            recipes = NULL;
        }
    }
    fclose(f);
    if (recipes && seed) {
        uint32_t kept = 0;
        for (uint32_t i = 0; i < h->count; i++)
            if (recipe_formats_ok(&recipes[i])) recipes[kept++] = recipes[i];
        LOG("[vk] pipeline seed: %u of %u recipes fit this GPU\n", kept, h->count);
        h->count = kept;
        h->checksum = hash_words(0, recipes, kept * sizeof *recipes);
        if (!kept) { free(recipes); recipes = NULL; }
    }
    return recipes;
}

static bool warm_graphics_pipeline(const PipeRecipe *r, uint64_t hash, bool use_libraries) {
    uint32_t slot = pipe_slot(hash);
    if (slot == PIPE_TAB || pipe_tab[slot].pipe) return false;
    Pipe *pp = calloc(1, sizeof *pp);
    if (!pp) return false;
    memcpy((uint8_t *)&pp->k + PIPE_STATE_OFFSET, r->state, sizeof r->state);
    if (r->spv[0] && pp->k.ncolor <= 8 && pp->k.poly_fill <= 1 && pp->k.topo_class <= 2)
        pp->k.prog = prewarm_program(r, use_libraries);
    if (pp->k.prog && (!use_libraries || !link_pipeline(pp))) build_pipeline(pp);
    if (atomic_load(&pp->state) != 1) {
        free(pp);
        return false;
    }
    pp->recorded = true;
    pipe_tab[slot].key = hash;
    pipe_tab[slot].pipe = pp;
    return true;
}

static bool warm_uses_libraries(void) {
    const char *shared = getenv("ODYSSEY_SHARED_LIBS");
    return g_vk.gpl && (!shared || atoi(shared) != 0);
}

static void prewarm_post_pipelines(void);

static uint32_t *prioritize_first_scene(const PipeRecipe *recipes, uint32_t count, uint32_t recent) {
    if (!game_is_odyssey()) return NULL;
    uint64_t keys[FIRST_SCENE_PIPELINES];
    uint32_t n = load_first_scene_keys(keys);
    if (!n) return NULL;
    uint32_t *order = malloc(count * sizeof *order);
    uint64_t *hashes = malloc(count * sizeof *hashes);
    if (!order || !hashes) { free(order); free(hashes); return NULL; }
    for (uint32_t i = 0; i < count; i++) hashes[i] = pipe_recipe_hash(&recipes[i]);
    uint32_t next = 0;
    for (uint32_t j = 0; j < n; j++) {
        if (!keys[j]) continue;
        for (uint32_t i = 0; i < count; i++) {
            if (hashes[i] != keys[j]) continue;
            hashes[i] = 0;
            order[next++] = i;
            break;
        }
    }
    LOG("[vk] prioritizing %u first-scene pipelines at startup\n", next);
    for (uint32_t i = count - recent; i < count; i++) if (hashes[i]) order[next++] = i;
    for (uint32_t i = 0; i < count - recent; i++) if (hashes[i]) order[next++] = i;
    free(hashes);
    return order;
}

#define RECORDED_NEXT "cache/prepare-pipelines.next"
static int prepare_recorded_pipelines(void) {
    extern void vk_headless_init(void);
    extern void vk_pcache_load_headless(void);
    extern void vk_pipeline_cache_save(void);
    vk_headless_init();
    vk_pcache_load_headless();
    prewarm_post_pipelines();
    PipeRecipeHeader h;
    PipeRecipe *recipes = load_pipeline_recipes(&h);
    if (!recipes) {
        LOG("[prep] no compatible recorded pipeline states for this device\n");
        remove(RECORDED_NEXT);
        vk_pipeline_cache_save();
        return 0;
    }
    bool libraries = warm_uses_libraries();
    uint32_t start = 0;
    unsigned long long checksum;
    FILE *f = fopen(RECORDED_NEXT, "r");
    if (f) {
        if (fscanf(f, "%llx %u", &checksum, &start) != 2 || checksum != h.checksum || start >= h.count)
            start = 0;
        fclose(f);
    }
    const char *limit = getenv("ODYSSEY_PRECOMPILE_MEM_MB");
    uint64_t budget = (limit ? strtoull(limit, NULL, 10) : 3072ull) * 1048576ull;
    uint64_t baseline = plat_private_bytes();
    uint32_t next = start, graphics = 0, compute = 0;
    while (next < h.count) {
        const PipeRecipe *r = &recipes[next++];
        uint64_t hash = pipe_recipe_hash(r);
        if (r->spv[1]) graphics += warm_graphics_pipeline(r, hash, libraries);
        else compute += prewarm_compute(r, hash);
        if ((next - start) % 16 == 0 || next == h.count) {
            f = fopen("cache/prepare.progress", "w");
            if (f) { fprintf(f, "%u %u 2\n", next, h.count); fclose(f); }
            if (budget && plat_private_bytes() > baseline + budget) break;
        }
    }
    free(recipes);
    vk_pipeline_cache_save();
    LOG("[prep] recorded %u graphics and %u compute pipelines, %u unavailable (%u..%u of %u)\n",
        graphics, compute, next - start - graphics - compute, start, next, h.count);
    if (next == h.count) {
        remove(RECORDED_NEXT);
        return 0;
    }
    f = fopen(RECORDED_NEXT ".tmp", "w");
    if (!f) return 1;
    bool wrote = fprintf(f, "%llx %u\n", (unsigned long long)h.checksum, next) > 0;
    wrote = fclose(f) == 0 && wrote;
    if (wrote && SDL_RenamePath(RECORDED_NEXT ".tmp", RECORDED_NEXT)) return 2;
    remove(RECORDED_NEXT ".tmp");
    return 1;
}

void vk_pipeline_prewarm(bool build) {
    const char *e = getenv("ODYSSEY_PIPELINE_PREWARM");
    pipe_prewarm_enabled = e ? atoi(e) != 0 : !settings_low_memory();
    if (build) prewarm_post_pipelines();
    if (!pipe_prewarm_enabled) return;
    if (build) prewarm_prepared_compute();
    PipeRecipeHeader h;
    PipeRecipe *recipes = load_pipeline_recipes(&h);
    if (!recipes) return;

    uint64_t t0 = plat_now_ns(), memory = plat_private_bytes();
    // Allow a longer replay on machines with enough RAM to keep the known pipelines alive.
    const char *mem_mb = getenv("ODYSSEY_PIPELINE_PREWARM_MEM_MB");
    const char *time_ms = getenv("ODYSSEY_PIPELINE_PREWARM_MS");
    unsigned set_ms, set_mb;  // (the menu's Warm-up at start; the environment variables win, for tests)
    settings_warmup_budget(&set_ms, &set_mb);
    uint64_t memory_budget = (mem_mb ? strtoull(mem_mb, NULL, 10) : set_mb) * 1048576ull;
    uint64_t time_budget = (time_ms ? strtoull(time_ms, NULL, 10) : set_ms) * 1000000ull;
    bool use_libraries = warm_uses_libraries();
    uint32_t warmed = 0, compute = 0;
    // Give recently discovered pipelines a share of the fixed startup budget. Otherwise
    // long caches can stop at the same early entries on every boot and miss gameplay effects.
    uint32_t recent = odyssey_native_120_enabled() ? h.count / 4 : 0;
    if (recent > 768) recent = 768;
    uint32_t *order = build ? prioritize_first_scene(recipes, h.count, recent) : NULL;
    uint32_t attempted = 0;
    for (; build && attempted < h.count; attempted++) {
        uint32_t index = order ? order[attempted] : attempted < recent ? h.count - recent + attempted : attempted - recent;
        const PipeRecipe *r = &recipes[index];
        // Driver allocations for libraries can be much larger than the recipe file. Keep startup bounded.
        if (attempted && attempted % 16 == 0 &&
            (plat_private_bytes() > memory + memory_budget || plat_now_ns() - t0 > time_budget)) break;
        uint64_t hash = pipe_recipe_hash(r);
        if (!r->spv[1]) { compute += prewarm_compute(r, hash); continue; }
        warmed += warm_graphics_pipeline(r, hash, use_libraries);
    }
    free(order);
    for (uint32_t i = 0; i < h.count; i++) pipe_recipe_add(&recipes[i]);
    pipe_recipe_saved = pipe_recipe_count;
    free(recipes);
    atomic_store(&stat_compiles, 0);
    atomic_store(&stat_links, 0);
    if (build)
        LOG("[vk] prewarmed %u graphics and %u compute pipelines (%u attempted of %u recorded) in %.2f s\n",
            warmed, compute, attempted, h.count, (plat_now_ns() - t0) / 1e9);
    else
        LOG("[vk] kept %u pipeline recipes for the next normal start\n", h.count);
}

// (ODYSSEY_HUGE_AHEAD=1, tests) what a draw of a huge program costs: a link with one RGBA8 target, as a draw would
static void huge_link_test(Prog *p) {
    if (!g_vk.gpl || !p->lib_fs || !p->lib_pr[1]) return;
    Pipe tp = {.k = {.prog = p, .ncolor = 1, .samples = 1, .poly_fill = 1, .topo_class = 2}};
    tp.k.color[0] = VK_FORMAT_R8G8B8A8_UNORM;
    tp.k.write_mask[0] = 0xF;
    vk_render_lock();  // (the interface parts' tables are the render thread's)
    uint64_t t0 = plat_now_ns();
    bool ok = link_pipeline(&tp);
    LOG("[shader] huge program fs=%llx: test link %s in %.1f ms (ram %llu MB)\n", (unsigned long long)p->code[1],
        ok ? "done" : "failed", (plat_now_ns() - t0) / 1e6, (unsigned long long)(plat_private_bytes() >> 20));
    vk_render_unlock();
}

// ================================================================== feedback snapshots (renderer.m resolve_feedback)
static NvnTexture *feedback_tex[4];
static VkImageView feedback_view[4];
static int nfeedback;
static uint64_t g_cur_fs_hash;
#ifdef _WIN32
#endif
static NvnTexture *snap_tex[8];
static bool snap_ok[8];
static const char *snap_why[8];
static const char *snap_reason = "?";
static void snapshots_invalidate(NvnTexture *t) {
    for (int i = 0; i < 8; i++) if ((!t || snap_tex[i] == t) && snap_ok[i]) { snap_ok[i] = false; snap_why[i] = snap_reason; }
}

typedef struct { NvnTexture *key; TexBackend copy; } Snap;
static Snap snaps[16];
static int nsnaps;

static NvnTexture *shader_texture_object(int stage, ShaderTexture *st_) {
    int slot = ((int)st_->handle_offset - 0x20) / 8;
    uint64_t h = (slot >= 0 && slot < 32) ? st.tex[stage][slot] : 0;
    uint32_t tic = h & 0xFFFFF;
    if (!h || !st.tpool || tic >= (uint32_t)st.tpool->count) return NULL;
    return ((NvnTexEntry *)st.tpool->entries)[tic].tex;
}

// Survey the pre-HUD scene inputs without readback, jitter, or altering the frame.
static void dlss_probe_scene(ShaderInfo *fs) {
    static int enabled = -1;
    static uint64_t next_frame;
    if (enabled < 0) {
        const char *probe = getenv("ODYSSEY_DLSS_PROBE");
        enabled = probe && !strcmp(probe, "1");
    }
    bool upscale = false;
#ifdef _WIN32
    upscale = vk_temporal_enabled();
#endif
    if (!enabled && !upscale) return;
    uint64_t frame = renderer_present_count();
    if (!upscale && frame < next_frame) return;
    NvnTexture *target = st.rt.color[0];
    const TexBackend *color = NULL, *depth = NULL;
    for (int i = 0; i < fs->ntex; i++) {
        NvnTexture *t = shader_texture_object(1, &fs->tex[i]);
        const TexBackend *b = t ? t->backend : NULL;
        if (!b || !b->image) continue;
        if (t->width < target->width * 3u || t->height < target->height * 3u) continue;
        if (b->format == VK_FORMAT_B10G11R11_UFLOAT_PACK32) color = b;
        if (b->format == VK_FORMAT_D32_SFLOAT) depth = b;
    }
    if (!color || !depth || color->width != depth->width || color->height != depth->height) return;
#ifdef _WIN32
    if (upscale) vk_dlss_capture_depth(depth, frame);
#endif
    if (!enabled || frame < next_frame) return;
    next_frame = frame + 120;
    LOG("[dlss] frame %llu bloom candidate fs %016llx: %ux%u HDR color + D32 depth -> %ux%u guest bloom\n",
        (unsigned long long)frame, (unsigned long long)g_cur_fs_hash, color->width, color->height,
        target->width, target->height);
}

static bool is_attachment(NvnTexture *t) {
    if (!t) return false;
    if (t == st.rt.depth) return true;
    for (int i = 0; i < st.rt.ncolor; i++) if (t == st.rt.color[i]) return true;
    return false;
}

static TexBackend *snapshot_image(NvnTexture *t, TexBackend *src) {
    Snap *s = NULL;
    for (int i = 0; i < nsnaps; i++) if (snaps[i].key == t) s = &snaps[i];
    if (!s) {
        if (nsnaps == 16) { static int rr; s = &snaps[rr++ & 15]; } else s = &snaps[nsnaps++];
        s->key = t;
    }
    TexBackend *d = &s->copy;
    if (d->image && (d->width != src->width || d->height != src->height || d->format != src->format)) {
        vk_views_forget(d);  // (resolution change)
        vk_defer_image(d->image, d->mem);
        d->image = VK_NULL_HANDLE;
        d->mem = VK_NULL_HANDLE;
    }
    if (!d->image) {
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = src->format,
                                .extent = {src->width, src->height, 1}, .mipLevels = 1, .arrayLayers = 1,
                                .samples = (VkSampleCountFlagBits)src->samples, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &d->image, &d->mem, NULL));
        d->format = src->format; d->width = src->width; d->height = src->height; d->depth = 1; d->layers = 1; d->levels = 1;
        d->samples = src->samples; d->aspect = src->aspect; d->base_type = VK_IMAGE_VIEW_TYPE_2D;
        vk_image_init_layout(vk_cmd(), d->image, d->aspect);
    }
    return d;
}

// (debug frames) a depth image's depth values as a greyscale PGM, stretched over its own min..max
static void debug_save_depth(TexBackend *b, const char *path) {
    if (!b || !b->image || !(b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) || b->format != VK_FORMAT_D32_SFLOAT_S8_UINT) return;
    uint64_t n = (uint64_t)b->width * b->height;
    VkSpan out = vk_readback(n * 4);
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    VkBufferImageCopy c = {.bufferOffset = out.off, .imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1},
                           .imageExtent = {b->width, b->height, 1}};
    vkCmdCopyImageToBuffer(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, out.buf, 1, &c);
    renderer_flush();
    const float *d = out.ptr;
    float lo = 1e30f, hi = -1e30f;
    for (uint64_t i = 0; i < n; i++) { if (d[i] < lo) lo = d[i]; if (d[i] > hi) hi = d[i]; }
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P5 %u %u 255\n", b->width, b->height);
    for (uint64_t i = 0; i < n; i++) fputc(hi > lo ? (int)((d[i] - lo) / (hi - lo) * 255.0f) : 0, f);
    fclose(f);
    LOG("[vk] saved %s (%ux%u depth %.6f..%.6f)\n", path, b->width, b->height, lo, hi);
}

// (debug) ODYSSEY_COPY_TRACE=<frame>: draws' attachments / sampled textures, copies and snapshots of that frame ("[ct]")
static bool ct_on(void) {
    static const char *e = (const char *)1;
    if (e == (const char *)1) e = getenv("ODYSSEY_COPY_TRACE");
    if (!e) return false;
    static uint64_t memo_f = ~0ull;
    static bool memo;
    uint64_t f = renderer_present_count();
    if (f == memo_f) return memo;
    memo_f = f;
    memo = false;
    for (const char *p = e; *p;) {
        if ((uint64_t)strtoll(p, (char **)&p, 10) == f) { memo = true; break; }
        if (*p == ',') p++; else break;
    }
    if (memo) LOG("[ct] ===== frame %llu\n", (unsigned long long)f);
    return memo;
}
static void ct_draw(Prog *pr, const char *what) {
    LOG("[ct] %s fs %016llx rt", what, (unsigned long long)g_cur_fs_hash);
    for (int i = 0; i < st.rt.ncolor; i++) if (st.rt.color[i]) LOG(" c%d=%p/%#x", i, (void *)st.rt.color[i], st.rt.color[i]->format);
    if (st.rt.depth) LOG(" d=%p/%#x%s", (void *)st.rt.depth, st.rt.depth->format, st.ds[0] && st.ds[1] ? "W" : "");
    LOG(" cm %08x vp %.0f %.0f %.0f %.0f sc %d %d %d %d | tex", st.channel_mask, st.viewport[0][0], st.viewport[0][1], st.viewport[0][2],
        st.viewport[0][3], st.scissor[0][0], st.scissor[0][1], st.scissor[0][2], st.scissor[0][3]);
    ShaderInfo *infos[2] = {pr->vi, pr->fi};
    for (int sg = 0; sg < 2; sg++)
        for (int i = 0; i < (infos[sg] ? infos[sg]->ntex : 0); i++) {
            NvnTexture *t = shader_texture_object(sg, &infos[sg]->tex[i]);
            if (t) LOG(" %s%p/%#x", sg ? "" : "v:", (void *)t, t->format);
        }
    LOG("\n");
}

// ================================================================== copy elision (depth copies)
// The game copies its main depth buffer whole, every frame, into textures that are then only sampled while the
// source keeps its depth (the water / transparency depth input), or that get their depth cleared right away
// (stencil kept). At 3x internal resolution each such copy is ~0.15 ms of pure bandwidth. Instead of copying, the
// destination becomes an alias: sampling it (and copying from it) reads the source's image. The alias ends before
// anything changes either side:
//   - the source is about to be overwritten whole (both aspects cleared): the two images swap - the destination
//     keeps the contents without a copy, the source is cleared in the destination's old image;
//   - the destination is about to be overwritten in some aspects (its depth cleared): only the other aspects are
//     copied (stencil, a quarter of the bytes);
//   - anything else (a draw writing depth / stencil, a partial clear, a copy or upload into either, the
//     destination bound as an attachment, a texture freed or reallocated): the real copy is made first.
// Only same-size, same-format, single-level, single-sample depth/stencil images, whole-image copies.
// ODYSSEY_VK_COPY_OPT=0: always copy (the old behaviour).
typedef struct { NvnTexture *src, *dst; VkImage simg, dimg; } CopyAlias;
extern uint32_t vk_view_gen;
static CopyAlias aliases[8];
static int naliases;
static uint64_t stat_alias_made, stat_alias_renamed, stat_alias_partial, stat_alias_copied;

static int copy_opt(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("ODYSSEY_VK_COPY_OPT"); on = e && *e ? atoi(e) : 1; }
    return on;
}

// still valid: both textures keep the images the alias was made with (a resolution change reallocates them)
static bool alias_valid(const CopyAlias *a) {
    const TexBackend *s = a->src->backend, *d = a->dst->backend;
    return s && d && s->image == a->simg && d->image == a->dimg;
}

static void alias_drop(int i) {
    aliases[i] = aliases[--naliases];
    vk_view_gen++;  // (exec.c's per-frame view memo: the destination's views change)
}

// the real copy of the aspects asked for, into the destination's own image
static void alias_copy(CopyAlias *a, VkImageAspectFlags aspects, const char *why) {
    TexBackend *s = a->src->backend, *d = a->dst->backend;
    aspects &= d->aspect;
    if (!aspects) return;
    vk_end_pass();
    st.rt_dirty = true;
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    gp_mark(cb, "copy texture (deferred, %s%s) %ux%u", why, aspects == d->aspect ? "" : ", stencil only", a->dst->width, a->dst->height);
    VkImageCopy c = {.srcSubresource = {aspects, 0, 0, 1}, .dstSubresource = {aspects, 0, 0, 1}, .extent = {d->width, d->height, 1}};
    vkCmdCopyImage(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    if (aspects == d->aspect) stat_alias_copied++; else stat_alias_partial++;
    if (ct_on()) LOG("[ct] alias copy %p -> %p aspects %#x (%s)\n", (void *)a->src, (void *)a->dst, aspects, why);
}

// t is about to be written; full: the aspects of it the write overwrites whole (0: unknown / partial)
static void alias_before_write(NvnTexture *t, VkImageAspectFlags full, const char *why) {
    if (!naliases || !t) return;
    for (int i = naliases - 1; i >= 0; i--) {
        CopyAlias *a = &aliases[i];
        if (!alias_valid(a)) { alias_drop(i); continue; }
        if (a->dst != t) continue;
        // (ODYSSEY_VK_COPY_OPT=2: all aspects even then - for comparing the stencil-only copy's cost)
        alias_copy(a, ((TexBackend *)t->backend)->aspect & (copy_opt() == 2 ? ~0u : ~full), why);
        alias_drop(i);
    }
    NvnTexture *heir = NULL;  // t overwritten whole: one destination can take its image instead of a copy
    if (t->backend && (((TexBackend *)t->backend)->aspect & ~full) == 0)
        for (int i = 0; i < naliases && !heir; i++) if (aliases[i].src == t) heir = aliases[i].dst;
    for (int i = naliases - 1; i >= 0; i--) {
        CopyAlias *a = &aliases[i];
        if (a->src != t || a->dst == heir) continue;
        alias_copy(a, ((TexBackend *)a->dst->backend)->aspect, why);
        alias_drop(i);
    }
    int rename = -1;
    for (int i = 0; i < naliases; i++) if (heir && aliases[i].src == t && aliases[i].dst == heir) rename = i;
    if (rename >= 0) {
        CopyAlias *a = &aliases[rename];
        TexBackend *s = a->src->backend, *d = a->dst->backend;
        vk_end_pass();
        st.rt_dirty = true;
        VkImage im = s->image; s->image = d->image; d->image = im;
        VmaAllocation m = s->mem; s->mem = d->mem; d->mem = m;
        vk_views_forget(s);
        vk_views_forget(d);
        stat_alias_renamed++;
        if (ct_on()) LOG("[ct] alias rename %p -> %p (%s)\n", (void *)a->src, (void *)a->dst, why);
        alias_drop(rename);
    }
}

// the texture whose image holds t's contents (t itself unless it is an alias)
static NvnTexture *alias_read(NvnTexture *t) {
    for (int i = 0; i < naliases; i++)
        if (aliases[i].dst == t) {
            if (alias_valid(&aliases[i])) return aliases[i].src;
            alias_drop(i);
            return t;
        }
    return t;
}

// copy_texture: a whole-image copy that can be an alias instead (true: done)
static bool alias_make(NvnTexture *src, NvnTexture *dst, TexBackend *s, TexBackend *d) {
    if (!copy_opt() || src == dst) return false;
    if (!(s->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))) return false;
    if (s->format != d->format || s->width != d->width || s->height != d->height || s->aspect != d->aspect || s->levels != 1 ||
        d->levels != 1 || s->layers != 1 || d->layers != 1 || s->depth != 1 || d->depth != 1 || s->samples != 1 || d->samples != 1 ||
        s->base_type != d->base_type || s->presented || d->presented || !s->image || !d->image || !s->gpu_owned || !d->gpu_owned)
        return false;
    // the destination's previous contents (or alias) are replaced whole; a destination that is itself the source of
    // aliases gets those resolved first
    alias_before_write(dst, d->aspect, "copy dst");
    src = alias_read(src);  // (a copy of an alias: of its source)
    s = src->backend;
    if (src == dst) return true;  // (copied back onto itself)
    if (naliases == (int)(sizeof aliases / sizeof aliases[0])) return false;
    aliases[naliases++] = (CopyAlias){src, dst, s->image, d->image};
    vk_view_gen++;
    stat_alias_made++;
    return true;
}

// textures.c: t is being freed - an alias source hands its contents over first
void vk_alias_texture_destroyed(NvnTexture *t) {
    if (naliases) alias_before_write(t, 0, "freed");
}
void vk_alias_reset(void) { naliases = 0; }  // (reboot: every texture goes)

// (renderer_stats, once per stats interval) copies made aliases / renamed / partly or fully copied later
void vk_alias_stats(void) {
    static int n;
    if (!stat_alias_made || (n++ > 5 && n % 30)) return;
    LOG("[vk] copy alias: %llu made, %llu renamed, %llu stencil-only, %llu copied later (live %d)\n",
        (unsigned long long)stat_alias_made, (unsigned long long)stat_alias_renamed, (unsigned long long)stat_alias_partial,
        (unsigned long long)stat_alias_copied, naliases);
    stat_alias_made = stat_alias_renamed = stat_alias_partial = stat_alias_copied = 0;
}

// draws: attachments written (or a destination bound at all) end the aliases involved
static void alias_draw_check(void) {
    if (!naliases) return;
    for (int i = 0; i < st.rt.ncolor; i++)
        if (st.rt.color[i]) alias_before_write(st.rt.color[i], 0, "color target");
    NvnTexture *d = st.rt.depth;
    if (!d) return;
    bool writes = (st.ds[0] && st.ds[1]) || (st.ds[3] && ((st.stencil_mask[0] | st.stencil_mask[1]) & 0xFF));
    bool is_dst = false;
    for (int i = 0; i < naliases; i++) is_dst |= aliases[i].dst == d;
    if (writes || is_dst) alias_before_write(d, 0, writes ? "depth write" : "dst bound");
}

static void resolve_feedback(Prog *pr) {
    nfeedback = 0;
    static int off = -1;
    if (off < 0) off = getenv("ODYSSEY_NO_FEEDBACK") != NULL;
    if (off) return;
    ShaderInfo *infos[2] = {pr->vi, pr->fi};
    for (int sg = 0; sg < 2; sg++)
        for (int i = 0; i < infos[sg]->ntex; i++) {
            NvnTexture *t = shader_texture_object(sg, &infos[sg]->tex[i]);
            if (!is_attachment(t)) continue;
            bool have = false;
            for (int k = 0; k < nfeedback; k++) have |= feedback_tex[k] == t;
            if (have || nfeedback == 4) continue;
            static int nosnap = -1;
            if (nosnap < 0) nosnap = getenv("ODYSSEY_NO_SNAPSHOT_REUSE") != NULL;
            int si = -1;
            for (int k = 0; k < 8; k++) if (snap_tex[k] == t) si = k;
            bool self_write = t == st.rt.depth && st.ds[0] && st.ds[1];
            TexBackend *src = vk_tex(t, true);
            Snap *existing = NULL;
            for (int k = 0; k < nsnaps; k++) if (snaps[k].key == t && snaps[k].copy.image) existing = &snaps[k];
            VkImageAspectFlags sa = src->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : src->aspect;
            static const VkComponentMapping id = {0};
            if (!nosnap && si >= 0 && (snap_ok[si] || self_write) && existing) {
                feedback_tex[nfeedback] = t;
                feedback_view[nfeedback++] = vk_image_view(&existing->copy, VK_IMAGE_VIEW_TYPE_2D, src->format, 0, 1, 0, 1, id, sa);
                continue;
            }
            stat_feedback_copies++;
            if (ct_on()) LOG("[ct] snapshot %p %ux%u (was %s, si %d)\n", (void *)t, src->width, src->height, si >= 0 ? snap_why[si] : "new", si);
            vk_end_pass();
            TexBackend *dst = snapshot_image(t, src);
            VkCommandBuffer cb = vk_cmd();
            vk_full_barrier(cb);
            gp_mark(cb, "feedback snapshot %ux%u fmt %d", src->width, src->height, src->format);
            VkImageCopy rg = {.srcSubresource = {src->aspect, 0, 0, 1}, .dstSubresource = {src->aspect, 0, 0, 1},
                              .extent = {src->width, src->height, 1}};
            vkCmdCopyImage(cb, src->image, VK_IMAGE_LAYOUT_GENERAL, dst->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
            if (g_debug_frame && (src->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) {
                static int nd;
                char path[128];
                snprintf(path, sizeof path, "build/fbdepth_%03d_src.pgm", nd);
                debug_save_depth(src, path);
                snprintf(path, sizeof path, "build/fbdepth_%03d_snap.pgm", nd++);
                debug_save_depth(dst, path);
                cb = vk_cmd();
            }
            feedback_tex[nfeedback] = t;
            feedback_view[nfeedback++] = vk_image_view(dst, VK_IMAGE_VIEW_TYPE_2D, src->format, 0, 1, 0, 1, id, sa);
            if (si < 0) { static int rr; si = rr++ & 7; snap_tex[si] = t; }
            snap_ok[si] = true;
            st.rt_dirty = true;
        }
}

// ================================================================== resource binding
static VkDescriptorBufferInfo ubo_span(VkSpan s, uint64_t size) {
    uint64_t max = g_vk.props.limits.maxUniformBufferRange;
    return (VkDescriptorBufferInfo){s.buf, s.off, size > max ? max : size};
}

// Constant buffer k of a stage (renderer.m bind_cbufs), copied into transient memory.
static VkDescriptorBufferInfo cbuf_info(int stage, ShaderInfo *si, int k, uint32_t gx, uint32_t gy, uint32_t gz) {
    static const uint8_t zeros[0x10000];
    if (k == 0) {
        uint32_t drv[0x800 / 4] = {0};
        if (stage == 5) {
            drv[0x200 / 4] = gx; drv[0x204 / 4] = gy; drv[0x208 / 4] = gz;
            for (int i = 0; i < 16; i++) {
                uint32_t *u = &drv[(0x210 + 16 * i) / 4];
                u[0] = (uint32_t)st.ubuf[5][i].addr; u[1] = (uint32_t)(st.ubuf[5][i].addr >> 32); u[2] = (uint32_t)st.ubuf[5][i].size;
                uint32_t *e = &drv[(0x310 + 16 * i) / 4];
                e[0] = (uint32_t)st.sbuf[5][i].addr; e[1] = (uint32_t)(st.sbuf[5][i].addr >> 32); e[2] = (uint32_t)st.sbuf[5][i].size;
            }
        } else {
            uint32_t base = 0x110 + 0x100 * (stage == 1 ? 4 : 0);
            for (int i = 0; i < 16; i++) {
                uint32_t *e = &drv[(base + 16 * i) / 4];
                e[0] = (uint32_t)st.sbuf[stage][i].addr;
                e[1] = (uint32_t)(st.sbuf[stage][i].addr >> 32);
                e[2] = (uint32_t)st.sbuf[stage][i].size;
            }
            // c[0x0][0x610]: the largest point size - a vertex shader writing gl_PointSize clamps to it
            // (min(size, c[0x0][0x610])). Left 0, every point sprite was size 0 and drew nothing: the fluid surfaces'
            // disturbance points (al::FluidSimulateWave::drawPoint) among them, so Cap Kingdom's fog never parted
            // around Mario or rolled (a flat sheet).
            float maxpt = g_vk.props.limits.pointSizeRange[1] > 0 ? g_vk.props.limits.pointSizeRange[1] : 2048.0f;
            memcpy(&drv[0x610 / 4], &maxpt, 4);
        }
        uint64_t size = si->cbuf_size[0] > sizeof drv ? sizeof drv : (si->cbuf_size[0] ? si->cbuf_size[0] : 256);
        if (size < 0x500 && stage == 5) size = 0x500;
        return ubo_span(copy_dedupe(stage == 5 ? DEDUPE_DRV_C : stage == 1 ? DEDUPE_DRV_F : DEDUPE_DRV_V, drv, size, 256), size);
    }
    if (k == 1) {  // compiler constants stored after the shader code
        NvnShaderStage *ss = &st.program->stages[stage];
        uint64_t size = ss->const_size ? ss->const_size : 16;
        if (size < si->cbuf_size[1]) size = si->cbuf_size[1];
        return ubo_span(guest_copy(ss->code_addr + ss->const_offset, size), size);  // (constant: shared per submission)
    }
    int idx = k - 3;  // cbuf_base
    Range r = (idx >= 0 && idx < 18) ? st.ubuf[stage][idx] : (Range){0, 0};
    uint64_t size = si->cbuf_size[k] ? si->cbuf_size[k] : 256;
    if (!r.addr) return ubo_span(vk_transient_copy(zeros, size, 256), size);
    if ((si->cbuf_dynamic & (1u << k)) && r.size) size = r.size > 0x10000 ? 0x10000 : r.size;
    if (size > r.size && r.size) size = r.size;
    if (size < 16) size = 16;
    // (frame interpolation, runtime/nvn/interp.c: the frame's copy is logged; a replay binds the logged / blended one -
    // compared over the bytes the shader reads)
    uint64_t src = r.addr;
    const void *pose = stage == 0 ? odyssey_pose_render_palette(src, size) : NULL;
    if (pose) return ubo_span(vk_transient_copy(pose, size, 256), size);
    if (g_interp_log && stage < 2) src = (uint64_t)(uintptr_t)interp_cbuf(stage, k, idx, (const void *)(uintptr_t)r.addr, size, si->cbuf_size[k]);
    // snapshot: later CPU writes must not leak in. Shared by the draws of one submission binding the same range:
    // within a command list nothing writes guest memory (no inline UpdateUniformBuffer: that writes through below
    // and drops the shared copies).
    return ubo_span(guest_copy(src, size), size);
}

// Global memory (shader LDG/STG, maxwell_gmem_base = HEAP_BASE): the guest heap imported as a GPU buffer
// (VK_EXT_external_memory_host), zero-copy like metal.m's g_guest_buffer, so compute results land in guest
// memory. The import pins its pages, so it covers only the heap the game set up (SetHeapSize), made on first use.
// ODYSSEY_VK_NO_GMEM=1: no import, reads return zero.
static VkBuffer gmem_buf;
static VkDeviceMemory gmem_mem;
static uint64_t gmem_size;

static bool gmem_import(uint64_t size) {
    PFN_vkGetMemoryHostPointerPropertiesEXT props =
        (PFN_vkGetMemoryHostPointerPropertiesEXT)vkGetDeviceProcAddr(g_vk.dev, "vkGetMemoryHostPointerPropertiesEXT");
    VkMemoryHostPointerPropertiesEXT mp = {.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    if (!props || props(g_vk.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, (void *)(uintptr_t)HEAP_BASE, &mp) != VK_SUCCESS ||
        !mp.memoryTypeBits)
        return false;
    VkImportMemoryHostPointerInfoEXT imp = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
                                            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                            .pHostPointer = (void *)(uintptr_t)HEAP_BASE};
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &imp, .allocationSize = size,
                               .memoryTypeIndex = (uint32_t)__builtin_ctz(mp.memoryTypeBits)};
    VkDeviceMemory mem;
    if (vkAllocateMemory(g_vk.dev, &ai, NULL, &mem) != VK_SUCCESS) return false;
    VkExternalMemoryBufferCreateInfo eb = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                                           .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &eb, .size = size,
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT};
    VkBuffer buf;
    if (vkCreateBuffer(g_vk.dev, &bi, NULL, &buf) != VK_SUCCESS) { vkFreeMemory(g_vk.dev, mem, NULL); return false; }
    vkBindBufferMemory(g_vk.dev, buf, mem, 0);
    if (gmem_buf) {  // the heap grew: the old import may still be in use by submitted work
        vk_wait_seq(submit_seq);
        vkDestroyBuffer(g_vk.dev, gmem_buf, NULL);
        vkFreeMemory(g_vk.dev, gmem_mem, NULL);
    }
    gmem_buf = buf;
    gmem_mem = mem;
    gmem_size = size;
    LOG("[vk] guest heap imported for shader global memory: %llu MB\n", (unsigned long long)(size >> 20));
    return true;
}

static uint64_t gmem_tried;  // heap size the import was last attempted for (gmem_info imports / re-imports)

static VkDescriptorBufferInfo gmem_info(void) {
    static int off = -1;
    if (off < 0) off = getenv("ODYSSEY_VK_NO_GMEM") != NULL || !g_vk.host_import;
    uint64_t want = (kernel_heap_size() + 0xFFFF) & ~0xFFFFull;
    if (want > HEAP_MAX) want = HEAP_MAX;
    if (!off && want > gmem_size && !gmem_import(want)) {
        LOG("[vk] guest heap import failed (%llu MB): shader global memory reads return zero\n", (unsigned long long)(want >> 20));
        off = 1;
    }
    if (gmem_buf) return (VkDescriptorBufferInfo){gmem_buf, 0, gmem_size};
    static uint32_t zero[1024];
    VkSpan s = vk_transient_copy(zero, sizeof zero, 256);
    return (VkDescriptorBufferInfo){s.buf, s.off, sizeof zero};
}

// Linear render targets written back to guest memory: a texture with a pitch (agl's createAglTextureDataLinear) that
// the GPU draws into is one the game reads on the CPU - al::getF32FromLinearTextureF16 / getColorFromLinearTexture:
// the fog's surface height (al::OceanWave::calcPos -> OrthoDepthTexture::tryCalcDepth, the top-down depth under it;
// the fluid surfaces' displacement), the poison's paint map (PaintObj). The GPU's result stayed in its image, so the CPU
// read zeros: Cap Kingdom's fog surface came out 3000 units up (1600 instead of ~-1400), every ripple Mario or Cappy
// made there was rejected as too far below it (OceanWave::tryAddRippleWithRange), and the fog never parted. Now each
// pass's linear colour targets are copied, on the GPU, into their place in the imported guest heap after the pass:
// the CPU sees them a frame later at most, as on the Switch (where the GPU writes that memory itself).
// ODYSSEY_LINEAR_WRITEBACK=1: on. OFF by default: with it the game faults (access to 0xffffffffffffffff) a few
// seconds into Cap Kingdom even with only R16F / RGBA16F targets written back; the fog height (calcPos) changed but
// stayed wrong (~1500-1900 instead of near -1400). Work in progress (docs: fog-reference-review/claude-handoff.md).
static void linear_writeback(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("ODYSSEY_LINEAR_WRITEBACK"); on = e && atoi(e) != 0; }  // (opt-in until the crash below is understood)
    int n = pass_nlin;
    pass_nlin = 0;
    if (!on || !gmem_buf) return;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    for (int i = 0; i < n; i++) {
        NvnTexture *t = pass_lin[i];
        // SetStride alone does not select pitch-linear storage (see builder_storage_size).
        // In particular agl can leave a pitch on a block-linear render target. Copying
        // stride * height there writes beyond the allocation the builder requested.
        if (!t || !(t->flags & 0x10) || !t->pool || t->pool_offset > t->pool->size) continue;
        TexBackend *b = t ? (TexBackend *)t->backend : NULL;
        const FormatInfo *fi = t ? t->layout.fmt : NULL;
        // (only the formats the game's CPU readers take: F16 - R16F, RG16F, RGBA16F - and RGBA8. The R32F linear targets
        // in Cap Kingdom are something else: written back, the game faulted on a -1 pointer)
        if (t->format != 0x05 && t->format != 0x11 && t->format != 0x29 && t->format != 0x25) continue;
        if (!b || !b->image || !fi || !fi->bpb || fi->bw != 1 || fi->bh != 1 || b->guest_format || b->width != t->width ||
            b->height != t->height || (b->aspect & ~VK_IMAGE_ASPECT_COLOR_BIT) || t->stride % fi->bpb)
            continue;
        uint64_t row = (uint64_t)t->width * fi->bpb;
        if (!t->height || t->stride < row || t->stride / fi->bpb > UINT32_MAX || t->addr < HEAP_BASE) continue;
        uint64_t available = t->pool->size - t->pool_offset;
        // The final row has no trailing padding in VkBufferImageCopy's destination range.
        if (row > available || (t->height > 1 && t->stride > (available - row) / (t->height - 1))) continue;
        uint64_t off = t->addr - HEAP_BASE, size = t->stride * (uint64_t)(t->height - 1) + row;
        if (off > gmem_size || size > gmem_size - off || off % fi->bpb) continue;
        if (!cb) { cb = vk_cmd(); vk_full_barrier(cb); }
        VkBufferImageCopy c = {.bufferOffset = off, .bufferRowLength = (uint32_t)(t->stride / fi->bpb),
                               .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {t->width, t->height, 1}};
        vkCmdCopyImageToBuffer(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, gmem_buf, 1, &c);
        static int nlog;
        if (nlog++ < 12)
            LOG("[vk] linear render target %ux%u f%#x at %llx (pitch %llu) written back to guest memory\n", t->width, t->height,
                t->format, (unsigned long long)t->addr, (unsigned long long)t->stride);
    }
    if (cb) vk_full_barrier(cb);
}

// Compute dispatches that use global memory: the imported heap is host memory across PCIe on a discrete GPU, and a
// shader storing to it in a loop crawls (the little chain chomps in Madam MeowMeow's house: ~8 ms a frame on a GTX 1660
// Super and on an RTX 3080 Ti alike, which the game then waits for on its fence - 49-53 fps there on the 1660). Before
// such a dispatch its bound storage buffers (sorted, merged) are copied into a device-local buffer, the address table
// (binding GMAP_BUFFER_INDEX) tells the shader (translate.c, GLSL compute) where each range sits, loads and stores
// inside them use that copy, and a shader that stores gets the ranges copied back to the heap right after it (the
// game alternates its buffer sets per frame: the CPU isn't writing them meanwhile). Accesses outside the table use
// the heap as before. ODYSSEY_VK_GMEM_FAST=0: no copy (empty table: everything goes to the heap, the old behaviour).
#define GFAST_SIZE (32ull << 20)
static VkBuffer gfast_buf;
static VmaAllocation gfast_mem;
static uint32_t gmap_tab[(GMAP_MAX + 1) * 4];  // this dispatch's table (stage_set copies it into binding 20)
static VkBufferCopy gfast_cp[GMAP_MAX];       // its copies heap -> gfast_buf (reversed after a storing dispatch)
static int gfast_n;

static VkDescriptorBufferInfo gfast_info(void) {
    if (!gfast_buf) {
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = GFAST_SIZE,
                                 .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VK_CHECK(vmaCreateBuffer(g_vk.vma, &bi, &ai, &gfast_buf, &gfast_mem, NULL));
    }
    return (VkDescriptorBufferInfo){gfast_buf, 0, GFAST_SIZE};
}

static void gfast_prepare(VkCommandBuffer cb) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("ODYSSEY_VK_GMEM_FAST"); on = !e || atoi(e) != 0; }
    memset(gmap_tab, 0, sizeof gmap_tab);
    gfast_n = 0;
    gmem_info();  // (the heap import, made on first use)
    gfast_info();
    if (!on || !gmem_buf) return;
    struct { uint64_t lo, hi; } r[16];
    int n = 0;
    for (int i = 0; i < 16; i++) {  // heap offsets of the bound storage buffers, 256-aligned
        uint64_t a = st.sbuf[5][i].addr, sz = st.sbuf[5][i].size;
        if (!a || !sz || a < HEAP_BASE || a - HEAP_BASE + sz > gmem_size) continue;
        uint64_t lo = (a - HEAP_BASE) & ~255ull, hi = (a - HEAP_BASE + sz + 255) & ~255ull;
        if (hi > gmem_size) hi = gmem_size;
        int k = n++;
        while (k > 0 && r[k - 1].lo > lo) { r[k] = r[k - 1]; k--; }  // (insertion sort by start)
        r[k].lo = lo; r[k].hi = hi;
    }
    int m = 0;
    for (int i = 0; i < n; i++) {  // merge overlapping / nearby ranges (a small gap costs less than another table entry)
        if (m && r[i].lo <= r[m - 1].hi + 4096) { if (r[i].hi > r[m - 1].hi) r[m - 1].hi = r[i].hi; }
        else r[m++] = r[i];
    }
    VkBufferCopy cp[16];
    uint64_t off = 0;
    for (int i = 0; i < m; i++) {
        uint64_t len = r[i].hi - r[i].lo;
        if (m > GMAP_MAX || off + len > GFAST_SIZE) return;  // (too much: the heap as before)
        cp[i] = (VkBufferCopy){r[i].lo, off, len};
        gmap_tab[i * 4 + 0] = (uint32_t)r[i].lo;
        gmap_tab[i * 4 + 1] = (uint32_t)len;
        gmap_tab[i * 4 + 2] = (uint32_t)(off >> 2);
        off += (len + 255) & ~255ull;
    }
    if (!m) return;
    if (getenv("ODYSSEY_LOG_GFAST")) {  // (debug) =<shader code address hex>: that shader's first tables
        static int nlog;
        if (st.program->stages[5].code_addr == strtoull(getenv("ODYSSEY_LOG_GFAST"), NULL, 16) && nlog++ < 40) {
            LOG("[gfast] cs %llx: %d ranges, %llu KB:", (unsigned long long)st.program->stages[5].code_addr, m, (unsigned long long)(off >> 10));
            for (int i = 0; i < m; i++) LOG(" %llx+%llx", (unsigned long long)(r[i].lo + HEAP_BASE), (unsigned long long)(r[i].hi - r[i].lo));
            LOG("\n");
        }
    }
    vkCmdCopyBuffer(cb, gmem_buf, gfast_buf, (uint32_t)m, cp);
    vk_full_barrier(cb);
    gmap_tab[GMAP_MAX * 4] = (uint32_t)m;
    memcpy(gfast_cp, cp, (size_t)m * sizeof cp[0]);
    gfast_n = m;
}

// After a storing dispatch: its ranges back to the heap (before anything else reads them: draws, the CPU after the fence).
static void gfast_finish(VkCommandBuffer cb) {
    if (!gfast_n) return;
    VkBufferCopy back[GMAP_MAX];
    for (int i = 0; i < gfast_n; i++) back[i] = (VkBufferCopy){gfast_cp[i].dstOffset, gfast_cp[i].srcOffset, gfast_cp[i].size};
    vk_full_barrier(cb);
    vkCmdCopyBuffer(cb, gfast_buf, gmem_buf, (uint32_t)gfast_n, back);
    vk_full_barrier(cb);
    gfast_n = 0;
}

// ---- vertex / index cache
// Reading vertex / index data in place from the imported heap crosses PCIe for every vertex fetched on a discrete GPU
// (outdoors at 3x internal resolution on an RTX 3080 Ti: ~0.7-1.9 ms of GPU a frame; more on PCIe 3.0), and copying it
// per draw instead moves about as much onto the CPU. So each range bound for drawing gets a device-local copy, refreshed
// only where the CPU wrote since (the OS's page write tracking, plat_written_pages, asked at submission: vc_flush): one
// sequential GPU copy of the written pages in the submission's upload buffer (which completes before its draws) instead
// of fetches across the bus. Overlapping ranges are separate entries; the per-page generation keeps them all current.
// Pages the GPU writes itself (a compute dispatch's copy-back, gfast_finish) are never cached: draws over them read the
// heap in place as before, and so does everything when the OS gives no write tracking. ODYSSEY_VK_VCACHE=0: off.
#define VC_PAGE 4096ull
#define VC_TAB 8192                   // entries (open addressing)
#define VC_MAX_RANGE (64ull << 20)    // larger bound ranges: read in place
#define VC_BUDGET (768ull << 20)      // device-local bytes kept; the least recently used go first
typedef struct { uint64_t lo, hi, used_seq; uint32_t gen, epoch; VkBuffer buf; VmaAllocation mem; bool gpu; } VcEnt;
static VcEnt vc_tab[VC_TAB];
static uint32_t vc_pend[VC_TAB];  // entries the pending submission draws from (vc_flush brings them up to date)
static int vc_npend;
static uint32_t *vc_page_gen;  // per heap page: the generation of its last CPU write seen (vc_gen when observed)
static uint8_t *vc_page_gpu;   // per heap page: written by the GPU
static uint32_t vc_gen = 1, vc_epoch = 1;  // (epoch: bumped when pages turn GPU-written: entries check again)
static uint64_t vc_bytes;
#define VC_DEAD 1024
static struct { VkBuffer b; VmaAllocation m; uint64_t seq; } vc_dead[VC_DEAD];
static int vc_ndead;
static int vc_on = -1;
// (profiler: draws served / read in place, bytes refreshed, write tracking queries, written pages seen, query time)
uint64_t prof_vc_hits, prof_vc_bypass, prof_vc_copied, prof_vc_queries, prof_vc_scan_pages, prof_vc_scan_ns;

static void vc_free_dead(bool all) {
    uint64_t done = vk_completed_seq();
    int k = 0;
    for (int i = 0; i < vc_ndead; i++) {
        if (!all && vc_dead[i].seq > done) { vc_dead[k++] = vc_dead[i]; continue; }
        vmaDestroyBuffer(g_vk.vma, vc_dead[i].b, vc_dead[i].m);
    }
    vc_ndead = k;
}

static bool vc_drop(VcEnt *e) {  // (freed once the submissions that may read it are done)
    if (vc_ndead == VC_DEAD) { vk_wait_seq(vk_pending_seq() - 1); vc_free_dead(false); }
    if (vc_ndead == VC_DEAD) return false;  // (all read by the pending submission: keep it)
    vc_dead[vc_ndead].b = e->buf; vc_dead[vc_ndead].m = e->mem; vc_dead[vc_ndead++].seq = e->used_seq;
    vc_bytes -= e->hi - e->lo;
    memset(e, 0, sizeof *e);
    return true;
}

// The page tables (vc_page_gen / vc_page_gpu) exist whenever the OS gives write tracking: the vertex cache and the
// texture upload checks (vk_pages_changed) share them. Each write tracking query resets the OS bits, so every query
// stamps the pages it saw written with a new generation, and each user compares against the generation it last saw.
static int pt_state = -1;  // 1: tracking, 0: none
static bool pt_init(void) {
    if (pt_state >= 0) return pt_state;
    uint64_t pg = VC_PAGE, one;
    int n = plat_written_pages(HEAP_BASE, VC_PAGE, &one, 1, &pg);  // (is there write tracking?)
    pt_state = n >= 0 && pg == VC_PAGE;
    if (!pt_state) { LOG("[vk] no page write tracking (%d, page %llu)\n", n, (unsigned long long)pg); return false; }
    vc_page_gen = calloc(HEAP_MAX / VC_PAGE, sizeof *vc_page_gen);
    vc_page_gpu = calloc(HEAP_MAX / VC_PAGE, 1);
    vc_page_gen[0] = ++vc_gen;  // (the probe took page 0's tracking)
    return true;
}

// Texture upload checks (textures.c): may guest memory [addr, addr + size) have changed since generation `since`?
// 0: no CPU write since, 1: written, -1: unknown (no tracking, outside the heap, or pages the GPU writes).
// *now: the generation to pass next time.
int vk_pages_changed(uint64_t addr, uint64_t size, uint32_t since, uint32_t *now) {
    if (!size || addr < HEAP_BASE || addr - HEAP_BASE + size > HEAP_MAX || !pt_init()) return -1;
    uint64_t lo = (addr - HEAP_BASE) & ~(VC_PAGE - 1), hi = (addr - HEAP_BASE + size + VC_PAGE - 1) & ~(VC_PAGE - 1);
    static uint64_t pages[4096];
    uint64_t pg = VC_PAGE;
    int n = plat_written_pages(HEAP_BASE + lo, hi - lo, pages, 4096, &pg);
    if (n < 0) return -1;
    uint32_t g = ++vc_gen;
    if (n >= 4096) for (uint64_t p = lo / VC_PAGE; p < hi / VC_PAGE; p++) vc_page_gen[p] = g;
    else for (int k = 0; k < n; k++) vc_page_gen[(pages[k] - HEAP_BASE) / VC_PAGE] = g;
    *now = g;
    int changed = 0;
    for (uint64_t p = lo / VC_PAGE; p < hi / VC_PAGE; p++) {
        if (vc_page_gpu[p]) return -1;
        if (vc_page_gen[p] > since) changed = 1;
    }
    return changed;
}

// Heap pages [lo, hi) (offsets) written by the GPU: never cached from now on.
static void vc_mark_gpu(uint64_t lo, uint64_t hi) {
    if (!pt_init()) return;
    for (uint64_t p = lo / VC_PAGE; p < (hi + VC_PAGE - 1) / VC_PAGE && p < HEAP_MAX / VC_PAGE; p++) vc_page_gpu[p] = 1;
    vc_epoch++;
}

// A storing shader (stage 0 / 1 / 5) may write any of its bound storage buffers.
static void vc_mark_sbufs(int stage) {
    for (int i = 0; i < 16; i++) {
        uint64_t a = st.sbuf[stage][i].addr, sz = st.sbuf[stage][i].size;
        if (a >= HEAP_BASE && sz && a - HEAP_BASE < HEAP_MAX) vc_mark_gpu(a - HEAP_BASE, a - HEAP_BASE + sz);
    }
}

static bool vcache_lookup(uint64_t addr, uint64_t size, VkBuffer *buf, VkDeviceSize *off) {
    if (vc_on < 0) {
        const char *e = getenv("ODYSSEY_VK_VCACHE");
        vc_on = !e || atoi(e) != 0;
        if (vc_on && !pt_init()) {
            LOG("[vk] vertex cache off: no page write tracking\n");
            vc_on = 0;
        }
    }
    if (!vc_on || !gmem_buf || size > VC_MAX_RANGE) return false;
    uint64_t lo = (addr - HEAP_BASE) & ~(VC_PAGE - 1), hi = (addr - HEAP_BASE + size + VC_PAGE - 1) & ~(VC_PAGE - 1);
    if (hi > gmem_size) return false;
    uint64_t pend = vk_pending_seq();
    uint64_t h = (lo * 0x9E3779B97F4A7C15ull) ^ (hi * 0xC2B2AE3D27D4EB4Full);
    uint32_t slot = (uint32_t)(h ^ (h >> 32)) & (VC_TAB - 1), free_slot = ~0u, old_slot = ~0u;
    VcEnt *e = NULL;
    for (int probe = 0; probe < 16; probe++, slot = (slot + 1) & (VC_TAB - 1)) {
        VcEnt *c = &vc_tab[slot];
        if (c->buf && c->lo == lo && c->hi == hi) { e = c; break; }
        if (!c->buf && free_slot == ~0u) free_slot = slot;
        if (c->buf && c->used_seq < pend && (old_slot == ~0u || c->used_seq < vc_tab[old_slot].used_seq)) old_slot = slot;
    }
    if (!e) {  // a new range: its own device-local buffer (gen 0: copied whole at the submission)
        if (vc_npend == VC_TAB) return false;
        if (vc_ndead) vc_free_dead(false);
        if (free_slot == ~0u) {
            if (old_slot == ~0u || !vc_drop(&vc_tab[old_slot])) return false;
            free_slot = old_slot;
        }
        while (vc_bytes + (hi - lo) > VC_BUDGET) {  // over budget: the least recently used entry goes
            int lru = -1;
            for (int i = 0; i < VC_TAB; i++)
                if (vc_tab[i].buf && vc_tab[i].used_seq < pend && (lru < 0 || vc_tab[i].used_seq < vc_tab[lru].used_seq)) lru = i;
            if (lru < 0 || !vc_drop(&vc_tab[lru])) return false;
        }
        e = &vc_tab[free_slot];
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = hi - lo,
                                 .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (vmaCreateBuffer(g_vk.vma, &bi, &ai, &e->buf, &e->mem, NULL) != VK_SUCCESS) { memset(e, 0, sizeof *e); return false; }
        e->lo = lo; e->hi = hi;
        vc_bytes += hi - lo;
    }
    if (e->epoch != vc_epoch) {  // (pages turned GPU-written since: is this range one of them?)
        e->gpu = false;
        for (uint64_t p = lo / VC_PAGE; p < hi / VC_PAGE && !e->gpu; p++) e->gpu = vc_page_gpu[p];
        e->epoch = vc_epoch;
    }
    if (e->gpu) { prof_vc_bypass++; return false; }
    if (e->used_seq != pend) {  // first use in this submission: brought up to date by vc_flush before it runs
        e->used_seq = pend;
        vc_pend[vc_npend++] = (uint32_t)(e - vc_tab);
    }
    prof_vc_hits++;
    *buf = e->buf;
    *off = addr - HEAP_BASE - lo;
    return true;
}

static int vc_pend_cmp(const void *a, const void *b) {
    uint64_t x = vc_tab[*(const uint32_t *)a].lo, y = vc_tab[*(const uint32_t *)b].lo;
    return x < y ? -1 : x > y;
}

// At submission (vk_submit_extra, before the upload buffer closes): the pages the CPU wrote in the ranges this
// submission draws from - one write tracking query per run of nearby ranges (a query costs about as much as scanning
// ~64 pages: ranges closer than that share one) - and their copies into the cache, ahead of the draws.
void vc_flush(void) {
    if (!vc_npend) return;
    qsort(vc_pend, (size_t)vc_npend, sizeof vc_pend[0], vc_pend_cmp);
    uint32_t g = ++vc_gen;
    static uint64_t pages[4096];
    uint64_t t0 = SDL_GetTicksNS();
    for (int i = 0; i < vc_npend;) {
        uint64_t lo = vc_tab[vc_pend[i]].lo, hi = vc_tab[vc_pend[i]].hi;
        int j = i + 1;
        for (; j < vc_npend && vc_tab[vc_pend[j]].lo <= hi + 64 * VC_PAGE; j++)
            if (vc_tab[vc_pend[j]].hi > hi) hi = vc_tab[vc_pend[j]].hi;
        uint64_t pg = VC_PAGE;
        int n = plat_written_pages(HEAP_BASE + lo, hi - lo, pages, 4096, &pg);
        prof_vc_queries++;
        if (n < 0 || n >= 4096) for (uint64_t p = lo / VC_PAGE; p < hi / VC_PAGE; p++) vc_page_gen[p] = g;  // (all)
        else for (int k = 0; k < n; k++) vc_page_gen[(pages[k] - HEAP_BASE) / VC_PAGE] = g;
        if (n > 0) prof_vc_scan_pages += (uint64_t)n;
        i = j;
    }
    prof_vc_scan_ns += SDL_GetTicksNS() - t0;
    VkCommandBuffer cb = vk_upload_cmd();
    for (int i = 0; i < vc_npend; i++) {
        VcEnt *e = &vc_tab[vc_pend[i]];
        VkBufferCopy cp[64];
        int nc = 0;
        for (uint64_t p = e->lo / VC_PAGE; p < e->hi / VC_PAGE; p++) {
            if (e->gen && vc_page_gen[p] <= e->gen) continue;
            uint64_t o = p * VC_PAGE;
            if (nc && cp[nc - 1].srcOffset + cp[nc - 1].size == o) { cp[nc - 1].size += VC_PAGE; continue; }
            if (nc == 64) { nc = 1; cp[0] = (VkBufferCopy){e->lo, 0, e->hi - e->lo}; break; }  // (scattered: the whole range)
            cp[nc++] = (VkBufferCopy){o, o - e->lo, VC_PAGE};
        }
        if (nc) {
            vkCmdCopyBuffer(cb, gmem_buf, e->buf, (uint32_t)nc, cp);
            for (int k = 0; k < nc; k++) prof_vc_copied += cp[k].size;
        }
        e->gen = g;
    }
    vc_npend = 0;
}

// Vertex / index data from the imported guest heap (renderer.m bind_guest reads g_guest_buffer the same way): the
// device-local cache above, else read in place - no per-draw copy, no index scan for the copy size. The game fences its
// CPU writes as on the Switch. Falls back to copies outside the heap or without the import; ODYSSEY_VK_COPY_VERTICES=1
// forces copies.
static bool guest_direct(uint64_t addr, uint64_t size, VkBuffer *buf, VkDeviceSize *off) {
    static int copy = -1;
    if (copy < 0) copy = getenv("ODYSSEY_VK_COPY_VERTICES") != NULL;
    if (copy || addr < HEAP_BASE) return false;
    if (addr + size > HEAP_BASE + gmem_size && kernel_heap_size() != gmem_tried) { gmem_tried = kernel_heap_size(); gmem_info(); }
    if (!gmem_buf || addr + size > HEAP_BASE + gmem_size) return false;
    if (vcache_lookup(addr, size, buf, off)) return true;
    *buf = gmem_buf;
    *off = addr - HEAP_BASE;
    return true;
}

static int g_cs_n;  // (compute program table below)

// In-place reboot (device.c backend_reset; GPU idle): drop what is keyed by guest addresses - programs by code
// address (the old Prog / Pipe objects are leaked, so pipeline keys holding their pointers can't collide), compute
// programs, shared copies - and the heap import: its pages are replaced (re-imported on first use).
void vk_exec_reset(void) {
    memset(prog_tab, 0, sizeof prog_tab);
    memset(pipe_tab, 0, sizeof pipe_tab);
    g_cs_n = 0;
    gcopy_gen++;
    if (gmem_buf) {
        vkDestroyBuffer(g_vk.dev, gmem_buf, NULL);
        vkFreeMemory(g_vk.dev, gmem_mem, NULL);
        gmem_buf = VK_NULL_HANDLE;
        gmem_mem = VK_NULL_HANDLE;
    }
    gmem_size = gmem_tried = 0;
    for (int i = 0; i < VC_TAB; i++)  // (the vertex cache: copies of the old pages)
        if (vc_tab[i].buf) vmaDestroyBuffer(g_vk.vma, vc_tab[i].buf, vc_tab[i].mem);
    memset(vc_tab, 0, sizeof vc_tab);
    vc_npend = 0;
    vc_free_dead(true);
    vc_bytes = 0;
    if (vc_page_gpu) memset(vc_page_gpu, 0, HEAP_MAX / VC_PAGE);
    vc_epoch++;
    st_defaults();
}

static void texture_info(int stage, ShaderInfo *si, int i, VkDescriptorImageInfo *out, float sc[2], uint32_t wh[2]) {
    ShaderTexture *st_ = &si->tex[i];
    int slot = ((int)st_->handle_offset - 0x20) / 8;  // 64-bit handles from offset 0x20
    uint64_t h = (slot >= 0 && slot < 32) ? st.tex[stage][slot] : 0;
    VkImageView view = VK_NULL_HANDLE;
    VkSampler smp = VK_NULL_HANDLE;
    NvnTexture *sampled_texture = NULL;
    uint32_t tic = h & 0xFFFFF, tsc = (uint32_t)(h >> 20) & 0xFFF;
    sc[0] = sc[1] = 1;
    wh[0] = wh[1] = 0;
    if (h && st.tpool && tic < (uint32_t)st.tpool->count) {
        NvnTexEntry *e = &((NvnTexEntry *)st.tpool->entries)[tic];
        // (frame interpolation: the frame's entry - the game rewrites the pool while replays run)
        if (g_interp_log && stage < 2) e = (NvnTexEntry *)interp_texentry(stage, i, e, sizeof *e);
        if (e->tex) {
            sampled_texture = e->tex;
            // Per-frame memo: ~2400 lookups per overworld frame recomputed the view (format / range math, cache
            // probe). The first lookup of a frame still runs vk_view (upload check); a view that may have been
            // destroyed (eviction, image re-created) bumps vk_view_gen. ODYSSEY_VK_NO_TEX_MEMO=1 disables.
            extern uint32_t vk_view_gen;
            extern uint64_t vk_frame_counter(void);
            static int nomemo = -1;
            if (nomemo < 0) nomemo = getenv("ODYSSEY_VK_NO_TEX_MEMO") != NULL;
            // (a replay drawing into twins, textures.c: its own memo, valid until the next twin starts - reads move to it)
            static struct { NvnTexture *tex; uint64_t vh, frame; uint32_t gen; uint8_t type, has_view; VkImageView view; } memo_g[1024], memo_r[1024];
            extern __thread bool g_twin_redirect;
            extern uint32_t vk_twin_epoch;
            __typeof__(memo_g[0]) *memo = g_twin_redirect ? memo_r : memo_g;
            uint64_t vh = e->has_view ? hash_words(0, &e->view, sizeof e->view) : 0,
                     frame = g_twin_redirect ? vk_twin_epoch : vk_frame_counter();
            uint32_t ms = (uint32_t)((((uintptr_t)e->tex >> 4) ^ vh ^ (uint64_t)st_->type * 0x9E3779B97F4A7C15ull) & 1023);
            if (!nomemo && memo[ms].tex == e->tex && memo[ms].vh == vh && memo[ms].frame == frame && memo[ms].gen == vk_view_gen &&
                memo[ms].type == st_->type && memo[ms].has_view == e->has_view) {
                view = memo[ms].view;
            } else {
                view = vk_view(naliases ? alias_read(e->tex) : e->tex, &e->view, e->has_view, false, st_->type);
                memo[ms].tex = e->tex; memo[ms].vh = vh; memo[ms].frame = frame; memo[ms].gen = vk_view_gen;
                memo[ms].type = (uint8_t)st_->type; memo[ms].has_view = e->has_view; memo[ms].view = view;
            }
            renderer_texture_scale(e->tex, &sc[0], &sc[1]);
            wh[0] = e->tex->width; wh[1] = e->tex->height;
            for (int k = 0; k < nfeedback; k++)
                if (e->tex == feedback_tex[k] && (st_->type == TEXTYPE_2D || st_->type == TEXTYPE_2D_ARRAY)) {
                    view = feedback_view[k];
                    if (st_->type == TEXTYPE_2D_ARRAY) view = vk_dummy_view(TEXTYPE_2D_ARRAY);  // (2D snapshot as array: rare)
                }
        }
    }
    if (h && st.spool && tsc < (uint32_t)st.spool->count) {
        NvnSamplerDesc *desc = &((NvnSamplerDesc *)st.spool->entries)[tsc];
#ifdef _WIN32
        const TexBackend *sampled = sampled_texture ? sampled_texture->backend : NULL;
        float bias = stage == 1 && st.ds[0] && st.rt.depth && sampled && sampled->levels > 1 &&
                     !(sampled->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) && desc->min_filter >= 2 && !desc->compare_mode
                         ? vk_dlss_surface_lod_bias(st.rt.depth->backend) : 0;
        if (bias < 0) {
            NvnSamplerDesc adjusted = *desc;
            adjusted.backend = NULL;
            float limit = g_vk.props.limits.maxSamplerLodBias;
            adjusted.lod_bias = fmaxf(-limit, fminf(limit, adjusted.lod_bias + bias));
            smp = vk_sampler(&adjusted); // cached by sampler state; the guest descriptor stays untouched
        } else
#endif
            smp = vk_sampler(desc);
    }
    if (!view) view = vk_dummy_view(st_->type);
    if (!smp) smp = vk_default_sampler(true);
    *out = (VkDescriptorImageInfo){smp, view, VK_IMAGE_LAYOUT_GENERAL};
}

// Writes the descriptor set of one stage. gx..gz: compute group counts (driver constants).
// pre_ii: texture descriptors already resolved by the caller (draw: fragment textures, needed earlier for the
// viewport decision), or NULL.
static float g_mirror_w;  // mirror mode's composite: its render target width (rscale[65].x; 0 otherwise)

static VkDescriptorSet stage_set(Prog *p, VkDescriptorSetLayout layout, uint32_t set, int stage, ShaderInfo *si, const float *rscale_row0,
                                 const float tint[8], uint32_t gx, uint32_t gy, uint32_t gz, const VkDescriptorImageInfo *pre_ii,
                                 const VkImageView *storage_views) {
    VkDescriptorSet ds = vk_alloc_set(layout);
    VkWriteDescriptorSet w[MAX_BIND];
    VkDescriptorBufferInfo bi[MAX_BIND];
    VkDescriptorImageInfo ii[MAX_BIND];
    int n = 0;
    int sg = stage == 1 ? 1 : 0;
    float rs[66][4] = {{1, 1, 0, 0}};  // ([65]: mirror mode, renderer.m)
    if (rscale_row0) memcpy(rs[0], rscale_row0, 16);
    rs[65][0] = g_mirror_w;
    for (int i = 0; i < p->nbinds; i++) {
        VkBindingInfo *b = &p->binds[i];
        if (b->set != set) continue;
        w[n] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = b->binding,
                                      .descriptorCount = 1, .descriptorType = b->type};
        if (b->type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER && b->binding >= 32) {
            int ti = (int)b->binding - 32;
            float sc[2]; uint32_t wh[2];
            if (ti < si->ntex && pre_ii && ti < 64) { ii[n] = pre_ii[ti]; sc[0] = tex_sc[sg][ti][0]; sc[1] = tex_sc[sg][ti][1]; wh[0] = tex_wh[sg][ti][0]; wh[1] = tex_wh[sg][ti][1]; }
            else if (ti < si->ntex) texture_info(stage, si, ti, &ii[n], sc, wh);
            else { ii[n] = (VkDescriptorImageInfo){vk_default_sampler(true), vk_dummy_view(TEXTYPE_2D), VK_IMAGE_LAYOUT_GENERAL}; sc[0] = sc[1] = 1; wh[0] = wh[1] = 0; }
            if (ti < 64) {
                tex_sc[sg][ti][0] = sc[0]; tex_sc[sg][ti][1] = sc[1];
                tex_wh[sg][ti][0] = wh[0]; tex_wh[sg][ti][1] = wh[1];
            }
            w[n].pImageInfo = &ii[n];
        } else if (b->type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE && storage_views &&
                   b->binding >= SHADER_IMAGE_BINDING && b->binding - SHADER_IMAGE_BINDING < (uint32_t)si->nimages) {
            ii[n] = (VkDescriptorImageInfo){.imageView = storage_views[b->binding - SHADER_IMAGE_BINDING],
                                            .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
            w[n].pImageInfo = &ii[n];
        } else if (b->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && b->binding < 17) {
            bi[n] = cbuf_info(stage, si, (int)b->binding, gx, gy, gz);
            w[n].pBufferInfo = &bi[n];
        } else if (b->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
            bi[n] = b->binding == GFAST_BUFFER_INDEX ? gfast_info() : gmem_info();
            w[n].pBufferInfo = &bi[n];
        } else {
            continue;  // rscale / tint: written below, once the textures (and their scales) are known
        }
        n++;
    }
    for (int i = 0; i < p->nbinds; i++) {
        VkBindingInfo *b = &p->binds[i];
        if (b->set != set || b->type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
            (b->binding != 18 && b->binding != 19 && b->binding != GMAP_BUFFER_INDEX)) continue;
        w[n] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = b->binding,
                                      .descriptorCount = 1, .descriptorType = b->type};
        if (b->binding == GMAP_BUFFER_INDEX) {  // (compute) this dispatch's fast-copy table: gfast_prepare
            bi[n] = ubo_span(vk_transient_copy(gmap_tab, sizeof gmap_tab, 256), sizeof gmap_tab);
        } else if (b->binding == 18) {
            for (int t = 0; t < si->ntex && t < 64; t++) { rs[1 + t][0] = tex_sc[sg][t][0]; rs[1 + t][1] = tex_sc[sg][t][1]; }
            bi[n] = ubo_span(copy_dedupe(sg ? DEDUPE_RSCALE_F : DEDUPE_RSCALE_V, rs, sizeof rs, 256), sizeof rs);
        } else {
            float zero[8] = {0};
            bi[n] = ubo_span(copy_dedupe(DEDUPE_TINT, tint ? tint : zero, 32, 256), 32);
        }
        w[n].pBufferInfo = &bi[n];
        n++;
    }
    if (n) vkUpdateDescriptorSets(g_vk.dev, (uint32_t)n, w, 0, NULL);
    return ds;
}

// ================================================================== compute
typedef struct { Prog p; ShaderInfo *si; VkPipeline pipe; uint32_t local[3]; bool split_x; } CsProg;

static void build_compute(void *arg) {
    CsProg *cp = arg;
    size_t n = 0;
    uint32_t *spv = vk_glsl_to_spirv(cp->si->msl, STAGE_COMPUTE, "cs", &n);
    if (!spv) { atomic_store(&cp->p.state, 2); return; }
    PipeRecipe recipe = compute_recipe(vk_spirv_last_key, cp->local);
    cp->p.nbinds = vk_reflect(spv, n, cp->p.binds, MAX_BIND);
    if (!compute_bindings_supported(&cp->p)) { free(spv); atomic_store(&cp->p.state, 2); return; }
    cp->p.layout = make_layout(&cp->p, 1, VK_SHADER_STAGE_COMPUTE_BIT, 0);
    bool recordable = pipe_prewarm_enabled && recipe.spv[0];
    VkShaderModule module = vk_module(spv, n);
    if (module) {
        cp->pipe = create_compute_pipeline(module, cp->p.layout, cp->local);
        vkDestroyShaderModule(g_vk.dev, module, NULL);
        atomic_fetch_add(&stat_compiles, 1);
    }
    free(spv);
    if (recordable && cp->pipe) pipe_recipe_remember(&recipe);
    atomic_store(&cp->p.state, cp->pipe ? 1 : 2);
}

#define CS_TAB 2048
static struct CsEnt { uint64_t addr; CsProg *cp; } g_cs_tab[CS_TAB];  // compute programs by code address

static CsProg *compute_program(NvnProgram *np) {
    NvnShaderStage *ss = &np->stages[5];
    if (!ss->code_addr) return NULL;
    struct CsEnt *tab = g_cs_tab;
    int n = g_cs_n;
    for (int i = 0; i < n; i++) if (tab[i].addr == ss->code_addr) return tab[i].cp;
    CsProg *cp = calloc(1, sizeof *cp);
    if (n < CS_TAB) { tab[n].addr = ss->code_addr; tab[n].cp = cp; g_cs_n = n + 1; }
    maxwell_gmem_base = HEAP_BASE;
    maxwell_gmem_size = HEAP_MAX;
    for (int i = 0; i < 3; i++) {
        cp->local[i] = ss->control ? *(const uint32_t *)(ss->control + 0x748 + 4 * i) : 1;
        if (!cp->local[i]) cp->local[i] = 1;
    }
    cp->split_x = compute_split_x(cp->local);
    if (cp->split_x) cp->local[0] /= 3;
    ShaderKey k = {.glsl = 1, .split_compute_x = cp->split_x};
    pthread_mutex_lock(&translate_lock);
    cp->si = maxwell_translate((const uint8_t *)(uintptr_t)ss->code_addr, ss->const_offset ? ss->const_offset : 0x10000, STAGE_COMPUTE, &k);
    pthread_mutex_unlock(&translate_lock);
    cp->p.vi = cp->si;
    if (getenv("ODYSSEY_LOG_COMPUTE") && ss->control) {  // (control words around the work group size @0x748)
        const uint32_t *cw = (const uint32_t *)(ss->control + 0x730);
        LOG("[compute] cs %llx control @0x730:", (unsigned long long)ss->code_addr);
        for (int i = 0; i < 16; i++) LOG(" %x", cw[i]);
        LOG("\n");
    }
    if (!cp->si->msl) {
        LOG("[compute] translate failed: %s (code %llx)\n", cp->si->error, (unsigned long long)ss->code_addr);
        atomic_store(&cp->p.state, 2);
        return cp;
    }
    if (!compute_local_supported(cp->local)) {
        LOG("[compute] unsupported workgroup %ux%ux%u for code %llx\n", cp->local[0], cp->local[1], cp->local[2],
            (unsigned long long)ss->code_addr);
        atomic_store(&cp->p.state, 2);
        return cp;
    }
    save_glsl(cp->si->msl, ss->code_addr, "cs");
    if (pipe_prewarm_enabled) {
        PipeRecipe recipe = compute_recipe(vk_spirv_source_key(cp->si->msl, STAGE_COMPUTE), cp->local);
        uint32_t slot = compute_warm_slot(pipe_recipe_hash(&recipe));
        if (slot != COMPUTE_WARM_SLOTS && compute_warm[slot].pipe) {
            const Prog *warm = compute_warm[slot].prog;
            cp->p.nbinds = warm->nbinds;
            memcpy(cp->p.binds, warm->binds, (size_t)warm->nbinds * sizeof warm->binds[0]);
            cp->p.layout = warm->layout;
            cp->p.set_layout[0] = warm->set_layout[0];
            cp->pipe = compute_warm[slot].pipe;
            pipe_recipe_remember(&recipe);
            atomic_store(&cp->p.state, 1);
            return cp;
        }
    }
    run_job(build_compute, cp);
    return cp;
}

static void dispatch_compute(uint32_t gx, uint32_t gy, uint32_t gz) {
    snap_reason = "compute"; snapshots_invalidate(NULL);
    g_draw_relaxed = false;
    if (!st.program || !gx || !gy || !gz) return;
    CsProg *cp = compute_program(st.program);
    if (!cp || !built(&cp->p.state, NULL)) { stat_skipped++; return; }
    if (cp->split_x && gx > g_vk.props.limits.maxComputeWorkGroupCount[0] / 3) { stat_skipped++; return; }
    {   // ODYSSEY_SKIP_CS=<code address hex> | all (debug): skip that compute shader's dispatches, to see what it does
        static uint64_t skip = ~0ull;
        if (skip == ~0ull) {
            const char *e = getenv("ODYSSEY_SKIP_CS");
            skip = !e ? 0 : !strcmp(e, "all") ? 1 : strtoull(e, NULL, 16);
        }
        if (skip && (skip == 1 || st.program->stages[5].code_addr == skip)) { stat_skipped++; return; }
    }
    if (cp->si->uses_gmem && getenv("ODYSSEY_LOG_CS_SBUF")) {  // (debug) the storage buffers bound to gmem dispatches
        static uint64_t seen_cs[16];
        static int seen_n[16];
        int k = 0;
        while (k < 15 && seen_cs[k] && seen_cs[k] != st.program->stages[5].code_addr) k++;
        seen_cs[k] = st.program->stages[5].code_addr;
        if (seen_n[k]++ < 24) {
            LOG("[cs] %llx %ux%ux%u:", (unsigned long long)st.program->stages[5].code_addr, gx, gy, gz);
            for (int i = 0; i < 16; i++)
                if (st.sbuf[5][i].addr) LOG(" sb%d %llx+%llx", i, (unsigned long long)st.sbuf[5][i].addr, (unsigned long long)st.sbuf[5][i].size);
            LOG("\n");
        }
    }
    vk_end_pass();
    VkImageView storage_views[SHADER_IMAGE_LIMIT];
    NvnTexture *written[SHADER_IMAGE_LIMIT] = {0};
    for (int i = 0; i < cp->si->nimages; i++) {
        const ShaderImage *img = &cp->si->images[i];
        uint64_t handle = st.img[5][img->slot];
        uint32_t index = handle & 0xFFFFF;
        NvnTexEntry *entry = handle && st.tpool && index < (uint32_t)st.tpool->count ?
                             &((NvnTexEntry *)st.tpool->entries)[index] : NULL;
        const NvnTextureView *view = entry && entry->has_view ? &entry->view : NULL;
        uint32_t format = !entry || !entry->tex ? 0 : view && (view->setmask & 4) ? view->format : entry->tex->format;
        storage_views[i] = entry && entry->tex && (img->access != SHADER_IMAGE_WRITE || !rt_kind(format)) ?
                           vk_storage_view(entry->tex, view, img) : VK_NULL_HANDLE;
        if (!storage_views[i]) {
            static int reports;
            if (reports++ < 16) LOG("[compute] unsupported storage image slot %u, format %#x\n", img->slot, format);
            stat_skipped++;
            return;
        }
        if (img->access != SHADER_IMAGE_READ) written[i] = entry->tex;
    }
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    gp_mark(cb, "dispatch %ux%ux%u (local %ux%ux%u) cs %llx gmem %d", gx, gy, gz, cp->local[0], cp->local[1], cp->local[2],
            (unsigned long long)st.program->stages[5].code_addr, cp->si->uses_gmem);
    dl_note("dispatch %ux%ux%u cs %llx gmem %d", gx, gy, gz, (unsigned long long)st.program->stages[5].code_addr, cp->si->uses_gmem);
    nfeedback = 0;
    if (ct_on() || g_debug_frame) {
        LOG("[ct] dispatch cs %llx %ux%ux%u | tex", (unsigned long long)st.program->stages[5].code_addr, gx, gy, gz);
        for (int i = 0; i < cp->si->ntex; i++) {
            NvnTexture *t = shader_texture_object(5, &cp->si->tex[i]);
            if (t) LOG(" %s%p/%#x", cp->si->tex[i].is_image ? "I:" : "", (void *)t, t->format);
        }
        LOG("\n");
    }
    if (cp->si->uses_gmem_map) gfast_prepare(cb);
    VkDescriptorSet ds = stage_set(&cp->p, cp->p.set_layout[0], 0, 5, cp->si, NULL, NULL, gx, gy, gz, NULL, storage_views);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cp->pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cp->p.layout, 0, 1, &ds, 0, NULL);
    vk_checkpoint(cb, "dispatch cs=%llx %ux%ux%u gmem %d", (unsigned long long)st.program->stages[5].code_addr, gx, gy, gz,
                  cp->si->uses_gmem);
    vkCmdDispatch(cb, cp->split_x ? gx * 3 : gx, gy, gz);
    for (int i = 0; i < cp->si->nimages; i++)
        if (written[i]) renderer_mark_gpu_owned(written[i]);
    if (cp->si->uses_gmem_map && cp->si->uses_gmem_store) gfast_finish(cb);
    if (cp->si->uses_gmem_store) vc_mark_sbufs(5);
    stat_dispatches++;
}

// ================================================================== draws
static void debug_draw(Prog *pr, CmdRec *r, bool indexed);
static void debug_raw_before(Prog *pr);

// Quads / fans / polygons as a 32-bit triangle index list.
static uint32_t *expand_indices(uint32_t mode, uint32_t count, const void *indices, int itype, uint32_t first, uint32_t *out_count) {
    uint32_t tris = mode == 7 ? count / 4 * 2 : count >= 3 ? count - 2 : 0;
    uint32_t *out = malloc(tris * 3 * 4 + 4);
    uint32_t n = 0;
#define IDX(i) (indices ? (itype == 0 ? ((const uint8_t *)indices)[i] : itype == 1 ? ((const uint16_t *)indices)[i] : ((const uint32_t *)indices)[i]) : first + (i))
    if (mode == 7) {
        for (uint32_t q = 0; q + 3 < count; q += 4) {
            out[n++] = IDX(q); out[n++] = IDX(q + 1); out[n++] = IDX(q + 2);
            out[n++] = IDX(q); out[n++] = IDX(q + 2); out[n++] = IDX(q + 3);
        }
    } else {
        for (uint32_t i = 1; i + 1 < count; i++) { out[n++] = IDX(0); out[n++] = IDX(i); out[n++] = IDX(i + 1); }
    }
#undef IDX
    *out_count = n;
    return out;
}

static bool topology(uint32_t mode, VkPrimitiveTopology *t, uint8_t *cls) {
    switch (mode) {
    case 0: *t = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; *cls = 0; return true;
    case 1: *t = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; *cls = 1; return true;
    case 3: *t = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; *cls = 1; return true;
    case 4: *t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; *cls = 2; return true;
    case 5: *t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; *cls = 2; return true;
    default: *t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; *cls = 2; return false;
    }
}

static void draw_(CmdRec *r, bool indexed);
static void draw(CmdRec *r, bool indexed) {
    PROF_T0();
    draw_(r, indexed);
    PROF_ADD(prof_draw);
}
static uint64_t prof_tp_;

// (wide screen, draw_) A layout pane that covers the screen: its ui2d vertex uniforms (560 bytes: the projection in
// floats 0-15, the pane's model-view rows 16-27, its rect 132-135: width, height, left, top; y up) put it across all
// of the 16:9 screen's -640..640 (a background band, a dimmer, a fade, a captured image), or magnify it 2.5 times or
// more: the scene transitions' masks (a hole and the black around it, 5 panes scaled about 3 times: stretched
// together, each alone wouldn't cover the sides; the HUD itself is never above 1).
static bool ui2d_covers_screen(const float *u, uint64_t size) {
    if (!u || size != 560) return false;
    if (!(u[0] > 0) || u[1] != 0 || u[2] != 0 || u[14] != -1 || u[12] != 0 || u[13] != 0) return false;  // (layouts' perspective)
    float w = u[132], h = u[133], l = u[134], t = u[135];
    if (!(w > 0) || !(h > 0) || !(w < 1e5f) || !(h < 1e5f)) return false;
    if (hypotf(u[16], u[20]) >= 2.5f || hypotf(u[17], u[21]) >= 2.5f) return true;
    float lo = 1e9f, hi = -1e9f;
    for (int i = 0; i < 4; i++) {
        float x = l + (i & 1 ? w : 0), y = t - (i & 2 ? h : 0), X = u[16] * x + u[17] * y + u[19];
        lo = fminf(lo, X), hi = fmaxf(hi, X);
    }
    return lo <= -639.0f && hi >= 639.0f;
}

static void draw_(CmdRec *r, bool indexed) {
    if (prof) prof_tp_ = SDL_GetTicksNS();
    g_draw_relaxed = false;
    if (!st.program) { stat_skipped++; return; }
    g_draw_fullscreen = r->x[2] <= 6 && st.rt.ncolor && st.rt.color[0] && st.viewport[0][2] >= st.rt.color[0]->width * 0.9f &&
                        st.viewport[0][3] >= st.rt.color[0]->height * 0.9f;
    g_draw_must_wait = g_draw_fullscreen;
    if (g_draw_fullscreen && odyssey_native_120_enabled()) {
        // A viewport can cover the target even when the draw is clipped to a small area.
        bool clipped = st.scissor[0][2] > 0 && st.scissor[0][3] > 0 &&
                       (st.scissor[0][2] < st.rt.color[0]->width * 0.9f || st.scissor[0][3] < st.rt.color[0]->height * 0.9f);
        // An overlay blended over color already drawn in this pass can arrive a frame later.
        bool overlay = in_pass && !st.rt_dirty && pass_color_drawn && st.rt.ncolor == 1 && !st.ds[1] &&
                       (st.color_state[0] & 1) && !rt_kind(st.rt.color[0]->format) && st.blend[0][5] == 1 &&
                       (st.blend[0][2] == 2 || st.blend[0][2] == 6);  // add with One or OneMinusSrcAlpha
        g_draw_must_wait = !clipped && !overlay;
    }
    {   // (sync_shaders) a draw that may be drawn once waits while a scene warms up, the others don't
        static int all = -1;
        if (all < 0) { const char *e = getenv("ODYSSEY_WARMUP_WAIT"); all = e && !strcmp(e, "all"); }
        NvnTexture *t0 = st.rt.ncolor ? st.rt.color[0] : st.rt.depth;
        bool once = g_draw_fullscreen || !t0 || t0->depth > 1 || (uint64_t)t0->width * t0->height <= 256 * 256;
        g_draw_relaxed = !all && !once && !g_settings.shader_wait;
    }
    ShaderKey vkey, fkey;
    base_keys(&vkey, &fkey);
    for (int i = 0; i < st.rt.ncolor; i++) if (st.rt.color[i]) fkey.rt_kind[i] = (uint8_t)rt_kind(st.rt.color[i]->format);
    {   // the game's last copy of its picture into the window (through an 8-bit sRGB view): dithered (translate.c
        // rt_kind 3; the picture before it has half floats: textures.c "Banding")
        bool deband = vk_deband();
        for (int i = 0; deband && i < st.rt.ncolor; i++) {
            NvnTexture *t = st.rt.color[i];
            VkFormat vf = t && st.rt.has_cview[i] && (st.rt.cview[i].setmask & 4) ? vk_format(st.rt.cview[i].format) : VK_FORMAT_UNDEFINED;
            if (t && t->window && fkey.rt_kind[i] == 0 && (vf == VK_FORMAT_R8G8B8A8_SRGB || vf == VK_FORMAT_B8G8R8A8_SRGB))
                fkey.rt_kind[i] = 3;
        }
    }
    Prog *pr = get_program(st.program, &vkey, &fkey);
    int program_step = pr ? atomic_load(&pr->step) : PSTEP_DONE;
    // Pipeline replay only needs the program's published metadata. A ready pipeline can draw while the
    // new program instance is still restoring its own libraries.
    bool lookup_warmed = pipe_prewarm_enabled && (program_step == PSTEP_LIBS || program_step == PSTEP_LIBSING);
    if (!pr || (!lookup_warmed && !(pr->huge ? atomic_load(&pr->state) == 1 : built(&pr->state, pr)))) {
        stat_skipped++;
        if (pr && atomic_load(&pr->state) == 2) prog_tell_failure(pr);
        bool t3d = st.rt.ncolor && st.rt.color[0] && st.rt.color[0]->depth > 1;
        static int n3d;
        if (g_debug_frame || (t3d && n3d++ < 20))
            LOG("[draw] skipped (program %s): vs=%llx fs=%llx rt %d%s\n", !pr ? "missing" : atomic_load(&pr->state) == 2 ? "failed" : "compiling",
                (unsigned long long)st.program->stages[0].code_addr, (unsigned long long)st.program->stages[1].code_addr, st.rt.ncolor,
                st.rt.ncolor && st.rt.color[0] && st.rt.color[0]->depth > 1 ? " (3D target)" : "");
        return;
    }
    g_mirror_w = 0;
    {   // fragment-shader hash: skip lists (debug)
        static uint64_t last_fs, last_hash;
        uint64_t fs = st.program->stages[1].code_addr;
        if (fs != last_fs) {
            last_fs = fs;
            last_hash = fs ? hmix(1469598103934665603ull, (const void *)(uintptr_t)(fs + 0x30), 0x200) : 0;
        }
        static uint64_t skip[16];
        static int nskip = -1;
        if (nskip < 0) {
            nskip = 0;
            for (const char *p = getenv("ODYSSEY_SKIP_FSHASH"); p && *p && nskip < 16;) {
                skip[nskip++] = strtoull(p, (char **)&p, 16);
                while (*p == ',') p++;
            }
        }
        for (int i = 0; i < nskip; i++) if (last_hash == skip[i]) { stat_skipped++; return; }
        static uint64_t skip_vs[16];  // ODYSSEY_SKIP_VS=<vertex program address hex>[,...] (debug): those draws left out
        static int nskip_vs = -1;
        if (nskip_vs < 0) {
            nskip_vs = 0;
            for (const char *p = getenv("ODYSSEY_SKIP_VS"); p && *p && nskip_vs < 16;) {
                skip_vs[nskip_vs++] = strtoull(p, (char **)&p, 16);
                while (*p == ',') p++;
            }
        }
        for (int i = 0; i < nskip_vs; i++) if (st.program->stages[0].code_addr == skip_vs[i]) { stat_skipped++; return; }
        if (g_debug_frame && getenv("ODYSSEY_LOG_FSHASH")) LOG("[fshash] fs=%llx hash %016llx\n", (unsigned long long)fs, (unsigned long long)last_hash);
        g_cur_fs_hash = last_hash;
    }
    if (g_interp_log) {  // (frame interpolation, runtime/nvn/interp.c: the draw's identity across frames)
        // the draw arguments (not the index buffer's address: dynamic geometry's buffers move every frame), the programs
        // and the target's class (format, depth-only, shape - not its size: the game's dynamic resolution changes it)
        uint64_t args[7];
        memcpy(args, r->x, sizeof args);
        if (indexed) args[3] = 0;
        NvnTexture *kt = st.rt.ncolor ? st.rt.color[0] : st.rt.depth;
        uint64_t ids[4] = {st.program->stages[0].code_addr, st.program->stages[1].code_addr,
                           kt ? (uint64_t)kt->format << 32 | (st.rt.ncolor ? 0 : 1u << 31) | (kt->height ? kt->width * 16 / kt->height : 0) : 0, 0};
        // A square depth-only target is a shadow map: which one (and which layer, level) is part of the identity. The
        // game draws its cascades one per frame in turn into the same class of target: paired with the frame before's
        // draws - another cascade - the replay drew this one with a light matrix half-way between the two (Tostarena:
        // a huge dark silhouette on the ground for an in-between, ice with holes - the flicker while the camera moved)
        if (kt && !st.rt.ncolor && kt->width == kt->height)
            ids[3] = (uint64_t)(uintptr_t)kt ^ (uint64_t)(st.rt.has_dview ? st.rt.dview.min_layer | (uint32_t)st.rt.dview.base_level << 16 : 0) << 48;
        // (a shadow pass for interp.c's light rule: a square depth-only target - not the scene's depth pre-pass, whose
        // camera block that rule took for a light's: snapped with a light while the colour pass blended, the pass's
        // depth-equal test failed - a parasol near the camera black in the in-betweens)
        if (!interp_draw(hmix(hmix(1469598103934665603ull, args, sizeof args), ids, sizeof ids), ids[3] != 0)) return;
    }
    debug_raw_before(pr);  // (ODYSSEY_RAW_AT_FS: in any frame, the first draw of that program)
    if (g_draw_fullscreen && st.rt.color[0]->format == 0x3f) dlss_probe_scene(pr->fi);
    if (g_debug_frame) debug_draw(pr, r, indexed);
    else {
        static int log_cubes = -1;  // ODYSSEY_LOG_CUBES=2: every draw into a cube map, as in a debug frame
        if (log_cubes < 0) log_cubes = getenv("ODYSSEY_LOG_CUBES") ? atoi(getenv("ODYSSEY_LOG_CUBES")) : 0;
        static long long log_fs = -1;  // ODYSSEY_LOG_FS=<fragment program address>: its draws, as in a debug frame
        if (log_fs < 0) { const char *e = getenv("ODYSSEY_LOG_FS"); log_fs = e ? (long long)strtoull(e, NULL, 16) : 0; }
        bool cube = st.rt.ncolor && st.rt.color[0] && (st.rt.color[0]->target == 8 || st.rt.color[0]->target == 9);
        if ((cube && log_cubes >= 2) || (log_fs && st.program->stages[1].code_addr == (uint64_t)log_fs)) debug_draw(pr, r, indexed);
    }
    if (ct_on()) ct_draw(pr, "draw");
    alias_draw_check();
    resolve_feedback(pr);
    snap_reason = "draw-depthwrite";
    if (st.rt.depth && st.ds[0] && st.ds[1]) snapshots_invalidate(st.rt.depth);
    snap_reason = "draw-colorwrite";
    for (int i = 0; i < st.rt.ncolor; i++)
        if (st.rt.color[i] && ((st.channel_mask >> (i * 4)) & 0xF)) snapshots_invalidate(st.rt.color[i]);
    begin_pass();
    VkCommandBuffer cb = vk_cmd();

    uint32_t mode = (uint32_t)r->x[0];
    VkPrimitiveTopology topo;
    uint8_t cls;
    bool native = topology(mode, &topo, &cls);
    if (!native && mode != 6 && mode != 7 && mode != 9) { stat_skipped++; return; }

    uint64_t prof_tk_ = prof ? SDL_GetTicksNS() : 0;
    if (prof) prof_pre += prof_tk_ - prof_tp_;
    // ---- pipeline
    PipeKey k;
    memset(&k, 0, sizeof k);
    uint32_t akind[4] = {0};
    k.prog = pr;
    k.ncolor = pass_ncolor;
    memcpy(k.color, pass_color, sizeof k.color);
    k.depth = pass_depth;
    k.stencil = pass_stencil;
    k.samples = pass_samples;
    for (int i = 0; i < 32; i++) {
        if (!(pr->vi->attr_mask & (1u << i))) continue;
        k.attr_mask |= 1u << i;
        uint32_t a = i < st.nattrib ? st.attrib[i] : 0x2E;
        uint32_t fmt = a & 0xFF, stream = (a >> 8) & 0x1F, off = a >> 13;
        if (i >= st.nattrib || fmt == 0 || stream >= 16) {  // unbound attribute: (0, 0, 0, 1) from a constant stream
            k.attr_fmt[i] = VK_FORMAT_R32G32B32A32_SFLOAT;
            k.attr_bind[i] = ZERO_BINDING;
            k.used_streams |= 1u << ZERO_BINDING;
            continue;
        }
        uint32_t kind;
        k.attr_fmt[i] = vertex_format_supported(vertex_input_format(fmt, &kind));
        akind[i >> 3] |= kind << ((i & 7) * 4);
        k.attr_off[i] = off;
        k.attr_bind[i] = stream;
        k.used_streams |= 1u << stream;
    }
    for (int s = 0; s < 16; s++) {
        if (!(k.used_streams & (1u << s))) continue;
        k.stride[s] = s < st.nstream ? st.stream[s][0] : 0;
        k.divisor[s] = s < st.nstream && k.stride[s] ? st.stream[s][1] : 0;
        if (k.divisor[s] > 1 && !g_vk.divisor) k.divisor[s] = 1;
    }
    for (uint32_t i = 0; i < k.ncolor && i < 8; i++) {
        if (!st.rt.color[i] || k.color[i] == VK_FORMAT_UNDEFINED) continue;
        // Targets the fragment shader doesn't output keep their contents on Metal, but are undefined in Vulkan
        // (NVIDIA writes black): mask them. Foliage depth pre-passes left black silhouettes (user 2026-10-08).
        k.write_mask[i] = ((pr->fi->out_mask >> (i * 4)) & 0xF) ? (st.channel_mask >> (i * 4)) & 0xF : 0;
        if (((st.color_state[0] >> i) & 1) && !rt_kind(st.rt.color[i]->format)) {
            k.blend_on |= (uint8_t)(1u << i);
            memcpy(k.blend[i], st.blend[i], 8);
        }
    }
    k.a2c = st.ms[0xC] != 0;
    k.topo_class = cls;
    k.poly_fill = st.poly[2] != 1;
    bool recordable = pipe_prewarm_enabled && pr->spv_key[0] && pr->spv_key[1];
    PipeRecipe recipe;
    if (recordable) recipe = pipe_recipe(&k);
    uint64_t ph = recordable ? pipe_recipe_hash(&recipe) : hash_words(0xcbf29ce484222325ull, &k, sizeof k);
    if (!ph) ph = 1;
    uint32_t pslot = pipe_slot(ph);
    if (pslot == PIPE_TAB) {
        static int full;
        if (!full++) LOG("[vk] pipeline table full (%d)\n", PIPE_TAB);
        stat_skipped++;
        return;
    }
    Pipe *pp = pipe_tab[pslot].pipe;
    if (!pp) {
        if (lookup_warmed && !(pr->huge ? atomic_load(&pr->state) == 1 : built(&pr->state, pr))) {
            stat_skipped++;
            return;
        }
        pp = calloc(1, sizeof *pp);
        pp->k = k;
        pipe_tab[pslot].key = ph;
        pipe_tab[pslot].pipe = pp;
        static int logp = -1;
        if (logp < 0) logp = getenv("ODYSSEY_LOG_PROGS") != NULL;
        if (logp) LOG("[prog] pipe vs=%llx fs=%llx frame %llu ready %d\n", (unsigned long long)pr->code[0],
                      (unsigned long long)pr->code[1], (unsigned long long)renderer_present_count(), atomic_load(&pr->state));
        if (g_vk.gpl && link_pipeline(pp)) {}
        else if (pr->huge || !sync_shaders()) queue_job(build_pipeline, pp, 0);
        else run_job(build_pipeline, pp);
    }
    if (!(pr->huge ? atomic_load(&pp->state) == 1 : built(&pp->state, pr))) { stat_skipped++; return; }
    if (recordable && !pp->recorded) {
        pipe_recipe_remember(&recipe);
        pp->recorded = true;
    }
    if (recordable) record_first_scene_pipeline(pp, ph);
    ss_enabled();
    if (ss_need(SS_PIPE, ss.pipe == pp->pipe)) { ss.pipe = pp->pipe; vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pp->pipe); }
    if (k.attr_mask && ss_need(SS_AK, !memcmp(ss.ak, akind, sizeof akind))) {
        memcpy(ss.ak, akind, sizeof akind);
        vkCmdPushConstants(cb, pr->layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof akind, akind);
    }
    uint64_t prof_td_ = prof ? SDL_GetTicksNS() : 0;
    if (prof) prof_key += prof_td_ - prof_tk_;

    // ---- depth / stencil (dynamic)
    static int nodt = -1;
    if (nodt < 0) nodt = getenv("ODYSSEY_NO_DEPTH_TEST") != NULL;
    bool dt = st.ds[0] && !nodt && pass_depth;
    if (ss_need(SS_DT, ss.dt == (VkBool32)dt)) { ss.dt = dt; vkCmdSetDepthTestEnable(cb, dt); }
    VkBool32 dw = dt && st.ds[1];
    if (ss_need(SS_DW, ss.dw == dw)) { ss.dw = dw; vkCmdSetDepthWriteEnable(cb, dw); }
    uint8_t dfunc = st.ds[2];
    {   // ODYSSEY_DEPTH_FUNC_MAP=from:to (debug): run NVN depth func `from` as `to`
        static int map_from = -1, map_to;
        if (map_from < 0) { const char *e = getenv("ODYSSEY_DEPTH_FUNC_MAP"); map_from = 0; if (e) sscanf(e, "%d:%d", &map_from, &map_to); }
        if (map_from && dfunc == map_from) dfunc = (uint8_t)map_to;
    }
    VkCompareOp dop = dt ? compare_fn(dfunc) : VK_COMPARE_OP_ALWAYS;
    // EQUAL against a depth pre-pass written by a different vertex shader (foliage, trees) needs both shaders to land
    // on identical depths. They do since the translator stopped using unpackHalf2x16 (NVIDIA ran the half math after
    // it in fp16, differently per shader: black slashes in foliage). ODYSSEY_VK_EQUAL_TOL=1 brings back the old
    // workaround: LESS_OR_EQUAL with a few ulps of bias towards the camera, which also let walls paint over the
    // plants stuck flat on them (beach) - so it is off.
    static int tol = -1;
    if (tol < 0) tol = getenv("ODYSSEY_VK_EQUAL_TOL") != NULL;
    bool equal_tol = dop == VK_COMPARE_OP_EQUAL && tol;
    if (equal_tol) dop = VK_COMPARE_OP_LESS_OR_EQUAL;
    if (ss_need(SS_DOP, ss.dop == dop)) { ss.dop = dop; vkCmdSetDepthCompareOp(cb, dop); }
    bool sten = st.ds[3] && pass_stencil;
    if (ss_need(SS_STEN, ss.sten == (VkBool32)sten)) { ss.sten = sten; vkCmdSetStencilTestEnable(cb, sten); }
    if (sten) {
        uint32_t ops = st.ds[5] | st.ds[6] << 8 | st.ds[7] << 16;
        for (int f = 0; f < 2; f++) {
            uint32_t o = f ? ops >> 12 : ops;
            VkStencilFaceFlags face = f ? VK_STENCIL_FACE_BACK_BIT : VK_STENCIL_FACE_FRONT_BIT;
            uint32_t v[7] = {stencil_op(o & 0xF), stencil_op((o >> 8) & 0xF), stencil_op((o >> 4) & 0xF),
                             compare_fn(f ? st.ds[4] >> 4 : st.ds[4] & 0xF), st.stencil_vmask[f] & 0xFF, st.stencil_mask[f] & 0xFF,
                             st.stencil_ref[f]};
            if (!ss_need(f ? SS_SFACE1 : SS_SFACE0, !memcmp(v, ss.sface[f], sizeof v))) continue;
            memcpy(ss.sface[f], v, sizeof v);
            vkCmdSetStencilOp(cb, face, (VkStencilOp)v[0], (VkStencilOp)v[1], (VkStencilOp)v[2], (VkCompareOp)v[3]);
            vkCmdSetStencilCompareMask(cb, face, v[4]);
            vkCmdSetStencilWriteMask(cb, face, v[5]);
            vkCmdSetStencilReference(cb, face, v[6]);
        }
    }
    if (ss_need(SS_BC, !memcmp(ss.bc, st.blend_color, sizeof ss.bc))) { memcpy(ss.bc, st.blend_color, sizeof ss.bc); vkCmdSetBlendConstants(cb, st.blend_color); }

    // ---- rasterizer
    uint8_t cull = st.poly[0];
    if (vkey.flip_x) cull = 0;  // (mirror mode's composite: one full-screen quad, either winding)
    VkCullModeFlags cm = cull == 1 ? VK_CULL_MODE_FRONT_BIT : cull == 2 ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    if (ss_need(SS_CULL, ss.cull == cm)) { ss.cull = cm; vkCmdSetCullMode(cb, cm); }
    bool ccw = st.poly[1] == 1;
    static int flipw = -1;  // ODYSSEY_VK_FLIP_WINDING: invert the front-face mapping (A/B against Metal)
    if (flipw < 0) flipw = getenv("ODYSSEY_VK_FLIP_WINDING") != NULL;
    bool front_ccw = (ccw != vkey.flip_y) != (flipw != 0);
    VkFrontFace ff = front_ccw ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    if (ss_need(SS_FF, ss.ff == ff)) { ss.ff = ff; vkCmdSetFrontFace(cb, ff); }
    VkPrimitiveTopology tp = native ? topo : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    if (ss_need(SS_TOPO, ss.topo == tp)) { ss.topo = tp; vkCmdSetPrimitiveTopology(cb, tp); }
    VkBool32 pre = st.prim_restart && native && (mode == 3 || mode == 5);
    if (ss_need(SS_PRE, ss.pre == pre)) { ss.pre = pre; vkCmdSetPrimitiveRestartEnable(cb, pre); }
    {
        float f[3] = {0, 0, 0};
        static int off = -1;
        if (off < 0) off = getenv("ODYSSEY_NO_DEPTH_BIAS") != NULL;
        bool on = (st.poly[3] & 4) && !off;
        if (on) memcpy(f, st.poly_offset, sizeof f);
        if (equal_tol) { f[1] -= 8; on = true; }  // (see the EQUAL comment above: a few depth ulps nearer)
        if (ss_need(SS_DBE, ss.dbe == (VkBool32)on)) { ss.dbe = on; vkCmdSetDepthBiasEnable(cb, on); }
        if (ss_need(SS_DB, !memcmp(ss.db, f, sizeof f))) { memcpy(ss.db, f, sizeof f); vkCmdSetDepthBias(cb, f[1], f[2], f[0]); }
    }

    // ---- textures first (their scales feed the widescreen decision and the rscale table), then the viewport
    float *vp = st.viewport[0];
    float zn = st.depth_range[0][0], zf = st.depth_range[0][1];
    vp_sx = pass_sx; vp_sy = pass_sy; vp_ox = 0; vp_oy = 0;
    // (the fragment shaders' tint uniform - recolouring a draw - stays off: nothing sets it for Odyssey; kept in the
    // shaders so their translation, and with it the shader caches, stays as it is)
    float tint[8] = {0};
    // fragment textures are bound before deciding the viewport (tex_sc / tex_wh), rscale depends on the decision:
    // build the vertex set now and the fragment set after the decision (stage_set fills tex_sc for stage 1).
    if (prof) prof_dyn += SDL_GetTicksNS() - prof_td_;
    PROF_T0();
    VkDescriptorSet vset = stage_set(pr, pr->set_layout[0], 0, 0, pr->vi, NULL, NULL, 0, 0, 0, NULL, NULL);
    PROF_ADD(prof_sets);
    VkDescriptorImageInfo fii[64];
    uint64_t prof_tx_ = prof ? SDL_GetTicksNS() : 0;
    for (int i = 0; i < pr->fi->ntex && i < 64; i++) {
        float sc[2]; uint32_t wh[2];
        texture_info(1, pr->fi, i, &fii[i], sc, wh);
        tex_sc[1][i][0] = sc[0]; tex_sc[1][i][1] = sc[1]; tex_wh[1][i][0] = wh[0]; tex_wh[1][i][1] = wh[1];
    }
    if (prof) prof_tex += SDL_GetTicksNS() - prof_tx_;
    if (fabsf(pass_sx - pass_sy) > 0.01f * pass_sy && st.rt.ncolor && renderer_texture_presented(st.rt.color[0])) {
        // Draws into the swapchain image (see renderer.m): composites fill the window, UI goes in a centred 16:9 box.
        bool reads_scene = false;
        for (int i = 0; i < pr->fi->ntex && i < 64; i++)
            if ((tex_sc[1][i][0] != 1 || tex_sc[1][i][1] != 1) && tex_wh[1][i][0] == st.rt.color[0]->width &&
                tex_wh[1][i][1] == st.rt.color[0]->height)
                reads_scene = true;
        bool fullscreen_quad = r->x[2] <= 6 && st.viewport[0][0] == 0 && st.viewport[0][1] == 0 &&
                               st.viewport[0][2] >= st.rt.color[0]->width && st.viewport[0][3] >= st.rt.color[0]->height;
        bool fullscreen_fill = pr->fi->ntex == 0 && fullscreen_quad;
        if (fullscreen_quad && !reads_scene && pr->fi->ntex == 1 && (tex_sc[1][0][0] != 1 || tex_sc[1][0][1] != 1)) reads_scene = true;
        static uint64_t wide_frame = ~0ull;
        if (reads_scene) wide_frame = renderer_present_count();
        bool fade_fill = false;
        if (fullscreen_fill && st.ubuf[1][0].addr && st.ubuf[1][0].size >= 12) {
            const float *c = (const float *)(uintptr_t)st.ubuf[1][0].addr;
            bool black = c[0] < 0.01f && c[1] < 0.01f && c[2] < 0.01f, white = c[0] > 0.99f && c[1] > 0.99f && c[2] > 0.99f;
            fade_fill = black || white;
        }
        bool composite = reads_scene || fade_fill || (fullscreen_fill && wide_frame == renderer_present_count());
        if (!composite) wide_frame = ~0ull;
        if (!composite) {
            int pw, ph;
            pass_size(&pw, &ph);
            float u = fminf(pass_sx, pass_sy);
            vp_sx = vp_sy = u;
            vp_ox = (pw - st.rt.color[0]->width * u) * 0.5f;
            vp_oy = (ph - st.rt.color[0]->height * u) * 0.5f;
        }
    }
    {
        // wide screen (runtime/hle/odyssey_wide.c): the layouts' projection shows the 16:9 screen (-640..640) in the
        // middle of a wider target. A pane that covers it (ui2d_covers_screen) gets the viewport stretched by the
        // target's shape about the centre: it covers the whole width again.
        float k = 1.0f;
        if (pass_sx > pass_sy * 1.01f && ui2d_covers_screen((const float *)(uintptr_t)st.ubuf[0][0].addr, st.ubuf[0][0].size))
            k = pass_sx / pass_sy;
        float vx = vp[0] * vp_sx + vp_ox, vw = vp[2] * vp_sx;
        if (k > 1.0f) { vx -= vw * (k - 1) / 2; vw *= k; }
        float jitter[2] = {0};
#ifdef _WIN32
        if (st.rt.depth && st.ds[0])
            vk_dlss_viewport_jitter(st.rt.depth->backend, renderer_present_count(), jitter);
#endif
        set_viewport(vx + jitter[0], vp[1] * vp_sy + vp_oy + jitter[1], vw, vp[3] * vp_sy, zn, zf);
        if (g_debug_frame) {
            VkRect2D sr = scissor_rect();
            LOG("[draw]   vp %.2f %.2f %.2f %.2f (stretch %.3f, fs hash %016llx) scissor %d %d %u %u | guest sc %d %d %d %d | pass %ux%u s %.4f %.4f\n",
                vx, vp[1] * vp_sy + vp_oy, vw, vp[3] * vp_sy, k, (unsigned long long)g_cur_fs_hash, sr.offset.x, sr.offset.y,
                sr.extent.width, sr.extent.height, st.scissor[0][0], st.scissor[0][1], st.scissor[0][2], st.scissor[0][3], pass_w, pass_h,
                vp_sx, vp_sy);
        }
    }
    apply_scissor();
    float rs0[4] = {vp_sx, vp_sy, vp_ox, vp_oy};
    uint64_t prof_t1_ = prof ? SDL_GetTicksNS() : 0;
    VkDescriptorSet fset = stage_set(pr, pr->set_layout[1], 1, 1, pr->fi, rs0, tint, 0, 0, 0, fii, NULL);
    if (prof) prof_sets += SDL_GetTicksNS() - prof_t1_;
    VkDescriptorSet sets[2] = {vset, fset};
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pr->layout, 0, 2, sets, 0, NULL);  // (always new sets)
    if (pr->vi->uses_gmem_store) vc_mark_sbufs(0);
    if (pr->fi->uses_gmem_store) vc_mark_sbufs(1);

    uint64_t prof_t2_ = prof ? SDL_GetTicksNS() : 0;
    // ---- indices (copied; the largest index bounds the vertex data copied below)
    uint32_t first = 0, count = (uint32_t)r->x[2], inst, base_inst, max_index = 0;
    int32_t base_vertex = 0;
    VkSpan ib = {0};
    VkIndexType itype_vk = VK_INDEX_TYPE_UINT32;
    uint32_t icount = 0;
    bool use_index = indexed || !native;
    const void *scan_src = NULL;  // deferred index scan (direct index buffer)
    uint32_t scan_isz = 0, scan_restart = 0;
    // Vertices past the end of a bound vertex buffer read as zeros on the Switch: Odyssey's screen composites (the
    // waterfall mist, the half-resolution particle buffer onto the scene) draw nine vertices from a buffer holding one
    // full-screen triangle, and the two extra triangles collapse to a point. Vulkan fetches past that range into the
    // heap or the upload buffer instead - extra triangles that composite the particles again (a hard-edged dark area
    // behind Cap Kingdom's smoke). A non-indexed list draw stops at the last whole primitive its buffers hold.
    bool bounded = false;
    if (!indexed && (topo == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST || topo == VK_PRIMITIVE_TOPOLOGY_LINE_LIST ||
                     topo == VK_PRIMITIVE_TOPOLOGY_POINT_LIST)) {
        uint64_t cap = UINT64_MAX;
        for (int i = 0; i < 32; i++) {
            if (!((k.attr_mask >> i) & 1)) continue;
            uint32_t s = k.attr_bind[i];
            if (s >= 16 || k.divisor[s] || !k.stride[s] || !st.vbuf[s].addr || !st.vbuf[s].size) continue;
            uint64_t size = st.vbuf[s].size, off = k.attr_off[i];
            uint64_t n = size > off ? (size - off + k.stride[s] - 1) / k.stride[s] : 0;  // (vertices starting inside)
            if (n < cap) cap = n;
        }
        uint64_t first_v = r->x[1];
        if (cap != UINT64_MAX && first_v + count > cap) {
            uint32_t per = topo == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST ? 3 : topo == VK_PRIMITIVE_TOPOLOGY_LINE_LIST ? 2 : 1;
            uint32_t n = cap > first_v ? (uint32_t)(cap - first_v) : 0;
            if (g_debug_frame) LOG("[draw] %u vertices asked, the buffers hold %u from %llu: bounded\n", count, n,
                                   (unsigned long long)first_v);
            count = n / per * per;
            bounded = true;
        }
    }
    static long long snap_max = -1;
    if (snap_max < 0) { const char *e = getenv("ODYSSEY_VK_SNAPSHOT_VERTICES"); snap_max = e ? atoll(e) : 2048; }
    bool snap_draw = bounded && count <= 12 && snap_max >= 36;
    if (!indexed) {
        first = (uint32_t)r->x[1]; base_inst = (uint32_t)r->x[3]; inst = (uint32_t)r->x[4] ? (uint32_t)r->x[4] : 1;
        max_index = first + count;
        if (!native) {
            uint32_t *e = expand_indices(mode, count, NULL, 0, first, &icount);
            ib = vk_transient_copy(e, (uint64_t)icount * 4 + 4, 4);
            free(e);
        }
    } else {
        uint32_t itype = (uint32_t)r->x[1];
        uint64_t iaddr = r->x[3];
        base_vertex = (int32_t)r->x[4];
        base_inst = (uint32_t)r->x[5];
        inst = (uint32_t)r->x[6] ? (uint32_t)r->x[6] : 1;
        const void *src = (const void *)(uintptr_t)iaddr;
        uint32_t restart = st.prim_restart ? st.restart_index : 0xFFFFFFFFu;
        if (native && itype != 0) {
            uint32_t isz = itype == 1 ? 2 : 4;
            itype_vk = itype == 1 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
            icount = count;
            if (iaddr % isz == 0 && guest_direct(iaddr, (uint64_t)count * isz, &ib.buf, &ib.off)) {
                scan_src = src; scan_isz = isz; scan_restart = restart;  // max_index only if a stream gets copied
            } else {
                ib = vk_transient_copy(src, (uint64_t)count * isz + 4, 4);
                if (isz == 2) { const uint16_t *s = src; for (uint32_t i = 0; i < count; i++) if (s[i] > max_index && s[i] != (uint16_t)restart) max_index = s[i]; }
                else { const uint32_t *s = src; for (uint32_t i = 0; i < count; i++) if (s[i] > max_index && s[i] != restart) max_index = s[i]; }
            }
        } else if (native) {  // 8-bit indices: widen to 16
            ib = vk_transient((uint64_t)count * 2 + 4, 4);
            uint16_t *w = ib.ptr;
            const uint8_t *s = src;
            for (uint32_t i = 0; i < count; i++) { w[i] = s[i] == 0xFF && st.prim_restart ? 0xFFFF : s[i]; if (s[i] > max_index && w[i] != 0xFFFF) max_index = s[i]; }
            itype_vk = VK_INDEX_TYPE_UINT16;
            icount = count;
        } else {
            uint32_t *e = expand_indices(mode, count, src, (int)itype, 0, &icount);
            for (uint32_t i = 0; i < icount; i++) if (e[i] > max_index) max_index = e[i];
            ib = vk_transient_copy(e, (uint64_t)icount * 4 + 4, 4);
            free(e);
        }
        if (!scan_src) {
            max_index += 1;
            if (base_vertex > 0) max_index += (uint32_t)base_vertex;
        }
    }
    if (use_index && ss_need(SS_IB, ss.ib == ib.buf && ss.io == ib.off && ss.it == itype_vk)) {
        ss.ib = ib.buf; ss.io = ib.off; ss.it = itype_vk;
        vkCmdBindIndexBuffer(cb, ib.buf, ib.off, itype_vk);
    }

    // ---- vertex streams: the range this draw can read, copied once per submission
    for (int s = 0; s < 17; s++) {
        if (!(k.used_streams & (1u << s))) continue;
        VkSpan sp;
        if (s == ZERO_BINDING) {
            static const float zero[4] = {0, 0, 0, 1};
            sp = vk_transient_copy(zero, 16, 16);
        } else {
            Range v = st.vbuf[s];
            VkBuffer dbuf;
            VkDeviceSize doff;
            if (!snap_draw && v.addr && v.size && guest_direct(v.addr, v.size, &dbuf, &doff)) {
                if (ss_on && (ss.vb_known >> s & 1) && ss.vb[s] == dbuf && ss.vo[s] == doff) continue;
                ss.vb[s] = dbuf; ss.vo[s] = doff; ss.vb_known |= 1u << s;
                vkCmdBindVertexBuffers(cb, (uint32_t)s, 1, &dbuf, &doff);
                continue;
            }
            if (scan_src) {  // copying after all: the index scan bounds the copy
                if (scan_isz == 2) { const uint16_t *q = scan_src; for (uint32_t i = 0; i < icount; i++) if (q[i] > max_index && q[i] != (uint16_t)scan_restart) max_index = q[i]; }
                else { const uint32_t *q = scan_src; for (uint32_t i = 0; i < icount; i++) if (q[i] > max_index && q[i] != scan_restart) max_index = q[i]; }
                max_index += 1;
                if (base_vertex > 0) max_index += (uint32_t)base_vertex;
                scan_src = NULL;
            }
            uint64_t stride = k.stride[s], need;
            if (!stride) need = 256;
            else if (k.divisor[s]) need = ((uint64_t)base_inst + inst + k.divisor[s] - 1) / k.divisor[s] * stride + 256;
            else need = (uint64_t)max_index * stride + 256;
            if (v.size && need > v.size) need = v.size;
            if (!v.addr) { static const uint8_t zero[256]; sp = vk_transient_copy(zero, 256, 16); }
            else if (snap_draw) sp = vk_transient_copy((const void *)(uintptr_t)v.addr, need, 256);
            else sp = guest_copy(v.addr, need ? need : 16);
        }
        VkDeviceSize off = sp.off;
        if (ss_on && (ss.vb_known >> s & 1) && ss.vb[s] == sp.buf && ss.vo[s] == off) continue;
        ss.vb[s] = sp.buf; ss.vo[s] = off; ss.vb_known |= 1u << s;
        vkCmdBindVertexBuffers(cb, (uint32_t)s, 1, &sp.buf, &off);
    }

    if (prof) prof_vtx += SDL_GetTicksNS() - prof_t2_;
    // ---- draw
    if (g_vk.checkpoints)
        vk_checkpoint(cb, "draw%s vs=%llx fs=%llx count %u inst %u rt %d %ux%u f%#x", use_index ? " indexed" : "",
                      (unsigned long long)st.program->stages[0].code_addr, (unsigned long long)st.program->stages[1].code_addr,
                      use_index ? icount : count, inst, st.rt.ncolor, st.rt.ncolor && st.rt.color[0] ? st.rt.color[0]->width : 0,
                      st.rt.ncolor && st.rt.color[0] ? st.rt.color[0]->height : 0, st.rt.ncolor && st.rt.color[0] ? st.rt.color[0]->format : 0);
    if (!use_index) vkCmdDraw(cb, count, inst, first, base_inst);
    else vkCmdDrawIndexed(cb, icount, inst, 0, base_vertex, base_inst);
    stat_draws++;
    if (k.write_mask[0]) pass_color_drawn = true;
    dbg_draws++;
    debug_raw_after();
    debug_tex_trace("draw");
    if (g_debug_frame) { debug_probe(); debug_watch_max(); debug_rg16f_max(); }
    {   // ODYSSEY_MARK_RG16F=<x>,<y>,<w>,<h>,<value> (tests): after the fluid surface's final blend into its persistent
        // height map (fs 72698eae00), that rectangle of it set to value - a marker whose hollow shows where the fog
        // reads that texel
        static int mark = -1;
        static int mx, my, mw, mh;
        static float mv;
        if (mark < 0) {
            const char *e = getenv("ODYSSEY_MARK_RG16F");
            mark = e && sscanf(e, "%d,%d,%d,%d,%f", &mx, &my, &mw, &mh, &mv) == 5 && mw > 0 && mh > 0;
        }
        if (mark && st.program->stages[1].code_addr == 0x72698eae00ull && st.rt.ncolor && st.rt.color[0] &&
            st.rt.color[0]->format == 0x11 && st.rt.color[0]->backend && st.rt.color[0]->width == 256) {
            int x0 = mx < 0 ? 0 : mx, y0 = my < 0 ? 0 : my;
            int x1 = mx + mw > 256 ? 256 : mx + mw, y1 = my + mh > 256 ? 256 : my + mh;
            if (x1 > x0 && y1 > y0) {
                vk_end_pass();
                st.rt_dirty = true;
                uint32_t w = (uint32_t)(x1 - x0), h = (uint32_t)(y1 - y0);
                VkSpan s = vk_transient((uint64_t)w * h * 4, 16);
                uint16_t hv;
                { _Float16 f = (_Float16)mv; memcpy(&hv, &f, 2); }
                uint16_t *px = s.ptr;
                for (uint32_t i = 0; i < w * h * 2; i++) px[i] = hv;
                VkCommandBuffer cb = vk_cmd();
                vk_full_barrier(cb);
                VkBufferImageCopy c = {.bufferOffset = s.off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                       .imageOffset = {x0, y0, 0}, .imageExtent = {w, h, 1}};
                vkCmdCopyBufferToImage(cb, s.buf, ((TexBackend *)st.rt.color[0]->backend)->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
                vk_full_barrier(cb);
            }
        }
    }
    {   // ODYSSEY_FILL_RG16F=<r>,<g> (tests): every 256x256 RG16F target filled with that value after each draw into it -
        // does what samples it (Cap Kingdom's fog) change with it?
        static int fill = -1;
        static float fv[2];
        if (fill < 0) { const char *e = getenv("ODYSSEY_FILL_RG16F"); fill = e && sscanf(e, "%f,%f", &fv[0], &fv[1]) == 2; }
        if (fill && st.rt.ncolor && st.rt.color[0] && st.rt.color[0]->format == 0x11 && st.rt.color[0]->backend &&
            st.rt.color[0]->width == 256 && st.rt.color[0]->height == 256) {
            vk_end_pass();
            st.rt_dirty = true;
            VkCommandBuffer cb = vk_cmd();
            vk_full_barrier(cb);
            VkClearColorValue cv = {.float32 = {fv[0], fv[1], 0, 0}};
            VkImageSubresourceRange rg = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdClearColorImage(cb, ((TexBackend *)st.rt.color[0]->backend)->image, VK_IMAGE_LAYOUT_GENERAL, &cv, 1, &rg);
            vk_full_barrier(cb);
        }
    }
    cum_draws++;
    if (g_gpuprof_frame && gp_n) {
        if (!gp_draws[gp_n - 1]) {  // first draw of the row: its fragment shader hash
            size_t l = strlen(gp_label[gp_n - 1]);
            snprintf(gp_label[gp_n - 1] + l, sizeof gp_label[0] - l, ", first fs %016llx", (unsigned long long)g_cur_fs_hash);
        }
        gp_draws[gp_n - 1]++;
    }
}

static void debug_raw_before(Prog *pr) {  // (ODYSSEY_RAW_AT_FS=<fs>[:<from frame>[,<from>...][:<target 0 texel address>]])
    static long long at = -1;  // (each from frame: one dump, the first as build/raw_*, the next as build/raw1_*, ...)
    static unsigned long long from[8], rt_addr;
    static int nfrom, cur;
    if (at < 0) {
        const char *e = getenv("ODYSSEY_RAW_AT_FS"), *c = e ? strchr(e, ':') : NULL, *c2 = c ? strchr(c + 1, ':') : NULL;
        at = e ? (long long)strtoull(e, NULL, 16) : 0;
        for (const char *f = c ? c + 1 : NULL; f && nfrom < 8; f = strchr(f, ',') && (!c2 || strchr(f, ',') < c2) ? strchr(f, ',') + 1 : NULL)
            from[nfrom++] = strtoull(f, NULL, 10);
        rt_addr = c2 ? strtoull(c2 + 1, NULL, 16) : 0;
    }
    if (raw_state == 2 && cur + 1 < nfrom) {
        raw_state = 0;
        snprintf(raw_prefix, sizeof raw_prefix, "build/raw%d_", ++cur);
    }
    if (raw_state || !at || st.program->stages[1].code_addr != (uint64_t)at || renderer_present_count() < from[cur]) return;
    if (rt_addr && !(st.rt.ncolor && st.rt.color[0] && st.rt.color[0]->addr == rt_addr)) return;
    char path[64];
    for (int i = 0; i < pr->fi->ntex; i++) {
        NvnTexture *t = shader_texture_object(1, &pr->fi->tex[i]);
        if (!t || !t->backend) continue;
        snprintf(path, sizeof path, "%stex%d.bin", raw_prefix, i);
        LOG("[vk] raw tex%d: texture %p addr %llx %ux%u f%#x target %u levels %u (frame %llu)\n", i, (void *)t, (unsigned long long)t->addr,
            t->width, t->height, t->format, t->target, t->levels, (unsigned long long)renderer_present_count());
        debug_raw_dump(t->backend, path);
    }
    snprintf(path, sizeof path, "%starget_before.bin", raw_prefix);
    if (st.rt.ncolor && st.rt.color[0] && st.rt.color[0]->backend) debug_raw_dump(st.rt.color[0]->backend, path);
    raw_state = 1;
}

static void debug_draw(Prog *pr, CmdRec *r, bool indexed) {
    LOG("[draw] %s inst %llu mode %llu count %llu prog vs=%llx fs=%llx | vs cb %#x fs cb %#x | rt %d", indexed ? "e" : "a",
        (unsigned long long)(indexed ? r->x[6] : r->x[4]), (unsigned long long)r->x[0], (unsigned long long)r->x[2],
        (unsigned long long)st.program->stages[0].code_addr, (unsigned long long)st.program->stages[1].code_addr,
        pr->vi->cbuf_mask, pr->fi->cbuf_mask, st.rt.ncolor);
    for (int i = 0; i < st.rt.ncolor; i++) if (st.rt.color[i]) LOG(" %ux%u/f%#x", st.rt.color[i]->width, st.rt.color[i]->height, st.rt.color[i]->format);
    if (st.rt.depth) LOG(" d%#x %p", st.rt.depth->format, (void *)st.rt.depth);
    LOG(" | vp %.0f %.0f %.0f %.0f | attrs %#x blend %#x ds %02x%02x%02x poly %02x%02x chmask %08x\n", st.viewport[0][0], st.viewport[0][1],
        st.viewport[0][2], st.viewport[0][3], pr->vi->attr_mask, st.color_state[0], st.ds[0], st.ds[1], st.ds[2], st.poly[0], st.poly[1],
        st.channel_mask);
    if (st.color_state[0] & 1)  // target 0's blend: src/dst color, src/dst alpha factors, color/alpha equations (NVN enums)
        LOG("[draw]   blend0 %x %x %x %x eq %x %x\n", st.blend[0][1], st.blend[0][2], st.blend[0][3], st.blend[0][4], st.blend[0][5],
            st.blend[0][6]);
    LOG("[draw]   depth range %g %g, poly mode %x offset enables %x (%g %g %g), attr0 %#x stride %u\n", st.depth_range[0][0],
        st.depth_range[0][1], st.poly[2], st.poly[3], st.poly_offset[0], st.poly_offset[1], st.poly_offset[2],
        st.nattrib ? st.attrib[0] : 0, st.nstream ? st.stream[0][0] : 0);
    if (!indexed && r->x[2] <= 12 && st.nattrib && st.nstream && st.vbuf[0].addr && ((st.attrib[0] >> 8) & 0x1F) == 0 &&
        st.stream[0][0] >= 8) {  // a few vertices (screen passes): their first attribute as floats, first..first+count
        uint32_t stride = st.stream[0][0], off = st.attrib[0] >> 13;
        LOG("[draw]   first %llu, a0:", (unsigned long long)r->x[1]);
        for (uint64_t v = r->x[1]; v < r->x[1] + r->x[2]; v++) {
            uint64_t at = (uint64_t)v * stride + off;
            if (at + 8 > st.vbuf[0].size) { LOG(" (past the buffer)"); break; }
            const float *f = (const float *)(uintptr_t)(st.vbuf[0].addr + at);
            if (stride >= 12) LOG(" (%g %g %g)", f[0], f[1], f[2]);
            else LOG(" (%g %g)", f[0], f[1]);
        }
        LOG("\n");
    }
    if (getenv("ODYSSEY_DEBUG_DATA")) {  // every attribute the vertex shader reads: NVN format / stream / offset
        LOG("[draw]   attribs:");
        for (int i = 0; i < 32; i++)
            if ((pr->vi->attr_mask >> i) & 1) {
                uint32_t a = i < st.nattrib ? st.attrib[i] : 0;
                LOG(" a%d=%#x/s%u+%u", i, a & 0xFF, (a >> 8) & 0x1F, a >> 13);
            }
        LOG(" | streams:");
        for (int s = 0; s < st.nstream && s < 16; s++)
            if (st.vbuf[s].addr || st.stream[s][0])
                LOG(" s%d %llx+%llu stride %u div %u", s, (unsigned long long)st.vbuf[s].addr, (unsigned long long)st.vbuf[s].size,
                    st.stream[s][0], st.stream[s][1]);
        LOG("\n");
        if (((pr->vi->attr_mask >> 11) & 1) && st.ubuf[0][3].addr) {  // skinning: every bone matrix's translation (cb6)
            const float *m = (const float *)(uintptr_t)st.ubuf[0][3].addr;
            LOG("[draw]   bones (%llu bytes):", (unsigned long long)st.ubuf[0][3].size);
            for (uint64_t b = 0; b < st.ubuf[0][3].size / 48 && b < 64; b++) LOG(" %llu:(%.0f %.0f %.0f)", (unsigned long long)b, m[b * 12 + 3], m[b * 12 + 7], m[b * 12 + 11]);
            LOG("\n");
        }
        if (st.vbuf[0].addr && st.nstream && st.stream[0][0] >= 12) {  // positions: their bounding box
            const uint8_t *p = (const uint8_t *)(uintptr_t)st.vbuf[0].addr;
            uint32_t stride = st.stream[0][0], nv = (uint32_t)(st.vbuf[0].size / stride);
            float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
            for (uint32_t v = 0; v < nv; v++)
                for (int k = 0; k < 3; k++) {
                    float f = ((const float *)(p + (uint64_t)v * stride))[k];
                    if (f < mn[k]) mn[k] = f;
                    if (f > mx[k]) mx[k] = f;
                }
            LOG("[draw]   positions %u: (%g %g %g) .. (%g %g %g)\n", nv, mn[0], mn[1], mn[2], mx[0], mx[1], mx[2]);
            // 4-bone skinning (RGBA8 weights a10, RGBA8UI indices a11, 3x4 bone rows in cb6) on the CPU: world bounds
            uint32_t a10 = st.nattrib > 10 ? st.attrib[10] : 0, a11 = st.nattrib > 11 ? st.attrib[11] : 0;
            uint32_t sw = (a10 >> 8) & 0x1F, si = (a11 >> 8) & 0x1F;
            if (((pr->vi->attr_mask >> 10) & 3) == 3 && (a10 & 0xFF) == 0x25 && (a11 & 0xFF) == 0x27 && sw < 16 && si < 16 &&
                st.vbuf[sw].addr && st.vbuf[si].addr && st.ubuf[0][3].addr) {
                const float *bm = (const float *)(uintptr_t)st.ubuf[0][3].addr;
                uint32_t nb = (uint32_t)(st.ubuf[0][3].size / 48), sws = st.stream[sw][0], sis = st.stream[si][0];
                float wmn[3] = {1e30f, 1e30f, 1e30f}, wmx[3] = {-1e30f, -1e30f, -1e30f};
                for (uint32_t v = 0; v < nv; v++) {
                    const float *pos = (const float *)(p + (uint64_t)v * stride);
                    const uint8_t *w = (const uint8_t *)(uintptr_t)(st.vbuf[sw].addr + (a10 >> 13) + (uint64_t)v * sws);
                    const uint8_t *ix = (const uint8_t *)(uintptr_t)(st.vbuf[si].addr + (a11 >> 13) + (uint64_t)v * sis);
                    float o[3] = {0, 0, 0};
                    for (int k = 0; k < 4; k++) {
                        if (ix[k] >= nb) continue;
                        const float *m = bm + ix[k] * 12;
                        for (int r = 0; r < 3; r++) o[r] += w[k] / 255.0f * (m[r * 4] * pos[0] + m[r * 4 + 1] * pos[1] + m[r * 4 + 2] * pos[2] + m[r * 4 + 3]);
                    }
                    for (int k = 0; k < 3; k++) { if (o[k] < wmn[k]) wmn[k] = o[k]; if (o[k] > wmx[k]) wmx[k] = o[k]; }
                }
                LOG("[draw]   skinned on the CPU: (%g %g %g) .. (%g %g %g)\n", wmn[0], wmn[1], wmn[2], wmx[0], wmx[1], wmx[2]);
            }
        }
        for (int i = 10; i < 12; i++) {  // (skinning: weights a10, bone indices a11) their first vertices' raw bytes
            if (!((pr->vi->attr_mask >> i) & 1) || i >= st.nattrib) continue;
            uint32_t a = st.attrib[i], s = (a >> 8) & 0x1F;
            if (s >= 16 || !st.vbuf[s].addr) continue;
            const uint8_t *p = (const uint8_t *)(uintptr_t)(st.vbuf[s].addr + (a >> 13));
            uint32_t stride = s < st.nstream ? st.stream[s][0] : 0;
            LOG("[draw]   a%d stream %u stride %u size %llu:", i, s, stride, (unsigned long long)st.vbuf[s].size);
            for (int v = 0; v < 6; v++) LOG(" %02x%02x%02x%02x", p[v * stride], p[v * stride + 1], p[v * stride + 2], p[v * stride + 3]);
            if (stride >= 4 && ((a & 0xFF) == 0x27 || (a & 0xFF) == 0x25)) {  // all vertices: largest byte / byte sums
                uint32_t nv = (uint32_t)(st.vbuf[s].size / stride), mxb = 0, mns = 1024, mxs = 0;
                for (uint32_t v = 0; v < nv; v++) {
                    const uint8_t *q = p + (uint64_t)v * stride;
                    uint32_t sum = q[0] + q[1] + q[2] + q[3];
                    for (int k = 0; k < 4; k++) if (q[k] > mxb) mxb = q[k];
                    if (sum < mns) mns = sum;
                    if (sum > mxs) mxs = sum;
                }
                LOG(" | %u vertices: largest byte %u, byte sums %u..%u", nv, mxb, mns, mxs);
            }
            LOG("\n");
        }
    }
    if (indexed && getenv("ODYSSEY_DEBUG_DATA")) {  // index type, buffer, base vertex; the index range used
        uint32_t itype = (uint32_t)r->x[1], cnt = (uint32_t)r->x[2], mn = UINT32_MAX, mx = 0;
        const void *ib = (const void *)(uintptr_t)r->x[3];
        for (uint32_t i = 0; ib && i < cnt; i++) {
            uint32_t v = itype == 0 ? ((const uint8_t *)ib)[i] : itype == 1 ? ((const uint16_t *)ib)[i] : ((const uint32_t *)ib)[i];
            if (v < mn) mn = v;
            if (v > mx) mx = v;
        }
        LOG("[draw]   indices type %u at %llx, base vertex %d: %u..%u\n", itype, (unsigned long long)r->x[3], (int32_t)r->x[4], mn, mx);
        // ODYSSEY_DUMP_DRAW=<index count>: such a draw's indices and vertex stream 0 to build/drawdump_<count>_<n>.bin
        // (header: index type, index count, base vertex, stream 0 size; then the indices, then stream 0)
        const char *dd = getenv("ODYSSEY_DUMP_DRAW");
        if (dd && (uint32_t)atoi(dd) == cnt && ib && st.vbuf[0].addr) {
            static int ndump;
            char path[96];
            snprintf(path, sizeof path, "build/drawdump_%u_%d.bin", cnt, ndump++);
            FILE *f = fopen(path, "wb");
            if (f) {
                uint32_t hdr[4] = {itype, cnt, (uint32_t)r->x[4], (uint32_t)st.vbuf[0].size};
                fwrite(hdr, 4, 4, f);
                fwrite(ib, itype == 0 ? 1 : itype == 1 ? 2 : 4, cnt, f);
                fwrite((const void *)(uintptr_t)st.vbuf[0].addr, 1, st.vbuf[0].size, f);
                fclose(f);
                LOG("[draw]   dumped to %s\n", path);
            }
        }
    }
    if (st.ds[3])  // stencil: funcs (front | back << 4), ops, then ref / write mask / compare mask per face
        LOG("[draw]   stencil func %02x ops %02x%02x%02x ref %x/%x mask %x/%x vmask %x/%x%s\n", st.ds[4], st.ds[5], st.ds[6], st.ds[7],
            st.stencil_ref[0], st.stencil_ref[1], st.stencil_mask[0], st.stencil_mask[1], st.stencil_vmask[0], st.stencil_vmask[1],
            pass_stencil ? "" : " (no stencil attachment)");
    if (getenv("ODYSSEY_DEBUG_DATA")) {  // vertex streams 0-1 and fragment cbufs: the first floats
        for (int s = 0; s < 2; s++) {
            if (!st.vbuf[s].addr) continue;
            const float *v = (const float *)(uintptr_t)st.vbuf[s].addr;
            LOG("[draw]   vbuf%d %llx size %llu:", s, (unsigned long long)st.vbuf[s].addr, (unsigned long long)st.vbuf[s].size);
            for (int k = 0; k < 16; k++) LOG(" %.4g", v[k]);
            LOG("\n");
        }
        for (int s = 0; s < 2; s++) {  // compiler constants (c1): after the code, offset / size from the control section
            const ShaderInfo *si = s ? pr->fi : pr->vi;
            const NvnShaderStage *ss = &st.program->stages[s];
            if (!((si->cbuf_mask >> 1) & 1)) continue;
            const float *u = (const float *)(uintptr_t)(ss->code_addr + ss->const_offset);
            LOG("[draw]   %ccb1 (compiler constants) offset %#x size %#x, shader reads %#x:", s ? 'f' : 'v', ss->const_offset, ss->const_size,
                si->cbuf_size[1]);
            for (int k = 0; k < 24 && k * 4 < (int)ss->const_size; k++) LOG(" %.6g", u[k]);
            LOG("\n");
        }
        for (int s = 0; s < 2; s++)  // shader cbuf c is NVN uniform binding c - 3 (cbuf_base, as stage_set)
            for (int c = 3; c < 18; c++) {
                const ShaderInfo *si = s ? pr->fi : pr->vi;
                const Range *b = &st.ubuf[s][c - 3];
                if (!((si->cbuf_mask >> c) & 1) || !b->addr) continue;
                const float *u = (const float *)(uintptr_t)b->addr;
                LOG("[draw]   %ccb%d %llx size %u:", s ? 'f' : 'v', c, (unsigned long long)b->addr, (unsigned)b->size);
                static int nfl = -1;  // (ODYSSEY_DEBUG_CB_FLOATS=<n>: more than the first 96)
                if (nfl < 0) nfl = getenv("ODYSSEY_DEBUG_CB_FLOATS") ? atoi(getenv("ODYSSEY_DEBUG_CB_FLOATS")) : 96;
                for (int k = 0; k < nfl && k * 4 < (int)b->size; k++) LOG(" %.6g", u[k]);
                LOG("\n");
            }
        for (int st_ = 0; st_ < 2; st_++) {  // small textures (LUTs, exposure) as the GPU has them
            const ShaderInfo *si = st_ ? pr->fi : pr->vi;
            for (int i = 0; i < si->ntex; i++) {
                NvnTexture *t = shader_texture_object(st_, (ShaderTexture *)&si->tex[i]);
                if (!t || !t->backend || (uint64_t)t->width * t->height * (t->depth ? t->depth : 1) > 4096) continue;
                char what[32];
                snprintf(what, sizeof what, "%s tex%d", st_ ? "fs" : "vs", i);
                debug_tex_readback(t->backend, what);
                extern void nvn_log_overlapping_textures(const NvnTexture *self, uint64_t addr, uint64_t size);
                LOG("[draw]   (%s: %p addr %llx size %#llx target %u)\n", what, (void *)t, (unsigned long long)t->addr,
                    (unsigned long long)tegra_total_size(&t->layout), t->target);
                if (t->addr) nvn_log_overlapping_textures(t, t->addr, tegra_total_size(&t->layout));
            }
        }
    }
    {   // ODYSSEY_LOG_CB=<code address>[,...]: for a draw using that vertex or fragment shader, its constant buffers
        // (each bound one's first 32 vec4, as floats) - a shader's transforms next to the data it reads
        static uint64_t want[8];
        static int nwant = -1;
        if (nwant < 0) {
            nwant = 0;
            const char *e = getenv("ODYSSEY_LOG_CB");
            while (e && *e && nwant < 8) { char *end; want[nwant++] = strtoull(e, &end, 16); e = *end == ',' ? end + 1 : NULL; }
        }
        for (int k = 0; k < nwant; k++) {
            int stg = st.program->stages[0].code_addr == want[k] ? 0 : st.program->stages[1].code_addr == want[k] ? 1 : -1;
            if (stg < 0) continue;
            for (int cb = 0; cb < 18; cb++) {
                uint64_t a = st.ubuf[stg][cb].addr, sz = st.ubuf[stg][cb].size;
                if (!a || a < GUEST_LO || a >= GUEST_HI) continue;
                const float *f = (const float *)(uintptr_t)a;
                uint64_t nv = sz / 16 < 32 ? sz / 16 : 32;
                for (uint64_t v = 0; v < nv; v++)
                    LOG("[cb] %s cb%d[%llu] = %g %g %g %g\n", stg ? "fs" : "vs", cb + 3, (unsigned long long)v, f[v * 4], f[v * 4 + 1],
                        f[v * 4 + 2], f[v * 4 + 3]);
            }
        }
    }
    for (int i = 0; i < pr->vi->ntex && i < 64; i++) {  // vertex textures (displacement maps): which, size, format
        NvnTexture *t = shader_texture_object(0, &pr->vi->tex[i]);
        if (!t) { LOG("[draw]   vtex%d: none\n", i); continue; }
        LOG("[draw]   vtex%d: %p addr %llx %ux%u f%#x lv %u\n", i, (void *)t, (unsigned long long)t->addr, t->width, t->height,
            t->format, t->layout.levels);
    }
    for (int i = 0; i < pr->fi->ntex && i < 64; i++) {  // fragment textures: guest size / format, allocation, scale
        NvnTexture *t = shader_texture_object(1, &pr->fi->tex[i]);
        const TexBackend *b = t ? (const TexBackend *)t->backend : NULL;
        if (!t) { LOG("[draw]   tex%d: none\n", i); continue; }
        LOG("[draw]   tex%d: %p %ux%u f%#x lv %u -> %ux%u scale %.4f %.4f%s", i, (void *)t, t->width, t->height, t->format,
            b ? b->levels : 0, b ? b->width : 0, b ? b->height : 0, b ? b->sx : 0, b ? b->sy : 0, b && b->scalable ? " scalable" : "");
        int slot = ((int)pr->fi->tex[i].handle_offset - 0x20) / 8;  // (its sampler, as texture_info picks it)
        uint64_t h = slot >= 0 && slot < 32 ? st.tex[1][slot] : 0;
        {   // the component swizzle it is sampled with: the texture's own, and its pool entry's view (if it set one)
            uint32_t tic = (uint32_t)h & 0xFFFFF;
            const NvnTexEntry *e = h && st.tpool && tic < (uint32_t)st.tpool->count ? &((const NvnTexEntry *)st.tpool->entries)[tic] : NULL;
            LOG(" | swz tex %u%u%u%u", t->swizzle[0], t->swizzle[1], t->swizzle[2], t->swizzle[3]);
            if (e && e->has_view) LOG(" view(mask %#x) %u%u%u%u", e->view.setmask, e->view.swizzle[0], e->view.swizzle[1],
                                      e->view.swizzle[2], e->view.swizzle[3]);
        }
        uint32_t tsc = (uint32_t)(h >> 20) & 0xFFF;
        if (h && st.spool && tsc < (uint32_t)st.spool->count) {
            const NvnSamplerDesc *sd = &((const NvnSamplerDesc *)st.spool->entries)[tsc];
            LOG(" | sampler %u: min %u mag %u wrap %u %u %u reduction %u lod %.2f..%.2f bias %.2f aniso %.0f compare %u/%u", tsc,
                sd->min_filter, sd->mag_filter, sd->wrap[0], sd->wrap[1], sd->wrap[2], sd->reduction, sd->lod_min, sd->lod_max,
                sd->lod_bias, sd->max_aniso, sd->compare_mode, sd->compare_func);
        }
        LOG("\n");
    }
}

typedef struct { NvnTexture *tex; uint64_t id, addr, size; } PaintShared;
static PaintShared *paint_shared;
static size_t paint_shared_count, paint_shared_capacity;
static uint64_t paint_cpu_epoch;

static bool paint_register_shared(NvnTexture *t, uint64_t span) {
    size_t slot = paint_shared_count;
    for (size_t i = 0; i < paint_shared_count; i++) {
        if (paint_shared[i].tex == t && paint_shared[i].id == t->debug_id) return true;
        if (!paint_shared[i].tex->backend || paint_shared[i].tex->live <= 0) slot = i;
    }
    if (slot == paint_shared_count) {
        if (paint_shared_count == paint_shared_capacity) {
            size_t capacity = paint_shared_capacity ? paint_shared_capacity * 2 : 16;
            void *p = realloc(paint_shared, capacity * sizeof *paint_shared);
            if (!p) return false;
            paint_shared = p; paint_shared_capacity = capacity;
        }
        paint_shared_count++;
    }
    paint_shared[slot] = (PaintShared){t, t->debug_id, t->addr, span};
    return true;
}

static bool paint_write_overlaps(const PaintShared *p, uint64_t addr, uint64_t size) {
    return p->tex->backend && p->tex->live > 0 && p->tex->debug_id == p->id &&
        (addr >= p->addr ? addr - p->addr < p->size : p->addr - addr < size);
}

// Only textures fully synchronized for PaintObj's verified CPU filter qualify.
// An explicit game CPU flush then hands those bytes back to the GPU uploader.
void renderer_paint_cpu_written(uint64_t addr, uint64_t size) {
    if (!size) return;
    bool any = false;
    for (size_t i = 0; i < paint_shared_count; i++) any |= paint_write_overlaps(&paint_shared[i], addr, size);
    if (!any) return;
    vk_end_pass();
    wait_why = "paint CPU edits";
    renderer_flush();
    for (size_t i = 0; i < paint_shared_count; i++) if (paint_write_overlaps(&paint_shared[i], addr, size)) {
        TexBackend *b = paint_shared[i].tex->backend;
        b->gpu_owned = false; b->dynamic = true; b->uploaded = false;
        b->paint_cpu_pending = true;
        b->checked_frame = UINT64_MAX;
    }
    paint_cpu_epoch++;
    vk_view_gen++;
    st.rt_dirty = true;
    static unsigned count;
    if (++count <= 4) LOG("[paint-sync] CPU edits flushed at %llx +%llu\n", (unsigned long long)addr, (unsigned long long)size);
}

// Experimental CPU feedback: read only the resource and texel explicitly requested
// by the game. Stage on the render thread; never DMA into the live guest heap.
void renderer_cpu_texel(RqCpuTexel *q) {
    NvnTexture *t = tex_trace_find(q->addr);
    TexBackend *b = t ? t->backend : NULL;
    const FormatInfo *fi = t ? t->layout.fmt : NULL;
    unsigned bytes = q->component_bytes ? q->component_bytes : 2;
    bool format_ok = t && ((bytes == 2 && (t->format == 0x05 || t->format == 0x11 || t->format == 0x29)) ||
        (bytes == 4 && q->components == 1 && t->format == 0x25 && b && b->gpu_owned));
    if (!b || !b->image || !fi || !format_ok || !q->components || q->components > 4 || fi->bpb != q->components * bytes || q->component >= q->components ||
        b->guest_format ||
        b->width != t->width || b->height != t->height || b->layers != 1 || b->depth != 1 || b->samples != 1 ||
        q->x >= b->width || q->y >= b->height || b->width > 1024 || b->height > 1024) return;
    uint64_t row_bytes = (uint64_t)b->width * fi->bpb;
    if (q->snapshot && (bytes != 4 || !nvn_flags_linear(t->flags) || q->guest_pitch != t->stride ||
        !nvn_linear_rows_fit(q->guest_capacity, q->guest_pitch, row_bytes, b->height) ||
        !nvn_linear_rows_fit(q->snapshot_capacity, row_bytes, row_bytes, b->height))) return;
    typedef struct { uint64_t id, frame, size, epoch; uint8_t *bytes; } Cache;
    static Cache cache[8];
    static unsigned next;
    Cache *e = NULL;
    for (int i = 0; i < 8; i++) if (cache[i].id == t->debug_id) { e = &cache[i]; break; }
    if (!e) { e = &cache[next++ % 8]; e->id = t->debug_id; e->frame = UINT64_MAX; }
    uint64_t frame = renderer_present_count(), size = (uint64_t)b->width * b->height * fi->bpb;
    if (q->refresh_snapshot || e->frame != frame || e->size != size || (bytes == 4 && e->epoch != paint_cpu_epoch)) {
        vk_end_pass();
        st.rt_dirty = true;
        VkCommandBuffer cb = vk_cmd();
        vk_full_barrier(cb);
        VkSpan out = vk_readback(size);
        VkBufferImageCopy cp = {.bufferOffset = out.off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                .imageExtent = {b->width, b->height, 1}};
        vkCmdCopyImageToBuffer(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, out.buf, 1, &cp);
        wait_why = bytes == 4 ? "paint CPU feedback probe" : "fog CPU feedback probe";
        renderer_flush();
        if (e->size != size) { free(e->bytes); e->bytes = malloc(size); e->size = size; }
        if (!e->bytes) { e->size = 0; return; }
        memcpy(e->bytes, out.ptr, size);
        e->frame = frame;
        e->epoch = paint_cpu_epoch;
    }
    if (q->snapshot) {
        uint64_t span = q->guest_pitch * (b->height - 1) + row_bytes;
        if (!paint_register_shared(t, span)) return;
        memcpy(q->snapshot, e->bytes, size);
        q->snapshot_width = b->width; q->snapshot_height = b->height;
    }
    memcpy(&q->value, e->bytes + ((uint64_t)q->y * b->width + q->x) * fi->bpb + q->component * bytes, bytes);
    q->valid = true;
}

// ================================================================== texture copies
typedef struct { uint64_t tex; NvnTextureView view; uint8_t has_view; int32_t region[6]; } TexRefRec;

static void copy_region_dims(NvnTexture *t, const TexRefRec *ref, const FormatInfo **fi, int32_t rg[6], int *level,
                             uint64_t *row, uint64_t *img) {
    *fi = nvn_format_info(t->format);
    memcpy(rg, ref->region, sizeof ref->region);
    if (!rg[3]) rg[3] = (int32_t)t->width;
    if (!rg[4]) rg[4] = (int32_t)t->height;
    if (!rg[5]) rg[5] = 1;
    *level = ref->has_view && (ref->view.setmask & 1) ? ref->view.base_level : 0;
    uint32_t wb = ((uint32_t)rg[3] + (*fi)->bw - 1) / (*fi)->bw, hb = ((uint32_t)rg[4] + (*fi)->bh - 1) / (*fi)->bh;
    *row = st.copy_row_stride ? st.copy_row_stride : (uint64_t)wb * (*fi)->bpb;
    *img = st.copy_image_stride ? st.copy_image_stride : *row * hb;
}

static void copy_buffer_to_texture(uint64_t src, const TexRefRec *dst) {
    snap_reason = "buf2tex"; snapshots_invalidate(NULL);
    NvnTexture *t = (NvnTexture *)(uintptr_t)dst->tex;
    if (!t || !src) return;
    alias_before_write(t, 0, "upload");
    vk_end_pass();
    const FormatInfo *fi;
    int32_t rg[6];
    int level;
    uint64_t row, img;
    copy_region_dims(t, dst, &fi, rg, &level, &row, &img);
    if (!fi || !fi->bpb) return;
    TexBackend *b = vk_tex(t, false);  // prior CPU contents uploaded first
    renderer_mark_gpu_owned(t);
    if (b->aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) return;
    if (b->astc_bw) {  // (blocks would need the CPU decode: not seen in the game yet)
        static int nlog;
        if (nlog++ < 5) LOG("[vk] buffer -> ASTC texture copy skipped (%ux%u)\n", t->width, t->height);
        return;
    }
    bool array = b->layers > 1 && b->depth == 1;
    static int n3d;
    static int ncube;
    if (((b->depth > 1 || g_debug_frame) && n3d++ < 20) || (getenv("ODYSSEY_LOG_CUBES") && (t->target == 8 || t->target == 9) && ncube++ < 200))
        LOG("[copybuf] %llx -> %p %ux%ux%u f%#x lv%d region %d,%d,%d %dx%dx%d row %llu img %llu\n", (unsigned long long)src, (void *)t,
            t->width, t->height, t->depth, t->format, level, rg[0], rg[1], rg[2], rg[3], rg[4], rg[5], (unsigned long long)row,
            (unsigned long long)img);
    uint32_t hb = ((uint32_t)rg[4] + fi->bh - 1) / fi->bh, wb = ((uint32_t)rg[3] + fi->bw - 1) / fi->bw;
    uint32_t out_bpb = fi->bpb == 3 ? 4 : fi->bpb;
    uint32_t nz = (uint32_t)rg[5];
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    uint64_t out_row = (uint64_t)wb * out_bpb;
    VkSpan s = vk_transient(out_row * hb * nz, 16);
    for (uint32_t z = 0; z < nz; z++)
        for (uint32_t y = 0; y < hb; y++) {
            const uint8_t *sr = (const uint8_t *)(uintptr_t)(src + (uint64_t)z * img + (uint64_t)y * row);
            uint8_t *dr = (uint8_t *)s.ptr + ((uint64_t)z * hb + y) * out_row;
            if (out_bpb == fi->bpb) memcpy(dr, sr, out_row);
            else for (uint32_t x = 0; x < wb; x++) { memcpy(dr + x * 4, sr + x * 3, 3); dr[x * 4 + 3] = 255; }
        }
    VkBufferImageCopy c = {.bufferOffset = s.off, .bufferRowLength = wb * fi->bw, .bufferImageHeight = hb * fi->bh,
                           .imageSubresource = {b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : b->aspect,
                                                (uint32_t)level, array ? (uint32_t)rg[2] : 0, array ? nz : 1},
                           .imageOffset = {rg[0], rg[1], array ? 0 : rg[2]},
                           .imageExtent = {(uint32_t)rg[3], (uint32_t)rg[4], array ? 1 : nz}};
    vk_copy_buffer_to_image(cb, b, s.buf, &c);  // (guest texels; a scaled allocation gets them scaled in)
}

// What the GPU writes that the CPU may read - draws into linear textures, copies to / from them, texture -> buffer
// copies (the poison's PaintObj reads its paint map on the CPU: al::getColorFromLinearTexture; issue #3): logged, the
// first 200 of each, so a player's log shows the path. ODYSSEY_LOG_LINEAR=0: off.
static bool log_linear(void) {
    static int on = -1;
    if (on < 0) on = !getenv("ODYSSEY_LOG_LINEAR") || atoi(getenv("ODYSSEY_LOG_LINEAR")) != 0;
    return on;
}

static void copy_texture_to_buffer(const TexRefRec *src, uint64_t dst) {
    NvnTexture *t = (NvnTexture *)(uintptr_t)src->tex;
    if (log_linear()) {
        static int n;
        if (n++ < 200 && t) LOG("[linear] texture %p %ux%u f%#x%s -> buffer %llx (frame %llu)\n", (void *)t, t->width, t->height,
                                t->format, t->flags & 0x10 ? " linear" : "", (unsigned long long)dst,
                                (unsigned long long)renderer_present_count());
    }
    if (!t || !dst || dst < HEAP_BASE) return;
    t = alias_read(t);
    vk_end_pass();
    const FormatInfo *fi;
    int32_t rg[6];
    int level;
    uint64_t row, img;
    copy_region_dims(t, src, &fi, rg, &level, &row, &img);
    if (!fi || !fi->bpb || fi->bpb == 3 || row % fi->bpb) return;
    TexBackend *b = vk_tex(t, false);
    float tsx, tsy;
    renderer_texture_scale(t, &tsx, &tsy);
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    VkImage srcimg = b->image;
    VmaAllocation small_mem = NULL;
    if (((tsx != 1 || tsy != 1) && level == 0 && !renderer_format_is_depth(t->format)) || (b->guest_format && level == 0)) {
        // the CPU expects guest-sized texels in the game's format: filter the scaled allocation down / convert first
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                                .format = b->guest_format ? b->guest_format : b->format,
                                .extent = {t->width, t->height, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &srcimg, &small_mem, NULL));
        vk_image_init_layout(cb, srcimg, VK_IMAGE_ASPECT_COLOR_BIT);
        VkImageBlit bl = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{0, 0, 0}, {(int32_t)b->width, (int32_t)b->height, 1}},
                          .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{0, 0, 0}, {(int32_t)t->width, (int32_t)t->height, 1}}};
        vkCmdBlitImage(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, srcimg, VK_IMAGE_LAYOUT_GENERAL, 1, &bl,
                       tsx != 1 || tsy != 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
        vk_full_barrier(cb);
    }
    bool array = b->layers > 1 && b->depth == 1;
    uint32_t nz = (uint32_t)rg[5];
    uint64_t total = img * nz;
    VkSpan out = vk_readback(total);
    VkBufferImageCopy c = {.bufferOffset = out.off, .bufferRowLength = (uint32_t)(row / fi->bpb) * fi->bw,
                           .bufferImageHeight = (uint32_t)(img / row) * fi->bh,
                           .imageSubresource = {b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : b->aspect,
                                                (uint32_t)level, array ? (uint32_t)rg[2] : 0, array ? nz : 1},
                           .imageOffset = {rg[0], rg[1], array ? 0 : rg[2]},
                           .imageExtent = {(uint32_t)rg[3], (uint32_t)rg[4], array ? 1 : nz}};
    vkCmdCopyImageToBuffer(cb, srcimg, VK_IMAGE_LAYOUT_GENERAL, out.buf, 1, &c);
    wait_why = "texture -> buffer readback";
    renderer_flush();  // the CPU may read the result right after the fence
    memcpy((void *)(uintptr_t)dst, out.ptr, total);
    if (small_mem) vmaDestroyImage(g_vk.vma, srcimg, small_mem);
}

static void copy_texture(const TexRefRec *src, const TexRefRec *dst, uint64_t flags) {
    snap_reason = "tex2tex"; snapshots_invalidate(NULL);
    NvnTexture *st_ = (NvnTexture *)(uintptr_t)src->tex, *dt = (NvnTexture *)(uintptr_t)dst->tex;
    if (!st_ || !dt) return;
    if (((st_->flags | dt->flags) & 0x10) && log_linear()) {
        static int n;
        if (n++ < 200) LOG("[linear] copy %p %ux%u f%#x%s -> %p addr %llx %ux%u f%#x%s (frame %llu)\n", (void *)st_, st_->width,
                           st_->height, st_->format, st_->flags & 0x10 ? " linear" : "", (void *)dt, (unsigned long long)dt->addr,
                           dt->width, dt->height, dt->format, dt->flags & 0x10 ? " linear" : "",
                           (unsigned long long)renderer_present_count());
    }
    vk_end_pass();
    if (dl_on == 2) renderer_flush();  // (ODYSSEY_VK_DEVLOST_TRACE=2: each copy its own submission, waited on)
    gp_mark(vk_cmd(), "copy texture %ux%u -> %ux%u", st_->width, st_->height, dt->width, dt->height);
    dl_note("copy texture %ux%u f%#x -> %ux%u f%#x (%p -> %p)", st_->width, st_->height, st_->format, dt->width, dt->height,
            dt->format, (void *)st_, (void *)dt);
    TexBackend *s = vk_tex(st_, false), *d = vk_tex(dt, true);
    renderer_mark_gpu_owned(dt);
    if (renderer_texture_presented(dt) && st_->width == dt->width && st_->height == dt->height) renderer_texture_mark_presented(st_);
    renderer_texture_first_write(dt);
    renderer_texture_level_fresh(dt);
    const int32_t *sr = src->region, *dr = dst->region;
    float ssx, ssy, dsx, dsy;
    renderer_texture_scale(st_, &ssx, &ssy);
    renderer_texture_scale(dt, &dsx, &dsy);
    int sx0 = (int)lroundf(sr[0] * ssx), sy0 = (int)lroundf(sr[1] * ssy);
    int dx0 = (int)lroundf(dr[0] * dsx), dy0 = (int)lroundf(dr[1] * dsy);
    int sw = sr[3] ? (int)lroundf(sr[3] * ssx) : (int)s->width, sh = sr[4] ? (int)lroundf(sr[4] * ssy) : (int)s->height;
    int dw = dr[3] ? (int)lroundf(dr[3] * dsx) : (int)d->width, dh = dr[4] ? (int)lroundf(dr[4] * dsy) : (int)d->height;
    uint32_t slv = src->has_view && (src->view.setmask & 1) ? src->view.base_level : 0;
    uint32_t dlv = dst->has_view && (dst->view.setmask & 1) ? dst->view.base_level : 0;
    uint32_t sly = src->has_view && (src->view.setmask & 2) ? src->view.min_layer : 0;
    uint32_t dly = dst->has_view && (dst->view.setmask & 2) ? dst->view.min_layer : 0;
    if (slv >= s->levels || dlv >= d->levels || sly >= s->layers || dly >= d->layers) return;
    {   // whole-image regions are the level's extent, and every region stays inside its level: scaled allocations
        // (internal resolution below 1x) round guest regions past the edge, and out-of-bounds copies / blits are
        // undefined in Vulkan (garbage that changed every frame on NVIDIA: flicker in the depth pyramid's users)
        int slw = (int)level_extent(s->width, slv), slh = (int)level_extent(s->height, slv);
        int dlw = (int)level_extent(d->width, dlv), dlh = (int)level_extent(d->height, dlv);
        if (!sr[3]) sw = slw - sx0;
        if (!sr[4]) sh = slh - sy0;
        if (!dr[3]) dw = dlw - dx0;
        if (!dr[4]) dh = dlh - dy0;
        if (sx0 + sw > slw) sw = slw - sx0;
        if (sy0 + sh > slh) sh = slh - sy0;
        if (dx0 + dw > dlw) dw = dlw - dx0;
        if (dy0 + dh > dlh) dh = dlh - dy0;
        if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
    }
    static int n3d;
    static int ncube;  // (cube destinations: ODYSSEY_LOG_CUBES=1)
    if (g_debug_frame || ct_on() || ((s->depth > 1 || d->depth > 1) && n3d++ < 20) ||
        (getenv("ODYSSEY_LOG_CUBES") && (dt->target == 8 || dt->target == 9) && ncube++ < 200))
        LOG("[copytex] %p %ux%u f%#x lv%u (%d,%d %dx%d) [%ux%u] -> %p %ux%u f%#x lv%u (%d,%d %dx%d) [%ux%u] phys (%d,%d %dx%d) -> (%d,%d %dx%d) flags %llx\n",
            (void *)st_, st_->width, st_->height, st_->format, slv, sr[0], sr[1], sr[3], sr[4], s->width, s->height, (void *)dt, dt->width,
            dt->height, dt->format, dlv, dr[0], dr[1], dr[3], dr[4], d->width, d->height, sx0, sy0, sw, sh, dx0, dy0, dw, dh,
            (unsigned long long)flags);
    if (copy_opt() && sx0 == 0 && sy0 == 0 && dx0 == 0 && dy0 == 0 && sw == (int)s->width && sh == (int)s->height && dw == (int)d->width &&
        dh == (int)d->height && slv == 0 && dlv == 0 && sly == 0 && dly == 0 && alias_make(st_, dt, s, d)) {
        if (ct_on()) LOG("[ct] alias made %p -> %p\n", (void *)st_, (void *)dt);
        return;
    }
    alias_before_write(dt, 0, "copy dst");
    if (naliases) s = alias_read(st_)->backend;
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    vk_checkpoint(cb, "copy texture %ux%u f%#x -> %ux%u f%#x (%d,%d %dx%d -> %d,%d %dx%d)", s->width, s->height, st_->format, d->width,
                  d->height, dt->format, sx0, sy0, sw, sh, dx0, dy0, dw, dh);
    VkImageAspectFlags sa = s->aspect, da = d->aspect;
    if (s->samples > 1 && d->samples == 1 && sw == dw && sh == dh && s->format == d->format && !(sa & VK_IMAGE_ASPECT_DEPTH_BIT)) {
        VkImageResolve rv = {.srcSubresource = {sa, slv, sly, 1}, .srcOffset = {sx0, sy0, 0}, .dstSubresource = {da, dlv, dly, 1},
                             .dstOffset = {dx0, dy0, 0}, .extent = {(uint32_t)sw, (uint32_t)sh, 1}};
        vkCmdResolveImage(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rv);
        return;
    }
    if (s->samples != d->samples) return;
    if (sw == dw && sh == dh && s->format == d->format) {
        VkImageCopy c = {.srcSubresource = {sa, slv, sly, 1}, .srcOffset = {sx0, sy0, 0}, .dstSubresource = {da, dlv, dly, 1},
                         .dstOffset = {dx0, dy0, 0}, .extent = {(uint32_t)sw, (uint32_t)sh, 1}};
        vkCmdCopyImage(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        return;
    }
    // scaled / converting copy: the game builds its depth pyramid this way (nearest), colour copies filter if asked
    bool depth = (sa | da) & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
    if (depth && s->format != d->format) return;
    VkImageBlit bl = {.srcSubresource = {depth ? (sa & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : sa) : sa, slv, sly, 1},
                      .srcOffsets = {{sx0, sy0, 0}, {sx0 + sw, sy0 + sh, 1}},
                      .dstSubresource = {depth ? (da & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : da) : da, dlv, dly, 1},
                      .dstOffsets = {{dx0, dy0, 0}, {dx0 + dw, dy0 + dh, 1}}};
    bool linear = (flags & 1) && !depth && !rt_kind(st_->format);
    dl_note("  blit %p lv%u/%u %ux%u (%d,%d %dx%d) -> %p lv%u/%u %ux%u (%d,%d %dx%d) %s", (void *)s->image, slv, s->levels, s->width,
            s->height, sx0, sy0, sw, sh, (void *)d->image, dlv, d->levels, d->width, d->height, dx0, dy0, dw, dh, linear ? "linear" : "nearest");
    vkCmdBlitImage(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
}

// ================================================================== present helpers (device.c)
static VkPipelineLayout present_layout;
static VkDescriptorSetLayout present_set_layout;

static VkPipeline present_pipeline(VkFormat fmt) {
    static struct { VkFormat f; VkPipeline p; } cache[4];
    for (int i = 0; i < 4; i++) if (cache[i].p && cache[i].f == fmt) return cache[i].p;
    if (!present_layout) {
        VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
        VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &b};
        VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, &present_set_layout));
        VkPushConstantRange r = {VK_SHADER_STAGE_VERTEX_BIT, 0, 32};
        VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                         .pSetLayouts = &present_set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &r};
        VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &present_layout));
    }
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vk_builtin_shader("present.vert"), .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = vk_builtin_shader("present.frag"), .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xF};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1,
                                              .pAttachments = &cba};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1,
                                        .pColorAttachmentFormats = &fmt};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = present_layout};
    VkPipeline p;
    VK_CHECK(vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &p));
    for (int i = 0; i < 4; i++) if (!cache[i].p) { cache[i].f = fmt; cache[i].p = p; break; }
    return p;
}

// Scanout shows the bytes the game wrote: sRGB images are read through a UNORM view (no decode), like metal.m.
static VkImageView raw_view(TexBackend *b) {
    VkFormat f = b->format == VK_FORMAT_R8G8B8A8_SRGB ? VK_FORMAT_R8G8B8A8_UNORM
               : b->format == VK_FORMAT_B8G8R8A8_SRGB ? VK_FORMAT_B8G8R8A8_UNORM : b->format;
    static const VkComponentMapping id = {0};
    return vk_image_view(b, VK_IMAGE_VIEW_TYPE_2D, f, 0, 1, 0, 1, id, VK_IMAGE_ASPECT_COLOR_BIT);
}

// ---- post passes (Video tab "Anti-aliasing" / "Upscaler"): SMAA 1x at the game image's size, then AMD FSR 1 (EASU
// upscale to the image's size in the window, RCAS sharpen) when it is smaller than its place in the window, as
// metal.m does with MetalFX. Each pass: a fullscreen triangle into an RGBA8 image (gamma-encoded, like the input);
// up to three inputs (bindings 0-2), sampled linear / clamp.
static VkPipelineLayout post_layout;
static VkDescriptorSetLayout post_set_layout;

static VkPipeline post_pipeline(const char *fs) {
    static struct { const char *fs; VkPipeline p; } cache[8];
    for (int i = 0; i < 8; i++) if (cache[i].p && !strcmp(cache[i].fs, fs)) return cache[i].p;
    if (!post_layout) {
        VkDescriptorSetLayoutBinding b[3];
        for (uint32_t i = 0; i < 3; i++)
            b[i] = (VkDescriptorSetLayoutBinding){i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
        VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 3, .pBindings = b};
        VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, &post_set_layout));
        VkPushConstantRange r = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
        VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                         .pSetLayouts = &post_set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &r};
        VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &post_layout));
    }
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vk_builtin_shader("post.vert"), .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = vk_builtin_shader(fs), .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xF};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1,
                                              .pAttachments = &cba};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkFormat fmt = !strcmp(fs, "cas.frag") || !strcmp(fs, "dlss_rcas.frag") ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1,
                                        .pColorAttachmentFormats = &fmt};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = post_layout};
    VkPipeline p;
    VK_CHECK(vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &p));
    for (int i = 0; i < 8; i++) if (!cache[i].p) { cache[i].fs = fs; cache[i].p = p; break; }
    return p;
}

static void prewarm_post_pipelines(void) {
    static const char *const filters[] = {
        "smaa_edge.frag", "smaa_weight.frag", "smaa_blend.frag", "easu.frag", "rcas.frag",
#ifdef _WIN32
        "cas.frag",
        "dlss_rcas.frag",
#endif
    };
    for (size_t i = 0; i < sizeof filters / sizeof *filters; i++) post_pipeline(filters[i]);
}

// Post image `i` at w x h. 0, 1: SMAA edges / weights, 2: SMAA output,
// 3, 4: FSR EASU / RCAS (RGBA8). 5, 6: CAS / RCAS on DLSS (half float).
static TexBackend *post_image(int i, uint32_t w, uint32_t h) {
    static TexBackend img[7];
    TexBackend *d = &img[i];
    if (d->image && (d->width != w || d->height != h)) {
        vk_views_forget(d);
        vk_defer_image(d->image, d->mem);
        d->image = VK_NULL_HANDLE;
        d->mem = VK_NULL_HANDLE;
    }
    if (!d->image) {
        VkFormat format = i >= 5 ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                                .format = format, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
                                .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &d->image, &d->mem, NULL));
        d->format = format; d->width = w; d->height = h; d->depth = 1; d->layers = 1; d->levels = 1;
        d->samples = 1; d->aspect = VK_IMAGE_ASPECT_COLOR_BIT; d->base_type = VK_IMAGE_VIEW_TYPE_2D;
        vk_image_init_layout(vk_cmd(), d->image, d->aspect);
    }
    return d;
}

static VkImageView post_view(TexBackend *d) {
    static const VkComponentMapping id = {0};
    return vk_image_view(d, VK_IMAGE_VIEW_TYPE_2D, d->format, 0, 1, 0, 1, id, VK_IMAGE_ASPECT_COLOR_BIT);
}

// SMAA's lookup textures (runtime/nvn/smaa_data.c): 0 = area (160 x 560 R8G8), 1 = search (64 x 16 R8), uploaded once.
static VkImageView smaa_lut(int i) {
    static TexBackend t[2];
    TexBackend *d = &t[i];
    if (!d->image) {
        extern const unsigned char *smaa_area_tex(void);
        extern const unsigned char *smaa_search_tex(void);
        uint32_t w = i ? 64 : 160, h = i ? 16 : 560, bpp = i ? 1 : 2;
        VkFormat f = i ? VK_FORMAT_R8_UNORM : VK_FORMAT_R8G8_UNORM;
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f,
                                .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &d->image, &d->mem, NULL));
        d->format = f; d->width = w; d->height = h; d->depth = 1; d->layers = 1; d->levels = 1;
        d->samples = 1; d->aspect = VK_IMAGE_ASPECT_COLOR_BIT; d->base_type = VK_IMAGE_VIEW_TYPE_2D;
        VkCommandBuffer cb = vk_cmd();
        vk_image_init_layout(cb, d->image, d->aspect);
        VkSpan s = vk_transient_copy(i ? smaa_search_tex() : smaa_area_tex(), (uint64_t)w * h * bpp, 16);
        VkBufferImageCopy c = {.bufferOffset = s.off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
        vkCmdCopyBufferToImage(cb, s.buf, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    }
    return post_view(d);
}

// One post pass (outside the window image's rendering): srcs (bindings 0..n-1) -> dst, fragment shader `fs`.
// clear: start from zero (SMAA's edge pass only writes edge pixels; its weights / output read them).
static void post_pass(VkCommandBuffer cb, const char *fs, const VkImageView *srcs, int n, TexBackend *dst, const float pc[8],
                      bool clear) {
    VkPipeline pipe = post_pipeline(fs);  // (before the rendering begins: created inside it, the NVIDIA driver crashed)
    vk_full_barrier(cb);
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = post_view(dst),
                                    .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                                    .loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {dst->width, dst->height}},
                          .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vkCmdBeginRendering(cb, &ri);
    VkDescriptorSet ds = vk_alloc_set(post_set_layout);
    VkDescriptorImageInfo ii[3];
    VkWriteDescriptorSet w[3];
    for (int i = 0; i < n; i++) {
        ii[i] = (VkDescriptorImageInfo){vk_default_sampler(true), srcs[i], VK_IMAGE_LAYOUT_GENERAL};
        w[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = (uint32_t)i,
                                      .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                      .pImageInfo = &ii[i]};
    }
    vkUpdateDescriptorSets(g_vk.dev, (uint32_t)n, w, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, post_layout, 0, 1, &ds, 0, NULL);
    VkViewport vp = {0, 0, (float)dst->width, (float)dst->height, 0, 1};
    VkRect2D sc = {{0, 0}, {dst->width, dst->height}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdPushConstants(cb, post_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32, pc);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
}

// The view the present samples: the game image, or its anti-aliased / upscaled version. ow x oh: the whole image's
// size in the window (its crop is drawn into the viewport).
static VkImageView post_process(VkCommandBuffer cb, TexBackend *b, double ow_d, double oh_d) {
    VkImageView v = raw_view(b);
    bool smaa = g_settings.antialias == AA_SMAA;
    uint32_t ow = (uint32_t)lround(ow_d), oh = (uint32_t)lround(oh_d);
    if (ow > 16384) ow = 16384;
    if (oh > 16384) oh = 16384;
#ifdef _WIN32
    VkImageView dlss = vk_dlss_upscale(cb, b, v, ow, oh, renderer_present_count());
    if (dlss) {
        bool fsr3 = g_settings.temporal_backend == TEMPORAL_FSR3 || g_settings.temporal_backend == TEMPORAL_FSR4;
        int cas_strength = *settings_temporal_cas_sharpness();
        bool cas = *settings_temporal_cas() && cas_strength > 0;
        int rcas_strength = fsr3 ? 0 : *settings_temporal_sharpness(); // FSR's own RCAS ran inside its dispatch
        if (!cas && !rcas_strength) return dlss;
        extern void vk_frame_suspend(void), vk_frame_resume(void);
        vk_frame_suspend();
        if (rcas_strength > 0) {
            TexBackend *sharp = post_image(6, ow, oh);
            float pc[8] = {exp2f(-2.0f * (1.0f - rcas_strength * 0.01f)), 0, 0, 0, (float)ow, (float)oh};
            post_pass(cb, "dlss_rcas.frag", &dlss, 1, sharp, pc, false);
            vk_full_barrier(cb);
            dlss = post_view(sharp);
        }
        if (cas) {
            TexBackend *sharp = post_image(5, ow, oh);
            float pc[8] = {cas_strength * 0.01f};
            post_pass(cb, "cas.frag", &dlss, 1, sharp, pc, false);
            vk_full_barrier(cb);
            dlss = post_view(sharp);
        }
        vk_frame_resume();
        return dlss;
    }
#endif
    bool fsr = g_settings.upscaler == UPSCALE_FSR1 && (ow > b->width + 1 || oh > b->height + 1);  // (an upscale only)
    if (!smaa && !fsr) return v;
    extern void vk_frame_suspend(void);
    extern void vk_frame_resume(void);
    vk_frame_suspend();
    if (smaa) {  // SMAA 1x: luma edges -> blending weights -> neighbourhood blending
        float w = (float)b->width, h = (float)b->height;
        float m[8] = {1.0f / w, 1.0f / h, w, h};
        TexBackend *edges = post_image(0, b->width, b->height), *weights = post_image(1, b->width, b->height);
        TexBackend *out = post_image(2, b->width, b->height);
        VkImageView in[3] = {v};
        post_pass(cb, "smaa_edge.frag", in, 1, edges, m, true);
        VkImageView wi[3] = {post_view(edges), smaa_lut(0), smaa_lut(1)};
        post_pass(cb, "smaa_weight.frag", wi, 3, weights, m, true);
        VkImageView bi[3] = {v, post_view(weights)};
        post_pass(cb, "smaa_blend.frag", bi, 2, out, m, false);
        v = post_view(out);
    }
    if (fsr) {
        float iw = (float)b->width, ih = (float)b->height;
        TexBackend *e = post_image(3, ow, oh), *r = post_image(4, ow, oh);
        float pe[8] = {iw / ow, ih / oh, 0.5f * iw / ow - 0.5f, 0.5f * ih / oh - 0.5f, iw, ih};
        VkImageView ei[3] = {v};
        post_pass(cb, "easu.frag", ei, 1, e, pe, false);
        v = post_view(e);
        if (g_settings.fsr1_sharpness > 0) {
            float pr[8] = {exp2f(-2.0f * (1.0f - g_settings.fsr1_sharpness * 0.01f)), 0, 0, 0, (float)ow, (float)oh};
            VkImageView ri_[3] = {v};
            post_pass(cb, "rcas.frag", ri_, 1, r, pr, false);
            v = post_view(r);
        }
    }
    vk_full_barrier(cb);
    vk_frame_resume();
    {
        static uint32_t piw, pih, pow_, poh;
        static int psm = -1, pfs = -1;
        if (b->width != piw || b->height != pih || ow != pow_ || oh != poh || smaa != psm || fsr != pfs)
            LOG("[post] %ux%u%s%s -> %ux%u\n", b->width, b->height, smaa ? " smaa" : "", fsr ? " fsr1" : "", ow, oh);
        piw = b->width; pih = b->height; pow_ = ow; poh = oh; psm = smaa; pfs = fsr;
    }
    return v;
}

// Inside the window image's rendering (device.c vk_frame_begin): the game image into (vx, vy, vw, vh).
void vk_present_draw(VkCommandBuffer cb, TexBackend *b, float vx, float vy, float vw, float vh, const float pv[8]) {
    if (!b || !b->image || (b->aspect & ~VK_IMAGE_ASPECT_COLOR_BIT)) return;
    VkImageView src = post_process(cb, b, pv[6] > 0 ? vw / pv[6] : vw, pv[7] > 0 ? vh / pv[7] : vh);
    VkPipeline p = present_pipeline(g_vk.sc_format);
    VkDescriptorSet ds = vk_alloc_set(present_set_layout);
    VkDescriptorImageInfo ii = {vk_default_sampler(true), src, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1,
                              .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii};
    vkUpdateDescriptorSets(g_vk.dev, 1, &w, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, present_layout, 0, 1, &ds, 0, NULL);
    VkViewport vp = {vx, vy, vw, vh, 0, 1};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdPushConstants(cb, present_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 32, pv);
    vkCmdDraw(cb, 3, 1, 0, 0);
    VkViewport full = {0, 0, (float)g_vk.sc_extent.width, (float)g_vk.sc_extent.height, 0, 1};
    vkCmdSetViewport(cb, 0, 1, &full);
}

// Screenshot of a game texture (presented crop applied to build/frame_* shots), via the present shader into RGBA8.
void renderer_save_texture_tex(TexBackend *b, const char *path) {
    if (!b || !b->image || (b->aspect & ~VK_IMAGE_ASPECT_COLOR_BIT)) return;
    extern const float *vk_shot_crop(void);
    float crop[4] = {0, 0, 1, 1};
    if (strstr(path, "frame_")) memcpy(crop, vk_shot_crop(), sizeof crop);
    uint32_t w = (uint32_t)(b->width * crop[2]), h = (uint32_t)(b->height * crop[3]);
    if (!w || !h) return;
    vk_end_pass();
    VkImage img;
    VmaAllocation mem;
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &img, &mem, NULL));
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    vk_image_init_layout(cb, img, VK_IMAGE_ASPECT_COLOR_BIT);
    VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                 .format = VK_FORMAT_R8G8B8A8_UNORM, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView tv;
    VK_CHECK(vkCreateImageView(g_vk.dev, &vci, NULL, &tv));
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = tv, .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {w, h}}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vkCmdBeginRendering(cb, &ri);
    VkPipeline p = present_pipeline(VK_FORMAT_R8G8B8A8_UNORM);
    VkDescriptorSet ds = vk_alloc_set(present_set_layout);
    VkDescriptorImageInfo ii = {vk_default_sampler(true), raw_view(b), VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet wr = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1,
                               .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii};
    vkUpdateDescriptorSets(g_vk.dev, 1, &wr, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, present_layout, 0, 1, &ds, 0, NULL);
    VkViewport vp = {0, 0, (float)w, (float)h, 0, 1};
    VkRect2D sc = {{0, 0}, {w, h}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    float pv[8] = {renderer_origin_lower_left() && !strstr(path, "window") ? 1.0f : 0.0f, 0, 0, 0, crop[0], crop[1], crop[2], crop[3]};
    vkCmdPushConstants(cb, present_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 32, pv);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
    vk_full_barrier(cb);
    VkSpan out = vk_readback((uint64_t)w * h * 4);
    VkBufferImageCopy c = {.bufferOffset = out.off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
    vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_GENERAL, out.buf, 1, &c);
    renderer_flush();
    vk_save_png(path, out.ptr, w, h);
    vkDestroyImageView(g_vk.dev, tv, NULL);
    vmaDestroyImage(g_vk.vma, img, mem);
}

// ================================================================== executor (renderer.m exec_list)
static void exec_list(uint64_t handle, int depth) {
    uint64_t p = (uint64_t)(uintptr_t)cmd_list(handle);  // (records live in host memory: runtime/nvn/cmdbuf.c)
    if (!p) return;
    for (int guard = 0; guard < 10000000; guard++) {
        CmdRec *r = (CmdRec *)(uintptr_t)p;
        void *x = r->extra;
        if (r->op < CMD_COUNT) op_counts[r->op]++;
        if (g_debug_frame) {  // ODYSSEY_DEBUG_CMDS=2: every command executed
            static int every = -1;
            if (every < 0) { const char *e = getenv("ODYSSEY_DEBUG_CMDS"); every = e && atoi(e) >= 2; }
            if (every) LOG("[cmd]   list %llx op %u x %llx %llx %llx\n", (unsigned long long)handle, r->op, (unsigned long long)r->x[0],
                           (unsigned long long)r->x[1], (unsigned long long)r->x[2]);
        }
        switch (r->op) {
        case CMD_END: return;
        case CMD_JUMP: p = r->x[0]; continue;
        case CMD_CALL: {
            const uint64_t *h = x;
            static int logcalls = -1;
            if (logcalls < 0) logcalls = getenv("ODYSSEY_DEBUG_CMDS") != NULL;
            for (uint64_t i = 0; i < r->x[0] && depth < 8; i++) {
                if (g_debug_frame && logcalls) LOG("[cmd] call list %llx (depth %d)\n", (unsigned long long)h[i], depth + 1);
                exec_list(h[i], depth + 1);
                if (g_debug_frame && logcalls) LOG("[cmd] return from %llx\n", (unsigned long long)h[i]);
            }
            break;
        }
        case CMD_BIND_PROGRAM: st.program = (NvnProgram *)(uintptr_t)r->x[0]; break;
        case CMD_BIND_BLEND: { uint8_t *b = x; memcpy(st.blend[b[0] & 7], b, 8); break; }
        case CMD_BIND_CHANNEL_MASK: memcpy(&st.channel_mask, x, 4); break;
        case CMD_BIND_COLOR: memcpy(st.color_state, x, 4); break;
        case CMD_BIND_DEPTH_STENCIL: memcpy(st.ds, x, 8); break;
        case CMD_BIND_MULTISAMPLE: memcpy(st.ms, x, 0x18); break;
        case CMD_BIND_POLYGON: memcpy(st.poly, x, 4); break;
        case CMD_BIND_VERTEX_ATTRIB:
            st.nattrib = (int)r->x[0] > 16 ? 16 : (int)r->x[0];
            memcpy(st.attrib, x, st.nattrib * 4);
            break;
        case CMD_BIND_VERTEX_STREAM:
            st.nstream = (int)r->x[0] > 16 ? 16 : (int)r->x[0];
            memcpy(st.stream, x, st.nstream * 8);
            break;
        case CMD_BIND_VERTEX_BUFFERS:
            for (uint64_t i = 0; i < r->x[1] && r->x[0] + i < 16; i++) memcpy(&st.vbuf[r->x[0] + i], (uint8_t *)x + i * 16, 16);
            if (g_debug_frame && getenv("ODYSSEY_DEBUG_CMDS"))
                LOG("[cmd] bind vertex buffers %llu+%llu: %llx\n", (unsigned long long)r->x[0], (unsigned long long)r->x[1],
                    (unsigned long long)*(const uint64_t *)x);
            break;
        case CMD_BIND_UNIFORM_BUFFERS:
            for (uint64_t i = 0; i < r->x[2] && r->x[1] + i < 18 && r->x[0] < 6; i++)
                memcpy(&st.ubuf[r->x[0]][r->x[1] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_BIND_STORAGE_BUFFERS:
            for (uint64_t i = 0; i < r->x[2] && r->x[1] + i < 16 && r->x[0] < 6; i++)
                memcpy(&st.sbuf[r->x[0]][r->x[1] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_BIND_TEXTURES:
            for (uint64_t i = 0; i < r->x[2] && r->x[1] + i < 32 && r->x[0] < 6; i++)
                memcpy(&st.tex[r->x[0]][r->x[1] + i], (uint8_t *)x + i * 8, 8);
            break;
        case CMD_BIND_IMAGES:
            for (uint64_t i = 0; i < r->x[2] && r->x[1] + i < 8 && r->x[0] < 6; i++)
                memcpy(&st.img[r->x[0]][r->x[1] + i], (uint8_t *)x + i * 8, 8);
            break;
        case CMD_SET_TEXTURE_POOL: st.tpool = (NvnDescPool *)(uintptr_t)r->x[0]; break;
        case CMD_SET_SAMPLER_POOL: st.spool = (NvnDescPool *)(uintptr_t)r->x[0]; break;
        case CMD_SET_RENDER_TARGETS: {
            const struct { uint64_t tex[8]; NvnTextureView views[8]; uint8_t has_view[8]; uint64_t depth;
                           NvnTextureView dview; uint8_t has_dview; } *rt = x;
            RtState prev = st.rt;  // the same targets again: keep the pass (a restart = full barrier + reload)
            memset(&st.rt, 0, sizeof st.rt);
            st.rt.ncolor = (int)r->x[0] > 8 ? 8 : (int)r->x[0];
            for (int i = 0; i < st.rt.ncolor; i++) {
                st.rt.color[i] = (NvnTexture *)(uintptr_t)rt->tex[i];
                st.rt.cview[i] = rt->views[i];
                st.rt.has_cview[i] = rt->has_view[i];
            }
            st.rt.depth = (NvnTexture *)(uintptr_t)rt->depth;
            st.rt.dview = rt->dview;
            st.rt.has_dview = rt->has_dview;
            static int always = -1;
            if (always < 0) always = getenv("ODYSSEY_VK_PASS_RESTART") != NULL;
            if (always || memcmp(&prev, &st.rt, sizeof prev)) st.rt_dirty = true;
            break;
        }
        case CMD_SET_VIEWPORTS:
            for (uint64_t i = 0; i < r->x[1] && r->x[0] + i < 16; i++) memcpy(st.viewport[r->x[0] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_SET_SCISSORS:
            for (uint64_t i = 0; i < r->x[1] && r->x[0] + i < 16; i++) memcpy(st.scissor[r->x[0] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_SET_DEPTH_RANGES:
            for (uint64_t i = 0; i < r->x[1] && r->x[0] + i < 16; i++) memcpy(st.depth_range[r->x[0] + i], (uint8_t *)x + i * 8, 8);
            break;
        case CMD_SET_BLEND_COLOR: memcpy(st.blend_color, x, 16); break;
        case CMD_SET_STENCIL: {
            int face = (int)r->x[0];
            for (int f = 0; f < 2; f++) {
                if (!(face & (1 << f))) continue;
                if (r->x[6] == 0) st.stencil_ref[f] = (uint32_t)r->x[1];
                else if (r->x[6] == 1) st.stencil_mask[f] = (uint32_t)r->x[1];
                else st.stencil_vmask[f] = (uint32_t)r->x[1];
            }
            break;
        }
        case CMD_SET_POLYGON_OFFSET: memcpy(st.poly_offset, r->f, sizeof st.poly_offset); break;
        case CMD_SET_PRIMITIVE_RESTART: st.prim_restart = r->x[0] != 0; st.restart_index = (uint32_t)r->x[1]; break;
        case CMD_SET_COPY_STRIDES: st.copy_row_stride = r->x[0]; st.copy_image_stride = r->x[1]; break;
        case CMD_CLEAR_COLOR: {
            snap_reason = "clear-color"; snapshots_invalidate(NULL);
            int index = (int)r->x[0];
            if (getenv("ODYSSEY_LOG_CLEARS") && index < st.rt.ncolor && st.rt.color[index])
                LOG("[cclear] frame %llu idx %d %ux%u f%#x mask %#x\n", (unsigned long long)renderer_present_count(), index,
                    st.rt.color[index]->width, st.rt.color[index]->height, st.rt.color[index]->format, (uint32_t)r->x[2]);
            if (ct_on() && index < st.rt.ncolor && st.rt.color[index])
                LOG("[ct] clear-color %p mask %#x sc %d %d %d %d\n", (void *)st.rt.color[index], (uint32_t)r->x[2], st.scissor[0][0], st.scissor[0][1],
                    st.scissor[0][2], st.scissor[0][3]);
            if (index < st.rt.ncolor && st.rt.color[index]) alias_before_write(st.rt.color[index], 0, "clear");
            if (index < st.rt.ncolor && st.rt.color[index]) clear(index, (int)r->x[5], x, (uint32_t)r->x[2], false, 0, false, 0, 0);
            debug_tex_trace("clear");
            break;
        }
        case CMD_CLEAR_TEXTURE: {
            // ClearTexture(cmd, texture, view, region, color, mask): a render-target clear of the texture alone (renderer.m)
            const struct { uint64_t tex; NvnTextureView view; uint8_t has_view; int32_t region[6]; float color[4]; } *ct = x;
            NvnTexture *t = (NvnTexture *)(uintptr_t)ct->tex;
            if (!t) break;
            snap_reason = "clear-texture"; snapshots_invalidate(NULL);
            alias_before_write(t, 0, "clear");
            RtState saved = st.rt;
            int32_t sc[4];
            memcpy(sc, st.scissor[0], sizeof sc);
            vk_end_pass();
            memset(&st.rt, 0, sizeof st.rt);
            st.rt.ncolor = 1;
            st.rt.color[0] = t;
            if (ct->has_view) { st.rt.cview[0] = ct->view; st.rt.has_cview[0] = 1; }
            st.rt_dirty = true;
            const int32_t *rg = ct->region;
            if (rg[3] > 0 && rg[4] > 0) { st.scissor[0][0] = rg[0]; st.scissor[0][1] = rg[1]; st.scissor[0][2] = rg[3]; st.scissor[0][3] = rg[4]; }
            else { st.scissor[0][0] = st.scissor[0][1] = 0; st.scissor[0][2] = st.scissor[0][3] = 16384; }
            clear(0, (int)r->x[6], ct->color, (uint32_t)r->x[4] ? (uint32_t)r->x[4] : 0xF, false, 0, false, 0, 0);
            vk_end_pass();
            st.rt = saved;
            memcpy(st.scissor[0], sc, sizeof sc);
            st.rt_dirty = true;
            break;
        }
        case CMD_CLEAR_DEPTH_STENCIL:
            snap_reason = "clear-depth"; snapshots_invalidate(NULL);
            if (ct_on() && st.rt.depth)
                LOG("[ct] clear-depth %p d%d s%d sc %d %d %d %d\n", (void *)st.rt.depth, r->x[0] != 0, r->x[2] != 0, st.scissor[0][0],
                    st.scissor[0][1], st.scissor[0][2], st.scissor[0][3]);
            if (st.rt.depth && naliases) {  // (copy aliases: which aspects of the depth target this clear replaces whole)
                NvnTexture *d = st.rt.depth;
                bool whole = st.scissor[0][0] <= 0 && st.scissor[0][1] <= 0 && (int64_t)st.scissor[0][0] + st.scissor[0][2] >= (int64_t)d->width &&
                             (int64_t)st.scissor[0][1] + st.scissor[0][3] >= (int64_t)d->height &&
                             !(st.rt.has_dview && (((st.rt.dview.setmask & 1) && st.rt.dview.base_level) ||
                                                   ((st.rt.dview.setmask & 2) && st.rt.dview.min_layer)));
                for (int i = 0; i < st.rt.ncolor; i++)  // (the clear covers the smallest attachment only)
                    if (st.rt.color[i] && (st.rt.color[i]->width < d->width || st.rt.color[i]->height < d->height)) whole = false;
                VkImageAspectFlags full = 0;
                if (whole && r->x[0] != 0) full |= VK_IMAGE_ASPECT_DEPTH_BIT;
                if (whole && renderer_format_has_stencil(d->format) && (r->x[2] & 0xFF) == 0xFF) full |= VK_IMAGE_ASPECT_STENCIL_BIT;
                if (!renderer_format_has_stencil(d->format)) full |= VK_IMAGE_ASPECT_STENCIL_BIT;
                if (ct_on())
                    LOG("[ct] alias clear %p full %#x (whole %d, x0 %llu x1 %#llx x2 %#llx, dview %d/%#x, ncolor %d)\n", (void *)d, full, whole,
                        (unsigned long long)r->x[0], (unsigned long long)r->x[1], (unsigned long long)r->x[2], st.rt.has_dview,
                        st.rt.dview.setmask, st.rt.ncolor);
                alias_before_write(d, full, "clear");
            }
            if (st.rt.depth) {
                bool has_stencil = renderer_format_has_stencil(st.rt.depth->format);
                if (g_debug_frame)
                    LOG("[draw] clear depth %p f%#x to %g (depth %d, stencil %u mask %#x)\n", (void *)st.rt.depth, st.rt.depth->format,
                        r->f[0], r->x[0] != 0, (uint32_t)r->x[1], (uint32_t)r->x[2]);
                clear(-1, 0, NULL, 0, r->x[0] != 0, r->f[0], has_stencil && r->x[2] != 0, (uint32_t)r->x[1], (uint32_t)r->x[2]);
            }
            break;
        case CMD_DISPATCH:
            if (g_interp_replaying) break;  // (frame interpolation: the frame's compute results are there already)
            if (g_interp_log) for (int i = 0; i < 16; i++) if (st.sbuf[5][i].addr) interp_dispatch_output(st.sbuf[5][i].addr, st.sbuf[5][i].size);
            dispatch_compute((uint32_t)r->x[0], (uint32_t)r->x[1], (uint32_t)r->x[2]); debug_tex_trace("dispatch"); break;
        case CMD_DRAW_ARRAYS: draw(r, false); break;
        case CMD_DRAW_ELEMENTS: draw(r, true); break;
        case CMD_UPDATE_UNIFORM_BUFFER:
            if (g_interp_replaying) break;  // (a replay's draws read their logged / blended uniforms)
            memcpy((void *)(uintptr_t)(r->x[0] + r->x[2]), x, r->x[3]);
            guest_copy_flush();  // (cbuf copies shared within the submission are stale now)
            break;
        case CMD_FENCE_SYNC: {
            if (g_interp_replaying) break;  // (replays leave the game's fences, counters and memory alone)
            static int fence_flush = -1;
            if (fence_flush < 0) fence_flush = getenv("ODYSSEY_FENCE_FLUSH") ? 1 : 0;
            if (fence_flush) renderer_flush();
            extern void nvn_sync_set_seq(uint64_t guest, uint64_t seq);
            if (r->x[1]) nvn_sync_set_seq(r->x[1], renderer_fence_seq());
            break;
        }
        case CMD_REPORT_COUNTER: {
            if (g_interp_replaying) break;
            // The game times its GPU work with these (two per frame) and drops to 30 fps when that looks slow: the
            // host clock now, replaced by the GPU's time at this point once the submission is done (cq_resolve).
            uint64_t *out = (uint64_t *)(uintptr_t)r->x[1];
            uint32_t type = (uint32_t)r->x[0];
            static uint32_t seen;
            if (type < 32 && !(seen & (1u << type))) { seen |= 1u << type; LOG("[nvn] ReportCounter type %u\n", type); }
            int mode = vk_counter_mode();
            static uint64_t frame_seen = ~0ull, frame_ts;
            extern uint64_t vk_frame_counter(void);
            if (mode == 1 && vk_frame_counter() != frame_seen) { frame_seen = vk_frame_counter(); frame_ts = ns_to_gpu_ticks(plat_now_ns()); }
            if (!out) break;
            // Counter value. SAMPLES_PASSED (1) is Odyssey's occlusion culling (agl::cull::OcclusionQuery::getZPassCount:
            // a model shows if its query box passed any sample): 0 hid models with a query box - parts of the fog sea
            // and more came and went with the camera. No real query here: every box counts as seen (no culling).
            out[0] = type == 1 ? 0x10000 : 0;
            out[1] = mode == 1 ? frame_ts : mode == 3 ? SDL_GetTicksNS() : ns_to_gpu_ticks(plat_now_ns());  // (3 legacy: ns, the old bug)
            static uint64_t *prev_out;  // (frame pacing: the pair's start)
            if (mode == 0 || mode == 2) out[1] = pace_end_counter(prev_out, out, out[1]);
            prev_out = out;
            {   // (test) ODYSSEY_TEST_COUNTER_DELAY_MS=<ms>: stall the host after a frame's first counter - a slow CPU
                static double delay = -1;
                static uint64_t delayed_frame = ~0ull;
                if (delay < 0) delay = getenv("ODYSSEY_TEST_COUNTER_DELAY_MS") ? atof(getenv("ODYSSEY_TEST_COUNTER_DELAY_MS")) : 0;
                if (delay > 0 && vk_frame_counter() != delayed_frame) {
                    delayed_frame = vk_frame_counter();
                    SDL_DelayPrecise((Uint64)(delay * 1e6));
                }
            }
            if (mode == 2 && type == 0) {  // (type 0: timestamp)
                VkCommandBuffer cb = vk_cmd();  // (first: may begin the buffer, which resets the slot's queries)
                CounterQ *q = &cq[cur_sub];
                pthread_mutex_lock(&cq_lock);
                if (q->pool && q->n < CQ_MAX) {
                    vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, q->pool, q->n);
                    q->out[q->n++] = out;
                }
                pthread_mutex_unlock(&cq_lock);
            }
            break;
        }
        case CMD_COPY_TEXTURE_TO_TEXTURE: { const TexRefRec *t = x; copy_texture(&t[0], &t[1], r->x[6]); debug_tex_trace("copy"); break; }
        case CMD_DOWNSAMPLE: { const TexRefRec *t = x; copy_texture(&t[0], &t[1], 1); break; }
        case CMD_COPY_BUFFER_TO_TEXTURE:
            if (g_interp_replaying) break;  // (the frame's upload is in the game's image: what the replay reads)
            copy_buffer_to_texture(r->x[0], x); debug_tex_trace("upload copy"); break;
        case CMD_COPY_TEXTURE_TO_BUFFER: if (!g_interp_replaying) copy_texture_to_buffer(x, r->x[3]); break;
        case CMD_COPY_BUFFER_TO_BUFFER:
            if (g_interp_replaying) break;
            wait_why = "buffer -> buffer copy";
            renderer_flush();
            memmove((void *)(uintptr_t)r->x[1], (void *)(uintptr_t)r->x[0], r->x[2]);
            break;
        default: break;
        }
        p += r->size;
    }
}

// Before a submission runs, the programs it binds start building, side by side on the workers: a scene's first frames,
// whose draws wait for every new program (sync_shaders), wait for the slowest instead of the sum of them, and in play a
// new program's build starts before the draws of the frame get to it. Each program object once (NvnProgram.backend,
// cleared when the game sets its shaders again); the keys as a draw would most likely use them.
static void prefetch_programs(uint64_t handle, int depth) {
    uint64_t p = (uint64_t)(uintptr_t)cmd_list(handle);
    for (int guard = 0; p && guard < 10000000; guard++) {
        CmdRec *r = (CmdRec *)(uintptr_t)p;
        switch (r->op) {
        case CMD_END: return;
        case CMD_JUMP: p = r->x[0]; continue;
        case CMD_CALL: {
            const uint64_t *h = (const uint64_t *)(void *)r->extra;
            for (uint64_t i = 0; i < r->x[0] && depth < 8; i++) prefetch_programs(h[i], depth + 1);
            break;
        }
        case CMD_BIND_PROGRAM: {
            NvnProgram *np = (NvnProgram *)(uintptr_t)r->x[0];
            if (!np || np->backend || (np->stage_mask & 3) != 3) break;
            np->backend = (void *)(uintptr_t)1;
            ShaderKey vkey, fkey;
            base_keys(&vkey, &fkey);
            Prog *pr = program_for(np, &vkey, &fkey, PF_PREFETCH);
            if (pr && atomic_load(&pr->state) == 0) prog_urge(pr);
            break;
        }
        default: break;
        }
        p += r->size;
    }
}

// Frame interpolation (runtime/nvn/interp.c): a replay of the frame's lists with the logged (blended) uniforms, from the
// executor's state at the frame's first list (saved with the frame: the interpolation thread replays frame N while the
// game logs N + 1) - the game's own state is restored after it.
void renderer_interp_replay(void) {
    const uint64_t *h;
    uint32_t n = interp_handles(&h);
    size_t sn;
    const void *s0 = interp_frame_state(&sn);
    if (!s0 || sn != sizeof st) return;
    vk_render_lock();
    __typeof__(st) st_end = st;
    bool log = g_interp_log;
    memcpy(&st, s0, sizeof st);
    st.rt_dirty = true;
    g_interp_log = true;
    interp_replay_begin();
    extern __thread bool g_twin_redirect;  // (textures.c: the replay draws into twins of the game's images)
    extern void vk_twins_begin(uint32_t epoch);
    extern uint32_t g_interp_replay_epoch;
    static int twins = -1;  // ODYSSEY_INTERP_TWINS=0: into the game's images (before)
    if (twins < 0) twins = !getenv("ODYSSEY_INTERP_TWINS") || atoi(getenv("ODYSSEY_INTERP_TWINS")) != 0;
    vk_twins_begin(g_interp_replay_epoch);
    g_twin_redirect = twins;
    for (uint32_t i = 0; i < n; i++) exec_list(h[i], 0);
    vk_end_pass();
    g_twin_redirect = false;
    renderer_submit();
    interp_replay_end();
    g_interp_log = log;
    st = st_end;
    st.rt_dirty = true;
    vk_render_unlock();
}

void backend_execute(uint64_t handle) {
    extern void interp_wait_replay(void);
    interp_wait_replay();  // (device.c: the previous frame's replay first - its draws must not interleave with these)
    vk_render_lock();
    if (interp_list_submitted(handle)) interp_frame_state_save(&st, sizeof st);
    PROF_T0();
    static int prefetch = -1;  // ODYSSEY_PREFETCH=0: off
    if (prefetch < 0) { const char *e = getenv("ODYSSEY_PREFETCH"); prefetch = !e || atoi(e) != 0; }
    // The walk over the whole list only finds programs it hasn't seen: none once a scene's programs were all bound
    // (it walked every command of every list each frame for nothing - 3% of a Steam Deck's CPU samples). So only
    // while the game sets up programs, and 600 frames after its last.
    static uint64_t seen_setups, quiet_since;
    uint64_t setups = atomic_load(&prog_setups), frame = renderer_present_count();
    if (setups != seen_setups) seen_setups = setups, quiet_since = frame;
    if (prefetch && game_is_odyssey() && frame - quiet_since < 600) prefetch_programs(handle, 0);
    exec_list(handle, 0);
    PROF_ADD(prof_exec);
    if (prof) cum_exec += SDL_GetTicksNS() - prof_t0_;
    static int sync_submit = -1;
    if (sync_submit < 0) sync_submit = getenv("ODYSSEY_SYNC_SUBMIT") ? 1 : 0;
    renderer_submit();
    vk_render_unlock();
    if (sync_submit) renderer_flush();  // (the wait outside the lock)
}
