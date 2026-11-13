#include "diagnostics.h"
#include <SDL3/SDL.h>
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#if defined(__i386__) || defined(__x86_64__)
#include <cpuid.h>
#endif
#ifdef _WIN32
#include <windows.h>
#include <process.h>
#endif
#include "paths.h"
#include "platform.h"
#include "rt.h"
#include "settings.h"
#include "version.h"

static char gpu[1024], fault[256];
static atomic_bool gpu_ready, fault_ready;
static atomic_flag fault_claimed = ATOMIC_FLAG_INIT;
static atomic_uint sequence;
static atomic_int copy_status = -1;
int diagnostics_copy_status(void) { return atomic_load(&copy_status); }
static char build[128] = "unavailable";

void diagnostics_init(void) {
    // A small streaming fingerprint identifies private builds sharing a version
    // number. Cache it at startup; do not open the binary from a crash handler.
    char path[1024], bytes[4096];
    if (!plat_exe_path(path, sizeof path)) return;
    FILE *file = fopen(path, "rb");
    if (!file) return;
    uint64_t hash = 14695981039346656037ull, size = 0;
    size_t got;
    while ((got = fread(bytes, 1, sizeof bytes, file))) {
        size += got;
        for (size_t i = 0; i < got; i++) { hash ^= (unsigned char)bytes[i]; hash *= 1099511628211ull; }
    }
    if (!ferror(file)) snprintf(build, sizeof build, "fnv1a64:%016llx (%llu bytes)",
        (unsigned long long)hash, (unsigned long long)size);
    fclose(file);
}

void diagnostics_gpu(const char *description) {
    snprintf(gpu, sizeof gpu, "%s", description);
    atomic_store(&gpu_ready, true);
}

void diagnostics_fault(const char *what, uint64_t address, uint64_t pc, uint64_t lr) {
    if (atomic_flag_test_and_set(&fault_claimed)) return;
    snprintf(fault, sizeof fault, "%s, address %#llx, host PC %#llx, host LR %#llx",
             what, (unsigned long long)address, (unsigned long long)pc, (unsigned long long)lr);
    atomic_store(&fault_ready, true);
}

static void cpu_description(char text[128]) {
    snprintf(text, 128, "unavailable");
#if defined(__i386__) || defined(__x86_64__)
    if (__get_cpuid_max(0x80000000, NULL) >= 0x80000004) {
        unsigned words[12];
        for (unsigned i = 0; i < 3; i++)
            __get_cpuid(0x80000002 + i, &words[4*i], &words[4*i+1], &words[4*i+2], &words[4*i+3]);
        memcpy(text, words, sizeof words);
        text[sizeof words] = 0;
    }
#elif defined(__linux__)
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof line, f)) {
            if (strncmp(line, "model name", 10) && strncmp(line, "Hardware", 8)) continue;
            char *value = strchr(line, ':');
            if (value) { snprintf(text, 128, "%s", value + 1); break; }
        }
        fclose(f);
    }
#endif
    text[strcspn(text, "\r\n")] = 0;
}

// Preserve startup hardware information and the most recent fault/call chain,
// while bounding a verbose log. Never enumerate directories or attach assets.
static void append_file(FILE *out, const char *title, const char *path, long limit, bool log) {
    fprintf(out, "\n--- %s ---\n", title);
    if (!path || !*path) { fprintf(out, "Unavailable (output may be in a terminal).\n"); return; }
    fprintf(out, "Path: %s\n", path);
    FILE *in = fopen(path, "rb");
    if (!in) { fprintf(out, "Unavailable: %s\n", strerror(errno)); return; }
    if (fseek(in, 0, SEEK_END) || ftell(in) < 0) { fclose(in); fprintf(out, "Cannot read file length.\n"); return; }
    long size = ftell(in), head = size > limit && log ? 65536 : 0;
    fseek(in, 0, SEEK_SET);
    char bytes[4096];
    long left = head ? head : size > limit ? limit : size;
    for (;;) {
        while (left > 0) {
            size_t got = fread(bytes, 1, left < (long)sizeof bytes ? (size_t)left : sizeof bytes, in);
            if (!got) break;
            fwrite(bytes, 1, got, out);
            left -= (long)got;
        }
        if (!head) break;
        fprintf(out, "\n[Middle of log omitted; original file is %ld bytes.]\n", size);
        head = 0;
        fseek(in, size - limit, SEEK_SET);
        left = limit;
    }
    if (size > limit && !log) fprintf(out, "\n[Settings truncated at %ld bytes.]\n", limit);
    fclose(in);
}

static bool report_directory(char directory[1024]) {
    char fallback[1024];
    const char *root = NULL, *log = plat_log_path();
    if (log && *log) snprintf(fallback, sizeof fallback, "%s", log);
    else if (rt_data_root() && *rt_data_root()) root = rt_data_root();
    else if (!plat_exe_path(fallback, sizeof fallback)) return false;
    if (!root || !*root) {
        char *a = strrchr(fallback, '/'), *b = strrchr(fallback, '\\');
        char *last = !a ? b : !b ? a : a > b ? a : b;
        if (!last) return false;
        *last = 0;
        root = fallback;
    }
    return snprintf(directory, 1024, "%s/reports", root) < 1024;
}

bool diagnostics_write_report(char *path, size_t capacity) {
    if (!capacity) return false;
    path[0] = 0;
    char directory[1024];
    if (!report_directory(directory)) return false;
    if (mkdir(directory, 0755) && errno != EEXIST) return false;
    time_t now = time(NULL);
    struct tm utc;
#ifdef _WIN32
    gmtime_s(&utc, &now);
    unsigned pid = (unsigned)_getpid();
#else
    gmtime_r(&now, &utc);
    unsigned pid = (unsigned)getpid();
#endif
    char date[32];
    strftime(date, sizeof date, "%Y%m%d-%H%M%S", &utc);
    int len = snprintf(path, capacity, "%s/report-%s-%u-%u.txt", directory, date, pid, atomic_fetch_add(&sequence, 1));
    if (len < 0 || (size_t)len >= capacity) { path[0] = 0; return false; }
    int flags = O_CREAT | O_EXCL | O_WRONLY;
#ifdef _WIN32
    flags |= O_BINARY;
#endif
    int fd = open(path, flags, 0600);
    if (fd < 0) { path[0] = 0; return false; }
    FILE *out = fdopen(fd, "wb");
    if (!out) { close(fd); remove(path); path[0] = 0; return false; }
    char cpu[128];
    cpu_description(cpu);
    char os[128];
    snprintf(os, sizeof os, "%s", SDL_GetPlatform());
#ifdef _WIN32
    LONG (WINAPI *version)(OSVERSIONINFOW *) = (void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    OSVERSIONINFOW v = {.dwOSVersionInfoSize = sizeof v};
    if (version && !version(&v)) snprintf(os, sizeof os, "Windows %lu.%lu build %lu", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
#endif
    fprintf(out, "Super Odyssey PC Recomp diagnostic report (format 1)\nApp: %s\nExecutable fingerprint: %s\nUTC: %s\nOS: %s\n"
                 "CPU: %s\nLogical processors: %d\nRAM: %llu MiB\nGPU / driver: %s\n"
                 "Fault: %s\n", APP_VERSION, build, date, os, cpu, SDL_GetNumLogicalCPUCores(),
                 (unsigned long long)(plat_total_ram() >> 20), atomic_load(&gpu_ready) ? gpu : "unavailable (see log)",
                 atomic_load(&fault_ready) ? fault : "No host exception recorded; see log for fatal errors and guest call chain.");
    Module *main = loader_module("main");
    if (main) {
        fprintf(out, "Game main build ID: ");
        for (unsigned i = 0; i < sizeof main->build_id; i++) fprintf(out, "%02x", main->build_id[i]);
        fprintf(out, "\n");
    } else fprintf(out, "Game main build ID: not loaded\n");
    fprintf(out, "Contains saved settings and a bounded log, copied as-is and including local paths. No process memory dump or additional game/key/save files are collected.\n");
    fflush(stderr);
    append_file(out, "Saved settings", settings_current_file(), 65536, false);
    append_file(out, "Log (startup and most recent entries)", plat_log_path(), 1024 * 1024, true);
    bool ok = !ferror(out);
    if (fclose(out)) ok = false;
    if (!ok) { remove(path); path[0] = 0; }
    return ok;
}

bool diagnostics_capture_crash(char *path, size_t capacity) {
    if (!diagnostics_write_report(path, capacity)) return false;
    char directory[1024], marker[1100];
    if (report_directory(directory)) {
        snprintf(marker, sizeof marker, "%s/last-crash.txt", directory);
        FILE *out = fopen(marker, "wb");
        if (out) { fprintf(out, "%s\n", strrchr(path, '/') + 1); fclose(out); }
    }
    return true;
}

bool diagnostics_last_crash(char *path, size_t capacity) {
    if (!capacity) return false;
    path[0] = 0;
    char directory[1024], marker[1100], name[160];
    if (!report_directory(directory)) return false;
    snprintf(marker, sizeof marker, "%s/last-crash.txt", directory);
    FILE *in = fopen(marker, "rb");
    if (!in) return false;
    bool read = fgets(name, sizeof name, in) != NULL;
    fclose(in);
    if (!read) return false;
    name[strcspn(name, "\r\n")] = 0;
    size_t n = strlen(name);
    if (n < 11 || strncmp(name, "report-", 7) || strcmp(name + n - 4, ".txt") || strpbrk(name, "/\\:")) return false;
    int len = snprintf(path, capacity, "%s/%s", directory, name);
    struct stat st;
    if (len < 0 || (size_t)len >= capacity || stat(path, &st) || !S_ISREG(st.st_mode)) { path[0] = 0; return false; }
    return true;
}

#ifndef _WIN32
typedef struct { char *path; unsigned id; } ClipboardRequest;
static atomic_uint copy_sequence;
static void SDLCALL copy_on_event_thread(void *data) {
    ClipboardRequest *request = data;
    if (atomic_load(&copy_sequence) == request->id) {
        bool copied = SDL_SetClipboardText(request->path);
        if (atomic_load(&copy_sequence) == request->id) atomic_store(&copy_status, copied ? 1 : -1);
    }
    SDL_free(request->path);
    SDL_free(request);
}
#endif

bool diagnostics_copy_path(const char *path) {
    atomic_store(&copy_status, -1);
#ifdef _WIN32
    // Match the native filename encoding used by this runtime's fopen/open.
    int count = MultiByteToWideChar(CP_ACP, 0, path, -1, NULL, 0);
    if (!count) return false;
    HGLOBAL data = GlobalAlloc(GMEM_MOVEABLE, (size_t)count * sizeof(wchar_t));
    if (!data) return false;
    wchar_t *text = GlobalLock(data);
    if (!text) { GlobalFree(data); return false; }
    MultiByteToWideChar(CP_ACP, 0, path, -1, text, count);
    GlobalUnlock(data);
    bool ok = false;
    // EmptyClipboard needs a real owner for SetClipboardData. A message-only
    // window also works before SDL creates its game window after an early fault.
    HWND owner = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, GetModuleHandleW(NULL), NULL);
    if (owner && OpenClipboard(owner)) {
        if (EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, data)) ok = true;
        CloseClipboard();
    }
    if (owner) DestroyWindow(owner);
    if (!ok) GlobalFree(data);
    atomic_store(&copy_status, ok ? 1 : -1);
    return ok;
#else
    // SDL clipboard APIs are main-thread-only. Do not wait while the renderer
    // owns its lock: the event thread might itself be waiting for rendering.
    ClipboardRequest *request = SDL_malloc(sizeof *request);
    if (!request) return false;
    request->path = SDL_strdup(path);
    if (!request->path) { SDL_free(request); return false; }
    request->id = atomic_fetch_add(&copy_sequence, 1) + 1;
    atomic_store(&copy_status, 0);
    if (SDL_RunOnMainThread(copy_on_event_thread, request, false)) return true;
    SDL_free(request->path); SDL_free(request);
    atomic_store(&copy_status, -1);
    return false;
#endif
}

void diagnostics_fatal_dialog(const char *message) {
    // Save before displaying the dialog: closing it or restarting the game must
    // not erase the only fault log. Export remains best-effort after corruption.
    char path[1200];
    bool saved = diagnostics_capture_crash(path, sizeof path);
    const SDL_MessageBoxButtonData buttons[] = {
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT | SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Close"},
        {0, 1, "Copy crash report"}};
    SDL_MessageBoxData box = {.flags = SDL_MESSAGEBOX_ERROR, .title = "Super Odyssey PC Recomp",
        .message = message, .numbuttons = 2, .buttons = buttons};
    int selected = 0;
    if (!SDL_ShowMessageBox(&box, &selected)) {
        fprintf(stderr, "[report] message box unavailable: %s\n", SDL_GetError());
#ifdef _WIN32
        MessageBoxA(NULL, message, box.title, MB_ICONERROR | MB_OK);
#else
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, box.title, message, NULL);
#endif
        return;
    }
    if (selected == 1) {
        char text[1600];
        if (saved || diagnostics_capture_crash(path, sizeof path)) {
            bool copied = diagnostics_copy_path(path) && diagnostics_copy_status() == 1;
            snprintf(text, sizeof text, "Report saved:\n%s\n\n%s", path,
                     copied ? "Its path is copied. Paste it into the file picker to attach the report." :
                              "Copy this path into the file picker to attach the report.");
        } else snprintf(text, sizeof text, "Could not create a report. The original log remains at:\n%s", plat_log_path());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, box.title, text, NULL);
    }
}
