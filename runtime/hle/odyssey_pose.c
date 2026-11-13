// Odyssey 1.0.0: evaluate skeletal clips between gameplay ticks for the native draws (120 FPS: the draw-only frame at
// the half tick; 90 FPS: every draw at its own fraction of a tick, runtime/hle/odyssey_120.c)
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "odyssey_v141.h"
#include "../rt.h"
#include "../odyssey_pose.h"
#include "../nvn/rqueue.h"

enum {
    MAX_MODELS = 128, MAX_TRACKS = 8, MAX_SAMPLES = 24, MAX_BONES = 512, MAX_SLOTS = 4,
    LOCAL_STRIDE = 0x60, WORLD_STRIDE = 0x40, PALETTE_STRIDE = 48,
    MAX_PALETTE_BYTES = MAX_BONES * PALETTE_STRIDE + 256,
};

typedef struct {
    uint64_t keeper, player, ctrl;
    float position[3], step[3];
} Model;

typedef struct {
    uint64_t skeleton, player, ctrl;
    uint8_t *local, *world;
    unsigned bones, track_count;
    float *tracks[MAX_TRACKS];
    float frames[MAX_TRACKS];
} Pose;

typedef struct {
    uint64_t addresses[MAX_SLOTS];
    uint32_t size;
    uint8_t data[];
} Palette;

struct OdysseyPoseFrame {
    unsigned count;
    Palette *palettes[MAX_SAMPLES];
};

static __thread Model models[MAX_MODELS];
static __thread Model previous_models[MAX_MODELS];
static __thread unsigned model_count;
static __thread unsigned previous_count;
static __thread uint64_t previous_tick;
static __thread bool capturing;
static __thread uint64_t player_keeper;
static uint64_t animation_calc, skeleton_world, skeleton_calc, palette_map;

// Owned by the render thread until present, or the game thread with synchronous rendering.
static OdysseyPoseFrame *render_frame;

static uint64_t pointer_at(uint64_t address) {
    return *(const uint64_t *)(uintptr_t)address;
}

bool odyssey_pose_enabled(void) {
#ifdef __APPLE__
    return false;
#else
    static int enabled = -1;
    if (enabled < 0) {
        const char *mode = getenv("ODYSSEY_POSE_120");
        enabled = !mode || !strcmp(mode, "1");
        if (enabled) {
            animation_calc = loader_find_symbol("_ZN2al13AnimPlayerSkl11calcSklAnimEv");
            skeleton_world = loader_find_symbol("_ZN2nn3g3d11SkeletonObj17CalculateWorldMtxERKNS_4util4neon22MatrixRowMajor4x3fTypeE");
            skeleton_calc = loader_find_symbol("_ZN2nn3g3d11SkeletonObj17CalculateSkeletonEi");
            // nn::gfx buffer Map in CalculateSkeleton (1.4.1: runtime/odyssey_v141.h; there CalculateWorldMtx is inlined
            // into its callers, so skeleton_world is 0 and the sampling stays off until it has another hook)
            palette_map = loader_main_is_v141() ? ODYSSEY_V141_BUFFER_MAP : 0x7000ae6e40ull;
            enabled = animation_calc && skeleton_world && skeleton_calc && dispatch_lookup(animation_calc) &&
                      dispatch_lookup(skeleton_world) && dispatch_lookup(skeleton_calc) && dispatch_lookup(palette_map);
            if (enabled) LOG("[pose-120] native skeletal sampling enabled\n");
        }
    }
    return enabled;
#endif
}

void odyssey_pose_begin(void) {
    model_count = 1;
    models[0] = (Model){0};
    player_keeper = 0;
    capturing = true;
}

void odyssey_pose_end(uint64_t tick) {
    capturing = false;
    if (!odyssey_motion_enabled()) return;
    unsigned count = tick == previous_tick + 1 ? previous_count : 0;
    for (unsigned i = 0; i < model_count; i++) {
        Model *model = &models[i];
        if (!model->keeper) continue;
        model->ctrl = pointer_at(model->keeper + 0x10);
        if (!model->ctrl) continue;
        const float *base = (const float *)(uintptr_t)(model->ctrl + 0x3c8);
        for (unsigned j = 0; j < 3; j++) model->position[j] = base[4 * j + 3];
        for (unsigned j = 0; j < count; j++) {
            const Model *last = &previous_models[j];
            if (last->keeper != model->keeper || last->player != model->player || last->ctrl != model->ctrl) continue;
            float step[3], travel = 0.0f;
            for (unsigned axis = 0; axis < 3; axis++) {
                step[axis] = model->position[axis] - last->position[axis];  // (a whole tick's movement)
                travel += step[axis] * step[axis];
            }
            // A teleport or an asset swap should show the new position immediately.
            if (travel <= 60.0f * 60.0f) memcpy(model->step, step, sizeof step);
            break;
        }
    }
    memcpy(previous_models, models, model_count * sizeof *models);
    previous_count = model_count;
    previous_tick = tick;
}

void odyssey_pose_player(uint64_t actor) {
    if (!capturing) return;
    // PlayerAnimator::updateModel gets the active costume actor from its model holder.
    uint64_t animator = pointer_at(actor + 0x168);
    uint64_t changer = animator ? pointer_at(animator) : 0;
    uint64_t holder = changer ? pointer_at(changer + 0x10) : 0;
    uint64_t costume = holder ? pointer_at(holder + 0x98) : 0;
    player_keeper = costume ? pointer_at(costume + 0x88) : 0;
}

void odyssey_pose_capture(Cpu *c) {
    if (!capturing) return;
    uint64_t keeper = c->x[0];
    uint64_t player = pointer_at(keeper + 0x20);
    static uint64_t skl_vtable;
    if (!skl_vtable) skl_vtable = loader_main_is_v141() ? ODYSSEY_V141_ANIMPLAYERSKL_VTABLE : 0x7001dd63f8ull;
    if (!player || pointer_at(player) != skl_vtable) return;  // al::AnimPlayerSkl
    unsigned i = keeper == player_keeper ? 0 : 1;
    while (i && i < model_count && models[i].keeper != keeper) i++;
    if (i == MAX_MODELS) return;
    if (i == model_count) model_count++;
    models[i] = (Model){.keeper = keeper, .player = player};
}

static bool find_pose(Pose *pose, Model model) {
    if (!model.keeper || pointer_at(model.keeper + 0x20) != model.player) return false;
    uint64_t ctrl = pointer_at(model.keeper + 0x10);
    uint64_t model_obj = ctrl ? pointer_at(ctrl) : 0;
    uint64_t skeleton = model_obj ? pointer_at(model_obj + 0x38) : 0;
    if (!skeleton) return false;
    unsigned bones = *(const uint16_t *)(uintptr_t)(skeleton + 0x50);
    uint8_t *local = (void *)(uintptr_t)pointer_at(skeleton + 0x18);
    uint8_t *world = (void *)(uintptr_t)pointer_at(skeleton + 0x20);
    if (!local || !world || !bones || bones > MAX_BONES) return false;
    *pose = (Pose){.skeleton = skeleton, .player = model.player, .ctrl = ctrl, .bones = bones, .local = local, .world = world};
    return true;
}

static bool advance_tracks(Pose *pose, float lead) {
    unsigned tracks = *(const uint32_t *)(uintptr_t)(pose->player + 0x20);
    uint64_t clips = pointer_at(pose->player + 0x28);
    if (!clips || tracks > MAX_TRACKS) return false;
    for (unsigned i = 0; i < tracks; i++) {
        uint64_t clip = pointer_at(clips + 8 * i);
        float *frame = clip ? (float *)(uintptr_t)pointer_at(clip + 8) : NULL;
        if (!frame) continue;
        float next = frame[0] + lead * frame[3];
        if (!isfinite(next) || next == frame[0] || next < frame[1] || next > frame[2]) continue;
        pose->tracks[pose->track_count] = frame;
        pose->frames[pose->track_count++] = frame[0];
        frame[0] = next;
    }
    return pose->track_count != 0;
}

static void evaluate_animation(Cpu *c, const Pose *pose) {
    Cpu sample = *c;
    sample.x[0] = pose->player;
    guest_call(&sample, animation_calc);
}

static void evaluate_world(Cpu *c, const Pose *pose) {
    // ModelCtrl::calc's base transform and scale, packed for CalculateWorldMtx.
    const float *base = (const float *)(uintptr_t)(pose->ctrl + 0x3c8);
    const float *scale = (const float *)(uintptr_t)(pose->ctrl + 0x3f8);
    float transform[16] = {0};
    for (unsigned row = 0; row < 3; row++) {
        for (unsigned col = 0; col < 3; col++) transform[col * 4 + row] = base[row * 4 + col] * scale[col];
        transform[12 + row] = base[row * 4 + 3];
    }
    Cpu sample = *c;
    sample.sp -= sizeof transform;
    memcpy((void *)(uintptr_t)sample.sp, transform, sizeof transform);
    sample.x[0] = pose->skeleton;
    sample.x[1] = sample.sp;
    guest_call(&sample, skeleton_world);
}

static Palette *sample_palette(Cpu *c, const Pose *pose) {
    unsigned slots = *(const uint32_t *)(uintptr_t)(pose->ctrl + 0x158);
    uint64_t resource = pointer_at(pose->skeleton);
    uint64_t blocks = pointer_at(pose->skeleton + 0x40);
    uint64_t size = pointer_at(pose->skeleton + 0x48);
    if (!slots || slots > MAX_SLOTS || !resource || !blocks || size > MAX_PALETTE_BYTES) return NULL;
    unsigned matrices = *(const uint16_t *)(uintptr_t)(resource + 0x4e) + *(const uint16_t *)(uintptr_t)(resource + 0x50);
    if (!matrices || matrices > MAX_BONES || size < matrices * PALETTE_STRIDE) return NULL;

    Palette *palette = calloc(1, sizeof *palette + size);
    if (!palette) return NULL;
    bool mapped = false;
    palette->size = (uint32_t)size;
    for (unsigned i = 0; i < slots; i++) {
        Cpu map = *c;
        map.x[0] = blocks + 0x48 * i;
        guest_call(&map, palette_map);
        uint64_t address = map.x[0];
        if (address < GUEST_LO || address > GUEST_HI - size) continue;
        palette->addresses[i] = address;
        mapped = true;
    }
    if (!mapped) { free(palette); return NULL; }

    // CalculateSkeleton writes to a copied SDK skeleton and a scratch buffer on the guest stack.
    // The actual ring buffers stay available to earlier queued draws and the next simulation tick.
    typedef struct {
        uint8_t skeleton[0x58];
        uint8_t block[0x48];
        uint64_t nvn_buffer;
        _Alignas(16) uint8_t data[];
    } Scratch;
    Cpu sample = *c;
    sample.sp -= (sizeof(Scratch) + size + 15) & ~15ull;
    Scratch *scratch = (void *)(uintptr_t)sample.sp;
    memcpy(scratch->skeleton, (const void *)(uintptr_t)pose->skeleton, sizeof scratch->skeleton);
    memset(scratch->block, 0, sizeof scratch->block);
    memset(scratch->data, 0, size);
    uint64_t block = (uint64_t)(uintptr_t)scratch->block;
    uint64_t nvn_buffer = (uint64_t)(uintptr_t)&scratch->nvn_buffer;
    memcpy(scratch->skeleton + 0x40, &block, sizeof block);
    memcpy(scratch->block + 8, &nvn_buffer, sizeof nvn_buffer);
    scratch->nvn_buffer = (uint64_t)(uintptr_t)scratch->data;
    sample.x[0] = sample.sp;
    sample.x[1] = 0;
    guest_call(&sample, skeleton_calc);
    memcpy(palette->data, scratch->data, size);
    return palette;
}

static Palette *sample_model(Cpu *c, Model model, float lead) {
    Pose pose;
    if (!find_pose(&pose, model)) return NULL;
    bool animate = advance_tracks(&pose, lead);
    bool move = lead != 0.0f && (model.step[0] != 0.0f || model.step[1] != 0.0f || model.step[2] != 0.0f);
    if (!animate && !move) return NULL;

    // Reuse the game's evaluator. Restore its working matrices and clip state before any gameplay code sees them.
    uint8_t local[MAX_BONES * LOCAL_STRIDE], world[MAX_BONES * WORLD_STRIDE];
    size_t local_size = pose.bones * LOCAL_STRIDE, world_size = pose.bones * WORLD_STRIDE;
    uint16_t *player_flags = (void *)(uintptr_t)(pose.player + 0x10);
    uint16_t *skeleton_flags = (void *)(uintptr_t)(pose.skeleton + 8);
    uint16_t saved_player_flags = *player_flags, saved_skeleton_flags = *skeleton_flags;
    memcpy(world, pose.world, world_size);
    if (animate) {
        memcpy(local, pose.local, local_size);
        evaluate_animation(c, &pose);
        evaluate_world(c, &pose);
    }
    if (move) {
        for (unsigned i = 0; i < pose.bones; i++) {
            float *translation = (float *)(void *)(pose.world + WORLD_STRIDE * i) + 12;
            for (unsigned j = 0; j < 3; j++) translation[j] += lead * model.step[j];
        }
    }
    Palette *palette = memcmp(world, pose.world, world_size) ? sample_palette(c, &pose) : NULL;
    if (palette && move && model.keeper == player_keeper) {
        static int trace = -1;
        static unsigned samples;
        if (trace < 0) trace = getenv("ODYSSEY_NATIVE_120_TRACE") != NULL;
        if (trace && !(samples++ % 15))
            LOG("[motion-120] Mario render offset %.3f %.3f %.3f\n", lead * model.step[0], lead * model.step[1], lead * model.step[2]);
    }

    if (animate) {
        for (unsigned i = 0; i < pose.track_count; i++) *pose.tracks[i] = pose.frames[i];
        evaluate_animation(c, &pose);
        memcpy(pose.local, local, local_size);
    }
    memcpy(pose.world, world, world_size);
    *player_flags = saved_player_flags;
    *skeleton_flags = saved_skeleton_flags;
    return palette;
}

void odyssey_pose_draw(Cpu *c, float lead) {
    OdysseyPoseFrame *frame = NULL;
    for (unsigned i = 0; i < model_count; i++) {
        Palette *palette = sample_model(c, models[i], lead);
        if (!palette) continue;
        if (!frame) {
            frame = calloc(1, sizeof *frame);
            if (!frame) { free(palette); return; }
        }
        frame->palettes[frame->count++] = palette;
        if (frame->count == MAX_SAMPLES) break;
    }
    if (!frame) return;
    if (rq_enabled() && !rq_on_thread()) {
        rq_push(RQ_ODYSSEY_POSE, frame, 0);
        kernel_gate_reenter();
    } else odyssey_pose_render_frame(frame);
}

void odyssey_pose_render_frame(OdysseyPoseFrame *frame) {
    if (render_frame) {
        for (unsigned i = 0; i < render_frame->count; i++) free(render_frame->palettes[i]);
        free(render_frame);
    }
    render_frame = frame;
}

const void *odyssey_pose_render_palette(uint64_t address, uint64_t size) {
    if (!render_frame) return NULL;
    for (unsigned i = 0; i < render_frame->count; i++) {
        const Palette *p = render_frame->palettes[i];
        for (unsigned j = 0; j < MAX_SLOTS; j++) {
            uint64_t start = p->addresses[j];
            if (!start || address < start) continue;
            uint64_t offset = address - start;
            if (offset <= p->size && size <= p->size - offset) return p->data + offset;
        }
    }
    return NULL;
}
