// Odyssey's stage scene hooks: the scene warm-up for shader builds (below), and a test tool - change to another stage
// as a door does, ODYSSEY_WARP=<stage>[:<entrance>[:<scenario>]][@<frame>] (e.g. CapWorldTowerStage@1500), within
// the kingdom; another kingdom's home stage with the Odyssey's flight, ODYSSEY_WARP=world:<stage>[@<frame>] (e.g.
// world:WaterfallWorldHomeStage@1500; a plain change there waits forever for heap: world resources only change so).
// Several, one after the other, separated by ';' (e.g. world:ForestWorldHomeStage@1500;ForestWorldBossStage@5500).
// StageScene::control (virtual, every frame of a stage) is wrapped: from that frame on, its first call builds a
// ChangeStageInfo on the guest stack (ChangeStageInfo(holder, entrance, stage, isReturn, scenario, subScenarioType))
// and hands it to GameDataFunction::tryChangeNextStage; the scene then changes stage as after a door. Entrance "" =
// the stage's default start, scenario -1 = the game picks it. ODYSSEY_LOG_STAGES=1 (or any warp): the frame each
// stage scene starts running.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../rt.h"
#include "../nvn/nvn.h"

// StageScene::mGameDataHolder (OdysseyDecomp StageScene.h; control() loads it): 0x2d0; 1.4.1: 0x2e0 (its control()
// reads it there as 1.0.0 does at 0x2d0)
#define STAGE_SCENE_HOLDER (loader_main_is_v141() ? 0x2e0 : 0x2d0)
#define CHANGE_STAGE_INFO_SIZE 0x278  // (static_assert in OdysseyDecomp ChangeStageInfo.h)

uint64_t renderer_present_count(void);

typedef struct {
    bool world;  // (world: "world:<stage>", another kingdom)
    char stage[128], entrance[128];
    int scenario;
    uint64_t at;
} Warp;
static Warp g_warps[64];  // (a sequence: "world:ForestWorldHomeStage@1500;ForestWorldBossStage@5500")
static int g_nwarps, g_next;
static GuestFn g_control;
static bool g_log;

static void warp_now(Cpu *c, uint64_t scene) {
    uint64_t holder = *(uint64_t *)(uintptr_t)(scene + STAGE_SCENE_HOLDER);
    if (!holder) return;
    // Resolve every helper before touching the guest stack. The 1.4.1 names come
    // from the build-ID-gated table; unknown builds still fail without dispatching.
    uint64_t world = loader_find_symbol("_ZN16GameDataFunction35tryChangeNextStageWithDemoWorldWarpE20GameDataHolderWriterPKc");
    uint64_t ctor = loader_find_symbol("_ZN15ChangeStageInfoC1EPK14GameDataHolderPKcS4_biNS_15SubScenarioTypeE");
    uint64_t change = loader_find_symbol("_ZN16GameDataFunction18tryChangeNextStageE20GameDataHolderWriterPK15ChangeStageInfo");
    if (g_warps[g_next].world ? !world : (!ctor || !change)) {
        LOG("[warp] required helpers unavailable for this game build: warps off\n");
        g_nwarps = g_next;
        return;
    }
    const Warp g_warp = g_warps[g_next++];
    Cpu saved = *c;
    uint64_t sp = c->sp;
    // below the caller's frame: the ChangeStageInfo, then both names; the calls' frames go below all of it
    uint64_t base = (sp - 0x400) & ~0xFull, info = base, entrance = base + 0x280, stage = base + 0x300;
    memset((void *)(uintptr_t)base, 0, sp - base);
    memcpy((void *)(uintptr_t)entrance, g_warp.entrance, strlen(g_warp.entrance) + 1);
    memcpy((void *)(uintptr_t)stage, g_warp.stage, strlen(g_warp.stage) + 1);
    c->sp = base;
    if (g_warp.world) {
        // another kingdom: what the Odyssey's takeoff does (next stage with the world warp demo), then the stage-change
        // flag (GameDataHolder+0x49), so rs::tryChangeNextStage ends the scene and HakoniwaSequence flies there
        c->x[0] = holder, c->x[1] = stage;
        guest_call(c, world);
        *(uint8_t *)(uintptr_t)(holder + 0x49) = 1;
    } else {
        c->x[0] = info, c->x[1] = holder, c->x[2] = entrance, c->x[3] = stage;
        c->x[4] = 0, c->x[5] = (uint32_t)g_warp.scenario, c->x[6] = 0;  // (not a return, scenario, no sub scenario)
        guest_call(c, ctor);
        c->x[0] = holder, c->x[1] = info;
        guest_call(c, change);
    }
    LOG("[warp] frame %llu: %s (entrance \"%s\", scenario %d) -> %s\n", (unsigned long long)renderer_present_count(),
        g_warp.stage, g_warp.entrance, g_warp.scenario, (c->x[0] & 1) ? "requested" : "refused");
    *c = saved;
}

// ---- ODYSSEY_SAVE_COMPLETE=<frame>: from that frame, once, the playing save becomes a completed one, done the game's own
// way (GameDataFile, OdysseyDecomp's names; offsets from the disassembly): every Power Moon of its moon list (hints at
// +0x9a0, 0x238 bytes each, a stage name each: setGotShine(index)), every kingdom unlocked, the game cleared, all moons
// paid to the Odyssey, each kingdom in its state after the ending - with the moon rock open where it has one (the
// world's scenario, +0x7d8) - every shop item bought and the coins full; then the game writes the save (startSaveDataWrite;
// purple coins stay as they were: the game marks those by their placement). For
// showing the kingdoms: build it on a copy (ODYSSEY_SAVE_DIR), never on a save someone plays.
static uint64_t g_complete_at;  // (frame + 1; 0: off)

static uint64_t gcall(Cpu *c, const char *sym, uint64_t a0, uint64_t a1) {
    c->x[0] = a0, c->x[1] = a1;
    guest_call(c, guest_sym(sym));
    return c->x[0];
}

static void complete_save(Cpu *c, uint64_t scene) {
    uint64_t holder = *(uint64_t *)(uintptr_t)(scene + STAGE_SCENE_HOLDER);
    if (!holder) return;
    uint64_t x[31], lr = c->x[30], sp = c->sp;
    memcpy(x, c->x, sizeof x);
    c->sp = (sp - 0x400) & ~0xFull;  // (the calls' frames below the caller's)
    uint64_t file = gcall(c, "_ZN16GameDataFunction15getGameDataFileE20GameDataHolderWriter", holder, 0);
    uint64_t hints = *(uint64_t *)(uintptr_t)(file + 0x9a0);
    int moons = 0, before = (int32_t)gcall(c, "_ZNK12GameDataFile11getShineNumEv", file, 0);
    for (int i = 0; i < 1024 && hints; i++) {
        const char *stage = *(const char **)(uintptr_t)(hints + (uint64_t)i * 0x238 + 8);  // (HintInfo::stageName)
        if (!stage || !stage[0]) continue;
        gcall(c, "_ZN12GameDataFile11setGotShineEi", file, (uint32_t)i);
        moons++;
    }
    LOG("[save] moons got: %d before, %d after setting %d\n", before, (int32_t)gcall(c, "_ZNK12GameDataFile11getShineNumEv", file, 0),
        moons);
    {   // (per kingdom: moons in the list, got - HintInfo::worldId +0x1cc, isGet +0x1d1)
        int total[32] = {0}, got[32] = {0};
        for (int i = 0; i < 1024 && hints; i++) {
            uint64_t h = hints + (uint64_t)i * 0x238;
            const char *stage = *(const char **)(uintptr_t)(h + 8);
            int w = *(int32_t *)(uintptr_t)(h + 0x1cc);
            if (!stage || !stage[0] || w < 0 || w >= 32) continue;
            total[w]++;
            got[w] += *(uint8_t *)(uintptr_t)(h + 0x1d1) != 0;
        }
        for (int w = 0; w < 32; w++) if (total[w]) LOG("[save]   kingdom %d: %d of %d in the list got\n", w, got[w], total[w]);
    }
    uint64_t worlds = *(uint64_t *)(uintptr_t)(*(uint64_t *)(uintptr_t)(file + 0x850) + 0x190);  // holder's WorldList
    int nworld = (int32_t)gcall(c, "_ZNK9WorldList11getWorldNumEv", worlds, 0);
    for (int w = 0; w < nworld; w++) gcall(c, "_ZN12GameDataFile11unlockWorldEi", file, (uint32_t)w);
    gcall(c, "_ZN12GameDataFile12setGameClearEv", file, 0);
    gcall(c, "_ZN12GameDataFile21addPayShineCurrentAllEv", file, 0);  // (every kingdom's, despite the name)
    int32_t *scenario = *(int32_t **)(uintptr_t)(file + 0x7d8);
    for (int w = 0; w < nworld && scenario; w++) {
        int rock = (int32_t)gcall(c, "_ZNK9WorldList21getMoonRockScenarioNoEi", worlds, (uint32_t)w);
        int after = (int32_t)gcall(c, "_ZNK9WorldList24getAfterEndingScenarioNoEi", worlds, (uint32_t)w);
        const char *name = (const char *)(uintptr_t)gcall(c, "_ZNK9WorldList16getMainStageNameEi", worlds, (uint32_t)w);
        int s = rock > 0 ? rock : after > 0 ? after : scenario[w];
        LOG("[save] kingdom %d %s: scenario %d -> %d (after the ending %d, moon rock open %d)\n", w, name ? name : "?",
            scenario[w], s, after, rock);
        scenario[w] = s;
    }
    gcall(c, "_ZN12GameDataFile10buyItemAllEv", file, 0);
    gcall(c, "_ZN12GameDataFile7addCoinEi", file, 9999);  // (the game caps it: 9999)
    // each moon got blocks saving until its moon-get demo ends (setGotShine: invalidateSaveForMoonGet) - without it
    // the save written was the one from before (9 bytes different: the time)
    gcall(c, "_ZN14GameDataHolder22validateSaveForMoonGetEv", holder, 0);
    gcall(c, "_ZN22SaveDataAccessFunction18startSaveDataWriteEP14GameDataHolder", holder, 0);
    LOG("[save] frame %llu: completed save - %d moons of the moon list got, %d kingdoms; writing it\n",
        (unsigned long long)renderer_present_count(), moons, nworld);
    memcpy(c->x, x, sizeof x);
    c->x[30] = lr, c->sp = sp;
}

// Scene warm-up for the renderer's shader builds (vulkan/exec.c sync_shaders): a stage scene that started less than
// WARMUP_FRAMES frames ago, or no stage scene running (title, loading between stages), builds new shaders blocking -
// Odyssey draws some targets once only, right at a scene's start. In play, builds go to the background.
#define WARMUP_FRAMES 120
static _Atomic uint64_t g_scene_start, g_last_control;  // (frames + 1, 0 = never)
static _Atomic uint64_t g_first_scene_start;
static _Atomic bool g_scene_new;  // StageScene::init ran: the next control call starts a scene (often at the old address)
static GuestFn g_init;
extern void input_motion_settle(void);

bool odyssey_scene_warming(void) {
    uint64_t now = renderer_present_count() + 1, last = g_last_control, start = g_scene_start;
    return !last || now - last > 2 || now - start < WARMUP_FRAMES;
}

// First save load only. A title screen can idle indefinitely before this window starts.
int odyssey_first_scene_warmup(void) {
    uint64_t start = g_first_scene_start;
    if (!start) return g_scene_new ? 1 : 0;
    return renderer_present_count() + 1 - start < WARMUP_FRAMES ? 1 : 2;
}

static void stage_scene_init(Cpu *c) {
    input_motion_settle();
    g_scene_new = true;
    g_init(c);
}

static void stage_scene_control(Cpu *c) {
    uint64_t now = renderer_present_count() + 1;
    if (g_scene_new) {
        if (!g_first_scene_start) g_first_scene_start = now;
        g_scene_new = false;
        g_scene_start = now;
        input_motion_settle(); // the first playable update after loading a save/stage
        if (g_log) {
            uint64_t current = loader_find_symbol("_ZN16GameDataFunction19getCurrentStageNameE22GameDataHolderAccessor");
            char name[128] = "unknown";
            if (current) {
                Cpu saved = *c;
                c->x[0] = *(uint64_t *)(uintptr_t)(c->x[0] + STAGE_SCENE_HOLDER);
                c->sp = (c->sp - 0x400) & ~0xFull;
                guest_call(c, current);
                uint64_t ptr = c->x[0];
                if (ptr >= GUEST_LO && ptr < GUEST_HI - sizeof name)
                    snprintf(name, sizeof name, "%.*s", (int)sizeof name - 1, (const char *)(uintptr_t)ptr);
                *c = saved;
            }
            LOG("[warp] stage scene %llx running at frame %llu stage \"%s\"\n",
                (unsigned long long)c->x[0], (unsigned long long)now - 1, name);
        }
    }
    g_last_control = now;
    if (g_complete_at && now >= g_complete_at && now - g_scene_start > 60) {  // (a scene well under way)
        g_complete_at = 0;
        complete_save(c, c->x[0]);
    }
    static int log_save = -1;  // ODYSSEY_LOG_SAVE=1: once a scene runs, each kingdom's state in the playing save
    if (log_save < 0) { log_save = getenv("ODYSSEY_LOG_SAVE") != NULL; if (log_save) LOG("[save] will log the save's kingdoms\n"); }
    if (log_save == 1 && now - g_scene_start > 60) {
        log_save = 2;
        uint64_t holder = *(uint64_t *)(uintptr_t)(c->x[0] + STAGE_SCENE_HOLDER), x[31], lr = c->x[30], sp = c->sp;
        memcpy(x, c->x, sizeof x);
        c->sp = (sp - 0x400) & ~0xFull;
        uint64_t file = gcall(c, "_ZN16GameDataFunction15getGameDataFileE20GameDataHolderWriter", holder, 0);
        uint64_t worlds = *(uint64_t *)(uintptr_t)(*(uint64_t *)(uintptr_t)(file + 0x850) + 0x190);
        int nworld = (int32_t)gcall(c, "_ZNK9WorldList11getWorldNumEv", worlds, 0);
        for (int w = 0; w < nworld; w++) {
            const char *name = (const char *)(uintptr_t)gcall(c, "_ZNK9WorldList16getMainStageNameEi", worlds, (uint32_t)w);
            int unlocked = (int)(gcall(c, "_ZNK12GameDataFile15isUnlockedWorldEi", file, (uint32_t)w) & 1);
            int moons = (int32_t)gcall(c, "_ZNK12GameDataFile11getShineNumEi", file, (uint32_t)w);
            LOG("[save] kingdom %d %s: unlocked %d, scenario %d, moons %d\n", w, name ? name : "?", unlocked,
                (*(int32_t **)(uintptr_t)(file + 0x7d8))[w], moons);
        }
        memcpy(c->x, x, sizeof x);
        c->x[30] = lr, c->sp = sp;
    }
    {   // ODYSSEY_LOG_STAGES=2: a line every 600 frames the scene runs (is it running?)
        static int every = -1;
        if (every < 0) every = getenv("ODYSSEY_LOG_STAGES") && atoi(getenv("ODYSSEY_LOG_STAGES")) == 2;
        if (every && !(now % 600)) LOG("[warp] scene %llx at frame %llu, warp %d of %d next at %llu\n", (unsigned long long)c->x[0],
                                       (unsigned long long)now, g_next, g_nwarps, g_next < g_nwarps ? (unsigned long long)g_warps[g_next].at : 0ull);
    }
    if (g_next < g_nwarps && renderer_present_count() >= g_warps[g_next].at) warp_now(c, c->x[0]);
    g_control(c);
}

// ---- ODYSSEY_LOG_LINREAD=1 (debug): al::getF32FromLinearTextureF16 / getColorFromLinearTexture - the CPU reading a
// texel of a linear texture (the fluid surfaces' height, al::FluidSimulateWave::tryCalcHeight; the poison's paint map,
// PaintObj::checkPaintAlpha). Each texture once: its memory (pool storage + offset), pitch, and which GPU textures
// overlap that memory - what has to be written back for the CPU to see what the GPU drew.
static GuestFn g_linread_f16, g_linread_color;
static void linread_log(Cpu *c, const char *what) {
    static uint64_t seen[64];
    static int nseen;
    uint64_t td = c->x[0];
    if (!td) return;
    for (int i = 0; i < nseen; i++) if (seen[i] == td) return;
    if (nseen < 64) seen[nseen++] = td;
    uint64_t pool = *(uint64_t *)(uintptr_t)td, off = *(uint32_t *)(uintptr_t)(td + 8);
    uint32_t pitch = *(uint32_t *)(uintptr_t)(td + 0x4c);
    uint64_t storage = pool ? HOST(NvnPool, pool)->storage : 0;
    LOG("[linread] %s: TextureData %llx pool %llx storage %llx + %#llx = %llx, pitch %u (x %lld y %lld)\n", what,
        (unsigned long long)td, (unsigned long long)pool, (unsigned long long)storage, (unsigned long long)off,
        (unsigned long long)(storage + off), pitch, (long long)(int32_t)c->x[1], (long long)(int32_t)c->x[2]);
    extern void nvn_log_overlapping_textures(const NvnTexture *self, uint64_t addr, uint64_t size);
    if (storage) nvn_log_overlapping_textures(NULL, storage + off, (uint64_t)pitch * 256);
}
static void linread_f16(Cpu *c) { linread_log(c, "F32 from F16"); g_linread_f16(c); }
static void linread_color(Cpu *c) { linread_log(c, "color"); g_linread_color(c); }
static void linread_boot(void) {
    static bool done;
    if (done || !getenv("ODYSSEY_LOG_LINREAD")) return;
    done = true;
    bool v141 = loader_main_is_v141();
    // (1.4.1: tools/match141.py - getF32FromLinearTextureF16 by call graph; getColorFromLinearTexture its neighbour,
    // the best candidate)
    uint64_t f16 = v141 ? 0x70007ac5f0ull : loader_find_symbol("_ZN2al26getF32FromLinearTextureF16EPKN3agl11TextureDataEiiii");
    uint64_t col = v141 ? 0x70007ac560ull : loader_find_symbol("_ZN2al25getColorFromLinearTextureEPKN3agl11TextureDataEii");
    if (f16 && (g_linread_f16 = dispatch_lookup(f16))) hle_override(f16, linread_f16, "linear texture read (log)");
    if (col && (g_linread_color = dispatch_lookup(col))) hle_override(col, linread_color, "linear texture colour read (log)");
    LOG("[linread] hooks %s %s\n", g_linread_f16 ? "f16" : "-", g_linread_color ? "color" : "-");
}

// ---- ODYSSEY_TRACE_CALLS=<addr>[,<addr>...] (debug, up to 8): each call of those game functions logged - caller (lr),
// x0..x3, s0..s3, the three floats x1 points at (a position, when it is one), and w0 on return. The first 40 calls of
// each, then every 200th.
static uint64_t g_tc_addr[8];
static GuestFn g_tc_orig[8];
static uint64_t g_tc_n[8];
static void tc_call(Cpu *c, int k) {
    uint64_t n = ++g_tc_n[k], lr = c->x[30], x0 = c->x[0], x1 = c->x[1], x2 = c->x[2], x3 = c->x[3];
    bool show = n <= 40 || !(n % 200);
    float pos[3] = {0, 0, 0}, sv[4];
    for (int i = 0; i < 4; i++) memcpy(&sv[i], &c->v[i], 4);
    if (x1 >= GUEST_LO && x1 < GUEST_HI - 12) memcpy(pos, (const void *)(uintptr_t)x1, 12);
    float in2[3] = {0, 0, 0}, out1[3] = {0, 0, 0};  // (x2's vec3 before; x1's after: an out parameter)
    if (x2 >= GUEST_LO && x2 < GUEST_HI - 12) memcpy(in2, (const void *)(uintptr_t)x2, 12);
    g_tc_orig[k](c);
    if (x1 >= GUEST_LO && x1 < GUEST_HI - 12) memcpy(out1, (const void *)(uintptr_t)x1, 12);
    float s0out;
    memcpy(&s0out, &c->v[0], 4);
    if (show)
        LOG("[call] %llx #%llu lr %llx x0 %llx x1 %llx (%g %g %g -> %g %g %g) x2 %llx (%g %g %g) s0-3 %g %g %g %g -> w0 %u s0 %g\n",
            (unsigned long long)g_tc_addr[k], (unsigned long long)n, (unsigned long long)lr, (unsigned long long)x0,
            (unsigned long long)x1, pos[0], pos[1], pos[2], out1[0], out1[1], out1[2], (unsigned long long)x2, in2[0], in2[1],
            in2[2], sv[0], sv[1], sv[2], sv[3], (unsigned)(c->x[0] & 0xFFFFFFFFu), s0out);
}
#define TC(k) static void tc_##k(Cpu *c) { tc_call(c, k); }
TC(0) TC(1) TC(2) TC(3) TC(4) TC(5) TC(6) TC(7)
static void (*const g_tc_fn[8])(Cpu *) = {tc_0, tc_1, tc_2, tc_3, tc_4, tc_5, tc_6, tc_7};
static void trace_calls_boot(void) {
    static bool done;
    const char *e = getenv("ODYSSEY_TRACE_CALLS");
    if (done || !e) return;
    done = true;
    for (int k = 0; k < 8 && e && *e; k++) {
        char *end;
        uint64_t a = strtoull(e, &end, 16);
        e = *end == ',' ? end + 1 : NULL;
        if (!a || !(g_tc_orig[k] = dispatch_lookup(a))) { LOG("[call] %llx: no function there\n", (unsigned long long)a); continue; }
        g_tc_addr[k] = a;
        hle_override(a, g_tc_fn[k], "call trace");
    }
}

void odyssey_boot(void) {
    linread_boot();
    trace_calls_boot();
    const char *e = getenv("ODYSSEY_WARP");
    g_log = (e && *e) || getenv("ODYSSEY_LOG_STAGES");
    g_complete_at = getenv("ODYSSEY_SAVE_COMPLETE") ? strtoull(getenv("ODYSSEY_SAVE_COMPLETE"), NULL, 10) + 1 : 0;
    g_nwarps = g_next = 0;
    for (const char *p = e; p && *p && g_nwarps < (int)(sizeof g_warps / sizeof *g_warps);) {  // (';' between the warps of a sequence)
        const char *end = strchr(p, ';');
        char spec[300];
        snprintf(spec, sizeof spec, "%.*s", end ? (int)(end - p) : (int)strlen(p), p);
        p = end ? end + 1 : NULL;
        Warp *w = &g_warps[g_nwarps];
        memset(w, 0, sizeof *w);
        w->world = !strncmp(spec, "world:", 6);
        if (w->world) memmove(spec, spec + 6, strlen(spec + 6) + 1);
        char *at = strchr(spec, '@');
        w->at = at ? strtoull(at + 1, NULL, 10) : 0;
        if (at) *at = 0;
        char *ent = strchr(spec, ':'), *scn = ent ? strchr(ent + 1, ':') : NULL;
        w->scenario = scn ? atoi(scn + 1) : -1;
        if (scn) *scn = 0;
        snprintf(w->entrance, sizeof w->entrance, "%s", ent ? ent + 1 : "");
        if (ent) *ent = 0;
        snprintf(w->stage, sizeof w->stage, "%s", spec);
        if (!w->stage[0]) continue;
        LOG("[warp] armed: %s%s (entrance \"%s\", scenario %d) from frame %llu\n", w->world ? "world " : "", w->stage,
            w->entrance, w->scenario, (unsigned long long)w->at);
        g_nwarps++;
    }
    static bool hooked;  // (always: the scene warm-up needs it too)
    if (hooked) return;
    hooked = true;
    uint64_t control = loader_find_symbol("_ZN10StageScene7controlEv"), init = loader_find_symbol("_ZN10StageScene4initERKN2al13SceneInitInfoE");
    if (!control || !init) {  // (a version without names: no warps, and every scene is warmed up as the title is)
        LOG("[warp] StageScene::control / init not found: no stage hooks\n");
        return;
    }
    g_control = dispatch_lookup(control);
    g_init = dispatch_lookup(init);
    hle_override(control, stage_scene_control, "StageScene::control (hooks)");
    hle_override(init, stage_scene_init, "StageScene::init (hooks)");
}
