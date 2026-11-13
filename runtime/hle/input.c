// Controller input: SDL keyboard/gamepad -> nn::hid Npad state.
//
// Both go through the player's bindings (runtime/hle/bindings.c, defaults there: keyboard WASD left stick, arrows
// d-pad, K/J/L/I = A/B/X/Y, Q/E = L/R, 1/3 = ZL/ZR, Enter / Backspace = Plus / Minus, numpad 8/4/5/6 right stick;
// controller by position, Switch A = east).
#include <SDL3/SDL.h>
#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../nvn/nvn.h"
#include "../nvn/renderer.h"
#include "../settings.h"
#include "../input_bind.h"
#include "../nvn/kbprompts.h"
#include "motion_gate.h"

enum {
    BTN_A = 0, BTN_B, BTN_X, BTN_Y, BTN_LSTICK, BTN_RSTICK, BTN_L, BTN_R, BTN_ZL, BTN_ZR, BTN_PLUS, BTN_MINUS,
    BTN_LEFT, BTN_UP, BTN_RIGHT, BTN_DOWN
};

_Static_assert(BTN_DOWN == BIND_DOWN && BTN_ZL == BIND_ZL, "bindings follow the button bits");

static _Atomic uint64_t key_buttons, pad_buttons;
static _Atomic uint64_t pending_presses;
static _Thread_local uint64_t tick_buttons;
static _Thread_local bool tick_input_active;
static _Atomic int32_t key_lx, key_ly, key_rx, key_ry, pad_lx, pad_ly, pad_rx, pad_ry;
static uint8_t keys_down[SDL_SCANCODE_COUNT];
static SDL_Gamepad *pad;
static _Atomic int64_t sampling;

// Host keys / buttons held by a test script (ODYSSEY_HOST_INPUT, input_frame): they go through the bindings
static uint8_t test_keys[SDL_SCANCODE_COUNT];
static _Atomic uint32_t test_pad;
static _Atomic int32_t test_lx, test_ly;  // (a resting stick's drift: "lstick" in ODYSSEY_HOST_INPUT)
static bool host_test;  // (then a real controller is left out: the player's own pad must not spoil the test)

static int held_key(int sc) { return sc > 0 && sc < SDL_SCANCODE_COUNT && (keys_down[sc] || test_keys[sc]); }

static void latch_presses(uint64_t buttons) {
    if (buttons && odyssey_native_120_enabled()) atomic_fetch_or(&pending_presses, buttons);
}

// Through the bindings (runtime/hle/bindings.c: g_bind_key), read on every key event, so a rebind applies at once.
static void recompute_keyboard(void) {
    uint64_t b = 0;
    for (int i = 0; i < BIND_PAD_COUNT; i++)
        if (held_key(g_bind_key[i])) b |= 1ull << i;
    latch_presses(b & ~atomic_exchange(&key_buttons, b));
    key_lx = (held_key(g_bind_key[BIND_LS_RIGHT]) - held_key(g_bind_key[BIND_LS_LEFT])) * 32767;
    key_ly = (held_key(g_bind_key[BIND_LS_UP]) - held_key(g_bind_key[BIND_LS_DOWN])) * 32767;
    key_rx = (held_key(g_bind_key[BIND_RS_RIGHT]) - held_key(g_bind_key[BIND_RS_LEFT])) * 32767;
    key_ry = (held_key(g_bind_key[BIND_RS_UP]) - held_key(g_bind_key[BIND_RS_DOWN])) * 32767;
}

// The Controls tab's stick deadzone (g_settings.stick_deadzone, percent): inside that circle a stick is at rest, the
// travel outside it is stretched to the full range (round: a diagonal keeps its direction). The SDL gamepad is read
// directly, so a deadzone set elsewhere (Steam Input's) doesn't apply.
static void deadzone(int32_t *x, int32_t *y) {
    float dz = g_settings.stick_deadzone / 100.0f;
    if (dz <= 0) return;
    float fx = *x / 32767.0f, fy = *y / 32767.0f, r = sqrtf(fx * fx + fy * fy);
    if (r <= dz) { *x = *y = 0; return; }
    float k = fminf((r - dz) / (1 - dz), 1) / r;
    *x = (int32_t)lroundf(fx * k * 32767), *y = (int32_t)lroundf(fy * k * 32767);
}

// Controller buttons (and the triggers as buttons) -> Switch buttons through g_bind_pad; the sticks stay the sticks.
static void recompute_pad(void) {
    uint32_t host = test_pad;
    if (pad && !host_test) {
        for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; i++)
            if (SDL_GetGamepadButton(pad, (SDL_GamepadButton)i)) host |= 1u << i;
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000) host |= 1u << PADB_LTRIGGER;
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000) host |= 1u << PADB_RTRIGGER;
    }
    uint64_t b = 0;
    for (int i = 0; i < BIND_PAD_COUNT; i++)
        if (g_bind_pad[i] >= 0 && g_bind_pad[i] < PADB_HOST_COUNT && (host >> g_bind_pad[i] & 1)) b |= 1ull << i;
    latch_presses(b & ~atomic_exchange(&pad_buttons, b));
    if (!pad || host_test) { pad_lx = test_lx, pad_ly = test_ly, pad_rx = pad_ry = 0; return; }
    int32_t x = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX), y = -SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
    deadzone(&x, &y);
    pad_lx = x, pad_ly = y;
    x = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX), y = -SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY);
    deadzone(&x, &y);
    pad_rx = x, pad_ry = y;
}

// Mouse camera (Controls tab "Mouse camera", `mouse_camera` in powermoon.ini): while the game has the window and no
// menu is up, the pointer is held in the window (SDL relative mode, input_mouse_window) and its movement turns the
// camera: the speed of the movement, sampled every ~12 ms (the game reads it several times a frame), is a stick's
// tilt. MOUSE_FULL counts a second make a full tilt at 100% sensitivity; any movement tilts at least MOUSE_MIN, past
// the camera's own threshold, so a slow movement still turns it.
// The game's camera takes it straight (hle__ZNK18ProjectCameraInput14calcInputStick below), with no stick's limit:
// a quick movement turns the camera as far as the mouse went, up to MOUSE_OVER times a full tilt. Elsewhere (no camera
// asking: the map, menus) it is the right stick, up to full tilt.
// ODYSSEY_MOUSE_FULL / ODYSSEY_MOUSE_MIN (0..0.9) / ODYSSEY_MOUSE_OVER (1..40) override them (tuning).
#define MOUSE_FULL 400.0f
#define MOUSE_MIN 0.25f
#define MOUSE_OVER 10.0f
static _Atomic uint64_t g_camera_asked_ns;  // the game's camera last read its stick (the mouse goes there, not the pad)
static _Atomic int32_t mouse_acc_x, mouse_acc_y;  // movement since the last sample, in 1/16 counts
static _Atomic bool mouse_held;                   // the pointer is held: its movement goes to the camera
static void mouse_add(float dx, float dy) {
    atomic_fetch_add(&mouse_acc_x, (int32_t)lroundf(dx * 16));
    atomic_fetch_add(&mouse_acc_y, (int32_t)lroundf(dy * 16));
}
static void mouse_forget(void) { atomic_store(&mouse_acc_x, 0), atomic_store(&mouse_acc_y, 0); }

// The mouse as a stick now (y up): its tilt, up to MOUSE_OVER (several readers in a frame see the same sample)
static void mouse_tilt(float *ox, float *oy) {
    static float full = 0, mn = -1, over = 0;
    if (mn < 0) {
        const char *f = getenv("ODYSSEY_MOUSE_FULL"), *m = getenv("ODYSSEY_MOUSE_MIN"), *o = getenv("ODYSSEY_MOUSE_OVER");
        full = f && atof(f) > 0 ? (float)atof(f) : MOUSE_FULL;
        mn = m ? fminf(fmaxf((float)atof(m), 0), 0.9f) : MOUSE_MIN;
        over = o ? fminf(fmaxf((float)atof(o), 1), 40) : MOUSE_OVER;
    }
    static uint64_t last;
    static float sx, sy;  // the last sample's tilt
    uint64_t now = plat_now_ns();
    double dt = last ? (now - last) / 1e9 : 0;
    *ox = *oy = 0;
    if (!last || dt > 0.25) { last = now; mouse_forget(); sx = sy = 0; return; }  // (first read, or after a pause)
    if (dt >= 0.012) {
        float dx = atomic_exchange(&mouse_acc_x, 0) / 16.0f, dy = atomic_exchange(&mouse_acc_y, 0) / 16.0f;
        last = now;
        float k = g_settings.mouse_sensitivity / 100.0f / full / (float)dt;  // counts -> tilt
        float vx = dx * k, vy = -dy * k * (g_settings.mouse_invert_y ? -1 : 1);  // (screen y is down)
        float r = sqrtf(vx * vx + vy * vy);
        if (r > 0) {  // the direction kept, the size from MOUSE_MIN up (past 1: as fast as it moved, to MOUSE_OVER)
            float t = r <= 1 ? mn + (1 - mn) * r : fminf(r, over);
            vx *= t / r, vy *= t / r;
        }
        sx = vx, sy = vy;
    }
    *ox = sx, *oy = sy;
}

// The mouse's share of the right stick (nn::hid units), when the game's camera isn't taking it itself
static void mouse_stick(int32_t *rx, int32_t *ry) {
    if (plat_now_ns() - atomic_load(&g_camera_asked_ns) < 100000000ull) return;  // (the camera has it: below)
    float x, y;
    mouse_tilt(&x, &y);
    float r = sqrtf(x * x + y * y);
    if (r > 1) x /= r, y /= r;  // (a stick's reach)
    *rx += (int32_t)lroundf(x * 32767), *ry += (int32_t)lroundf(y * 32767);
}

// The mouse for the game's camera, once per camera update (its several reads in one update come within a moment of
// each other and get the same): exactly the movement since the last update, as the tilt that turns the camera that
// far at 60 updates a second - no smoothing over time and no minimum, so a sideways movement stays sideways and the
// camera goes as far as the mouse did. The camera ignores a tilt under its threshold (0.3, the game's
// CameraPoserFollowLimitFunction::getRotateStickThreshold): a smaller movement is kept and given with the next ones
// once they add up past it (and forgotten after a pause), so slow movements turn the camera too.
#define CAMERA_THRESHOLD 0.32f
static void mouse_camera_frame(float *ox, float *oy) {
    static uint64_t last;
    static float fx, fy, cx, cy;  // this update's tilt; the counts not given yet
    uint64_t now = plat_now_ns();
    if (now - last > 6000000ull) {  // (a new update: more than 6 ms since the last read)
        if (now - last > 250000000ull) cx = cy = 0;  // (after a pause: what was left over is old)
        last = now;
        cx += atomic_exchange(&mouse_acc_x, 0) / 16.0f, cy += atomic_exchange(&mouse_acc_y, 0) / 16.0f;
        static float full = 0;
        if (!full) { const char *f = getenv("ODYSSEY_MOUSE_FULL"); full = f && atof(f) > 0 ? (float)atof(f) : MOUSE_FULL; }
        float k = g_settings.mouse_sensitivity / 100.0f * 60.0f / full;  // counts in a 60th of a second -> tilt
        float vx = cx * k, vy = -cy * k * (g_settings.mouse_invert_y ? -1 : 1);  // (screen y is down)
        float r = sqrtf(vx * vx + vy * vy);
        if (r >= CAMERA_THRESHOLD) {
            if (r > MOUSE_OVER) vx *= MOUSE_OVER / r, vy *= MOUSE_OVER / r;
            fx = vx, fy = vy, cx = cy = 0;
        } else fx = fy = 0;  // (kept for the next update)
    }
    *ox = fx, *oy = fy;
}

// ProjectCameraInput::calcInputStick(this, sead::Vector2f *out) (Odyssey; runtime/hle/hle_symbols.txt): the game's
// camera reads its stick here - the right stick as the player's camera functions make it
// (PlayerCameraFunction::calcCameraMoveInput(out, this->actor), what the game's own body does), and the mouse on top,
// with no stick's limit (mouse_camera_frame). (The poser's options - inverted axes - apply after it, as to the stick.)
void hle__ZNK18ProjectCameraInput14calcInputStickEPN4sead7Vector2IfEE(Cpu *c) {
    uint64_t self = c->x[0], out = c->x[1];
    c->x[0] = out;
    c->x[1] = *(uint64_t *)(uintptr_t)(self + 8);  // (its actor)
    guest_call(c, guest_sym("_ZN20PlayerCameraFunction19calcCameraMoveInputEPN4sead7Vector2IfEEPKN2al9LiveActorE"));
    atomic_store(&g_camera_asked_ns, plat_now_ns());
    extern bool menu_input_captured(void);  // (runtime/ui/menu.mm: a menu has the controls)
    if (!out || menu_input_captured()) return;
    float x, y;
    mouse_camera_frame(&x, &y);
    float *v = (float *)(uintptr_t)out;
    v[0] += x, v[1] += y;
    static int lm = -1, n;
    if (lm < 0) lm = getenv("ODYSSEY_LOG_MOUSE") != NULL;
    if (lm && (x || y) && n++ % 20 == 0) LOG("[input] mouse camera: camera stick %.2f %.2f\n", v[0], v[1]);
}

// Main thread, every pass of the window's event loop: the pointer is held while the game has it (the window in front,
// no menu / name entry / installer up, the setting on); otherwise it is free and shown as usual.
extern bool menu_input_captured(void);  // runtime/ui/menu.mm: overlay menu or name entry up (it has the keys / pad)
void input_mouse_window(SDL_Window *w) {
    bool want = w && g_settings.mouse_camera && !host_test && !menu_input_captured() && SDL_GetKeyboardFocus() == w;
    if (want == SDL_GetWindowRelativeMouseMode(w)) { mouse_held = want; return; }
    if (!SDL_SetWindowRelativeMouseMode(w, want)) {
        static int nlog;
        if (nlog++ < 3) LOG("[input] mouse camera: relative mode %s failed: %s\n", want ? "on" : "off", SDL_GetError());
        return;
    }
    mouse_held = want;
    mouse_forget();
    static int nlog;
    if (nlog++ < 8) LOG("[input] mouse camera: pointer %s\n", want ? "held (moves the camera)" : "free");
}

// Forget held keys (the menu took focus); the gamepad is re-read on its next event.
void input_reset(void) {
    memset(keys_down, 0, sizeof keys_down);
    recompute_keyboard();
    atomic_store(&pending_presses, 0);
    mouse_forget();
}

volatile int g_debug_frame_request;  // F9: dump the next frame's passes (build/pass_*.png) + draw log

// Events a test pushes (ODYSSEY_HOST_INPUT type: / sdlkey: / sdlpad:) carry this device id: they go the whole way, menu first.
#define TEST_DEVICE 0x7E57u
static bool test_event(const SDL_Event *e) {
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: return e->key.which == TEST_DEVICE;
    case SDL_EVENT_TEXT_INPUT: return e->text.windowID == TEST_DEVICE;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP: return e->gbutton.which == TEST_DEVICE;
    default: return false;
    }
}

// Scripted test runs ignore the user's own input (a key or stick touched while a test drives Link spoils it):
// ODYSSEY_NO_USER_INPUT=1, on by default whenever ODYSSEY_INPUT / ODYSSEY_DOORJUMP_TEST / ODYSSEY_PLACE is set
// (or ODYSSEY_HOST_INPUT / ODYSSEY_BIND_TEST; ODYSSEY_NO_USER_INPUT=0 lets it through again).
bool input_user_blocked(const SDL_Event *e) {
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("ODYSSEY_NO_USER_INPUT");
        on = v ? atoi(v) != 0 : (getenv("ODYSSEY_INPUT") || getenv("ODYSSEY_DOORJUMP_TEST") || getenv("ODYSSEY_PLACE") ||
                                 getenv("ODYSSEY_HOST_INPUT") || getenv("ODYSSEY_BIND_TEST"));
        if (on) LOG("[input] scripted run: keyboard, mouse and controllers ignored\n");
    }
    if (!on) return false;
    if (test_event(e)) return false;  // (ODYSSEY_HOST_INPUT type: / sdlkey: / sdlpad: - the test's own presses)
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: case SDL_EVENT_TEXT_INPUT: case SDL_EVENT_TEXT_EDITING:
    case SDL_EVENT_MOUSE_MOTION: case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL:
    case SDL_EVENT_GAMEPAD_ADDED: case SDL_EVENT_GAMEPAD_AXIS_MOTION: case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: case SDL_EVENT_JOYSTICK_ADDED: case SDL_EVENT_JOYSTICK_AXIS_MOTION:
    case SDL_EVENT_JOYSTICK_BUTTON_DOWN: case SDL_EVENT_JOYSTICK_BUTTON_UP:
        return true;
    default:
        return false;
    }
}

// Controller family for the button prompts (runtime/nvn/texpatch.c): 0 Nintendo / other / none, 1 Xbox, 2 PlayStation.
// ODYSSEY_PAD_TYPE=xbox|playstation|nintendo pretends one is attached (tests).
static _Atomic int pad_family;
static void note_family(void) {
    int f = 0;
    if (pad) switch (SDL_GetGamepadType(pad)) {
        case SDL_GAMEPAD_TYPE_XBOX360: case SDL_GAMEPAD_TYPE_XBOXONE: f = 1; break;
        case SDL_GAMEPAD_TYPE_PS3: case SDL_GAMEPAD_TYPE_PS4: case SDL_GAMEPAD_TYPE_PS5: f = 2; break;
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO: case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT: case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: break;
        default:  // (a Steam Deck's own controls, a generic pad: by what its bottom face button says)
            switch (SDL_GetGamepadButtonLabel(pad, SDL_GAMEPAD_BUTTON_SOUTH)) {
            case SDL_GAMEPAD_BUTTON_LABEL_A: f = 1; break;
            case SDL_GAMEPAD_BUTTON_LABEL_CROSS: f = 2; break;
            default: break;
            }
        }
    if (f != pad_family) LOG("[input] button prompts: %s controller\n", f == 1 ? "Xbox" : f == 2 ? "PlayStation" : "Nintendo / other");
    pad_family = f;
}
int input_pad_family(void) {
    static int env = -2;
    if (env == -2) {
        const char *e = getenv("ODYSSEY_PAD_TYPE");
        env = !e ? -1 : !strcmp(e, "xbox") ? 1 : !strcmp(e, "playstation") || !strcmp(e, "ps") ? 2 : 0;
    }
    return env >= 0 ? env : pad_family;
}

// ---------------------------------------------------------------- motion (the game's six-axis sensor)
// Odyssey reads the controller's motion for its motion moves (a shake throws the cap at a homing target, spins,
// tilting): nn::hid GetSixAxisSensorStates. It comes from the active controller's gyro and accelerometer (SDL),
// turned into the Switch's axes and units (as Switch emulators do: acceleration in G, rotation in turns a second,
// the angle and orientation integrated from it). Controls tab "Motion controls" (`motion` in powermoon.ini).
static pthread_mutex_t motion_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    bool on;              // the active pad has a gyro or an accelerometer
    float accel[3], gyro[3], angle[3], dir[9];
    uint64_t gyro_ns;     // the last gyro sample's time (sensor clock)
    int64_t sampling;
    uint64_t last_read_ns;
    MotionGate gate;
    MotionBias bias;
} mot = {.dir = {1, 0, 0, 0, 1, 0, 0, 0, 1},
         .gate.last = {[2] = -1, [9] = 1, [13] = 1, [17] = 1}};

void input_motion_settle(void) {
    pthread_mutex_lock(&motion_lock);
    motion_gate_settle(&mot.gate, plat_now_ns());
    mot.gyro_ns = 0;
    pthread_mutex_unlock(&motion_lock);
}

static void motion_open(void) {
    bool a = false, g = false;
    if (pad) {
        a = SDL_GamepadHasSensor(pad, SDL_SENSOR_ACCEL) && SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_ACCEL, true);
        g = SDL_GamepadHasSensor(pad, SDL_SENSOR_GYRO) && SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_GYRO, true);
    }
    pthread_mutex_lock(&motion_lock);
    mot.on = a || g;
    memset(mot.accel, 0, sizeof mot.accel);
    mot.accel[2] = -1;  // (at rest, lying flat)
    memset(mot.gyro, 0, sizeof mot.gyro);
    memset(mot.angle, 0, sizeof mot.angle);
    static const float id[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    memcpy(mot.dir, id, sizeof id);
    mot.gyro_ns = 0;
    motion_gate_init(&mot.gate);
    memset(&mot.bias, 0, sizeof mot.bias);
    motion_gate_settle(&mot.gate, plat_now_ns());
    pthread_mutex_unlock(&motion_lock);
    if (pad) LOG("[input] motion: %s%s%s\n", a ? "accelerometer" : "", a && g ? " and " : "", g ? "gyro" : a ? "" : "none on this controller");
}

// A rotation of the orientation by w (radians) about each axis: R' = R * exp([w]x) (Rodrigues), kept orthonormal
static void rotate_dir(float *R, const float w[3]) {
    float th = sqrtf(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
    if (th < 1e-9f) return;
    float k[3] = {w[0] / th, w[1] / th, w[2] / th}, c = cosf(th), s = sinf(th), t = 1 - c;
    float M[9] = {t * k[0] * k[0] + c,        t * k[0] * k[1] - s * k[2], t * k[0] * k[2] + s * k[1],
                  t * k[0] * k[1] + s * k[2], t * k[1] * k[1] + c,        t * k[1] * k[2] - s * k[0],
                  t * k[0] * k[2] - s * k[1], t * k[1] * k[2] + s * k[0], t * k[2] * k[2] + c};
    float o[9];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) o[i * 3 + j] = R[i * 3] * M[j] + R[i * 3 + 1] * M[3 + j] + R[i * 3 + 2] * M[6 + j];
    for (int i = 0; i < 3; i++) {  // (Gram-Schmidt on the rows)
        float *a = o + i * 3;
        for (int j = 0; j < i; j++) {
            float *b = o + j * 3, d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
            for (int q = 0; q < 3; q++) a[q] -= d * b[q];
        }
        float n = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
        if (n > 1e-6f) for (int q = 0; q < 3; q++) a[q] /= n;
    }
    memcpy(R, o, sizeof o);
}

static void motion_event(const SDL_GamepadSensorEvent *e) {
    if (!pad || SDL_GetGamepadID(pad) != e->which) return;
    const float G = SDL_STANDARD_GRAVITY, TURN = 6.28318531f;
    pthread_mutex_lock(&motion_lock);
    // SDL: x right, y up, z toward the player (m/s^2, rad/s). The Switch: x = -x, y = z, z = -y (a mirror: the
    // rotation's axes the other way round)
    if (menu_input_captured()) mot.gate.blocked = true;
    if (e->sensor == SDL_SENSOR_ACCEL) {
        mot.accel[0] = -e->data[0] / G, mot.accel[1] = e->data[2] / G, mot.accel[2] = -e->data[1] / G;
    } else if (e->sensor == SDL_SENSOR_GYRO) {
        float g[3] = {e->data[0], -e->data[2], e->data[1]};  // rad/s
        for (int i = 0; i < 3; i++) mot.gyro[i] = g[i] / TURN;
        uint64_t host_now = plat_now_ns();
        motion_bias_sample(&mot.bias, mot.accel, mot.gyro, host_now < mot.gate.settle_until_ns, host_now);
        uint64_t t = e->sensor_timestamp;
        bool integrate = g_settings.motion && !menu_input_captured() && !mot.gate.blocked &&
                         (!mot.gate.resume_ns || plat_now_ns() - mot.gate.resume_ns >= 250000000ull);
        if (integrate && mot.gyro_ns && t > mot.gyro_ns && t - mot.gyro_ns < 100000000ull) {
            float dt = (float)(t - mot.gyro_ns) / 1e9f;
            float w[3] = {mot.gyro[0] * TURN * dt, mot.gyro[1] * TURN * dt, mot.gyro[2] * TURN * dt};
            for (int i = 0; i < 3; i++) mot.angle[i] += mot.gyro[i] * dt;
            rotate_dir(mot.dir, w);
        }
        mot.gyro_ns = t;
    }
    static int log = -1;  // ODYSSEY_LOG_MOTION=1: the readings once a second
    static uint64_t logged;
    if (log < 0) log = getenv("ODYSSEY_LOG_MOTION") != NULL;
    if (log && plat_now_ns() - logged > 1000000000ull) {
        logged = plat_now_ns();
        LOG("[input] motion: accel %.2f %.2f %.2f G, gyro %.3f %.3f %.3f turns/s\n", mot.accel[0], mot.accel[1],
            mot.accel[2], mot.gyro[0], mot.gyro[1], mot.gyro[2]);
    }
    pthread_mutex_unlock(&motion_lock);
}

// GetSixAxisSensorHandles(SixAxisSensorHandle *out, int count, const NpadIdType &id, NpadStyleSet) -> how many:
// one (a Pro Controller's), two for a pair of Joy-Cons. The handle: style index, player, device index.
HLE_FUNC("_ZN2nn3hid23GetSixAxisSensorHandlesEPNS0_19SixAxisSensorHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE",
         hid_sixaxis_handles) {
    uint32_t *out = (uint32_t *)(uintptr_t)c->x[0];
    int count = (int)c->x[1];
    uint32_t id = *(uint32_t *)(uintptr_t)c->x[2], style = (uint32_t)c->x[3];
    int n = style & 4 ? 2 : 1;  // (JoyDual: left and right)
    if (n > count) n = count < 0 ? 0 : count;
    for (int i = 0; i < n; i++) out[i] = (style & 4 ? 5u : style & 2 ? 4u : 3u) | (id & 0xFF) << 8 | (uint32_t)i << 16;
    static int nlog;
    if (nlog++ < 4) LOG("[input] six-axis handles for player %u, style %#x: %d\n", id, style, n);
    c->x[0] = (uint64_t)n;
}
HLE_FUNC("_ZN2nn3hid18StartSixAxisSensorERKNS0_19SixAxisSensorHandleE", hid_sixaxis_start) {
    input_motion_settle();
    static int nlog;
    if (nlog++ < 4) LOG("[input] six-axis sensor %#x started\n", *(uint32_t *)(uintptr_t)c->x[0]);
}

// GetSixAxisSensorStates(SixAxisSensorState *out, int count, const SixAxisSensorHandle &) -> how many, newest first.
// SixAxisSensorState (0x60): s64 delta time (ns), s64 sampling number, float acceleration[3], angular velocity[3],
// angle[3], direction[3][3], u32 attributes (1: connected), u32 reserved.
HLE_FUNC("_ZN2nn3hid22GetSixAxisSensorStatesEPNS0_18SixAxisSensorStateEiRKNS0_19SixAxisSensorHandleE", hid_sixaxis_states) {
    uint8_t *s = (uint8_t *)(uintptr_t)c->x[0];
    if ((int)c->x[1] < 1) { c->x[0] = 0; return; }
    memset(s, 0, 0x60);
    uint64_t now = plat_now_ns();
    pthread_mutex_lock(&motion_lock);
    bool live = mot.on && g_settings.motion && !menu_input_captured();
    if (!mot.last_read_ns || (now > mot.last_read_ns && now - mot.last_read_ns > 500000000ull)) {
        motion_gate_settle(&mot.gate, now);
        mot.gyro_ns = 0;
    }
    *(int64_t *)s = mot.last_read_ns && now > mot.last_read_ns ? (int64_t)(now - mot.last_read_ns) : 5000000;
    mot.last_read_ns = now;
    *(int64_t *)(s + 8) = ++mot.sampling;
    float *f = (float *)(s + 0x10);
    if (live) {
        memcpy(f, mot.accel, 12);
        memcpy(f + 3, mot.gyro, 12);
        memcpy(f + 6, mot.angle, 12);
        memcpy(f + 9, mot.dir, 36);
    } else {
        f[2] = -1;  // (no motion: a controller lying still, so the game sees no shake)
        f[9] = f[13] = f[17] = 1;
    }
    // Preserve the last pose rather than injecting a sudden gravity/orientation
    // change when the menu takes control; discard menu movement on resume.
    motion_gate_sample(&mot.gate, f, !live, now);
    *(uint32_t *)(s + 0x58) = 1;
    {   // ODYSSEY_LOG_MOTION=1: what the game reads, once a second (how many samples it asks for, the values)
        static int log = -1;
        static uint64_t logged;
        if (log < 0) log = getenv("ODYSSEY_LOG_MOTION") != NULL;
        if (log && now - logged > 1000000000ull) {
            logged = now;
            LOG("[input] game reads motion (asks %d, handle %#x, %s): accel %.2f %.2f %.2f gyro %.3f %.3f %.3f\n", (int)c->x[1],
                *(uint32_t *)(uintptr_t)c->x[2], live ? "live" : "at rest", f[0], f[1], f[2], f[3], f[4], f[5]);
        }
    }
    pthread_mutex_unlock(&motion_lock);
    c->x[0] = 1;
}

// The active controller: the first one found, then whichever one a button was last pressed on (a second controller
// takes over at its first press; its icons and motion follow)
static void use_pad(SDL_Gamepad *p) {
    if (!p || p == pad) return;
    pad = p;
    LOG("[input] gamepad: %s\n", SDL_GetGamepadName(pad));
    note_family();
    motion_open();
}

void input_handle_event(const SDL_Event *e) {
    static bool scanned;  // pads present at startup: their ADDED event may have gone to the start screen's loop
    if (!scanned) {
        scanned = true;
        int n = 0;
        SDL_JoystickID *ids = SDL_GetGamepads(&n);
        for (int i = 0; i < n; i++) {
            SDL_Gamepad *p = SDL_OpenGamepad(ids[i]);
            if (!pad) use_pad(p);
        }
        SDL_free(ids);
    }
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP:
        // (a key that plays: not Alt / Ctrl / Shift / Windows or an F key - Alt+Tab, F11 and the like leave a
        // controller player's icons alone)
        if (e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat &&
            !(e->key.scancode >= SDL_SCANCODE_LCTRL && e->key.scancode <= SDL_SCANCODE_RGUI) &&
            !(e->key.scancode >= SDL_SCANCODE_F1 && e->key.scancode <= SDL_SCANCODE_F12))
            kbprompts_note_device(true);
        if (e->type == SDL_EVENT_KEY_DOWN && e->key.scancode == SDL_SCANCODE_F9 && !e->key.repeat) g_debug_frame_request = 1;
        if (e->key.scancode < SDL_SCANCODE_COUNT) keys_down[e->key.scancode] = e->type == SDL_EVENT_KEY_DOWN;
        recompute_keyboard();
        break;
    case SDL_EVENT_GAMEPAD_ADDED:
        {
            SDL_Gamepad *p = SDL_GetGamepadFromID(e->gdevice.which);
            if (!p) p = SDL_OpenGamepad(e->gdevice.which);
            if (!pad) use_pad(p);
        }
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        if (pad && SDL_GetGamepadID(pad) == e->gdevice.which) {
            SDL_CloseGamepad(pad);
            pad = NULL;
            int n = 0;  // (another one still connected takes over)
            SDL_JoystickID *ids = SDL_GetGamepads(&n);
            for (int i = 0; i < n && !pad; i++) if (ids[i] != e->gdevice.which) use_pad(SDL_GetGamepadFromID(ids[i]));
            SDL_free(ids);
            if (!pad) { note_family(); motion_open(); }
        } else {
            SDL_Gamepad *p = SDL_GetGamepadFromID(e->gdevice.which);
            if (p) SDL_CloseGamepad(p);
        }
        recompute_pad();
        break;
    case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:
        motion_event(&e->gsensor);
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        if (!pad || SDL_GetGamepadID(pad) != e->gbutton.which) use_pad(SDL_GetGamepadFromID(e->gbutton.which));
        kbprompts_note_device(false);
        // SDL's live state may already contain the release when its queued press is handled.
        if (pad && SDL_GetGamepadID(pad) == e->gbutton.which) {
            uint64_t buttons = 0;
            for (int i = 0; i < BIND_PAD_COUNT; i++)
                if (g_bind_pad[i] == e->gbutton.button) buttons |= 1ull << i;
            latch_presses(buttons);
        }
        // Fall through to refresh the live state and sticks.
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: case SDL_EVENT_GAMEPAD_BUTTON_UP:
        if (e->type == SDL_EVENT_GAMEPAD_AXIS_MOTION && abs(e->gaxis.value) > 16000) kbprompts_note_device(false);
        recompute_pad();
        break;
    case SDL_EVENT_MOUSE_MOTION:
        if (mouse_held) {
            mouse_add(e->motion.xrel, e->motion.yrel);
            if (fabsf(e->motion.xrel) + fabsf(e->motion.yrel) > 2) kbprompts_note_device(true);  // (not a jitter)
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        kbprompts_note_device(true);
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        input_reset();
        break;
    }
}

// Scripted input for testing: ODYSSEY_INPUT="frame:BUTTON:frames,..." (BUTTON = A,B,X,Y,L,R,ZL,ZR,PLUS,
// MINUS,LEFT,UP,RIGHT,DOWN, or STICK_UP/DOWN/LEFT/RIGHT, RSTICK_UP/DOWN/LEFT/RIGHT, STICK_<degrees>: full tilt at
// that angle, 0 = right, 90 = up).
static _Atomic uint64_t script_buttons;
static _Atomic int32_t script_lx, script_ly, script_rx, script_ry;

// Tests of the bindings, through them like a real player:
// ODYSSEY_HOST_INPUT="frame:key:<SDL key name>:frames,frame:pad:<button>:frames,..." holds a host key / controller
//   button (names as in powermoon.ini's bind_* lines: "Z", "Keypad 0", "south", "ltrigger"...);
//   "frame:lstick:<x>/<y>:frames" holds the controller's left stick there (raw SDL units, y up: a drifting stick);
//   "frame:mouse:<dx>/<dy>:frames" moves the mouse by that many counts each frame (screen y down: the mouse camera).
// "frame:type:<text>" types <text> as real key events (down, text, up per character), through the overlay menu / name
//   entry first like a player's typing; "{Escape}" (any SDL key name) presses that key. No ',' or ':' in <text>.
// "frame:sdlpad:<button>:frames" / "frame:sdlkey:<key>:frames" hold a controller button / key as real events (down
//   at frame, up `frames` later): they reach menus and the on-screen keyboard first, as a player's would.
// "frame:shot:<name>" saves the window (game + menus) to build/<name>.png.
// ODYSSEY_BIND_TEST="frame:pad|key:<input>:<host>,..." starts a rebind capture of <input> (bind_* name: a, zl,
//   ls_up...) at that frame (the Controls tab shows it waiting) and presses <host> 90 frames later ("esc" cancels,
//   "wait" lets it time out); "frame:reset:pad|key" resets to the defaults.
static void push_key(SDL_Scancode sc, SDL_Keymod mod, bool down) {
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.which = TEST_DEVICE, e.key.scancode = sc, e.key.mod = mod, e.key.down = down;
    e.key.key = SDL_GetKeyFromScancode(sc, mod, false);
    SDL_PushEvent(&e);
}
char g_window_shot_request[256];  // test script "shot": a path for the next presented frame
static void test_push(uint64_t frame, unsigned long start, unsigned long len, const char *dev, const char *arg) {
    SDL_Event e;
    memset(&e, 0, sizeof e);
    if (!strcmp(dev, "shot")) {
        extern char g_window_shot_request[256];  // (the macOS renderer saves the next frame there)
        if (frame == start) snprintf(g_window_shot_request, sizeof g_window_shot_request, "build/%s.png", arg);
        return;
    }
    if (!strcmp(dev, "sdlkey")) {
        if (frame == start || frame == start + len) push_key(SDL_GetScancodeFromName(arg), SDL_KMOD_NONE, frame == start);
        return;
    }
    if (!strcmp(dev, "sdlpad")) {
        int b = bind_pad_from_name(arg);
        if (b < 0 || b >= SDL_GAMEPAD_BUTTON_COUNT || (frame != start && frame != start + len)) return;
        e.type = frame == start ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
        e.gbutton.which = TEST_DEVICE, e.gbutton.button = (Uint8)b, e.gbutton.down = frame == start;
        SDL_PushEvent(&e);
        return;
    }
    if (frame != start) return;
    LOG("[input] test: typing \"%s\"\n", arg);
    static char ascii[128][2];  // (a text event keeps a pointer to its text: these live on)
    for (const char *p = arg; *p; p++) {
        if (*p == '{' && strchr(p, '}')) {  // a named key
            char nm[32];
            size_t n = (size_t)(strchr(p, '}') - p - 1);
            snprintf(nm, sizeof nm, "%.*s", (int)(n < sizeof nm ? n : sizeof nm - 1), p + 1);
            SDL_Scancode sc = SDL_GetScancodeFromName(nm);
            push_key(sc, SDL_KMOD_NONE, true), push_key(sc, SDL_KMOD_NONE, false);
            p += n + 1;
            continue;
        }
        SDL_Keymod mod = SDL_KMOD_NONE;
        SDL_Scancode sc = SDL_GetScancodeFromKey((SDL_Keycode)tolower((unsigned char)*p), &mod);
        if (isupper((unsigned char)*p)) mod |= SDL_KMOD_LSHIFT;
        push_key(sc, mod, true);
        int c = *p & 0x7f;
        ascii[c][0] = (char)c;
        memset(&e, 0, sizeof e);
        e.type = SDL_EVENT_TEXT_INPUT, e.text.windowID = TEST_DEVICE, e.text.text = ascii[c];
        SDL_PushEvent(&e);
        push_key(sc, mod, false);
    }
}

static void host_test_frame(uint64_t frame) {
    static const char *hspec, *bspec;
    static int init;
    if (!init) { init = 1; hspec = getenv("ODYSSEY_HOST_INPUT"); bspec = getenv("ODYSSEY_BIND_TEST"); host_test = hspec; }
    char buf[2048];
    if (hspec) {
        uint8_t keys[SDL_SCANCODE_COUNT] = {0};
        uint32_t padm = 0;
        int32_t sx = 0, sy = 0;
        snprintf(buf, sizeof buf, "%s", hspec);
        char *save = NULL;
        for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
            unsigned long start = 0, len = 10;
            char dev[8] = "", name[400] = "";
            if (sscanf(tok, "%lu:%7[a-z]:%399[^:]:%lu", &start, dev, name, &len) < 3) continue;
            if (!strcmp(dev, "type") || !strcmp(dev, "sdlpad") || !strcmp(dev, "sdlkey") || !strcmp(dev, "shot")) {
                test_push(frame, start, len, dev, name);
                continue;
            }
            if (frame < start || frame >= start + len) continue;
            if (!strcmp(dev, "key")) { SDL_Scancode sc = SDL_GetScancodeFromName(name); if (sc) keys[sc] = 1; }
            else if (!strcmp(dev, "lstick")) sscanf(name, "%d/%d", &sx, &sy);
            else if (!strcmp(dev, "mouse")) {  // the mouse moved by <dx>/<dy> counts each frame (the mouse camera)
                float mx, my;
                if (sscanf(name, "%f/%f", &mx, &my) == 2) mouse_add(mx, my);
            }
            else { int h = bind_pad_from_name(name); if (h >= 0) padm |= 1u << h; }
        }
        bool kch = memcmp(keys, test_keys, sizeof keys) != 0, pch = padm != test_pad || sx != test_lx || sy != test_ly;
        memcpy(test_keys, keys, sizeof keys);
        test_pad = padm;
        test_lx = sx, test_ly = sy;
        if (kch) recompute_keyboard();
        if (pch) recompute_pad();
    }
    if (bspec) {
        snprintf(buf, sizeof buf, "%s", bspec);
        char *save = NULL;
        for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
            unsigned long start = 0;
            char dev[8] = "", in[16] = "", host[40] = "";
            if (sscanf(tok, "%lu:%7[a-z]:%15[a-z_]:%39[^:]", &start, dev, in, host) < 3) continue;
            if (!strcmp(dev, "reset")) { if (frame == start) bind_reset(!strcmp(in, "key") ? BIND_DEV_KEY : BIND_DEV_PAD); continue; }
            int d = !strcmp(dev, "key") ? BIND_DEV_KEY : BIND_DEV_PAD;
            if (frame == start) {
                LOG("[input] test: capture %s %s\n", dev, in);
                bind_capture_begin(d, bind_slot_from_name(in));
            }
            if (frame == start + 90 && strcmp(host, "wait")) {
                SDL_Event e;
                memset(&e, 0, sizeof e);
                if (!strcmp(host, "esc") || d == BIND_DEV_KEY) {
                    e.type = SDL_EVENT_KEY_DOWN;
                    e.key.scancode = !strcmp(host, "esc") ? SDL_SCANCODE_ESCAPE : SDL_GetScancodeFromName(host);
                } else {
                    int h = bind_pad_from_name(host);
                    if (h >= PADB_LTRIGGER) {
                        e.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
                        e.gaxis.axis = h == PADB_LTRIGGER ? SDL_GAMEPAD_AXIS_LEFT_TRIGGER : SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
                        e.gaxis.value = 32767;
                    } else {
                        e.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
                        e.gbutton.button = (Uint8)h;
                    }
                }
                bind_capture_event(&e);
            }
        }
    }
}

static uint64_t g_pad_obj;
volatile int g_inject_plus;  // frames to hold Plus for the game (movie.c: A/B skip the opening on rando seeds)
RT_ON_RESET(input_reboot_reset) { g_pad_obj = 0; g_inject_plus = 0; }  // (reboot: the controller object was guest memory)
void input_frame(uint64_t frame) {
    if (g_pad_obj && getenv("ODYSSEY_DUMP_PAD") && frame % 600 == 0) {
        const uint8_t *m = (const uint8_t *)(uintptr_t)(g_pad_obj - 0x98);
        LOG("[pad] mgr %llx 4e0=%u 4e4=%u 4e8=%u 548=%#x 54c..=%02x %02x %02x 56c=%02x\n", g_pad_obj - 0x98,
            *(uint32_t *)(m + 0x4e0), *(uint32_t *)(m + 0x4e4), *(uint32_t *)(m + 0x4e8), *(uint32_t *)(m + 0x548),
            m[0x54c], m[0x54d], m[0x54e], m[0x56c]);
        for (int i = 0; i < 9; i++) {
            const uint8_t *o = m + 0x98 + 0x48 * i;
            uint64_t player = *(uint64_t *)(m + 0x10 * i);
            LOG("[pad]  ctl%d id %#x style %#x btn %#x conn %d %d | player %llx [8]=%u\n", i, *(uint32_t *)(o + 8),
                *(uint32_t *)(o + 0xc), *(uint32_t *)(o + 0x14), o[0x44], o[0x45], player,
                player ? *(uint32_t *)(uintptr_t)(player + 8) : 0);
        }
    }
    extern void watch_arm(void);
    watch_arm();
    host_test_frame(frame);
    const char *spec = getenv("ODYSSEY_INPUT");
    if (!spec) return;
    static const char *const names[] = {"A", "B", "X", "Y", "LSTICK", "RSTICK", "L", "R", "ZL", "ZR", "PLUS",
                                        "MINUS", "LEFT", "UP", "RIGHT", "DOWN"};
    uint64_t b = 0;
    int32_t lx = 0, ly = 0, rx = 0, ry = 0;
    static char buf[16384];  // (long scripted sessions: the perf walk is ~4 KB)
    snprintf(buf, sizeof buf, "%s", spec);
    for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
        unsigned long start = 0, len = 10;
        char name[32] = "";
        if (sscanf(tok, "%lu:%31[A-Z_0-9]:%lu", &start, name, &len) < 2) continue;
        if (frame < start || frame >= start + len) continue;
        if (!strcmp(name, "STICK_UP")) ly = 32767;
        else if (!strcmp(name, "STICK_DOWN")) ly = -32767;
        else if (!strcmp(name, "STICK_LEFT")) lx = -32767;
        else if (!strcmp(name, "STICK_RIGHT")) lx = 32767;
        else if (!strcmp(name, "RSTICK_UP")) ry = 32767;
        else if (!strcmp(name, "RSTICK_DOWN")) ry = -32767;
        else if (!strcmp(name, "RSTICK_LEFT")) rx = -32767;
        else if (!strcmp(name, "RSTICK_RIGHT")) rx = 32767;
        else if (!strncmp(name, "STICK_", 6) && name[6] >= '0' && name[6] <= '9') {
            double r = atoi(name + 6) * 3.14159265358979 / 180;
            lx = (int32_t)lround(cos(r) * 32767), ly = (int32_t)lround(sin(r) * 32767);
        }
        for (int i = 0; i < 16; i++) if (!strcmp(name, names[i])) b |= 1ull << i;
    }
    script_buttons = b;
    script_lx = lx;
    script_ly = ly;
    script_rx = rx;
    script_ry = ry;
}

extern bool menu_input_captured(void);  // runtime/ui/menu.mm: overlay menu or name entry up (it has the keys / pad)

// Buttons held right now (keyboard, gamepad, test script), independent of whether the game polls (movies).
uint64_t input_buttons_now(void) { return (menu_input_captured() ? 0 : key_buttons | pad_buttons) | script_buttons; }

// Keep a press visible for one gameplay update, even if it came and went between two polls.
void input_native_tick_begin(void) {
    uint64_t pressed = atomic_exchange(&pending_presses, 0);
    tick_buttons = menu_input_captured() ? 0 : key_buttons | pad_buttons | pressed;
    tick_input_active = true;
}

void input_native_tick_end(void) { tick_input_active = false; }

static int32_t clamp_axis(int32_t v) { return v > 32767 ? 32767 : v < -32767 ? -32767 : v; }

// NpadFullKeyState / NpadHandheldState / NpadJoy*State share this layout (0x28 bytes):
// {int64 sampling, u64 buttons, AnalogStick L {x,y}, AnalogStick R {x,y}, u32 attributes}
static void fill_state(Cpu *c) {
    uint8_t *s = (uint8_t *)(uintptr_t)c->x[0];
    uint32_t id = *(uint32_t *)(uintptr_t)c->x[1];
    memset(s, 0, 0x28);
    *(int64_t *)s = ++sampling;
    static int nlog;
    if (nlog < 6 && (sampling % 600) == 1) { nlog++; LOG("[input] GetNpadState id %#x lr %llx\n", id, c->x[30]); }
    if (id != 0) return;  // docked: only player 1 (FullKey); Handheld stays disconnected
    extern uint64_t g_watch_lo, g_watch_hi;
    if (id == 0 && !g_pad_obj) g_pad_obj = c->x[1] - 8;
    if (id == 0 && !g_watch_lo && getenv("ODYSSEY_WATCH_PAD")) {
        unsigned lo = 0x98 + 0x10, hi = 0x98 + 0x48;  // offsets from the controller manager
        sscanf(getenv("ODYSSEY_WATCH_PAD"), "%x,%x", &lo, &hi);
        g_watch_lo = c->x[1] - 8 - 0x98 + lo;
        g_watch_hi = c->x[1] - 8 - 0x98 + hi;
        LOG("[input] watching controller object %llx\n", c->x[1] - 8);
    }
    bool menu = menu_input_captured();  // the overlay menu / name entry has the controls (typing there isn't for the game)
    uint64_t b = (menu ? 0 : tick_input_active ? tick_buttons : key_buttons | pad_buttons) | script_buttons;
    {   // injected Plus: held for ~10 presented frames (the game polls several times per frame)
        extern uint64_t renderer_present_count(void);
        static uint64_t until;
        if (g_inject_plus > 0) { until = renderer_present_count() + 10; g_inject_plus = 0; }
        if (renderer_present_count() < until) b |= 1ull << BTN_PLUS;
    }
    int32_t lx = clamp_axis((menu ? 0 : key_lx + pad_lx) + script_lx), ly = clamp_axis((menu ? 0 : key_ly + pad_ly) + script_ly);
    int32_t mrx = 0, mry = 0;
    mouse_stick(&mrx, &mry);  // (sampled even under a menu: its movement there is dropped, not saved up)
    {   // ODYSSEY_LOG_MOUSE=1: the mouse's right stick as the game reads it
        static int lm = -1, n;
        if (lm < 0) lm = getenv("ODYSSEY_LOG_MOUSE") != NULL;
        if (lm && (mrx || mry) && n++ % 20 == 0) LOG("[input] mouse camera: right stick %d %d%s\n", mrx, mry, menu ? " (menu up: dropped)" : "");
    }
    int32_t rx = clamp_axis((menu ? 0 : key_rx + pad_rx + mrx) + script_rx), ry = clamp_axis((menu ? 0 : key_ry + pad_ry + mry) + script_ry);
    // stick directions also appear as buttons (bits 16-23)
    if (lx < -16000) b |= 1ull << 16;
    if (ly > 16000) b |= 1ull << 17;
    if (lx > 16000) b |= 1ull << 18;
    if (ly < -16000) b |= 1ull << 19;
    if (rx < -16000) b |= 1ull << 20;
    if (ry > 16000) b |= 1ull << 21;
    if (rx > 16000) b |= 1ull << 22;
    if (ry < -16000) b |= 1ull << 23;
    *(uint64_t *)(s + 8) = b;
    static uint64_t last_b;
    if (b != last_b && (getenv("ODYSSEY_INPUT") || getenv("ODYSSEY_HOST_INPUT")))
        LOG("[input] id %#x buttons %#llx (sampling %lld, lr %llx)\n", id, (unsigned long long)b, (long long)sampling, c->x[30]);
    last_b = b;
    *(int32_t *)(s + 16) = lx;
    *(int32_t *)(s + 20) = ly;
    *(int32_t *)(s + 24) = rx;
    *(int32_t *)(s + 28) = ry;
    *(uint32_t *)(s + 32) = 1;  // IsConnected
}

HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj", hid_state_fullkey) { fill_state(c); }
HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyDualStateERKj", hid_state_dual) { fill_state(c); }
HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyLeftStateERKj", hid_state_left) { fill_state(c); }
HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_17NpadHandheldStateERKj", hid_state_handheld) { fill_state(c); }
HLE_FUNC("_ZN2nn3hid12GetNpadStateEPNS0_17NpadJoyRightStateERKj", hid_state_right) { fill_state(c); }

// GetNpadStates(State *out, int count, const NpadIdType &id) -> how many it wrote, newest first (Odyssey): the newest
// state only - each call has a new sampling number, so the game sees a fresh sample every poll.
static void fill_states(Cpu *c) {
    if ((int32_t)c->x[1] < 1) { c->x[0] = 0; return; }
    c->x[1] = c->x[2];
    fill_state(c);
    c->x[0] = 1;
}
HLE_FUNC("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadFullKeyStateEiRKj", hid_states_fullkey) { fill_states(c); }
HLE_FUNC("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyDualStateEiRKj", hid_states_dual) { fill_states(c); }
HLE_FUNC("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyLeftStateEiRKj", hid_states_left) { fill_states(c); }
HLE_FUNC("_ZN2nn3hid13GetNpadStatesEPNS0_17NpadHandheldStateEiRKj", hid_states_handheld) { fill_states(c); }
HLE_FUNC("_ZN2nn3hid13GetNpadStatesEPNS0_17NpadJoyRightStateEiRKj", hid_states_right) { fill_states(c); }

// ShowControllerSupport(ControllerSupportResultInfo *out, const ControllerSupportArg &) -> Result (Odyssey, at boot):
// the system's "connect a controller" applet, which our am service doesn't have (the game waited for it forever).
// Answered at once: one player, on controller 1. ResultInfo: {s8 player count, pad[3], u32 selected NpadId}.
HLE_FUNC("_ZN2nn3hid21ShowControllerSupportEPNS0_27ControllerSupportResultInfoERKNS0_20ControllerSupportArgE",
         hid_show_controller_support) {
    uint8_t *out = (uint8_t *)(uintptr_t)c->x[0];
    if (out) {
        memset(out, 0, 8);
        out[0] = 1;
    }
    static int nlog;
    if (nlog++ < 4) LOG("[input] ShowControllerSupport: answered with 1 player on controller 1\n");
    c->x[0] = RES_OK;
}

// FullKey for player 1 only (we report docked mode, where Handheld is unavailable)
HLE_FUNC("_ZN2nn3hid15GetNpadStyleSetERKj", hid_style_set) {
    uint32_t id = *(uint32_t *)(uintptr_t)c->x[0];
    static int nlog;
    if (nlog++ < 4) LOG("[input] GetNpadStyleSet id %#x\n", id);
    c->x[0] = id == 0 ? 1 : 0;  // docked: no handheld controller
}

// BindNpadStyleSetUpdateEvent(id, SystemEventType*, clear mode): signaled once so the game sees a
// controller connected.
#include "../kernel.h"
HLE_FUNC("_ZN2nn3hid27BindNpadStyleSetUpdateEventERKjPNS_2os15SystemEventTypeENS3_14EventClearModeE", hid_bind_style_event) {
    uint32_t w, r;
    KEvent *ev = kernel_create_event_pair(&w, &r);
    pthread_mutex_lock(&g_klock);
    kernel_signal(ev);
    pthread_mutex_unlock(&g_klock);
    uint64_t clear_mode = c->x[2];
    c->x[0] = c->x[1];
    c->x[1] = r;
    c->x[2] = 0;
    c->x[3] = clear_mode;
    guest_call(c, guest_sym("_ZN2nn2os33AttachReadableHandleToSystemEventEPNS0_15SystemEventTypeEjbNS0_14EventClearModeE"));
    LOG("[input] style set update event bound\n");
}

// Vibration: the SDK's vibration node graph and sead's vibration thread stay recompiled and send the game's HD rumble -
// a low and a high band's amplitude for each device (left / right Joy-Con, or the two sides of a Pro Controller) -
// through the hid service (services/hid_set.c: SendVibrationValue(s)) to here: onto the gamepad's two motors, the
// stronger side per band (low band: the low-frequency motor). Sent again while it lasts, so a value runs 250 ms at most:
// a game that stops sending stops the rumble. Controls tab "Vibration" (`vibration` in powermoon.ini).
void input_vibration(uint32_t handle, float amp_low, float amp_high) {
    static float lo[2], hi[2];
    static uint16_t last_l, last_h;
    static uint64_t last_ns;
    static int log = -1;  // ODYSSEY_LOG_RUMBLE=1: each change sent to the gamepad
    if (log < 0) log = getenv("ODYSSEY_LOG_RUMBLE") != NULL;
    int side = handle & 1;
    lo[side] = isfinite(amp_low) ? amp_low : 0, hi[side] = isfinite(amp_high) ? amp_high : 0;
    float l = fmaxf(lo[0], lo[1]), h = fmaxf(hi[0], hi[1]);
    if (!g_settings.rumble) l = h = 0;
    // (the Joy-Cons' actuators are strong: the game's amplitudes are mostly 0.02-0.2, which a gamepad's motors barely
    // turn at - small ones get a boost, (a + a^0.4) / 2: 0.04 -> 0.2, 0.17 -> 0.37, 1 -> 1; below 0.015, nothing)
    l = l < 0.015f ? 0 : (fminf(l, 1) + powf(fminf(l, 1), 0.4f)) * 0.5f;
    h = h < 0.015f ? 0 : (fminf(h, 1) + powf(fminf(h, 1), 0.4f)) * 0.5f;
    uint16_t L = (uint16_t)(l * 65535.0f), H = (uint16_t)(h * 65535.0f);
    uint64_t now = plat_now_ns();
    if (L == last_l && H == last_h && (!(L | H) || now - last_ns < 100000000ull)) return;  // (the same: again every 100 ms)
    last_l = L, last_h = H, last_ns = now;
    if (log) LOG("[input] rumble %.2f %.2f\n", L / 65535.0, H / 65535.0);
    SDL_Gamepad *p = pad;
    if (p) SDL_RumbleGamepad(p, L, H, (L | H) ? 250 : 0);
}

HLE_FUNC("_ZN2nn3hid25InitializeVibrationDeviceERKNS0_21VibrationDeviceHandleE", hid_init_vibration) { (void)c; }
HLE_FUNC("_ZN2nn3hid25GetVibrationDeviceHandlesEPNS0_21VibrationDeviceHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", hid_vibration_handles) {
    uint32_t *out = (uint32_t *)(uintptr_t)c->x[0];
    int n = (int)c->x[1] < 2 ? (int)c->x[1] : 2;
    uint32_t id = *(uint32_t *)(uintptr_t)c->x[2];
    for (int i = 0; i < n; i++) out[i] = (id & 0xFF) << 8 | (uint32_t)i | 0x03000000u;  // type, player, position
    c->x[0] = (uint64_t)n;
}
HLE_FUNC("_ZN2nn3hid22GetVibrationDeviceInfoEPNS0_19VibrationDeviceInfoERKNS0_21VibrationDeviceHandleE", hid_vibration_info) {
    uint32_t *info = (uint32_t *)(uintptr_t)c->x[0];
    uint32_t h = *(uint32_t *)(uintptr_t)c->x[1];
    info[0] = 1;        // VibrationDeviceType_LinearResonantActuator
    info[1] = h & 1;    // position: left / right
}
