// Linux host layer (runtime/platform.h). x86-64 and aarch64.
#define _GNU_SOURCE
#include <SDL3/SDL.h>
#include <dlfcn.h>
#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/ucontext.h>
#include <time.h>
#include <unistd.h>

#include "../mods.h"
#include "../diagnostics.h"
#include "../paths.h"
#include "../platform.h"
#include "../rt.h"

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

// ---- guest address space
// Everything readable/writable from the start, like macOS: the guest maps at 4K granularity, and untouched pages
// cost nothing (MAP_NORESERVE: the 12 GB window doesn't count against overcommit).
// The guest heap in huge pages (2 MB, transparent huge pages): the renderer imports the heap as one GPU buffer
// (nvn/vulkan/exec.c gmem_import), and the amdgpu kernel driver walks such an imported range page by page on every
// submission - 3.1 GB in 4 KB pages took 4.3 ms per submission on a Steam Deck, 45% of the render thread. madvise lets
// the kernel assemble huge pages there (by default it only does where asked). ODYSSEY_THP=0: as before.
static void heap_huge_pages(uint64_t lo, uint64_t hi) {
    const char *e = getenv("ODYSSEY_THP");
    if (e && !atoi(e)) return;
    uint64_t a = HEAP_BASE > lo ? HEAP_BASE : lo, b = HEAP_BASE + HEAP_MAX < hi ? HEAP_BASE + HEAP_MAX : hi;
    if (a < b && madvise((void *)(uintptr_t)a, b - a, MADV_HUGEPAGE) != 0)
        fprintf(stderr, "[mem] huge pages for the guest heap not available (%s)\n", strerror(errno));
}

bool plat_reserve_guest(uint64_t lo, uint64_t hi) {
    void *p = mmap((void *)(uintptr_t)lo, hi - lo, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED) return false;
    if (p != (void *)(uintptr_t)lo) { munmap(p, hi - lo); return false; }  // (kernels before 4.17: just a hint)
    heap_huge_pages(lo, hi);
    return true;
}

// Fresh zero pages over the whole window (the old ones are released at once).
bool plat_reset_guest(uint64_t lo, uint64_t hi) {
    bool ok = mmap((void *)(uintptr_t)lo, hi - lo, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED,
                   -1, 0) == (void *)(uintptr_t)lo;
    if (ok) heap_huge_pages(lo, hi);
    return ok;
}

void plat_commit(uint64_t addr, uint64_t size) { (void)addr; (void)size; }

int plat_written_pages(uint64_t addr, uint64_t size, uint64_t *pages, int max, uint64_t *page) {  // (no write tracking)
    (void)addr; (void)size; (void)pages; (void)max; (void)page;
    return -1;
}

int plat_protect(uint64_t addr, uint64_t size, int prot) {
    int p = prot == PLAT_PROT_NONE ? PROT_NONE : prot == PLAT_PROT_READ ? PROT_READ : PROT_READ | PROT_WRITE;
    return mprotect((void *)(uintptr_t)addr, size, p);
}

// ---- time
uint64_t plat_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

void plat_sleep_until_ns(uint64_t t) {
    struct timespec ts = {.tv_sec = (time_t)(t / 1000000000u), .tv_nsec = (long)(t % 1000000000u)};
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {}
}

// Real-time scheduling where the user may have it (rtkit / limits.conf), else just a higher priority: keeps the
// vsync thread on time while the guest threads load every core.
void plat_thread_realtime(uint64_t period_ns) {
    (void)period_ns;
    struct sched_param sp = {.sched_priority = 1};
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0) return;
    setpriority(PRIO_PROCESS, (id_t)gettid(), -10);  // (per thread on Linux; fails quietly without CAP_SYS_NICE)
}

// ---- process
// Started from a desktop there is no terminal: stdout / stderr go to SuperOdysseyRecomp.log in the data folder
// (runtime/paths.h; rt_paths_init runs later, so the same rule is applied here). Processes started by this one (the
// re-exec for a fresh address layout) append to the same log.
static char g_log_path[1024];
const char *plat_log_path(void) { return g_log_path; }

static void log_to_file_without_terminal(void) {
    if (isatty(STDERR_FILENO)) return;
    char dir[900];
    const char *env = getenv("ODYSSEY_DATA_ROOT"), *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    struct stat st;
    if (env && *env) snprintf(dir, sizeof dir, "%s", env);
    else if (stat("extracted", &st) == 0 && S_ISDIR(st.st_mode)) return;  // dev mode: wherever stderr goes already
    // (before the data root is known: the default one, on the internal drive - small, whatever folder the game is in)
    else if (xdg && *xdg == '/') snprintf(dir, sizeof dir, "%s/" RT_LINUX_DIR, xdg);
    else snprintf(dir, sizeof dir, "%s/.local/share/" RT_LINUX_DIR, home && *home ? home : "/tmp");
    for (char *c = dir + 1; *c; c++)
        if (*c == '/') { *c = 0; mkdir(dir, 0755); *c = '/'; }
    mkdir(dir, 0755);
    snprintf(g_log_path, sizeof g_log_path, "%s/SuperOdysseyRecomp.log", dir);
    bool append = getenv("ODYSSEY_LOG_APPEND") != NULL;
    if (!freopen(g_log_path, append ? "a" : "w", stderr)) { g_log_path[0] = 0; return; }
    dup2(STDERR_FILENO, STDOUT_FILENO);
    setenv("ODYSSEY_LOG_APPEND", "1", 1);
}

#if defined(__x86_64__)
// The recompiled game code is built for x86-64-v3 (Makefile GENARCH: AVX2, FMA, BMI2): say so instead of crashing
// with an illegal instruction on an older CPU.
static void require_cpu(void) {
    __builtin_cpu_init();
    if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma") && __builtin_cpu_supports("bmi2")) return;
    fprintf(stderr, "FATAL: Super Odyssey PC Recomp needs a CPU with AVX2 (Intel Haswell / 2013 or newer, AMD Excavator / Zen or "
                    "newer).\n");
    exit(1);
}
#else
static void require_cpu(void) {}
#endif

void plat_init(void) {
    log_to_file_without_terminal();
    require_cpu();
}

// (inside an AppImage: the binary in its mount, which stays mounted while this process lives - re-exec included)
// Steam's game mode sets SteamDeck=1; the board says Valve (Jupiter: LCD, Galileo: OLED)
bool plat_is_steam_deck(void) {
    const char *e = getenv("SteamDeck");
    if (e && atoi(e) == 1) return true;
    char v[64] = "";
    FILE *f = fopen("/sys/class/dmi/id/board_vendor", "r");
    if (f) { if (!fgets(v, sizeof v, f)) v[0] = 0; fclose(f); }
    return !strncmp(v, "Valve", 5);
}

bool plat_exe_path(char *buf, size_t size) {
    ssize_t n = readlink("/proc/self/exe", buf, size - 1);
    if (n <= 0) return false;
    buf[n] = 0;
    return true;
}

// Our own command line, from /proc/self/cmdline (NUL-separated).
static char **self_argv(void) {
    static char buf[32768];
    static char *argv[256];
    int fd = open("/proc/self/cmdline", O_RDONLY);
    if (fd < 0) return NULL;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return NULL;
    buf[n] = 0;
    int k = 0;
    for (char *p = buf; p < buf + n && k < 255; p += strlen(p) + 1) argv[k++] = p;
    argv[k] = NULL;
    return argv;
}

void plat_exec(char *const argv[]) {
    if (argv) { execv(argv[0], argv); return; }
    char path[4096];
    char **av = self_argv();
    if (av && plat_exe_path(path, sizeof path)) execv(path, av);
}

// ---- diagnostics
// Exported symbols only (the Makefile links with -rdynamic); frames in the generated code are left to the caller,
// which maps them to guest functions (runtime/dispatch.c). Static runtime functions come out as the nearest guest
// function, so backtrace lines also give the offset in the executable (addr2line -fe SuperOdysseyRecomp <offset>).
const char *plat_symbol(uint64_t pc, uint64_t *offset) {
    Dl_info di = {0};
    if (!dladdr((void *)(uintptr_t)pc, &di) || !di.dli_sname) return NULL;
    if (offset) *offset = pc - (uint64_t)(uintptr_t)di.dli_saddr;
    return di.dli_sname;
}

// Fatal error without a terminal: say so in a message box rather than vanishing (the details are in the log).
uint64_t plat_private_bytes(void) { return 0; }
uint64_t plat_resident_bytes(void) {
    FILE *f = fopen("/proc/self/statm", "r");
    unsigned long long size = 0, rss = 0;
    if (f) {
        if (fscanf(f, "%llu %llu", &size, &rss) != 2) rss = 0;
        fclose(f);
    }
    return rss * (uint64_t)sysconf(_SC_PAGESIZE);
}
uint64_t plat_total_ram(void) { return (uint64_t)sysconf(_SC_PHYS_PAGES) * (uint64_t)sysconf(_SC_PAGESIZE); }
uint64_t plat_guest_bytes(void) { return 0; }
uint64_t plat_heap_bytes(void) { return 0; }
void plat_heap_histogram(void) {}
void plat_heap_check(const char *where) { (void)where; }

// An error before the game has a window of its own (the first start: no Vulkan driver, say), in a plain window of ours:
// SDL's text, the software renderer. SDL's message box draws its own X11 window without WM_CLASS and with a WM_NAME
// that older tools can't read (the AppImage catalog's test, xdotool 2016, never saw it), and in a Wayland session
// without Xwayland it needs zenity. Closed by Enter / Escape / a click / a controller button / the window's X.
static bool error_window(const char *text) {
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) return false;
    SDL_InitSubSystem(SDL_INIT_GAMEPAD);  // (a Steam Deck: the A button closes it)
    enum { W = 760, SCALE = 2, CH = 8 * SCALE, LINE = 12 * SCALE, MARGIN = 20 };
    const int cols = (W - 2 * MARGIN) / CH;
    // word-wrapped lines (a word longer than a line - a path - is cut where the line ends)
    static char lines[64][128];
    int n = 0, len = 0;
    for (const char *p = text; *p && n < 60;) {
        if (*p == '\n') { lines[n][len] = 0; n++, len = 0; p++; continue; }
        size_t word = strcspn(p, " \n");
        if (len && len + (int)word > cols) { lines[n][len] = 0; n++, len = 0; }
        if (*p == ' ' && !len) { p++; continue; }
        size_t take = word ? word : 1;
        if (take > (size_t)(cols - len)) take = (size_t)(cols - len);
        memcpy(lines[n] + len, p, take);
        len += (int)take, p += take;
        if (len >= cols) { lines[n][len] = 0; n++, len = 0; }
    }
    if (len) lines[n][len] = 0, n++;
    const char *hint = "Enter or a click closes this window.";
    int h = MARGIN * 2 + (n + 2) * LINE;
    SDL_Window *w = SDL_CreateWindow(RT_APP_NAME, W, h, 0);
    if (!w) return false;
    SDL_Renderer *r = SDL_CreateRenderer(w, "software");
    if (!r) r = SDL_CreateRenderer(w, NULL);
    if (!r) { SDL_DestroyWindow(w); return false; }
    SDL_SetRenderScale(r, SCALE, SCALE);
    for (bool done = false; !done;) {
        SDL_SetRenderDrawColor(r, 32, 32, 36, 255);
        SDL_RenderClear(r);
        SDL_SetRenderDrawColor(r, 235, 235, 235, 255);
        for (int i = 0; i < n; i++)
            SDL_RenderDebugText(r, MARGIN / (float)SCALE, (MARGIN + i * LINE) / (float)SCALE, lines[i]);
        SDL_SetRenderDrawColor(r, 150, 150, 160, 255);
        SDL_RenderDebugText(r, MARGIN / (float)SCALE, (MARGIN + (n + 1) * LINE) / (float)SCALE, hint);
        SDL_RenderPresent(r);
        SDL_Event e;
        if (!SDL_WaitEventTimeout(&e, 250)) continue;
        do {
            switch (e.type) {
            case SDL_EVENT_QUIT: case SDL_EVENT_WINDOW_CLOSE_REQUESTED: case SDL_EVENT_MOUSE_BUTTON_UP:
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN: done = true; break;
            case SDL_EVENT_KEY_DOWN:
                if (e.key.key == SDLK_RETURN || e.key.key == SDLK_ESCAPE || e.key.key == SDLK_SPACE) done = true;
                break;
            case SDL_EVENT_GAMEPAD_ADDED: SDL_OpenGamepad(e.gdevice.which); break;
            }
        } while (SDL_PollEvent(&e));
    }
    SDL_DestroyRenderer(r);
    SDL_DestroyWindow(w);
    return true;
}

// An error box that never hangs. Before the game's window: error_window. After it (a crash in play): SDL's message box,
// which tries zenity first and waits for it to end - and zenity can hang without ever showing a window (seen on
// SteamOS's desktop). With zenity out of PATH, SDL draws its own X11 box; only if that fails in a Wayland session (no
// Xwayland) is zenity tried. Only called just before the process ends, so the changed PATH doesn't matter.
void plat_error_box(const char *text) {
    int windows = 0;
    SDL_Window **list = SDL_WasInit(SDL_INIT_VIDEO) ? SDL_GetWindows(&windows) : NULL;
    SDL_free(list);
    if (!windows && error_window(text)) return;
    const char *path = getenv("PATH");
    char *saved = path ? strdup(path) : NULL;
    setenv("PATH", "", 1);
    bool shown = SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, RT_APP_NAME, text, NULL);
    if (saved) setenv("PATH", saved, 1);
    free(saved);
    if (!shown && getenv("WAYLAND_DISPLAY"))  // (no display at all: zenity would hang too)
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, RT_APP_NAME, text, NULL);
}

// Fatal error (rt_fatal_trace): the last FATAL / exception line of the log in a message box (as on Windows)
void plat_fatal_notice(void) {
    if (!g_log_path[0]) return;
    fflush(stderr);
    char text[2900] = "Super Odyssey PC Recomp stopped because of an error.";
    FILE *f = fopen(g_log_path, "rb");
    if (f) {
        static char tail[16384];
        fseek(f, 0, SEEK_END);
        long sz = ftell(f), from = sz > (long)sizeof tail - 1 ? sz - (long)sizeof tail + 1 : 0;
        fseek(f, from, SEEK_SET);
        size_t got = fread(tail, 1, sizeof tail - 1, f);
        tail[got] = 0;
        fclose(f);
        char *line = NULL;
        for (char *p = tail; (p = strstr(p, "FATAL: ")) != NULL; p++) line = p;
        if (!line) for (char *p = tail; (p = strstr(p, "*** ")) != NULL; p++) line = p;
        if (line) {
            char *end = strchr(line, '\n');
            if (end) *end = 0;
            snprintf(text, sizeof text, "Super Odyssey PC Recomp stopped because of an error:\n\n%.600s", line);
            // (VK_ERROR_OUT_OF_HOST_MEMORY -1 / VK_ERROR_OUT_OF_DEVICE_MEMORY -2: not a driver that is too old)
            if (strstr(line, "failed: -2 (") || strstr(line, "failed: -1 (")) {
                size_t l = strlen(text);
                snprintf(text + l, sizeof text - l, "\n\nThe graphics card ran out of memory. Lower the internal resolution "
                         "(menu > Graphics), or delete powermoon.ini in the game's folder to go back to the default settings.");
            } else if (strstr(line, "Vulkan") || strstr(line, "VK_") || strstr(line, "vk")) {
                size_t l = strlen(text);
                snprintf(text + l, sizeof text - l, "\n\nSuper Odyssey PC Recomp needs a graphics driver with Vulkan 1.3: "
                         "install your distribution's Mesa or NVIDIA driver (on a Steam Deck it is already there).");
            }
        }
    }
    size_t l = strlen(text);
    if (mods_applied())
        l += (size_t)snprintf(text + l, sizeof text - l, "\n\nMods are on. If this started with a new mod, move it out of "
                              "the mods folder: mods made for a later version of the game (1.3.0) can crash 1.0.0.");
    // The crash report (runtime/diagnostics.c) is saved before the box shows. Not diagnostics_fatal_dialog here: its
    // SDL message box with buttons goes through zenity, which can hang without a window (plat_error_box).
    char report[1200];
    if (diagnostics_capture_crash(report, sizeof report))
        snprintf(text + l, sizeof text - l, "\n\nCrash report (attach it to a bug report): %s", report);
    else snprintf(text + l, sizeof text - l, "\n\nDetails: %s", g_log_path);
    plat_error_box(text);
}

const char *rt_host_fn_name(uint64_t pc, uint64_t *offset);  // runtime/dispatch.c

void plat_print_backtrace(void) {
    void *bt[64];
    int n = backtrace(bt, 64);
    Dl_info self = {0};
    dladdr((void *)plat_print_backtrace, &self);
    for (int i = 0; i < n; i++) {
        uint64_t pc = (uint64_t)(uintptr_t)bt[i], off = 0;
        Dl_info di = {0};
        if (!dladdr(bt[i], &di)) di.dli_fname = NULL;
        bool ours = di.dli_fbase && di.dli_fbase == self.dli_fbase;
        const char *s = plat_symbol(pc, &off);
        if (!s && ours) s = rt_host_fn_name(pc, &off);
        const char *mod = di.dli_fname ? (strrchr(di.dli_fname, '/') ? strrchr(di.dli_fname, '/') + 1 : di.dli_fname) : "?";
        uint64_t moff = pc - (uint64_t)(uintptr_t)di.dli_fbase;
        if (s && ours) fprintf(stderr, "  #%-2d %016llx %s+%llx (SuperOdysseyRecomp+%llx)\n", i, (unsigned long long)pc, s,
                               (unsigned long long)off, (unsigned long long)moff);
        else if (s) fprintf(stderr, "  #%-2d %016llx %s+%llx (%s)\n", i, (unsigned long long)pc, s, (unsigned long long)off, mod);
        else fprintf(stderr, "  #%-2d %016llx %s+%llx\n", i, (unsigned long long)pc, mod, (unsigned long long)moff);
    }
}

static void crash_handler(int sig, siginfo_t *si, void *ctx) {
    ucontext_t *uc = ctx;
    static volatile int depth;
    if (sig == SIGABRT && depth++ > 0) { signal(SIGABRT, SIG_DFL); abort(); }  // (nested: just die)
    uint64_t fa = (uint64_t)(uintptr_t)si->si_addr;
#if defined(__aarch64__)
    uint64_t pc = uc->uc_mcontext.pc, lr = uc->uc_mcontext.regs[30];
#else
    // x86-64 has no link register: report the return address on top of the stack (valid at function entry)
    uint64_t pc = (uint64_t)uc->uc_mcontext.gregs[REG_RIP], sp = (uint64_t)uc->uc_mcontext.gregs[REG_RSP], lr = 0;
    if (sig != SIGSEGV || (fa & ~0xFFFull) != (sp & ~0xFFFull)) lr = *(uint64_t *)(uintptr_t)sp;
#endif
    if (rt_fault_watch(fa, pc)) return;
    char what[32];
    snprintf(what, sizeof what, "signal %d", sig);
    rt_fault_report(what, fa, pc, lr);
}

void plat_crash_handler_install(void) {
    stack_t ss = {.ss_sp = malloc(1 << 20), .ss_size = 1 << 20};
    sigaltstack(&ss, NULL);
    struct sigaction sa = {0};
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
}

// ---- child processes
#include <ftw.h>
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;

PlatProc plat_spawn(const char *const argv[], const char *log_path, bool append_log) {
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, log_path ? log_path : "/dev/null",
                                     O_WRONLY | O_CREAT | (append_log ? O_APPEND : O_TRUNC), 0644);
    posix_spawn_file_actions_adddup2(&fa, 1, 2);
    posix_spawnattr_t at;  // (its own process group: plat_proc_kill ends what it starts, e.g. a compiler's own child)
    posix_spawnattr_init(&at);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&at, 0);
    pid_t pid;
    int rc = posix_spawn(&pid, argv[0], &fa, &at, (char *const *)argv, environ);
    posix_spawnattr_destroy(&at);
    posix_spawn_file_actions_destroy(&fa);
    return rc ? 0 : (PlatProc)pid;
}

void plat_proc_kill(PlatProc p) {
    kill(-(pid_t)p, SIGKILL);
    waitpid((pid_t)p, NULL, 0);
}

int plat_proc_wait(PlatProc p, bool block) {
    int st = 0;
    pid_t r = waitpid((pid_t)p, &st, block ? 0 : WNOHANG);
    if (r == 0) return -1;
    if (r < 0) return 255;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 255;
}

static int rm_entry(const char *path, const struct stat *sb, int flag, struct FTW *ftw) {
    (void)sb, (void)flag, (void)ftw;
    return remove(path);
}
bool plat_remove_tree(const char *dir) { return nftw(dir, rm_entry, 16, FTW_DEPTH | FTW_PHYS) == 0; }

// End the process at once, skipping the library teardown: as on Windows, the GPU driver's threads crashed in exit()
// while the game's threads still submitted work.
void plat_terminate(void) { _exit(0); }
