// The render thread: the game's command lists are translated to the GPU API here, while the game's thread goes on
// with its next frame - as the Switch's GPU works through a queue while the CPU runs ahead, and as emulators run their
// GPU on a thread of its own. Translated on the submitting thread, the lists cost the game ~4.6 of its ~5.4 ms a frame
// in the Sand Kingdom (Ryzen 9700X, ~1700 draws): on 4-core handhelds (Steam Deck) the game fell to 30-35 fps.
//
// In order on the thread: lists (nvnQueueSubmitCommands), the queue's fences (their GPU submission is known once the
// lists before them are translated), the frame's end and its present. The game waits for the thread where it needs its
// results: a fence wait, a queue finish, freeing a program / pool / command buffer a queued list may use, and one frame
// ahead at most (the present). ODYSSEY_RENDER_THREAD=0: lists run on the submitting thread, as before (macOS always).
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>

#include "../hle.h"
#include "../kernel.h"
#include "../odyssey_pose.h"
#include "../platform.h"
#include "nvn.h"
#include "rqueue.h"
#ifdef _WIN32
#include "vulkan/dlss_probe.h"
#endif

uint64_t renderer_fence_seq(void);  // (the backend: the GPU submission being recorded)

#define RQ_N 8192
typedef struct { int type; void *p; uint64_t a, ticket; } RqItem;
static RqItem q[RQ_N];
static uint64_t q_head, q_tail;            // (q_mx) items [q_head, q_tail) queued
static uint64_t q_ticket;                  // (q_mx) the last ticket handed out
static _Atomic uint64_t q_done;            // the last ticket the thread finished
static _Atomic int q_presents;             // presents queued and not yet done
static pthread_mutex_t q_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t q_cv = PTHREAD_COND_INITIALIZER, q_done_cv = PTHREAD_COND_INITIALIZER;
static __thread bool on_thread;

bool rq_enabled(void) {
#ifdef __APPLE__
    return false;  // (the Metal backend presents from its own handlers: not with this thread)
#else
    static int on = -1;
    if (on < 0) { const char *e = getenv("ODYSSEY_RENDER_THREAD"); on = !e || atoi(e) != 0; }
    return on;
#endif
}
bool rq_on_thread(void) { return on_thread; }

static void *render_thread(void *arg) {
    (void)arg;
    on_thread = true;
    for (;;) {
        pthread_mutex_lock(&q_mx);
        while (q_head == q_tail) pthread_cond_wait(&q_cv, &q_mx);
        RqItem it = q[q_head % RQ_N];
        pthread_mutex_unlock(&q_mx);
        // ODYSSEY_LOG_RENDER_TIME=1: its work per frame (the lists' translation and submission; the frame hand-over; the
        // present, which waits for the window) - with ODYSSEY_LOG_GAME_TIME, what a frame costs the CPU (Steam Deck)
        static int rt_log = -1;
        static uint64_t rt_ns[3];
        static int rt_n;
        if (rt_log < 0) rt_log = getenv("ODYSSEY_LOG_RENDER_TIME") != NULL;
        uint64_t t0 = rt_log ? plat_now_ns() : 0;
        switch (it.type) {
        case RQ_EXEC: backend_execute(it.a); break;
        case RQ_FENCE: { NvnSync *s = it.p; s->seq = renderer_fence_seq(); __atomic_store_n(&s->signaled, 1, __ATOMIC_RELEASE); break; }
        case RQ_FRAME: backend_frame_done(it.p, it.a); break;
        case RQ_PRESENT: backend_present(it.p); atomic_fetch_sub(&q_presents, 1); break;
        case RQ_ODYSSEY_POSE: odyssey_pose_render_frame(it.p); break;
#ifndef __APPLE__
        case RQ_CPU_TEXEL: { extern void renderer_cpu_texel(RqCpuTexel *); renderer_cpu_texel(it.p); break; }
        case RQ_PAINT_WRITE: {
            extern void renderer_paint_cpu_written(uint64_t, uint64_t);
            RqPaintWrite *w = it.p; renderer_paint_cpu_written(w->addr, w->size); break;
        }
#endif
#ifdef _WIN32
        case RQ_DLSS_CAMERA: vk_dlss_set_camera(it.p); free(it.p); break;
#endif
        }
        if (rt_log) {
            rt_ns[it.type == RQ_PRESENT ? 2 : it.type == RQ_FRAME ? 1 : 0] += plat_now_ns() - t0;
            if (it.type == RQ_PRESENT && ++rt_n == 600) {
                LOG("[render-time] lists %.2f ms, frame hand-over %.2f ms, present %.2f ms a frame\n", rt_ns[0] / 600.0 / 1e6,
                    rt_ns[1] / 600.0 / 1e6, rt_ns[2] / 600.0 / 1e6);
                rt_ns[0] = rt_ns[1] = rt_ns[2] = 0, rt_n = 0;
            }
        }
        // (list records the game overwrote meanwhile: freed once nothing queued then, nor a replay, can reach them)
        extern void cmd_reap(uint64_t done);
        extern bool interp_replay_busy(void);
        if (!interp_replay_busy()) cmd_reap(it.ticket);
        pthread_mutex_lock(&q_mx);
        q_head++;
        atomic_store(&q_done, it.ticket);
        pthread_cond_broadcast(&q_done_cv);
        pthread_mutex_unlock(&q_mx);
    }
    return NULL;
}

static void rq_start(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, render_thread, NULL) == 0) pthread_detach(t);
}
uint64_t rq_push(int type, void *p, uint64_t a) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, rq_start);
    if (type == RQ_PRESENT) atomic_fetch_add(&q_presents, 1);
    pthread_mutex_lock(&q_mx);
    if (q_tail - q_head >= RQ_N) {  // (full: the thread is far behind)
        kernel_block_begin();
        while (q_tail - q_head >= RQ_N) pthread_cond_wait(&q_done_cv, &q_mx);
    }
    uint64_t t = ++q_ticket;
    q[q_tail % RQ_N] = (RqItem){type, p, a, t};
    q_tail++;
    pthread_cond_signal(&q_cv);
    pthread_mutex_unlock(&q_mx);
    return t;
}

uint64_t rq_last_ticket(void) {
    pthread_mutex_lock(&q_mx);
    uint64_t t = q_ticket;
    pthread_mutex_unlock(&q_mx);
    return t;
}

void rq_wait(uint64_t ticket) {
    if (on_thread || atomic_load(&q_done) >= ticket) return;
    kernel_block_begin();  // (the core's other threads run meanwhile: kernel.c core gates)
    pthread_mutex_lock(&q_mx);
    while (atomic_load(&q_done) < ticket) pthread_cond_wait(&q_done_cv, &q_mx);
    pthread_mutex_unlock(&q_mx);
}

void rq_drain(void) {
    if (on_thread || !rq_enabled()) return;
    pthread_mutex_lock(&q_mx);
    uint64_t t = q_ticket;
    pthread_mutex_unlock(&q_mx);
    rq_wait(t);
}

// (a reboot: what the old game queued runs out before it goes)
RT_ON_RESET(rq_reset) { rq_drain(); }

void rq_present_throttle(void) {
    if (on_thread || atomic_load(&q_presents) < 1) return;
    kernel_block_begin();
    pthread_mutex_lock(&q_mx);
    while (atomic_load(&q_presents) >= 1) pthread_cond_wait(&q_done_cv, &q_mx);
    pthread_mutex_unlock(&q_mx);
}
