// nn::vi (display/layer) and nv:: (graphics driver init), implemented on the host.
#include <stdlib.h>
#include <time.h>

#include "../hle.h"
#include "../kernel.h"

static uint32_t vsync_r, vsync_w;
static KEvent *vsync_event;
uint32_t vi_vsync_handle(void) { return vsync_r; }
volatile uint64_t g_vsync_count;  // vsyncs signalled so far (runtime/hle/framepace.c)
uint64_t g_native_window;
static pthread_mutex_t vsync_lock = PTHREAD_MUTEX_INITIALIZER;  // (host waiters: vi_wait_vsync)
static pthread_cond_t vsync_cond = PTHREAD_COND_INITIALIZER;
static uint64_t vsync_period_ns(void) {
    return 1000000000ull / (uint64_t)odyssey_native_hz();
}
static uint64_t base_ns, base_count;  // (vsync_lock) vsync number base_count + k is due at base_ns + k periods

static void *vsync_thread(void *arg) {
    (void)arg;
    uint64_t period = vsync_period_ns();
    plat_thread_realtime(period);
    uint64_t next = plat_now_ns();
    pthread_mutex_lock(&vsync_lock);
    base_ns = next, base_count = g_vsync_count;
    pthread_mutex_unlock(&vsync_lock);
    for (;;) {
        uint64_t want = vsync_period_ns();  // (the menu's Frame rate: 60, 90 or 120 Hz, changed while the game runs)
        if (want != period) {
            period = want;
            pthread_mutex_lock(&vsync_lock);
            base_ns = next, base_count = g_vsync_count;
            pthread_mutex_unlock(&vsync_lock);
        }
        next += period;
        plat_sleep_until_ns(next);
        uint64_t now = plat_now_ns();
        bool late = now > next + period * 3;
        if (late) {
            static int warned;
            if (warned++ < 20) LOG("[vi] vsync timer late by %llums (host throttling?)\n", (unsigned long long)((now - next) / 1000000));
            next = now;  // don't burst-signal to catch up
        }
        pthread_mutex_lock(&vsync_lock);
        __atomic_add_fetch(&g_vsync_count, 1, __ATOMIC_RELAXED);
        if (late) base_ns = next, base_count = g_vsync_count;
        pthread_cond_broadcast(&vsync_cond);
        pthread_mutex_unlock(&vsync_lock);
        KEvent *ev = __atomic_load_n(&vsync_event, __ATOMIC_ACQUIRE);
        if (ev) {
            pthread_mutex_lock(&g_klock);
            kernel_signal(ev);
            pthread_mutex_unlock(&g_klock);
        }
    }
    return NULL;
}

// The 60 Hz display timer runs from the first use on: the game's vsync event or NVN's present pacing
// (runtime/nvn/objects.c nvnQueuePresentTexture).
static void vsync_start_thread(void) {
    pthread_t t;
    pthread_create(&t, NULL, vsync_thread, NULL);
}
static void vsync_start(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, vsync_start_thread);
}

uint64_t vi_vsync_count(void) {
    vsync_start();
    return __atomic_load_n(&g_vsync_count, __ATOMIC_RELAXED);
}

// Blocks the calling (host or guest) thread until vsync number `target`; returns the count then.
uint64_t vi_wait_vsync(uint64_t target) {
    vsync_start();
    pthread_mutex_lock(&vsync_lock);
    while (__atomic_load_n(&g_vsync_count, __ATOMIC_RELAXED) < target) pthread_cond_wait(&vsync_cond, &vsync_lock);
    uint64_t n = g_vsync_count;
    pthread_mutex_unlock(&vsync_lock);
    return n;
}

// When vsync number `count` is due (host ns; the timer's schedule - it signals a little after). Frame interpolation
// (runtime/nvn/vulkan/device.c) places a frame's images by the vsync its present waits for.
uint64_t vi_vsync_time(uint64_t count) {
    vsync_start();
    pthread_mutex_lock(&vsync_lock);
    uint64_t t = count >= base_count ? base_ns + (count - base_count) * vsync_period_ns() : base_ns;
    pthread_mutex_unlock(&vsync_lock);
    return base_ns ? t : plat_now_ns();  // (the timer thread not started yet)
}

HLE_FUNC("_ZN2nn2vi10InitializeEv", vi_initialize) { (void)c; }
HLE_FUNC("_ZN2nn2vi8FinalizeEv", vi_finalize) { (void)c; }

HLE_FUNC("_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE", vi_open_default_display) {
    *(uint64_t *)(uintptr_t)c->x[0] = (uint64_t)hle_alloc(0x100);
    c->x[0] = 0;
}

HLE_FUNC("_ZN2nn2vi12CloseDisplayEPNS0_7DisplayE", vi_close_display) { (void)c; }

HLE_FUNC("_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE", vi_create_layer) {
    *(uint64_t *)(uintptr_t)c->x[0] = (uint64_t)hle_alloc(0x100);
    c->x[0] = 0;
}

HLE_FUNC("_ZN2nn2vi12DestroyLayerEPNS0_5LayerE", vi_destroy_layer) { (void)c; }
HLE_FUNC("_ZN2nn2vi12SetLayerCropEPNS0_5LayerEiiii", vi_set_layer_crop) { c->x[0] = 0; }
// (Odyssey; the output scaling is the renderer's)
HLE_FUNC("_ZN2nn2vi19SetLayerScalingModeEPNS0_5LayerENS0_11ScalingModeE", vi_set_layer_scaling_mode) { c->x[0] = 0; }

HLE_FUNC("_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE", vi_get_native_window) {
    if (!g_native_window) g_native_window = (uint64_t)hle_alloc(0x100);
    *(uint64_t *)(uintptr_t)c->x[0] = g_native_window;
    c->x[0] = 0;
}

// Reboot: the vsync timer and its event live on (the kernel's handle table doesn't): new handles at the next boot.
RT_ON_RESET(vi_reset) {
    vsync_r = vsync_w = 0;
    g_native_window = 0;  // (HLE heap)
}

HLE_FUNC("_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE", vi_get_vsync_event) {
    if (!vsync_event) {
        __atomic_store_n(&vsync_event, kernel_create_event_pair(&vsync_w, &vsync_r), __ATOMIC_RELEASE);
        vsync_start();
    } else if (!vsync_r) {
        vsync_w = kernel_new_handle(vsync_event);
        vsync_r = kernel_new_handle(vsync_event);
    }
    // nn::os::AttachReadableHandleToSystemEvent(event, handle, managed=false, clear_mode=AutoClear)
    c->x[1] = vsync_r;
    c->x[2] = 0;
    c->x[3] = 1;
    guest_call(c, guest_sym("_ZN2nn2os33AttachReadableHandleToSystemEventEPNS0_15SystemEventTypeEjbNS0_14EventClearModeE"));
    c->x[0] = 0;
}

HLE_FUNC("_ZN2nv18InitializeGraphicsEPvm", nv_initialize_graphics) { (void)c; }
HLE_FUNC("_ZN2nv16FinalizeGraphicsEv", nv_finalize_graphics) { (void)c; }
HLE_FUNC("_ZN2nv20SetGraphicsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", nv_set_allocator) { (void)c; }
HLE_FUNC("_ZN2nv28SetGraphicsDevtoolsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", nv_set_dt_allocator) { (void)c; }
