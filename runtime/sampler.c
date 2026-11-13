// Sampling profiler of the game thread (Linux; a debugging tool, off unless asked for):
//   ODYSSEY_SAMPLE_AT=<frame> ODYSSEY_SAMPLE_FRAMES=<n> (default 600)
// From that presented frame on, for n frames, the thread that presents (the game's main thread) is interrupted about
// 2000 times a second (SIGPROF from a timer thread) and where it was is recorded. Then build/samples.txt lists
// "count module+offset" (most first): the module is the program, the game code library (odyssey_game.so) or a system
// library, the offset an address in that file - names come from its symbols afterwards (nm), as the game code's are
// hidden from dladdr. Where the time goes per frame, in the game's own code or in the runtime around it.
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE  // (dladdr; before the first include)
#endif
#include "sampler.h"

#ifdef __linux__
#include <dirent.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include "rt.h"

#define MAXS (1 << 20)
static uintptr_t *g_pcs;
static _Atomic unsigned g_n;
static volatile int g_on;
static pid_t g_tid;

static void on_prof(int sig, siginfo_t *si, void *uc_) {
    (void)sig, (void)si;
    if (!g_on) return;
    ucontext_t *uc = uc_;
#if defined(__x86_64__)
    uintptr_t pc = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
#elif defined(__aarch64__)
    uintptr_t pc = (uintptr_t)uc->uc_mcontext.pc;
#else
    uintptr_t pc = 0;
#endif
    unsigned i = atomic_fetch_add(&g_n, 1);
    if (i < MAXS) g_pcs[i] = pc;
}

// The process's busiest thread over half a second (its CPU time: /proc/self/task/<tid>/stat utime + stime) - the
// thread that presents mostly waits; the one doing the frame's work may be another (a guest thread, the render thread)
static pid_t busiest_thread(void) {
    enum { MAXT = 512 };
    static pid_t tids[MAXT];
    static unsigned long long t0[MAXT];
    int n = 0;
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/task", (int)getpid());
    DIR *d = opendir(path);
    for (struct dirent *e; d && (e = readdir(d)) && n < MAXT;)
        if (e->d_name[0] >= '0' && e->d_name[0] <= '9') tids[n++] = (pid_t)atoi(e->d_name);
    if (d) closedir(d);
    unsigned long long now[MAXT];
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < n; i++) {
            char p[96], buf[1024];
            snprintf(p, sizeof p, "/proc/%d/task/%d/stat", (int)getpid(), (int)tids[i]);
            FILE *f = fopen(p, "r");
            unsigned long long ut = 0, st = 0;
            if (f && fgets(buf, sizeof buf, f)) {
                char *c = strrchr(buf, ')');  // (fields after the name: state is 3, utime 14, stime 15)
                int field = 2;
                for (char *tok = c ? strtok(c + 1, " ") : NULL; tok; tok = strtok(NULL, " ")) {
                    field++;
                    if (field == 14) ut = strtoull(tok, NULL, 10);
                    if (field == 15) { st = strtoull(tok, NULL, 10); break; }
                }
            }
            if (f) fclose(f);
            (pass ? now : t0)[i] = ut + st;
        }
        if (!pass) { struct timespec ts = {0, 500 * 1000 * 1000}; nanosleep(&ts, NULL); }
    }
    pid_t best = g_tid;
    unsigned long long most = 0;
    for (int i = 0; i < n; i++)
        if (now[i] - t0[i] > most) most = now[i] - t0[i], best = tids[i];
    return best;
}

static void *ticker(void *a) {
    (void)a;
    pid_t pid = getpid();
    pid_t presenter = g_tid;
    g_tid = busiest_thread();
    LOG("[sample] sampling thread %d (the presenting thread is %d)\n", (int)g_tid, (int)presenter);
    struct timespec ts = {0, 500 * 1000};  // (2 kHz)
    while (g_on) {
        syscall(SYS_tgkill, pid, g_tid, SIGPROF);
        nanosleep(&ts, NULL);
    }
    return NULL;
}

typedef struct { uintptr_t pc; unsigned n; } Row;
static int by_pc(const void *a, const void *b) {
    uintptr_t x = ((const Row *)a)->pc, y = ((const Row *)b)->pc;
    return x < y ? -1 : x > y;
}
static int by_n(const void *a, const void *b) { return (int)((const Row *)b)->n - (int)((const Row *)a)->n; }

static void dump(void) {
    unsigned n = atomic_load(&g_n);
    if (n > MAXS) n = MAXS;
    Row *r = malloc(sizeof *r * (n ? n : 1));
    for (unsigned i = 0; i < n; i++) r[i] = (Row){g_pcs[i], 1};
    qsort(r, n, sizeof *r, by_pc);
    unsigned m = 0;
    for (unsigned i = 0; i < n; i++) {
        if (m && r[m - 1].pc == r[i].pc) r[m - 1].n++;
        else r[m++] = r[i];
    }
    qsort(r, m, sizeof *r, by_n);
    mkdir("build", 0755);
    FILE *f = fopen("build/samples.txt", "w");
    if (f) {
        fprintf(f, "# %u samples of the game thread (~2000/s)\n", n);
        for (unsigned i = 0; i < m; i++) {
            Dl_info di;
            if (dladdr((void *)r[i].pc, &di) && di.dli_fname)
                fprintf(f, "%u %s+%#lx\n", r[i].n, strrchr(di.dli_fname, '/') ? strrchr(di.dli_fname, '/') + 1 : di.dli_fname,
                        (unsigned long)(r[i].pc - (uintptr_t)di.dli_fbase));
            else
                fprintf(f, "%u ?+%#lx\n", r[i].n, (unsigned long)r[i].pc);
        }
        fclose(f);
    }
    LOG("[sample] %u samples written to build/samples.txt\n", n);
    free(r);
}

void sampler_frame(uint64_t frame) {
    static long at = -2, frames;
    if (at == -2) {
        const char *e = getenv("ODYSSEY_SAMPLE_AT");
        at = e ? atol(e) : -1;
        frames = getenv("ODYSSEY_SAMPLE_FRAMES") ? atol(getenv("ODYSSEY_SAMPLE_FRAMES")) : 600;
    }
    if (at < 0) return;
    if ((long)frame == at && !g_on) {
        g_pcs = malloc(sizeof *g_pcs * MAXS);
        if (!g_pcs) { at = -1; return; }
        struct sigaction sa = {0};
        sa.sa_sigaction = on_prof;
        sa.sa_flags = SA_SIGINFO | SA_RESTART;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGPROF, &sa, NULL);
        g_tid = (pid_t)syscall(SYS_gettid);
        g_on = 1;
        pthread_t t;
        pthread_create(&t, NULL, ticker, NULL);
        pthread_detach(t);
        LOG("[sample] sampling the game thread from frame %ld for %ld frames\n", at, frames);
    } else if (g_on && (long)frame >= at + frames) {
        g_on = 0;
        at = -1;
        dump();
    }
}
#else
void sampler_frame(uint64_t frame) { (void)frame; }
#endif
