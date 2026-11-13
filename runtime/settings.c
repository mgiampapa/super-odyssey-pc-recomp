#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "hle.h"
#include "input_bind.h"
#include "platform.h"

#define SETTINGS_FILE settings_file()

// ODYSSEY_SETTINGS=<path>: another settings file (test runs never touch the player's powermoon.ini)
static const char *settings_file(void) {
    const char *e = getenv("ODYSSEY_SETTINGS");
    return e && *e ? e : "powermoon.ini";
}
const char *settings_current_file(void) { return settings_file(); }

Settings g_settings = {
    .window_mode = WINDOWED,
    .volume = 1.0f,
    .reverb = true,
    .movie_audio = true,
    .show_fps = false,
    .overlay_size = 1, .overlay_opacity = 80,
    .overlay_gpu = true, .overlay_cpu = true, .overlay_ram = true, .overlay_graph = true, .overlay_hardware = true,
    .overlay_render = true, .overlay_position = true,
#ifdef __APPLE__
    .upscaler = UPSCALE_METALFX,
#else
    .upscaler = UPSCALE_BILINEAR,
#endif
    .dlss_sharpness = 20,
    .dlss_rcas_sharpness = 20,
    .fsr3_mode = DLSS_QUALITY,
    .fsr3_sharpness = 20,
    .fsr3_cas_sharpness = 20,
    .fsr4_mode = DLSS_QUALITY, .fsr4_sharpness = 20, .fsr4_cas_sharpness = 20,
    .xess_mode = DLSS_QUALITY, .xess_sharpness = 20, .xess_cas_sharpness = 20,
    .fsr1_sharpness = 90,
    .deband = true,
    .rumble = true,
    .motion = true,
    .mouse_camera = true,
    .mouse_sensitivity = 100,
    .aniso = 16,
    .frame_rate = FPS_60,
    .shader_preload = false,  // (it costs a lot of memory: nvn/vulkan/exec.c preload_room)
    .skip_intro = true,
    .warmup = WARMUP_AUTO,
    .check_updates = true,
};

volatile bool g_app_unfocused;

// The battery saver (Steam Deck's defaults, and the menu's Performance > Battery saver): less work per frame. Odyssey's
// gameplay steps once per drawn frame, so a lower frame rate cap would slow the game down, not save power - the savings
// come from the picture instead: 75% of the display (960 x 600 on a Deck) upscaled with FSR 1, no SMAA, no extra
// frames (120 FPS, interpolation), 4x anisotropic filtering, the shorter warm-up at start.
void settings_battery_saver(void) {
    g_settings.res_mode = RES_Q75;
    g_settings.upscaler = UPSCALE_SHARP;
    g_settings.antialias = AA_OFF;
    g_settings.frame_rate = FPS_60;
    g_settings.frame_interp = false;
    g_settings.aniso = 4;
    g_settings.dlss_mode = DLSS_OFF;
    g_settings.warmup = WARMUP_NORMAL;
    // (no preloading: every program a stage sets up built in the background competed with the game thread on a Deck's
    // 4 cores - 26 fps with it, 40 without, in Cap Kingdom)
    g_settings.shader_preload = false;
}
static const char *const warmups[WARMUP_COUNT] = {"auto", "normal", "long"};

void settings_warmup_budget(unsigned *ms, unsigned *mb) {
    int ram_mb = SDL_GetSystemRAM();
    int w = g_settings.warmup == WARMUP_AUTO ? (ram_mb >= 16000 ? WARMUP_LONG : WARMUP_NORMAL) : g_settings.warmup;
    if (w == WARMUP_LONG) {  // (what the 120 FPS launcher used: 30 s, up to 12 GB - at most half of this PC's memory)
        *ms = 30000;
        *mb = ram_mb > 0 && ram_mb / 2 < 12288 ? (unsigned)(ram_mb / 2) : 12288;
    } else {
        *ms = 15000;
        *mb = 3072;
    }
}

static const char *const dlss_modes[DLSS_MODE_COUNT] = {"off", "dlaa", "quality", "balanced", "performance"};
static const char *const dlss_presets[DLSS_PRESET_COUNT] = {"default", "k", "l", "m"};
static const char *const temporal_backends[TEMPORAL_BACKEND_COUNT] = {"dlss", "fsr3", "fsr4", "xess"};
static const char *const fsr3_modes[DLSS_MODE_COUNT] = {"off", "native", "quality", "balanced", "performance"};
static const char *const window_modes[WINDOW_MODE_COUNT] = {"windowed", "borderless", "exclusive"};
static const char *const display_modes[DISPLAY_MODE_COUNT] = {"auto", "1080p", "1440p", "2160p"};

static int named_option(const char *value, const char *const *options, int count) {
    for (int i = 0; i < count; i++) if (!strcmp(value, options[i])) return i;
    return 0;
}

static int g_aniso_start = -1;
int settings_aniso_at_start(void) {
    if (g_aniso_start < 0) {
        const char *e = getenv("ODYSSEY_ANISO");  // (tests: ODYSSEY_ANISO=0 / 2 / 4 / 8 / 16)
        g_aniso_start = e ? atoi(e) : g_settings.aniso;
        if (g_aniso_start != 2 && g_aniso_start != 4 && g_aniso_start != 8 && g_aniso_start != 16) g_aniso_start = 0;
    }
    return g_aniso_start;
}

// The game's own text languages: its RomFS carries these per region (regionUS/USen|USes|USfr, regionEU/EUen|EUes|...,
// regionJP/JPja, regionKR/KRkr, regionTW/TWch, regionCN/CNch), and the game picks one from the system language it
// reads back (runtime/services/am.c), exactly as it does on the console. Codes as the Switch reports them.
const char *const g_language_codes[N_LANGUAGES] = {"auto", "ja", "en-US", "es", "fr", "de", "it", "nl", "ru", "ko",
                                                   "zh-CN", "zh-TW"};
const char *const g_language_names[N_LANGUAGES] = {"Auto", "Japanese", "English", "Spanish", "French", "German",
                                                   "Italian", "Dutch", "Russian", "Korean", "Chinese (Simplified)",
                                                   "Chinese (Traditional)"};

// LANG_AUTO: the host's own language (LANG=es_ES.UTF-8 -> es), en-US when it is none of the game's. Linux only:
// macOS / Windows apps get no locale variables from the system, so Auto is English there (pick one in settings).
static const char *host_language(void) {
#ifndef __linux__
    return "en-US";
#endif
    const char *e = getenv("LC_ALL");
    if (!e || !*e) e = getenv("LC_MESSAGES");
    if (!e || !*e) e = getenv("LANG");
    if (!e || !*e || !strcmp(e, "C") || !strcmp(e, "POSIX")) return "en-US";
    char l[8] = {0};
    size_t n = 0;
    for (const char *p = e; *p && *p != '.' && *p != '@' && *p != '_' && n < sizeof l - 1; p++) l[n++] = *p;
    static const struct { const char *loc, *code; } map[] = {{"ja", "ja"}, {"ko", "ko"}, {"ru", "ru"}, {"nl", "nl"},
                                                             {"de", "de"}, {"it", "it"}, {"fr", "fr"}, {"es", "es"}};
    for (size_t i = 0; i < sizeof map / sizeof *map; i++)
        if (!strcmp(l, map[i].loc)) return map[i].code;
    if (!strcmp(l, "zh")) return e[n] == '_' && e[n + 1] == 'T' ? "zh-TW" : "zh-CN";  // (TW / HK: traditional)
    return "en-US";
}

const char *settings_language(void) {
    int i = g_settings.language;
    return i > LANG_AUTO && i < N_LANGUAGES ? g_language_codes[i] : host_language();
}

const char *settings_language_name(void) {
    const char *c = settings_language();
    for (int i = 0; i < N_LANGUAGES; i++)
        if (!strcmp(c, g_language_codes[i])) return g_language_names[i];
    return "English";
}

static char **g_mod_off;  // the switched-off mods' folders (relative to mods/)
static int g_nmod_off;

bool settings_mod_off(const char *id) {
    for (int i = 0; i < g_nmod_off; i++)
        if (!strcmp(g_mod_off[i], id)) return true;
    return false;
}

void settings_set_mod_off(const char *id, bool off) {
    for (int i = 0; i < g_nmod_off; i++) {
        if (strcmp(g_mod_off[i], id)) continue;
        if (!off) { free(g_mod_off[i]); g_mod_off[i] = g_mod_off[--g_nmod_off]; }
        return;
    }
    if (!off) return;
    g_mod_off = realloc(g_mod_off, sizeof *g_mod_off * (g_nmod_off + 1));
    g_mod_off[g_nmod_off++] = strdup(id);
}

void settings_load(void) {
    static bool loaded;
    if (loaded) return;
    loaded = true;
    // Steam Deck: the battery saver's settings unless chosen otherwise (the settings file, read below, wins)
    if (plat_is_steam_deck()) settings_battery_saver();
    FILE *f = fopen(SETTINGS_FILE, "r");
    if (!f && !getenv("ODYSSEY_SETTINGS")) f = fopen("odyssey.ini", "r");  // (the settings of the earlier builds)
    if (!f && !getenv("ODYSSEY_SETTINGS")) f = fopen("larecomp.ini", "r");
    if (!f) return;
    char line[700];
    while (fgets(line, sizeof line, f)) {
        char key[64];
        char val[600];  // (a mod's folder can be long)
        if (sscanf(line, " %63[^= ] = %599[^\n]", key, val) < 2) continue;
        for (size_t n = strlen(val); n && (val[n - 1] == ' ' || val[n - 1] == '\r'); n--) val[n - 1] = 0;
        if (!strcmp(key, "fullscreen")) g_settings.window_mode = atoi(val) ? BORDERLESS : WINDOWED;
        else if (!strcmp(key, "window_mode")) g_settings.window_mode = named_option(val, window_modes, WINDOW_MODE_COUNT);
        else if (!strcmp(key, "display_resolution")) g_settings.display_resolution = named_option(val, display_modes, DISPLAY_MODE_COUNT);
        else if (!strcmp(key, "monitor")) { int m = atoi(val); g_settings.monitor = m >= 0 && m <= 64 ? m : 0; }
        else if (!strcmp(key, "mod_off")) settings_set_mod_off(val, true);
        else if (!strcmp(key, "volume")) g_settings.volume = (float)atof(val);
        else if (!strcmp(key, "reverb")) g_settings.reverb = atoi(val) != 0;
        else if (!strcmp(key, "movie_audio")) g_settings.movie_audio = atoi(val) != 0;
        else if (!strcmp(key, "show_fps")) g_settings.show_fps = atoi(val) != 0;
        else if (!strcmp(key, "performance_overlay")) g_settings.performance_overlay = atoi(val) != 0;
        else if (!strcmp(key, "overlay_corner")) { int v = atoi(val); g_settings.overlay_corner = v >= 0 && v < OVERLAY_CORNER_COUNT ? v : 0; }
        else if (!strcmp(key, "overlay_size")) { int v = atoi(val); g_settings.overlay_size = v >= 0 && v <= 2 ? v : 1; }
        else if (!strcmp(key, "overlay_opacity")) { int v = atoi(val); g_settings.overlay_opacity = v < 0 ? 0 : v > 100 ? 100 : v; }
        else if (!strcmp(key, "overlay_gpu")) g_settings.overlay_gpu = atoi(val) != 0;
        else if (!strcmp(key, "overlay_cpu")) g_settings.overlay_cpu = atoi(val) != 0;
        else if (!strcmp(key, "overlay_ram")) g_settings.overlay_ram = atoi(val) != 0;
        else if (!strcmp(key, "overlay_graph")) g_settings.overlay_graph = atoi(val) != 0;
        else if (!strcmp(key, "overlay_hardware")) g_settings.overlay_hardware = atoi(val) != 0;
        else if (!strcmp(key, "overlay_render")) g_settings.overlay_render = atoi(val) != 0;
        else if (!strcmp(key, "overlay_position")) g_settings.overlay_position = atoi(val) != 0;
        else if (!strcmp(key, "vibration")) g_settings.rumble = atoi(val) != 0;
        else if (!strcmp(key, "motion")) g_settings.motion = atoi(val) != 0;
        else if (!strcmp(key, "stick_deadzone")) g_settings.stick_deadzone = atoi(val) < 0 ? 0 : atoi(val) > 40 ? 40 : atoi(val);
        else if (!strcmp(key, "resolution")) {
            int n = atoi(val);
            g_settings.res_mode = !strcmp(val, "dynamic") ? RES_DYNAMIC : !strcmp(val, "75%") ? RES_Q75 :
                                  !strcmp(val, "50%") ? RES_Q50 : !strcmp(val, "25%") ? RES_Q25 : !strcmp(val, "1440p") ? RES_1440 :
                                  (n >= 1 && n <= 4) ? n : n > 4 ? 4 : RES_MATCH;
        }
        else if (!strcmp(key, "upscaler")) g_settings.upscaler = !strcmp(val, "bilinear") ? UPSCALE_BILINEAR : UPSCALE_SHARP;  // (metalfx / fsr1: this OS's)
        else if (!strcmp(key, "antialias")) g_settings.antialias = !strcmp(val, "off") ? AA_OFF : AA_SMAA;
        else if (!strcmp(key, "dlss_mode")) g_settings.dlss_mode = named_option(val, dlss_modes, DLSS_MODE_COUNT);
        else if (!strcmp(key, "dlss_preset")) g_settings.dlss_preset = named_option(val, dlss_presets, DLSS_PRESET_COUNT);
        else if (!strcmp(key, "temporal_backend")) g_settings.temporal_backend = named_option(val, temporal_backends, TEMPORAL_BACKEND_COUNT);
        else if (!strcmp(key, "fsr3_mode")) g_settings.fsr3_mode = named_option(val, fsr3_modes, DLSS_MODE_COUNT);
        else if (!strcmp(key, "fsr4_mode")) g_settings.fsr4_mode = named_option(val, fsr3_modes, DLSS_MODE_COUNT);
        else if (!strcmp(key, "xess_mode")) g_settings.xess_mode = named_option(val, fsr3_modes, DLSS_MODE_COUNT);
        else if (!strcmp(key, "fsr4_cas")) g_settings.fsr4_cas = atoi(val) != 0;
        else if (!strcmp(key, "xess_cas")) g_settings.xess_cas = atoi(val) != 0;
        else if (!strcmp(key, "fsr4_sharpness") || !strcmp(key, "fsr4_cas_sharpness") ||
                 !strcmp(key, "xess_sharpness") || !strcmp(key, "xess_cas_sharpness")) {
            int n = atoi(val); n = n < 0 ? 0 : n > 100 ? 100 : n;
            if (!strcmp(key, "fsr4_sharpness")) g_settings.fsr4_sharpness = n;
            else if (!strcmp(key, "fsr4_cas_sharpness")) g_settings.fsr4_cas_sharpness = n;
            else if (!strcmp(key, "xess_sharpness")) g_settings.xess_sharpness = n;
            else g_settings.xess_cas_sharpness = n;
        }
        else if (!strcmp(key, "fsr3_cas")) g_settings.fsr3_cas = atoi(val) != 0;
        else if (!strcmp(key, "fsr3_sharpness") || !strcmp(key, "fsr3_cas_sharpness")) {
            int n = atoi(val); n = n < 0 ? 0 : n > 100 ? 100 : n;
            if (!strcmp(key, "fsr3_sharpness")) g_settings.fsr3_sharpness = n;
            else g_settings.fsr3_cas_sharpness = n;
        }
        else if (!strcmp(key, "dlss_cas")) g_settings.dlss_cas = atoi(val) != 0;
        else if (!strcmp(key, "dlss_sharpness")) {
            int n = atoi(val);
            g_settings.dlss_sharpness = n < 0 ? 0 : n > 100 ? 100 : n;
        }
        else if (!strcmp(key, "fsr1_sharpness")) {
            int n = atoi(val);
            g_settings.fsr1_sharpness = n < 0 ? 0 : n > 100 ? 100 : n;
        }
        else if (!strcmp(key, "dlss_rcas_sharpness")) {
            int n = atoi(val);
            g_settings.dlss_rcas_sharpness = n < 0 ? 0 : n > 100 ? 100 : n;
        }
        else if (!strcmp(key, "deband")) g_settings.deband = atoi(val) != 0;
        else if (!strcmp(key, "aspect")) g_settings.aspect = !strcmp(val, "fill") ? ASPECT_FILL : ASPECT_16_9;
        else if (!strcmp(key, "frame_interp")) g_settings.frame_interp = atoi(val) != 0;
        else if (!strcmp(key, "aniso")) {
            int n = atoi(val);
            g_settings.aniso = n == 2 || n == 4 || n == 8 || n == 16 ? n : ANISO_GAME;  // ("game")
        }
        else if (!strcmp(key, "shader_preload")) g_settings.shader_preload = atoi(val) != 0;
        else if (!strcmp(key, "memory"))
            g_settings.memory = !strcmp(val, "normal") ? MEMORY_NORMAL : !strcmp(val, "low") ? MEMORY_LOW : MEMORY_AUTO;
        else if (!strcmp(key, "shader_wait")) g_settings.shader_wait = atoi(val) != 0;
        else if (!strcmp(key, "frame_rate")) g_settings.frame_rate = atoi(val) == 60 ? FPS_60 : atoi(val) == 90 ? FPS_90 : FPS_120;
        else if (!strcmp(key, "skip_intro")) g_settings.skip_intro = atoi(val) != 0;
        else if (!strcmp(key, "warmup")) g_settings.warmup = named_option(val, warmups, WARMUP_COUNT);
        else if (!strcmp(key, "mute_unfocused")) g_settings.mute_unfocused = atoi(val) != 0;
        else if (!strcmp(key, "check_updates")) g_settings.check_updates = atoi(val) != 0;
        else if (!strcmp(key, "mouse_camera")) g_settings.mouse_camera = atoi(val) != 0;
        else if (!strcmp(key, "mouse_sensitivity"))
            g_settings.mouse_sensitivity = atoi(val) < 25 ? 25 : atoi(val) > 400 ? 400 : atoi(val);
        else if (!strcmp(key, "mouse_invert_y")) g_settings.mouse_invert_y = atoi(val) != 0;
        else if (!strcmp(key, "button_prompts"))
            g_settings.prompts = !strcmp(val, "controller") ? PROMPTS_CONTROLLER : !strcmp(val, "keyboard") ? PROMPTS_KEYBOARD :
                                 !strcmp(val, "switch") ? PROMPTS_SWITCH : !strcmp(val, "xbox") ? PROMPTS_XBOX :
                                 !strcmp(val, "playstation") ? PROMPTS_PLAYSTATION : PROMPTS_AUTO;
        else if (!strcmp(key, "language")) {  // ("auto" or one of the game's codes; anything else: auto)
            g_settings.language = LANG_AUTO;
            for (int i = 1; i < N_LANGUAGES; i++)
                if (!strcmp(val, g_language_codes[i])) g_settings.language = i;
        }
        else bind_settings_load(key, val);  // bind_pad_* / bind_key_*: runtime/hle/bindings.c (others: ignored)
    }
    fclose(f);
    if (g_settings.volume < 0) g_settings.volume = 0;
    if (g_settings.volume > 2) g_settings.volume = 2;
    settings_aniso_at_start();  // (latched now: the menu's changes apply at the next start)
    LOG("[settings] loaded\n");
}

void settings_save(void) {
    FILE *f = fopen(SETTINGS_FILE, "w");
    if (!f) { LOG("[settings] cannot write %s\n", SETTINGS_FILE); return; }
    fprintf(f, "# Super Odyssey PC Recomp settings (edited from the in-game menu: F1, Esc, or the controller's Home button)\n");
    fprintf(f, "window_mode = %s\n", window_modes[g_settings.window_mode]);
    fprintf(f, "display_resolution = %s\n", display_modes[g_settings.display_resolution]);
    fprintf(f, "monitor = %d\n", g_settings.monitor);
    fprintf(f, "volume = %.2f\n", g_settings.volume);
    fprintf(f, "reverb = %d\n", g_settings.reverb);
    fprintf(f, "movie_audio = %d\n", g_settings.movie_audio);
    fprintf(f, "show_fps = %d\n", g_settings.show_fps);
    fprintf(f, "performance_overlay = %d\n", g_settings.performance_overlay);
    fprintf(f, "overlay_corner = %d\noverlay_size = %d\noverlay_opacity = %d\n", g_settings.overlay_corner,
            g_settings.overlay_size, g_settings.overlay_opacity);
    fprintf(f, "overlay_gpu = %d\noverlay_cpu = %d\noverlay_ram = %d\noverlay_graph = %d\n", g_settings.overlay_gpu,
            g_settings.overlay_cpu, g_settings.overlay_ram, g_settings.overlay_graph);
    fprintf(f, "overlay_hardware = %d\noverlay_render = %d\noverlay_position = %d\n", g_settings.overlay_hardware,
            g_settings.overlay_render, g_settings.overlay_position);
    fprintf(f, "vibration = %d\n", g_settings.rumble);
    fprintf(f, "motion = %d\n", g_settings.motion);
    fprintf(f, "stick_deadzone = %d\n", g_settings.stick_deadzone);
    fprintf(f, "upscaler = %s\n", g_settings.upscaler == UPSCALE_METALFX ? "metalfx" : g_settings.upscaler == UPSCALE_FSR1 ? "fsr1" : "bilinear");
    fprintf(f, "antialias = %s\n", g_settings.antialias == AA_SMAA ? "smaa" : "off");
    fprintf(f, "dlss_mode = %s\n", dlss_modes[g_settings.dlss_mode]);
    fprintf(f, "dlss_preset = %s\n", dlss_presets[g_settings.dlss_preset]);
    fprintf(f, "temporal_backend = %s\n", temporal_backends[g_settings.temporal_backend]);
    fprintf(f, "fsr3_mode = %s\n", fsr3_modes[g_settings.fsr3_mode]);
    fprintf(f, "fsr3_sharpness = %d\n", g_settings.fsr3_sharpness);
    fprintf(f, "fsr3_cas = %d\n", g_settings.fsr3_cas);
    fprintf(f, "fsr3_cas_sharpness = %d\n", g_settings.fsr3_cas_sharpness);
    fprintf(f, "fsr4_mode = %s\nfsr4_sharpness = %d\nfsr4_cas = %d\nfsr4_cas_sharpness = %d\n",
        fsr3_modes[g_settings.fsr4_mode], g_settings.fsr4_sharpness, g_settings.fsr4_cas, g_settings.fsr4_cas_sharpness);
    fprintf(f, "xess_mode = %s\nxess_sharpness = %d\nxess_cas = %d\nxess_cas_sharpness = %d\n",
        fsr3_modes[g_settings.xess_mode], g_settings.xess_sharpness, g_settings.xess_cas, g_settings.xess_cas_sharpness);
    fprintf(f, "dlss_cas = %d\n", g_settings.dlss_cas);
    fprintf(f, "dlss_sharpness = %d\n", g_settings.dlss_sharpness);
    fprintf(f, "dlss_rcas_sharpness = %d\n", g_settings.dlss_rcas_sharpness);
    fprintf(f, "fsr1_sharpness = %d\n", g_settings.fsr1_sharpness);
    fprintf(f, "deband = %d\n", g_settings.deband);
    fprintf(f, "aspect = %s\n", g_settings.aspect == ASPECT_FILL ? "fill" : "16:9");
    fprintf(f, "frame_interp = %d\n", g_settings.frame_interp);
    if (g_settings.aniso) fprintf(f, "aniso = %d\n", g_settings.aniso);
    else fprintf(f, "aniso = game\n");
    fprintf(f, "frame_rate = %d\n", g_settings.frame_rate == FPS_60 ? 60 : g_settings.frame_rate == FPS_90 ? 90 : 120);
    fprintf(f, "shader_preload = %d\n", g_settings.shader_preload);
    fprintf(f, "memory = %s\n", g_settings.memory == MEMORY_NORMAL ? "normal" : g_settings.memory == MEMORY_LOW ? "low" : "auto");
    fprintf(f, "shader_wait = %d\n", g_settings.shader_wait);
    fprintf(f, "skip_intro = %d\n", g_settings.skip_intro);
    fprintf(f, "warmup = %s\n", warmups[g_settings.warmup >= 0 && g_settings.warmup < WARMUP_COUNT ? g_settings.warmup : 0]);
    fprintf(f, "mute_unfocused = %d\n", g_settings.mute_unfocused);
    fprintf(f, "check_updates = %d\n", g_settings.check_updates);
    fprintf(f, "mouse_camera = %d\n", g_settings.mouse_camera);
    fprintf(f, "mouse_sensitivity = %d\n", g_settings.mouse_sensitivity);
    fprintf(f, "mouse_invert_y = %d\n", g_settings.mouse_invert_y);
    fprintf(f, "button_prompts = %s\n", g_settings.prompts == PROMPTS_CONTROLLER ? "controller" :
                                        g_settings.prompts == PROMPTS_KEYBOARD ? "keyboard" :
                                        g_settings.prompts == PROMPTS_SWITCH ? "switch" :
                                        g_settings.prompts == PROMPTS_XBOX ? "xbox" :
                                        g_settings.prompts == PROMPTS_PLAYSTATION ? "playstation" : "auto");
    if (g_settings.res_mode == RES_DYNAMIC) fprintf(f, "resolution = dynamic\n");
    else if (g_settings.res_mode == RES_Q75) fprintf(f, "resolution = 75%%\n");
    else if (g_settings.res_mode == RES_Q25) fprintf(f, "resolution = 25%%\n");
    else if (g_settings.res_mode == RES_Q50) fprintf(f, "resolution = 50%%\n");
    else if (g_settings.res_mode == RES_1440) fprintf(f, "resolution = 1440p\n");
    else if (g_settings.res_mode >= 1 && g_settings.res_mode <= 8) fprintf(f, "resolution = %dx\n", g_settings.res_mode);
    else fprintf(f, "resolution = match\n");
    fprintf(f, "language = %s\n", g_language_codes[g_settings.language > LANG_AUTO && g_settings.language < N_LANGUAGES
                                                     ? g_settings.language : LANG_AUTO]);
    for (int i = 0; i < g_nmod_off; i++) fprintf(f, "mod_off = %s\n", g_mod_off[i]);
    bind_settings_save(f);
    fclose(f);
}

// Low memory: chosen, or Auto on a PC with less than 12 GB of RAM (an 8 GB laptop - its integrated GPU's memory is that
// RAM too). ODYSSEY_LOW_MEMORY=1 / 0 decides for tests.
bool settings_low_memory(void) {
    const char *e = getenv("ODYSSEY_LOW_MEMORY");
    if (e && *e) return atoi(e) != 0;
    if (g_settings.memory != MEMORY_AUTO) return g_settings.memory == MEMORY_LOW;
    static int low = -1;
    if (low < 0) {
        extern uint64_t plat_total_ram(void);
        uint64_t ram = plat_total_ram();
        low = ram && ram < (12ull << 30);
        LOG("[settings] %llu MB of RAM: %s memory use\n", (unsigned long long)(ram >> 20), low ? "low" : "normal");
    }
    return low;
}
