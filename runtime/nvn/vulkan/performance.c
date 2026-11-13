// The performance overlay's numbers (runtime/ui/performance.h): CPU / GPU load, temperatures, power, memory. Sampled
// once a second on a thread of its own, and only while the overlay was drawn in the last 3 seconds.
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL_cpuinfo.h>
#include <SDL3/SDL_thread.h>
#include <SDL3/SDL_timer.h>
#include "../../platform.h"
#include "../../ui/performance.h"
#include "vk.h"

static _Atomic unsigned vram_mib, vram_budget_mib, res[4];

void vk_overlay_memory(uint64_t used, uint64_t budget) {
    atomic_store(&vram_mib, (unsigned)(used >> 20));
    atomic_store(&vram_budget_mib, (unsigned)(budget >> 20));
}

void vk_overlay_resolution(unsigned render_w, unsigned render_h, unsigned out_w, unsigned out_h) {
    atomic_store(&res[0], render_w), atomic_store(&res[1], render_h);
    atomic_store(&res[2], out_w), atomic_store(&res[3], out_h);
}

static char cpu_name[128];
static bool started;
static _Atomic int cpu_usage = -1, gpu_usage = -1, game_cpu = -1, gpu_temp = -1, gpu_power = -1, gpu_clock = -1, cpu_temp = -1;
static _Atomic unsigned ram_used_mib, ram_total_mib, game_ram_mib;
static _Atomic uint64_t last_visible;

// ---- NVIDIA: NVML, the driver's own library (nvml.dll / libnvidia-ml.so.1), loaded if it is there
#ifdef _WIN32
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <wchar.h>
static void *lib_open(const char *name) { return LoadLibraryExA(name, NULL, LOAD_LIBRARY_SEARCH_SYSTEM32); }
static void *lib_sym(void *lib, const char *name) { return (void *)GetProcAddress(lib, name); }
#define NVML_LIB "nvml.dll"
#else
#include <dirent.h>
#include <dlfcn.h>
#include <time.h>
#include <unistd.h>
static void *lib_open(const char *name) { return dlopen(name, RTLD_NOW | RTLD_LOCAL); }
static void *lib_sym(void *lib, const char *name) { return dlsym(lib, name); }
#define NVML_LIB "libnvidia-ml.so.1"
#endif

static struct {
    void *device;
    int (*temperature)(void *, int, unsigned *);
    int (*power)(void *, unsigned *);
    int (*clock)(void *, int, unsigned *);
    int (*rates)(void *, unsigned[2]);
} nvml;

static void nvml_open(void) {
    if (g_vk.props.vendorID != 0x10de) return;
    void *lib = lib_open(NVML_LIB);
    if (!lib) return;
    int (*init)(void) = lib_sym(lib, "nvmlInit_v2");
    int (*count_fn)(unsigned *) = lib_sym(lib, "nvmlDeviceGetCount_v2");
    int (*by_index)(unsigned, void **) = lib_sym(lib, "nvmlDeviceGetHandleByIndex_v2");
    int (*name_fn)(void *, char *, unsigned) = lib_sym(lib, "nvmlDeviceGetName");
    unsigned count = 0;
    if (!init || !count_fn || !by_index || init() || count_fn(&count) || !count) return;
    void *device = NULL;
    for (unsigned i = 0; i < count; i++) {  // (several NVIDIA cards: the one Vulkan renders on, by name)
        void *d = NULL;
        char name[96] = "";
        if (by_index(i, &d) || !d) continue;
        if (!device) device = d;
        if (name_fn && !name_fn(d, name, sizeof name) && strstr(g_vk.props.deviceName, name)) { device = d; break; }
    }
    if (!device) return;
    nvml.temperature = lib_sym(lib, "nvmlDeviceGetTemperature");
    nvml.power = lib_sym(lib, "nvmlDeviceGetPowerUsage");
    nvml.clock = lib_sym(lib, "nvmlDeviceGetClockInfo");
    nvml.rates = lib_sym(lib, "nvmlDeviceGetUtilizationRates");
    nvml.device = device;
}

static void nvml_sample(void) {
    if (!nvml.device) return;
    unsigned v = 0, r[2] = {0, 0};
    if (nvml.temperature && !nvml.temperature(nvml.device, 0, &v)) atomic_store(&gpu_temp, (int)v);
    if (nvml.power && !nvml.power(nvml.device, &v)) atomic_store(&gpu_power, (int)((v + 500) / 1000));  // (milliwatts)
    if (nvml.clock && !nvml.clock(nvml.device, 0, &v)) atomic_store(&gpu_clock, (int)v);
#ifndef _WIN32
    if (nvml.rates && !nvml.rates(nvml.device, r)) atomic_store(&gpu_usage, (int)r[0]);
#else
    (void)r;
#endif
}

static void game_sample(uint64_t cpu_ns, uint64_t now_ns) {  // this process's CPU time so far
    static uint64_t old_cpu, old_now;
    int cores = SDL_GetNumLogicalCPUCores();
    if (old_now && now_ns > old_now && cores > 0)
        atomic_store(&game_cpu, (int)((double)(cpu_ns - old_cpu) * 100.0 / ((double)(now_ns - old_now) * cores) + 0.5));
    old_cpu = cpu_ns, old_now = now_ns;
    atomic_store(&game_ram_mib, (unsigned)(plat_resident_bytes() >> 20));
}

#ifdef _WIN32
static LUID adapter;
static bool adapter_valid;

static uint64_t file_time(FILETIME t) {
    return (uint64_t)t.dwHighDateTime << 32 | t.dwLowDateTime;
}

static int gpu_load(PDH_HCOUNTER counter) {
    DWORD bytes = 0, count = 0;
    if (!counter || PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bytes, &count, NULL) != PDH_MORE_DATA)
        return -1;
    PDH_FMT_COUNTERVALUE_ITEM_W *items = malloc(bytes);
    if (!items) return -1;
    double engines[8][64] = {{0}};
    int result = -1;
    if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bytes, &count, items) == ERROR_SUCCESS) {
        for (DWORD i = 0; i < count; i++) {
            unsigned long high, low;
            unsigned node, engine;
            if (swscanf(items[i].szName, L"pid_%*u_luid_0x%lx_0x%lx_phys_%u_eng_%u", &high, &low, &node, &engine) != 4 ||
                high != (unsigned long)adapter.HighPart || low != adapter.LowPart || node >= 8 || engine >= 64)
                continue;
            PDH_FMT_COUNTERVALUE value = items[i].FmtValue;
            if (value.CStatus != PDH_CSTATUS_VALID_DATA && value.CStatus != PDH_CSTATUS_NEW_DATA) continue;
            double sum = (engines[node][engine] += value.doubleValue);
            if (sum > result) result = sum >= 100 ? 100 : (int)(sum + 0.5);
        }
    }
    free(items);
    return result;
}

static int sample_hardware(void *unused) {
    (void)unused;
    PDH_HQUERY query = NULL;
    PDH_HCOUNTER counter = NULL;
    if (adapter_valid && PdhOpenQueryW(NULL, 0, &query) == ERROR_SUCCESS)
        PdhAddEnglishCounterW(query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &counter);
    nvml_open();
    uint64_t old_idle = 0, old_total = 0;
    for (;;) {
        if (SDL_GetTicks() - atomic_load(&last_visible) < 3000) {
            FILETIME idle, kernel, user, made, gone;
            if (GetSystemTimes(&idle, &kernel, &user)) {
                uint64_t idle_ns = file_time(idle), total = file_time(kernel) + file_time(user);
                if (old_total && total > old_total) {
                    uint64_t elapsed = total - old_total, idle_delta = idle_ns - old_idle;
                    atomic_store(&cpu_usage, idle_delta >= elapsed ? 0 : (int)(100 * (elapsed - idle_delta) / elapsed));
                }
                old_idle = idle_ns;
                old_total = total;
            }
            if (GetProcessTimes(GetCurrentProcess(), &made, &gone, &kernel, &user))
                game_sample((file_time(kernel) + file_time(user)) * 100, SDL_GetTicksNS());
            MEMORYSTATUSEX mem = {.dwLength = sizeof mem};
            if (GlobalMemoryStatusEx(&mem)) {
                atomic_store(&ram_total_mib, (unsigned)(mem.ullTotalPhys >> 20));
                atomic_store(&ram_used_mib, (unsigned)((mem.ullTotalPhys - mem.ullAvailPhys) >> 20));
            }
            if (counter && PdhCollectQueryData(query) == ERROR_SUCCESS) atomic_store(&gpu_usage, gpu_load(counter));
            nvml_sample();
        } else {
            old_total = 0;
        }
        SDL_Delay(1000);
    }
}

static void start_hardware_sampler(void) {
    DWORD size = sizeof cpu_name;
    RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "ProcessorNameString",
                 RRF_RT_REG_SZ | RRF_ZEROONFAILURE, NULL, cpu_name, &size);
    VkPhysicalDeviceIDProperties id = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 props = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &id};
    vkGetPhysicalDeviceProperties2(g_vk.phys, &props);
    adapter_valid = id.deviceLUIDValid;
    if (adapter_valid) memcpy(&adapter, id.deviceLUID, sizeof adapter);
    SDL_Thread *thread = SDL_CreateThread(sample_hardware, "Hardware overlay", NULL);
    if (thread) SDL_DetachThread(thread);
}

#else  // ---- Linux: /proc, and the kernel's hwmon / drm files (AMD and Intel GPUs, the Steam Deck)

static long long file_number(const char *path) {
    FILE *f = fopen(path, "r");
    long long v = -1;
    if (f) { if (fscanf(f, "%lld", &v) != 1) v = -1; fclose(f); }
    return v;
}

static char gpu_dir[300], gpu_hwmon[400], cpu_hwmon[300];  // "" where there is none

// The first hwmon<N> folder in dir (a device's own "hwmon" folder)
static void first_hwmon(const char *dir, char *out, size_t n) {
    DIR *d = opendir(dir);
    out[0] = 0;
    if (!d) return;
    for (struct dirent *e; (e = readdir(d));)
        if (!strncmp(e->d_name, "hwmon", 5)) { snprintf(out, n, "%s/%s", dir, e->d_name); break; }
    closedir(d);
}

static void find_sensors(void) {
    for (int card = 0; card < 8 && !gpu_dir[0]; card++) {  // the drm card Vulkan renders on, by PCI vendor and device
        char path[340], dir[300];
        snprintf(dir, sizeof dir, "/sys/class/drm/card%d/device", card);
        snprintf(path, sizeof path, "%s/vendor", dir);
        FILE *f = fopen(path, "r");
        unsigned vendor = 0, device = 0;
        if (!f) continue;
        if (fscanf(f, "%x", &vendor) != 1) vendor = 0;
        fclose(f);
        snprintf(path, sizeof path, "%s/device", dir);
        if ((f = fopen(path, "r"))) { if (fscanf(f, "%x", &device) != 1) device = 0; fclose(f); }
        if (vendor != g_vk.props.vendorID || device != g_vk.props.deviceID) continue;
        snprintf(gpu_dir, sizeof gpu_dir, "%s", dir);
        snprintf(path, sizeof path, "%s/hwmon", dir);
        first_hwmon(path, gpu_hwmon, sizeof gpu_hwmon);
    }
    for (int i = 0; i < 32 && !cpu_hwmon[0]; i++) {  // the CPU's own temperature sensor
        char path[300], name[64] = "";
        snprintf(path, sizeof path, "/sys/class/hwmon/hwmon%d/name", i);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        if (!fgets(name, sizeof name, f)) name[0] = 0;
        fclose(f);
        if (!strncmp(name, "k10temp", 7) || !strncmp(name, "coretemp", 8) || !strncmp(name, "zenpower", 8))
            snprintf(cpu_hwmon, sizeof cpu_hwmon, "/sys/class/hwmon/hwmon%d", i);
    }
}

static int sample_hardware(void *unused) {
    (void)unused;
    find_sensors();
    nvml_open();
    unsigned long long old_idle = 0, old_total = 0;
    for (;;) {
        if (SDL_GetTicks() - atomic_load(&last_visible) < 3000) {
            FILE *f = fopen("/proc/stat", "r");
            unsigned long long v[8] = {0};
            if (f) {
                if (fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6],
                           &v[7]) == 8) {
                    unsigned long long idle = v[3] + v[4], total = 0;
                    for (int i = 0; i < 8; i++) total += v[i];
                    if (old_total && total > old_total)
                        atomic_store(&cpu_usage, (int)(100 * ((total - old_total) - (idle - old_idle)) / (total - old_total)));
                    old_idle = idle, old_total = total;
                }
                fclose(f);
            }
            struct timespec ts;
            if (!clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts))
                game_sample((uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec, SDL_GetTicksNS());
            if ((f = fopen("/proc/meminfo", "r"))) {
                char line[128];
                unsigned long long total = 0, avail = 0;
                while (fgets(line, sizeof line, f)) {
                    sscanf(line, "MemTotal: %llu", &total);
                    sscanf(line, "MemAvailable: %llu", &avail);
                }
                fclose(f);
                atomic_store(&ram_total_mib, (unsigned)(total >> 10));
                atomic_store(&ram_used_mib, (unsigned)((total - avail) >> 10));
            }
            char path[460];
            long long n;
            if (gpu_dir[0]) {
                snprintf(path, sizeof path, "%s/gpu_busy_percent", gpu_dir);
                if ((n = file_number(path)) >= 0) atomic_store(&gpu_usage, (int)n);
            }
            if (gpu_hwmon[0]) {
                snprintf(path, sizeof path, "%s/temp1_input", gpu_hwmon);
                if ((n = file_number(path)) >= 0) atomic_store(&gpu_temp, (int)(n / 1000));
                snprintf(path, sizeof path, "%s/power1_average", gpu_hwmon);
                if ((n = file_number(path)) < 0) { snprintf(path, sizeof path, "%s/power1_input", gpu_hwmon); n = file_number(path); }
                if (n >= 0) atomic_store(&gpu_power, (int)((n + 500000) / 1000000));
                snprintf(path, sizeof path, "%s/freq1_input", gpu_hwmon);
                if ((n = file_number(path)) >= 0) atomic_store(&gpu_clock, (int)(n / 1000000));
            }
            if (cpu_hwmon[0]) {
                snprintf(path, sizeof path, "%s/temp1_input", cpu_hwmon);
                if ((n = file_number(path)) >= 0) atomic_store(&cpu_temp, (int)(n / 1000));
            }
            nvml_sample();
        } else {
            old_total = 0;
        }
        SDL_Delay(1000);
    }
}

static void start_hardware_sampler(void) {
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f))
            if (!strncmp(line, "model name", 10) && strchr(line, ':')) {
                snprintf(cpu_name, sizeof cpu_name, "%s", strchr(line, ':') + 2);
                cpu_name[strcspn(cpu_name, "\n")] = 0;
                break;
            }
        fclose(f);
    }
    SDL_Thread *thread = SDL_CreateThread(sample_hardware, "Hardware overlay", NULL);
    if (thread) SDL_DetachThread(thread);
}
#endif

VulkanOverlayStats vk_overlay_stats(void) {
    atomic_store(&last_visible, SDL_GetTicks());
    if (!started) { started = true; start_hardware_sampler(); }
    VulkanOverlayStats s = {
        .cpu_name = cpu_name, .gpu_name = g_vk.props.deviceName,
        .vram_mib = atomic_load(&vram_mib), .vram_budget_mib = atomic_load(&vram_budget_mib),
        .cpu_usage = atomic_load(&cpu_usage), .gpu_usage = atomic_load(&gpu_usage), .game_cpu_usage = atomic_load(&game_cpu),
        .gpu_temp_c = atomic_load(&gpu_temp), .gpu_power_w = atomic_load(&gpu_power), .gpu_clock_mhz = atomic_load(&gpu_clock),
        .cpu_temp_c = atomic_load(&cpu_temp),
        .ram_used_mib = atomic_load(&ram_used_mib), .ram_total_mib = atomic_load(&ram_total_mib),
        .game_ram_mib = atomic_load(&game_ram_mib),
        .render_w = atomic_load(&res[0]), .render_h = atomic_load(&res[1]), .out_w = atomic_load(&res[2]), .out_h = atomic_load(&res[3]),
    };
    return s;
}
