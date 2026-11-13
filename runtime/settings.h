// User settings (powermoon.ini in the data root), edited from the in-game menu.
#pragma once
#include <stdbool.h>

enum { RES_MATCH = 0, /* 1..8 = 1x..8x */ RES_DYNAMIC = 9, RES_Q75 = 10, RES_Q25 = 11, RES_Q50 = 12, RES_1440 = 13 };
enum { UPSCALE_BILINEAR = 0, UPSCALE_METALFX = 1 /* macOS */, UPSCALE_FSR1 = 2 /* Windows: AMD FSR 1 */ };
#ifdef __APPLE__
#define UPSCALE_SHARP UPSCALE_METALFX  // each OS's non-bilinear upscaler (the default)
#else
#define UPSCALE_SHARP UPSCALE_FSR1
#endif
enum { AA_OFF = 0, AA_SMAA = 1 };
enum { OVERLAY_TOP_RIGHT, OVERLAY_TOP_LEFT, OVERLAY_BOTTOM_RIGHT, OVERLAY_BOTTOM_LEFT, OVERLAY_CORNER_COUNT };
enum { DLSS_OFF, DLSS_DLAA, DLSS_QUALITY, DLSS_BALANCED, DLSS_PERFORMANCE, DLSS_MODE_COUNT };
enum { DLSS_PRESET_DEFAULT, DLSS_PRESET_K, DLSS_PRESET_L, DLSS_PRESET_M, DLSS_PRESET_COUNT };
enum { TEMPORAL_DLSS, TEMPORAL_FSR3, TEMPORAL_FSR4, TEMPORAL_XESS, TEMPORAL_BACKEND_COUNT };
enum { WINDOWED, BORDERLESS, EXCLUSIVE_FULLSCREEN, WINDOW_MODE_COUNT };
enum { DISPLAY_AUTO, DISPLAY_1080, DISPLAY_1440, DISPLAY_2160, DISPLAY_MODE_COUNT };
static inline int settings_display_height(int mode) {
    const int heights[DISPLAY_MODE_COUNT] = {0, 1080, 1440, 2160};
    return heights[mode];
}
// Anisotropic filtering of surface textures: ANISO_GAME keeps the level the game asks for, else 2x..16x
enum { ANISO_GAME = 0 };
// Frame rate: FPS_90 / FPS_120 draw 90 / 120 times a second with gameplay at its 60 (runtime/hle/odyssey_120.c); FPS_60
// as on the Switch
enum { FPS_60 = 0, FPS_90 = 1, FPS_120 = 2, FPS_COUNT };
// The image in a window of another shape: 16:9 with bars, or (Vulkan) filling a wider window with more of the scene
// at the sides (runtime/hle/odyssey_wide.c)
enum { ASPECT_16_9 = 0, ASPECT_FILL = 1 };
// Button prompts: Auto follows the device used last (keyboard / mouse or controller)
// Memory use: Normal builds the shaders of what a stage sets up ahead (fewer stutters, ~3 GB more with NVIDIA's driver),
// Low builds each at its first draw; Auto is Low on a PC with less than 12 GB of RAM
enum { MEMORY_AUTO = 0, MEMORY_NORMAL, MEMORY_LOW, MEMORY_COUNT };
bool settings_low_memory(void);
// (Controller: the connected controller's own - Xbox, PlayStation, or the game's; the others always those)
enum { PROMPTS_AUTO = 0, PROMPTS_CONTROLLER = 1, PROMPTS_KEYBOARD = 2, PROMPTS_SWITCH, PROMPTS_XBOX, PROMPTS_PLAYSTATION,
       PROMPTS_COUNT };
// System language served to the game (runtime/services/am.c GetDesiredLanguage, services/hid_set.c GetLanguageCode):
// the game picks its text language from it, as it would from the Switch's own system setting. LANG_AUTO follows the
// host locale ($LC_ALL / $LC_MESSAGES / $LANG); the rest are the codes of the languages the game has text for.
enum { LANG_AUTO = 0, N_LANGUAGES = 12 };
extern const char *const g_language_codes[N_LANGUAGES];  // what the game is told: "ja", "en-US", "es", ...
extern const char *const g_language_names[N_LANGUAGES];  // for the menu ("Auto", "Japanese", ..., "Spanish")

typedef struct {
    int window_mode, display_resolution;
    int monitor;         // 0: current display; 1..N: SDL's current display enumeration
    int res_mode;        // RES_MATCH, 1..4 (N x the Switch's 1080p), RES_DYNAMIC, RES_Q75/50/25 (share of the display)
    int upscaler;        // UPSCALE_*: how a lower internal resolution reaches the window (MetalFX: macOS, FSR 1: Windows)
    int antialias;       // AA_*: anti-aliasing of the game image before it is scaled to the window
    int dlss_mode, dlss_preset;  // Windows NVIDIA modes and model hints, applied during play
    int temporal_backend, fsr3_mode, fsr3_sharpness, fsr3_cas_sharpness;
    bool fsr3_cas;
    int fsr4_mode, fsr4_sharpness, fsr4_cas_sharpness;
    int xess_mode, xess_sharpness, xess_cas_sharpness;
    bool fsr4_cas, xess_cas;
    bool dlss_cas;
    int dlss_sharpness;   // FidelityFX CAS percent, 0..100; zero disables sharpening
    int dlss_rcas_sharpness; // reconstruction sharpening, independent of optional CAS
    int fsr1_sharpness;   // FSR 1's RCAS percent, 0..100; stored independently
    int aspect;          // ASPECT_*: the image in a window wider than 16:9
    bool frame_interp;   // images between the game's frames (Vulkan: runtime/nvn/interp.c)
    bool deband;         // smooth gradients: the game's final picture in half floats, dithered into the window (Vulkan:
                         // nvn/vulkan/textures.c vk_deband)
    float volume;        // master volume, 1 = 100%
    bool reverb;         // the audio renderer's reverb effect
    bool movie_audio;
    bool show_fps, performance_overlay;
    // The performance overlay (runtime/ui/menu.mm draw_performance): where, how big, how see-through, and its parts
    int overlay_corner;   // OVERLAY_*
    int overlay_size;     // 0 small, 1 medium, 2 large
    int overlay_opacity;  // percent of the background, 0..100
    bool overlay_gpu, overlay_cpu, overlay_ram, overlay_graph, overlay_hardware, overlay_render, overlay_position;
    bool motion;         // the controller's gyro / accelerometer as the Switch's motion sensor (runtime/hle/input.c)
    bool rumble;         // the controller's vibration (the game's HD rumble: runtime/hle/input.c input_vibration)
    int stick_deadzone;  // percent of a controller stick's travel that counts as rest (runtime/hle/input.c), 0: none
    bool mouse_camera;   // the mouse turns the camera (the right stick; the pointer is held in the window while playing)
    int mouse_sensitivity;  // percent, 25..400 (runtime/hle/input.c mouse_stick)
    bool mouse_invert_y; // moving the mouse up turns the camera down
    int prompts;         // PROMPTS_*: the game's button icons as key caps / a mouse, or the controller's (nvn/kbprompts.c)
    int language;        // LANG_AUTO or an index into g_language_codes: the game's text language
    int aniso;           // ANISO_GAME or 2, 4, 8, 16: the most anisotropic filtering of filtered, mipmapped textures that
                         // tile (nvn/vulkan/textures.c vk_sampler; read at start)
    int frame_rate;      // FPS_*: the game's draws per second (gameplay stays at 60)
    int memory;          // MEMORY_*: how much memory the renderer may use to spare stutters (settings_low_memory)
    bool shader_preload; // a stage's shaders get their libraries when the stage sets them up, not at their first draw
                         // (nvn/vulkan/exec.c compile_program; more memory)
    bool shader_wait;    // a draw whose shader is still loading waits for it (a hitch) instead of being left out /
                         // its frame not being shown (exec.c builds_block)
    bool skip_intro;     // no Joy-Con tips screen timer before the title (runtime/hle/odyssey_120.c; read at start)
    int warmup;          // WARMUP_*: how long the start replays known pipelines (nvn/vulkan/exec.c vk_pipeline_prewarm)
    bool mute_unfocused; // no sound while the window isn't in front (services/audio.c)
    bool check_updates;  // look for a newer release at start (runtime/updater.c)
} Settings;
// The pipeline warm-up at start: Auto picks Long on PCs with 16 GB of memory or more
enum { WARMUP_AUTO = 0, WARMUP_NORMAL = 1, WARMUP_LONG = 2, WARMUP_COUNT };
// The start's warm-up budget for this PC: milliseconds and megabytes of extra memory
void settings_warmup_budget(unsigned *ms, unsigned *mb);
// The window isn't in front (set by the menu's event handling)
extern volatile bool g_app_unfocused;

extern Settings g_settings;

static inline int *settings_temporal_mode(void) {
    switch (g_settings.temporal_backend) {
    case TEMPORAL_FSR3: return &g_settings.fsr3_mode;
    case TEMPORAL_FSR4: return &g_settings.fsr4_mode;
    case TEMPORAL_XESS: return &g_settings.xess_mode;
    default: return &g_settings.dlss_mode;
    }
}
static inline int *settings_temporal_sharpness(void) {
    switch (g_settings.temporal_backend) {
    case TEMPORAL_FSR3: return &g_settings.fsr3_sharpness;
    case TEMPORAL_FSR4: return &g_settings.fsr4_sharpness;
    case TEMPORAL_XESS: return &g_settings.xess_sharpness;
    default: return &g_settings.dlss_rcas_sharpness;
    }
}
static inline bool *settings_temporal_cas(void) {
    switch (g_settings.temporal_backend) {
    case TEMPORAL_FSR3: return &g_settings.fsr3_cas;
    case TEMPORAL_FSR4: return &g_settings.fsr4_cas;
    case TEMPORAL_XESS: return &g_settings.xess_cas;
    default: return &g_settings.dlss_cas;
    }
}
static inline int *settings_temporal_cas_sharpness(void) {
    switch (g_settings.temporal_backend) {
    case TEMPORAL_FSR3: return &g_settings.fsr3_cas_sharpness;
    case TEMPORAL_FSR4: return &g_settings.fsr4_cas_sharpness;
    case TEMPORAL_XESS: return &g_settings.xess_cas_sharpness;
    default: return &g_settings.dlss_sharpness;
    }
}

void settings_load(void);
const char *settings_current_file(void);
void settings_save(void);
// The battery saver's settings (lower internal resolution with FSR 1, no extra frames or SMAA): the Steam Deck's
// defaults and a one-press preset in the menu (runtime/settings.c says what and why)
void settings_battery_saver(void);
// The language code to serve the game ("es", "en-US", ...): the setting, or the host locale for LANG_AUTO
const char *settings_language(void);
// That code's name for the menu ("Spanish")
const char *settings_language_name(void);
// Mods switched off in the menu (runtime/mods.c): their folders, one "mod_off = <folder>" line each
bool settings_mod_off(const char *id);
void settings_set_mod_off(const char *id, bool off);
// The anisotropy the game started with (the menu's value applies at the next start): 0 = the game's own
int settings_aniso_at_start(void);
