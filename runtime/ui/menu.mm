// In-game overlay menu built with Dear ImGui (Metal renderer on macOS, Vulkan elsewhere; input fed from SDL events).
//
// Threads: SDL events arrive on the main thread and are queued; the menu is laid out and drawn on the
// render thread inside backend_present, after the game image, into the same drawable.
#ifdef __APPLE__
#import <Metal/Metal.h>
#endif
#include <SDL3/SDL.h>
#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <atomic>
#include <vector>

#include "imgui.h"
#include "option_rows.h"
#include "performance.h"
#include "imgui_internal.h"  // (NavCursorVisible)
#include "osk.h"
#include "theme.h"
#ifdef __APPLE__
#include "imgui_impl_metal.h"
#else
#include "imgui_impl_vulkan.h"
extern "C" bool vk_imgui_backend_init(void);  // runtime/nvn/vulkan/imgui_glue.cpp
#endif

extern "C" {
#include "../hle.h"
#include "../mods.h"
#include "../diagnostics.h"
#include "../settings.h"
#include "../updater.h"
#include "menu.h"
#include "setup.h"
void input_reset(void);
void controls_bindings_draw(float scale);  // controls_ui.mm
#include "../input_bind.h"
#include "../paths.h"
}

// ------------------------------------------------------------------ state shared with the main thread
static std::atomic<bool> g_open{false};
static std::atomic<bool> g_quit_prompt{false};
static std::atomic<bool> g_setup{false};  // the first-start setup page (setup.mm) instead of the menu
static std::atomic<bool> g_text_busy{false};  // a text field / the on-screen keyboard has the keys (render thread: ui_text_busy)
static std::atomic<bool> g_want_window_apply{false};
static std::atomic<bool> g_draw_mouse_cursor{false};
static std::atomic<int> g_last_fullscreen{BORDERLESS};
struct DisplayChoice { SDL_DisplayID id; char label[160]; };
static std::vector<DisplayChoice> g_displays;
static pthread_mutex_t g_displays_mutex = PTHREAD_MUTEX_INITIALIZER;
static std::atomic<bool> g_displays_dirty{true};

// SDL display queries and window changes stay on the event thread. The renderer
// only consumes this snapshot to draw the settings row.
static void refresh_displays(void) {
    if (!g_displays_dirty.exchange(false)) return;
    int count = 0;
    SDL_DisplayID *ids = SDL_GetDisplays(&count);
    std::vector<DisplayChoice> choices;
    DisplayChoice automatic = {};
    snprintf(automatic.label, sizeof automatic.label, "Current display");
    choices.push_back(automatic);
    for (int i = 0; ids && i < count && i < 64; i++) {
        DisplayChoice d = {}; d.id = ids[i];
        const char *name = SDL_GetDisplayName(d.id);
        const SDL_DisplayMode *mode = SDL_GetDesktopDisplayMode(d.id);
        snprintf(d.label, sizeof d.label, "%d: %s (%d x %d)", i + 1, name ? name : "Display", mode ? mode->w : 0, mode ? mode->h : 0);
        choices.push_back(d);
    }
    SDL_free(ids);
    pthread_mutex_lock(&g_displays_mutex);
    g_displays.swap(choices);
    pthread_mutex_unlock(&g_displays_mutex);
}
extern "C" bool menu_stats_visible(void) {
    return g_settings.show_fps || g_settings.performance_overlay;
}

struct QEvent {
    enum { KEY, ANALOG, MOUSE_POS, MOUSE_BUTTON, WHEEL, TEXT, RESET } kind;
    ImGuiKey key;
    bool down;
    float value, x, y;
    int button;
    char text[32];
};
static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;
static std::vector<QEvent> q_events;
static float g_pixel_density = 1;

static void push(const QEvent &e) {
    pthread_mutex_lock(&q_lock);
    q_events.push_back(e);
    pthread_mutex_unlock(&q_lock);
}

static void reset_menu_events(void) {
    pthread_mutex_lock(&q_lock);
    q_events.clear();
    QEvent reset = {};
    reset.kind = QEvent::RESET;
    q_events.push_back(reset);
    pthread_mutex_unlock(&q_lock);
}

bool menu_is_open(void) { return g_open.load(); }
void menu_setup_mode(bool on) { g_setup.store(on); }
static void set_open(bool open);
extern "C" void menu_test_open(void) {
    set_open(true);
    if (getenv("ODYSSEY_MENU_TAB")) { extern int g_tab_request_ext; g_tab_request_ext = atoi(getenv("ODYSSEY_MENU_TAB")); }
}

void menu_tabs_focus(bool first_tab);
static void set_open(bool open) {
    if (open == g_open.load()) return;
    g_open.store(open);
    reset_menu_events();
    input_reset();  // nothing held while the menu had focus leaks into the game
    bind_capture_cancel();
    if (open) {
        static bool opened;  // the controller's cursor: the first tab (Controls) the first time this session, later the
        menu_tabs_focus(!opened);  // selected one
        opened = true;
    }
    else { settings_save(); osk_cancel(); g_text_busy = false; }
}

static ImGuiKey key_from_scancode(SDL_Scancode sc) {
    switch (sc) {
    case SDL_SCANCODE_ESCAPE: return ImGuiKey_Escape;
    case SDL_SCANCODE_UP: return ImGuiKey_UpArrow;
    case SDL_SCANCODE_DOWN: return ImGuiKey_DownArrow;
    case SDL_SCANCODE_LEFT: return ImGuiKey_LeftArrow;
    case SDL_SCANCODE_RIGHT: return ImGuiKey_RightArrow;
    case SDL_SCANCODE_RETURN: return ImGuiKey_Enter;
    case SDL_SCANCODE_KP_ENTER: return ImGuiKey_KeypadEnter;
    case SDL_SCANCODE_SPACE: return ImGuiKey_Space;
    case SDL_SCANCODE_TAB: return ImGuiKey_Tab;
    case SDL_SCANCODE_BACKSPACE: return ImGuiKey_Backspace;
    case SDL_SCANCODE_DELETE: return ImGuiKey_Delete;
    case SDL_SCANCODE_HOME: return ImGuiKey_Home;
    case SDL_SCANCODE_END: return ImGuiKey_End;
    case SDL_SCANCODE_PAGEUP: return ImGuiKey_PageUp;
    case SDL_SCANCODE_PAGEDOWN: return ImGuiKey_PageDown;
    case SDL_SCANCODE_LSHIFT: return ImGuiKey_LeftShift;
    case SDL_SCANCODE_RSHIFT: return ImGuiKey_RightShift;
    case SDL_SCANCODE_LCTRL: return ImGuiKey_LeftCtrl;
    case SDL_SCANCODE_RCTRL: return ImGuiKey_RightCtrl;
    case SDL_SCANCODE_LGUI: return ImGuiKey_LeftSuper;
    case SDL_SCANCODE_RGUI: return ImGuiKey_RightSuper;
    case SDL_SCANCODE_A: return ImGuiKey_A;
    case SDL_SCANCODE_C: return ImGuiKey_C;
    case SDL_SCANCODE_V: return ImGuiKey_V;
    case SDL_SCANCODE_X: return ImGuiKey_X;
    case SDL_SCANCODE_Z: return ImGuiKey_Z;
    case SDL_SCANCODE_Q: return ImGuiKey_Q;  // previous tab
    case SDL_SCANCODE_E: return ImGuiKey_E;  // next tab
    default: return ImGuiKey_None;
    }
}

// Nintendo layout: the right face button (A) confirms, the bottom one (B) goes back.
static ImGuiKey key_from_pad(Uint8 b) {
    switch (b) {
    case SDL_GAMEPAD_BUTTON_EAST: return ImGuiKey_GamepadFaceDown;   // activate
    case SDL_GAMEPAD_BUTTON_SOUTH: return ImGuiKey_GamepadFaceRight; // cancel / back
    case SDL_GAMEPAD_BUTTON_NORTH: return ImGuiKey_GamepadFaceUp;
    case SDL_GAMEPAD_BUTTON_WEST: return ImGuiKey_GamepadFaceLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return ImGuiKey_GamepadDpadUp;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return ImGuiKey_GamepadDpadDown;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return ImGuiKey_GamepadDpadLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return ImGuiKey_GamepadDpadRight;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return ImGuiKey_GamepadL1;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return ImGuiKey_GamepadR1;
    case SDL_GAMEPAD_BUTTON_START: return ImGuiKey_GamepadStart;
    case SDL_GAMEPAD_BUTTON_BACK: return ImGuiKey_GamepadBack;
    default: return ImGuiKey_None;
    }
}

// ---- software keyboard (runtime/hle/swkbd.c: the game's name entry etc.) --------------------------------------
// The game thread waits in menu_keyboard_run (re-presenting the last frame); this dialog takes the text: typed
// directly when the player last used a keyboard, an on-screen keyboard (d-pad + A, B deletes, Start / + OK) when
// they last used a controller.
static std::atomic<int> g_kbd{0};  // 1 open, 2 confirmed, 3 cancelled
static std::atomic<bool> g_last_pad{false};
static char g_kbd_text[96];
static int g_kbd_max = 8;
// A notice the game thread waits on in menu_notice_run, like the name entry (runtime/hle/applets.c: a system applet
// this version has nothing to show for)
static std::atomic<int> g_note{0};  // 1 shown, 2 closed
static char g_note_title[128], g_note_text[800];
static bool capturing() { return g_open.load() || g_kbd.load() == 1 || g_note.load() == 1 || g_setup.load(); }
// Draw the settings pointer in the overlay so it also appears in fullscreen.
static void sync_mouse_cursor(bool ui_active) {
    SDL_Window *mouse_window = SDL_GetMouseFocus();
    bool focused = mouse_window && mouse_window == SDL_GetKeyboardFocus();
    bool draw_cursor = focused && g_open.load() && !g_setup.load();
    if (draw_cursor && !g_draw_mouse_cursor.load()) {
        float x, y;
        SDL_GetMouseState(&x, &y);
        QEvent q = {};
        q.kind = QEvent::MOUSE_POS;
        q.x = x * g_pixel_density;
        q.y = y * g_pixel_density;
        push(q);
    }
    g_draw_mouse_cursor.store(draw_cursor);
    bool show = !focused || (ui_active && !draw_cursor);
    if (show == SDL_CursorVisible()) return;
    if (show) SDL_ShowCursor();
    else SDL_HideCursor();
}
// (frame interpolation's presenter, runtime/nvn/vulkan/device.c, draws the overlay without the render lock) nothing open
// that takes input or runs the menu's actions, and the overlay drawn once (ImGui uploads its fonts on the first frame)
static std::atomic<bool> g_drawn;
extern "C" bool menu_overlay_lockfree(void) { return !capturing() && (!menu_stats_visible() || g_drawn.load()); }
// The game gets no input while this is true (runtime/hle/input.c): menu or name entry up.
extern "C" bool menu_input_captured(void) { return capturing(); }

bool menu_handle_event(const SDL_Event *e) {
    if (e->type == SDL_EVENT_DISPLAY_ADDED || e->type == SDL_EVENT_DISPLAY_REMOVED) {
        g_displays_dirty.store(true);
        g_want_window_apply.store(true);
    }
    static SDL_JoystickID back_pad, start_pad;
    static bool stick_down[4];
    if (e->type == SDL_EVENT_WINDOW_FOCUS_GAINED) g_app_unfocused = false;
    if (e->type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        g_app_unfocused = true;  // (the audio's Mute in background)
        back_pad = start_pad = 0;
        for (bool &held : stick_down) held = false;
        if (capturing()) reset_menu_events();
        return false;  // also clear the game's input
    }
    if (e->type == SDL_EVENT_GAMEPAD_REMOVED) {
        if (back_pad == e->gdevice.which) back_pad = 0;
        if (start_pad == e->gdevice.which) start_pad = 0;
        if (capturing()) reset_menu_events();
        return false;
    }
    if (g_open.load() && bind_capture_event(e)) return true;  // Controls tab waiting for a new binding: it takes the press
    if (e->type == SDL_EVENT_KEY_DOWN) g_last_pad = false;
    if (e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) g_last_pad = true;
    if (g_kbd.load() == 1 && e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat) {  // Enter confirms, Esc cancels
        if (e->key.scancode == SDL_SCANCODE_RETURN || e->key.scancode == SDL_SCANCODE_KP_ENTER) { g_kbd = 2; return true; }
        if (e->key.scancode == SDL_SCANCODE_ESCAPE) { g_kbd = 3; return true; }
    }
    if (g_kbd.load() == 1 && e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && e->gbutton.button == SDL_GAMEPAD_BUTTON_START) {
        g_kbd = 2;
        return true;
    }
    if (g_note.load() == 1 && e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat &&  // Enter / Space / Esc: OK
        (e->key.scancode == SDL_SCANCODE_RETURN || e->key.scancode == SDL_SCANCODE_KP_ENTER ||
         e->key.scancode == SDL_SCANCODE_SPACE || e->key.scancode == SDL_SCANCODE_ESCAPE)) {
        g_note = 2;
        return true;
    }
    // typing (a text field, the on-screen keyboard, the name entry): keys are text, not menu shortcuts
    bool typing = g_kbd.load() == 1 || (g_open.load() && g_text_busy.load());
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN:
        if (!e->key.repeat && !g_setup.load() && (e->key.scancode == SDL_SCANCODE_F1 ||
                                                  (e->key.scancode == SDL_SCANCODE_ESCAPE && !g_open.load()))) {
            if (!typing && !g_quit_prompt.load()) set_open(!g_open.load());
            return true;
        }
        if (!e->key.repeat && (e->key.scancode == SDL_SCANCODE_F11 ||
                               (e->key.scancode == SDL_SCANCODE_RETURN && (e->key.mod & (SDL_KMOD_ALT | SDL_KMOD_GUI))) ||
                               (e->key.scancode == SDL_SCANCODE_F && (e->key.mod & SDL_KMOD_GUI) && (e->key.mod & SDL_KMOD_CTRL)))) {
            g_settings.window_mode = g_settings.window_mode == WINDOWED ? g_last_fullscreen.load() : WINDOWED;
            g_want_window_apply.store(true);
            settings_save();
            return true;
        }
        /* fallthrough */
    case SDL_EVENT_KEY_UP: {
        if (!capturing()) return false;
        bool down = e->type == SDL_EVENT_KEY_DOWN;
        if (down && e->key.repeat) return true;  // ImGui owns key repeat and text has its own events.
        ImGuiKey k = key_from_scancode(e->key.scancode);
        if (k != ImGuiKey_None) { QEvent q = {}; q.kind = QEvent::KEY; q.key = k; q.down = down; push(q); }
        return true;
    }
    case SDL_EVENT_TEXT_INPUT:
        if (!capturing()) return false;
        { QEvent q = {}; q.kind = QEvent::TEXT; strlcpy(q.text, e->text.text, sizeof q.text); push(q); }
        return true;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        bool down = e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        bool chord = back_pad && back_pad == start_pad;
        SDL_JoystickID pad = e->gbutton.which;
        if (e->gbutton.button == SDL_GAMEPAD_BUTTON_BACK && (down || back_pad == pad)) back_pad = down ? pad : 0;
        if (e->gbutton.button == SDL_GAMEPAD_BUTTON_START && (down || start_pad == pad)) start_pad = down ? pad : 0;
        if (down && !typing && !g_setup.load() &&
            ((!chord && back_pad && back_pad == start_pad) || e->gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE)) {
            if (!g_quit_prompt.load()) set_open(!g_open.load());
            return true;
        }
        if (!capturing()) return false;
        ImGuiKey k = key_from_pad(e->gbutton.button);
        if (k != ImGuiKey_None) { QEvent q = {}; q.kind = QEvent::KEY; q.key = k; q.down = down; push(q); }
        return true;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        float v = e->gaxis.value / 32767.0f;
        QEvent q = {};
        q.kind = QEvent::ANALOG;
        bool active = capturing();
        auto direction = [&](int i, ImGuiKey key, float amount) {
            stick_down[i] = amount > (stick_down[i] ? 0.35f : 0.55f);
            if (active) { q.key = key; q.value = stick_down[i] ? amount : 0; push(q); }
        };
        switch (e->gaxis.axis) {
        case SDL_GAMEPAD_AXIS_LEFTX:
            direction(0, ImGuiKey_GamepadLStickLeft, -v);
            direction(1, ImGuiKey_GamepadLStickRight, v);
            break;
        case SDL_GAMEPAD_AXIS_LEFTY:
            direction(2, ImGuiKey_GamepadLStickUp, -v);
            direction(3, ImGuiKey_GamepadLStickDown, v);
            break;
        case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: if (active) { q.key = ImGuiKey_GamepadL2; q.value = v; push(q); } break;
        case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: if (active) { q.key = ImGuiKey_GamepadR2; q.value = v; push(q); } break;
        default: break;
        }
        return active;
    }
    case SDL_EVENT_MOUSE_MOTION: {
        bool ui_active = capturing();
        sync_mouse_cursor(ui_active);
        if (!ui_active) return false;
        { QEvent q = {}; q.kind = QEvent::MOUSE_POS; q.x = e->motion.x * g_pixel_density; q.y = e->motion.y * g_pixel_density; push(q); }
        return true;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP:
        if (!capturing()) return false;
        {
            QEvent q = {};
            q.kind = QEvent::MOUSE_BUTTON;
            q.button = e->button.button == SDL_BUTTON_LEFT ? 0 : e->button.button == SDL_BUTTON_RIGHT ? 1 : 2;
            q.down = e->type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            push(q);
        }
        return true;
    case SDL_EVENT_MOUSE_WHEEL:
        if (!capturing()) return false;
        { QEvent q = {}; q.kind = QEvent::WHEEL; q.x = e->wheel.x; q.y = e->wheel.y; push(q); }
        return true;
    default:
        return false;
    }
}

void menu_request_window_apply(void) { g_want_window_apply.store(true); }

void menu_apply_window_requests(void *win) {
    SDL_Window *w = (SDL_Window *)win;
    refresh_displays();
    g_pixel_density = SDL_GetWindowPixelDensity(w);
    static bool text_input;
    bool want_text = capturing();
    sync_mouse_cursor(want_text);
    if (want_text != text_input) {
        if (want_text) SDL_StartTextInput(w);
        else SDL_StopTextInput(w);
        text_input = want_text;
    }
    static bool first = true;
    bool apply = g_want_window_apply.exchange(false) ||
                 (first && !getenv("ODYSSEY_WINDOW") && !getenv("ODYSSEY_HIDDEN"));
    first = false;
    if (!apply) return;

    int mode = g_settings.window_mode;
    int height = settings_display_height(g_settings.display_resolution);
    SDL_DisplayID display = SDL_GetDisplayForWindow(w);
    SDL_DisplayID target = 0;
    pthread_mutex_lock(&g_displays_mutex);
    if (g_settings.monitor > 0) {
        target = g_settings.monitor < (int)g_displays.size() ? g_displays[g_settings.monitor].id : SDL_GetPrimaryDisplay();
    }
    pthread_mutex_unlock(&g_displays_mutex);
    if (target && target != display) {
        if (!SDL_SetWindowFullscreen(w, false) || !SDL_SetWindowFullscreenMode(w, NULL) || !SDL_SyncWindow(w) ||
            !SDL_SetWindowPosition(w, SDL_WINDOWPOS_CENTERED_DISPLAY(target), SDL_WINDOWPOS_CENTERED_DISPLAY(target)) ||
            !SDL_SyncWindow(w)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO, "Could not move the game to the selected display: %s", SDL_GetError());
            return;
        }
        display = SDL_GetDisplayForWindow(w);
        SDL_Log("Game display: %s", SDL_GetDisplayName(display));
    }
    const SDL_DisplayMode *desktop = SDL_GetDesktopDisplayMode(display);
    bool applied;
    if (mode == WINDOWED) {
        applied = SDL_SetWindowFullscreen(w, false) && SDL_SetWindowFullscreenMode(w, NULL);
        if (applied && height) applied = SDL_SetWindowSize(w, height * 16 / 9, height);
    } else {
        g_last_fullscreen.store(mode);
        SDL_DisplayMode selected;
        const SDL_DisplayMode *exclusive = NULL;
        if (mode == EXCLUSIVE_FULLSCREEN && desktop) {
            int width = height ? height * 16 / 9 : desktop->w;
            int lines = height ? height : desktop->h;
            if (SDL_GetClosestFullscreenDisplayMode(display, width, lines, 0, true, &selected)) exclusive = &selected;
        }
        applied = (mode != EXCLUSIVE_FULLSCREEN || exclusive) &&
                  SDL_SetWindowFullscreenMode(w, exclusive) && SDL_SetWindowFullscreen(w, true);
        if (!applied && mode == EXCLUSIVE_FULLSCREEN) {
            SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO, "Exclusive fullscreen unavailable: %s. Using borderless.", SDL_GetError());
            applied = SDL_SetWindowFullscreenMode(w, NULL) && SDL_SetWindowFullscreen(w, true);
            if (applied) {
                g_last_fullscreen.store(BORDERLESS);
                g_settings.window_mode = BORDERLESS;
                settings_save();
            }
        }
    }
    if (!applied) SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO, "Could not apply display settings: %s", SDL_GetError());
}

// ------------------------------------------------------------------ render thread
static bool g_inited;
static ImGuiContext *g_ctx;  // ours (another UI would have its own context)
static ImFont *g_font, *g_font_big;
static ImFont *g_menu_font, *g_menu_font_big;  // the menu window (Roboto Medium); everything else keeps g_font
static int g_tab, g_tab_request = -2;  // (-2: no request; -1 is the Prelaunch tab)
static int g_focus_tab = -99;  // put the nav cursor on this tab next frame (-100: the selected one; -99: none)
static int g_focus_frames;

// The menu opening: the controller's cursor starts on the first tab the first time in a session,
// later on the tab that's selected
void menu_tabs_focus(bool first_tab) { g_focus_tab = first_tab ? -1 : -100, g_focus_frames = 0; }
int g_tab_request_ext = -1;

// The look (theme.h): deep sky blue, white text, pipe green as the accent, gold for changed values; rounded corners as
// the installer's card; tabs are plain words with a green line under the selected one (tabs_body draws it).
static void style_init(void) {
    ImGuiStyle &s = ImGui::GetStyle();
    ImGui::StyleColorsDark();
    s.WindowRounding = 14; s.FrameRounding = 7; s.GrabRounding = 7; s.TabRounding = 0; s.ChildRounding = 8;
    s.PopupRounding = 10; s.ScrollbarRounding = 7;
    s.WindowPadding = ImVec2(32, 26); s.FramePadding = ImVec2(14, 8); s.ItemSpacing = ImVec2(14, 12);
    s.WindowBorderSize = 0; s.ScrollbarSize = 14; s.TabBarBorderSize = 0;
    ImVec4 *c = s.Colors, none(0, 0, 0, 0);
    c[ImGuiCol_WindowBg] = kPanelV;
    c[ImGuiCol_ChildBg] = none;
    c[ImGuiCol_PopupBg] = ImVec4(kPanelV.x, kPanelV.y, kPanelV.z, 0.99f);
    c[ImGuiCol_Border] = kLineV;
    c[ImGuiCol_Text] = kTextV;
    c[ImGuiCol_TextDisabled] = kTextDimV;
    c[ImGuiCol_FrameBg] = c[ImGuiCol_Button] = c[ImGuiCol_Header] = c[ImGuiCol_TableHeaderBg] = kSurfaceV;
    c[ImGuiCol_FrameBgHovered] = c[ImGuiCol_ButtonHovered] = c[ImGuiCol_HeaderHovered] = kSurfaceHoverV;
    c[ImGuiCol_FrameBgActive] = c[ImGuiCol_ButtonActive] = c[ImGuiCol_HeaderActive] = kAccentDeepV;
    c[ImGuiCol_Tab] = c[ImGuiCol_TabSelected] = c[ImGuiCol_TabSelectedOverline] = none;
    c[ImGuiCol_TabDimmed] = c[ImGuiCol_TabDimmedSelected] = c[ImGuiCol_TabDimmedSelectedOverline] = none;
    c[ImGuiCol_TabHovered] = ImVec4(1, 1, 1, 0.06f);
    c[ImGuiCol_CheckMark] = c[ImGuiCol_SliderGrabActive] = c[ImGuiCol_NavCursor] = c[ImGuiCol_ScrollbarGrabActive] = kAccentV;
    c[ImGuiCol_SliderGrab] = ImVec4(kAccentV.x, kAccentV.y, kAccentV.z, 0.75f);
    c[ImGuiCol_Separator] = kLineV;
    c[ImGuiCol_TableRowBg] = none;
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.03f);
    c[ImGuiCol_ScrollbarBg] = none;
    c[ImGuiCol_ScrollbarGrab] = ImVec4(1, 1, 1, 0.14f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(1, 1, 1, 0.22f);
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = c[ImGuiCol_WindowBg];
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.45f);
}


#ifdef __APPLE__
static void init(id<MTLDevice> dev) {
#else
static void init(void) {
#endif
    IMGUI_CHECKVERSION();
    g_ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(g_ctx);
    ImGuiIO &io = ImGui::GetIO();
    io.KeyRepeatDelay = 0.45f;
    io.KeyRepeatRate = 0.10f;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.IniFilename = NULL;
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    const char *paths[] = {"/System/Library/Fonts/Avenir Next.ttc", "/System/Library/Fonts/Helvetica.ttc",
                           "C:/Windows/Fonts/segoeui.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                           "/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf"};
    for (const char *p : paths) {
        if (FILE *f = fopen(p, "rb")) fclose(f);  // a missing file is an assertion in ImGui, not a NULL return
        else continue;
        if (!g_font) g_font = io.Fonts->AddFontFromFileTTF(p, 34, &cfg);
        if (g_font && !g_font_big) { ImFontConfig b = cfg; b.FontNo = 0; g_font_big = io.Fonts->AddFontFromFileTTF(p, 56, &b); }
    }
    // the menu window itself: Roboto Medium (third_party/imgui/misc/fonts, Apache 2.0); the FPS counter, overlays and
    // on-screen keyboard keep the fonts above
    char menu_font[1024];
    if (const char *f = getenv("ODYSSEY_MENU_FONT")) strlcpy(menu_font, f, sizeof menu_font);  // (test) another face
    if (getenv("ODYSSEY_MENU_FONT") ||
        rt_resource("Roboto-Medium.ttf", "third_party/imgui/misc/fonts/Roboto-Medium.ttf", menu_font, sizeof menu_font)) {
        g_menu_font = io.Fonts->AddFontFromFileTTF(menu_font, 36, &cfg);
        g_menu_font_big = io.Fonts->AddFontFromFileTTF(menu_font, 58, &cfg);
    }
    if (!g_font) g_font = io.Fonts->AddFontDefault();
    if (!g_font_big) g_font_big = g_font;
    if (!g_menu_font || !g_menu_font_big) g_menu_font = g_font, g_menu_font_big = g_font_big;
    style_init();
#ifdef __APPLE__
    ImGui_ImplMetal_Init(dev);
#else
    vk_imgui_backend_init();
#endif
    g_inited = true;
}

static void drain_events(ImGuiIO &io) {
    pthread_mutex_lock(&q_lock);
    std::vector<QEvent> ev;
    ev.swap(q_events);
    pthread_mutex_unlock(&q_lock);
    for (const QEvent &q : ev) {
        switch (q.kind) {
        case QEvent::KEY: io.AddKeyEvent(q.key, q.down); break;
        case QEvent::ANALOG: io.AddKeyAnalogEvent(q.key, q.value > 0.1f, q.value); break;
        case QEvent::MOUSE_POS: io.AddMousePosEvent(q.x, q.y); break;
        case QEvent::MOUSE_BUTTON: io.AddMouseButtonEvent(q.button, q.down); break;
        case QEvent::WHEEL: io.AddMouseWheelEvent(q.x, q.y); break;
        case QEvent::TEXT: io.AddInputCharactersUTF8(q.text); break;
        case QEvent::RESET: io.ClearEventsQueue(); io.ClearInputKeys(); io.ClearInputMouse(); break;
        }
    }
}

// Section header: in the accent, capitals, as the option rows' sections (option_rows.mm opt_section).
static void header(const char *t) {
    char up[96];
    int i = 0;
    for (; t[i] && i < (int)sizeof up - 1; i++) up[i] = (char)toupper((unsigned char)t[i]);
    up[i] = 0;
    ImGui::Dummy(ImVec2(0, 4));
    ImGui::TextColored(kAccentV, "%s", up);
    ImGui::Separator();
    ImGui::Spacing();
}

static void hint(const char *t) {
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", t);
    ImGui::PopTextWrapPos();
}

// A row with a label on the left and a choice of options on the right (dpad left/right cycles).
// reserve: room kept free right of the combo (a button on the same line)
static bool choice(const char *label, int *value, const char *const *names, int n, float reserve = 0) {
    ImGui::PushID(label);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.45f);
    bool changed = false;
    ImGui::SetNextItemWidth(reserve > 0 ? -reserve : -1);
    if (ImGui::BeginCombo("##c", names[*value])) {
        for (int i = 0; i < n; i++) {
            if (ImGui::Selectable(names[i], i == *value)) { *value = i; changed = true; }
            if (i == *value) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemFocused() && !ImGui::IsPopupOpen("##c")) {
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight) || ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
            *value = (*value + 1) % n; changed = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft) || ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
            *value = (*value + n - 1) % n; changed = true;
        }
    }
    ImGui::PopID();
    return changed;
}

static bool toggle(const char *label, bool *v) {
    const char *names[] = {"Off", "On"};
    int i = *v;
    bool ch = choice(label, &i, names, 2);
    *v = i;
    return ch;
}

extern "C" void renderer_get_scale(float *sx, float *sy);
extern "C" int input_pad_family(void);  // runtime/hle/input.c
extern "C" double output_scale_dynamic_factor(void);
extern "C" bool vk_prepare_requested(void);       // runtime/nvn/vulkan/device.c (the menu's Compile shaders)
extern "C" void vk_prepare_request(bool on);
#ifdef _WIN32
extern "C" bool vk_temporal_enabled(void);
extern "C" bool vk_fsr3_available(void);
extern "C" bool vk_fsr4_available(void);
extern "C" bool vk_xess_available(void);
#endif

static void tab_display(void) {
    float scale = ImGui::GetFontSize() / 34;
    opt_begin("##video", scale);
    opt_section("Display");
    pthread_mutex_lock(&g_displays_mutex);
    if (!g_displays.empty()) {
        std::vector<const char *> labels;
        for (const DisplayChoice &d : g_displays) labels.push_back(d.label);
        int selected = g_settings.monitor >= 0 && g_settings.monitor < (int)labels.size() ? g_settings.monitor : 0;
        if (opt_choice("Monitor", &selected, labels.data(), (int)labels.size(), 0,
                       "Moves the game to the selected display. Works in windowed, borderless and exclusive fullscreen. "
                       "Current display leaves window placement to you. Display numbers follow the connected display list; "
                       "select again if that list changes. An unavailable display falls back to the primary display.")) {
            g_settings.monitor = selected;
            menu_request_window_apply();
        }
    }
    pthread_mutex_unlock(&g_displays_mutex);
    static const char *window_modes[] = {"Windowed", "Borderless", "Exclusive fullscreen"};
    if (opt_choice("Display mode", &g_settings.window_mode, window_modes, WINDOW_MODE_COUNT, WINDOWED,
                   "Borderless uses the desktop's resolution. Exclusive fullscreen requests a monitor display mode. "
                   "F11 and Alt+Enter toggle between the window and your last fullscreen mode."))
        g_want_window_apply.store(true);
    static const char *display_modes[] = {"Auto", "1080p", "1440p", "4K"};
    bool borderless = g_settings.window_mode == BORDERLESS;
    if (borderless) opt_disable_next();
    if (opt_choice("Display resolution", &g_settings.display_resolution, display_modes, DISPLAY_MODE_COUNT, DISPLAY_AUTO,
                   "Changes the window size or selects the closest exclusive fullscreen mode your monitor supports. "
                   "Auto keeps the current window size or uses the desktop resolution in exclusive fullscreen. "
                   "Borderless always uses the desktop resolution."))
        g_want_window_apply.store(true);
#ifndef __APPLE__  // (Vulkan: runtime/hle/odyssey_wide.c)
    opt_section("Screen shape");
    {
        static const char *asp[] = {"16:9", "Fill window"};
        opt_choice("Aspect ratio", &g_settings.aspect, asp, 2, ASPECT_16_9,
                   "For windows wider than 16:9 (ultrawide monitors). 16:9: the Switch's picture, with bars at the sides. "
                   "Fill window: the game shows more of the world at the sides; the HUD keeps its shape in the middle.");
    }
#endif
    opt_end();
}

// Graphics: how sharp and clean the 3D image is (resolution, upscaling, anti-aliasing, DLSS, textures)
static void tab_graphics(void) {
    float scale = ImGui::GetFontSize() / 34;
    opt_begin("##graphics", scale);
    opt_section("Resolution");
    // display order -> RES_* (the ini / enum order predates the menu order)
    // (up to 4x: 7680 x 4320, 8K - more only ran out of GPU memory, and sat next to 75% in the list)
    // (Match display in the middle: left from it 75% / 50% / 25%, right Dynamic then 1x-4x; user 2026-10-11)
    static const char *res[] = {"25%", "50%", "75%", "Match display", "Dynamic", "1080p (1x)", "1440p", "4K (2x)", "3x", "4x"};
    static const int res_mode[] = {RES_Q25, RES_Q50, RES_Q75, RES_MATCH, RES_DYNAMIC, 1, RES_1440, 2, 3, 4};
    enum { RES_MATCH_AT = 3 };
    enum { NRES = sizeof res_mode / sizeof res_mode[0] };
    {
        int ri = RES_MATCH_AT;
        for (int i = 0; i < NRES; i++) if (res_mode[i] == g_settings.res_mode) ri = i;
        float sx, sy;
        renderer_get_scale(&sx, &sy);
        char d[400];
        int n = snprintf(d, sizeof d, "How sharp the 3D image is. Match display: your screen's own resolution. Dynamic: "
                                      "as sharp as your GPU keeps at full speed. 1x-4x: 1080p times that (2x: 4K; the "
                                      "Switch renders 1600 x 900). 75%% / 50%% / 25%%: a share of the display, for "
                                      "slower machines.\n\nRendering at %.0f x %.0f.", 1600.0 * sx, 900.0 * sy);
        if (g_settings.res_mode == RES_DYNAMIC)
            snprintf(d + n, sizeof d - n, " Dynamic: %.0f%% of the display right now.", output_scale_dynamic_factor() * 100);
#ifdef _WIN32
        if (vk_temporal_enabled()) {
            snprintf(d, sizeof d, "Match display lets the temporal upscaler choose its usual input size. A fixed resolution overrides that "
                     "size. At or above the display resolution, upscaling is bypassed. Native AA/DLAA applies at equal sizes.\n\n"
                     "Rendering at %.0f x %.0f.", 1600.0 * sx, 900.0 * sy);
        }
#endif
        if (opt_choice("Internal resolution", &ri, res, NRES, RES_MATCH_AT, d, false)) g_settings.res_mode = res_mode[ri];
    }
    {   // each OS's own upscaler next to bilinear (UPSCALE_SHARP: MetalFX on macOS, AMD FSR 1 on Windows)
#ifdef __APPLE__
        static const char *ups[] = {"Bilinear", "MetalFX"};
        const char *ud = "How the image is scaled up to the window when it renders at a lower resolution. MetalFX: Apple's "
                         "upscaler, sharper. Bilinear: plain and soft.";
#else
        static const char *ups[] = {"Bilinear", "AMD FSR 1"};
        const char *ud = "How the image is scaled up to the window when it renders at a lower resolution. AMD FSR 1: "
                         "edge-aware upscaling and sharpening, sharper. Bilinear: plain and soft.";
#endif
        int ui = g_settings.upscaler == UPSCALE_BILINEAR ? 0 : 1;
        char udm[512];
        if (g_settings.res_mode == RES_MATCH
#ifdef _WIN32
            && !vk_temporal_enabled()
#endif
        ) {  // (renders at the window's size: nothing to scale up)
            snprintf(udm, sizeof udm, "%s\n\nNot used at Match display: the game already renders at your display's resolution.", ud);
            ud = udm;
            opt_disable_next();
        }
#ifdef __APPLE__
        constexpr int default_upscaler = 1;
#else
        constexpr int default_upscaler = 0;
#endif
        if (opt_choice("Spatial upscaler", &ui, ups, 2, default_upscaler, ud)) g_settings.upscaler = ui ? UPSCALE_SHARP : UPSCALE_BILINEAR;
    }
    opt_section("Anti-aliasing");
    static const char *aa[] = {"Off", "SMAA"};
    opt_choice("Anti-aliasing", &g_settings.antialias, aa, 2, AA_OFF,
               "Smooths jagged edges of the game image before it is scaled to the window. SMAA: finds the edges and "
               "blends only along them, so textures stay sharp. Most useful at a lower internal resolution.");
#ifdef _WIN32
    opt_section("Temporal upscaling");
    static const char *backends[] = {"NVIDIA DLSS", "AMD FSR 3.1", "AMD FSR 4 (Beta)", "Intel XeSS (Beta)"};
    opt_choice("Temporal upscaler", &g_settings.temporal_backend, backends, TEMPORAL_BACKEND_COUNT, TEMPORAL_DLSS,
               "Selects the temporal anti-aliasing/upscaling technique. Each keeps its own mode and sharpening settings. Changes during play.");
    int backend = g_settings.temporal_backend;
    bool fsr = backend == TEMPORAL_FSR3 || backend == TEMPORAL_FSR4;
    if (backend != TEMPORAL_DLSS) {
        static const char *modes[] = {"Off", "Native AA", "Quality", "Balanced", "Performance"};
        const char *help = backend == TEMPORAL_XESS ?
            "Experimental Intel XeSS-SR. Native AA, Quality (1.7x), Balanced (2x) and Performance (2.3x). Requires a compatible Vulkan driver and Intel's runtime. Changes during play." :
            "AMD temporal reconstruction. Native AA uses display resolution; other modes render at lower input resolutions. Changes during play.";
        if (backend == TEMPORAL_FSR4 && !vk_fsr4_available()) help =
            "FSR 4 ML is unavailable on this GPU/runtime. FSR 3.1 is used when supported; otherwise the game uses regular rendering. The status below shows the active path.";
        opt_choice(backend == TEMPORAL_XESS ? "XeSS mode" : backend == TEMPORAL_FSR4 ? "FSR 4 mode" : "FSR 3.1 mode",
                   settings_temporal_mode(), modes, DLSS_MODE_COUNT, DLSS_QUALITY, help);
        const char *status = *settings_temporal_mode() == DLSS_OFF ? "Off" : backend == TEMPORAL_XESS ?
            (vk_xess_available() ? "XeSS" : "Regular rendering") : backend == TEMPORAL_FSR4 && vk_fsr4_available() ? "FSR 4 ML" :
            vk_fsr3_available() ? (backend == TEMPORAL_FSR4 ? "FSR 3.1 fallback" : "FSR 3.1") : "Regular rendering";
        opt_disable_next(); opt_action("Upscaler status", status, "The available rendering path. Unsupported experimental techniques do not run on incompatible hardware.");
    } else {
        static const char *modes[] = {"Off", "DLAA", "Quality", "Balanced", "Performance"};
        static const char *presets[] = {"Default", "K", "L", "M"};
        opt_choice("DLSS mode", &g_settings.dlss_mode, modes, DLSS_MODE_COUNT, DLSS_OFF,
                   "DLAA anti-aliases at display resolution. Quality, Balanced and Performance render at progressively "
                   "lower resolutions. Changes apply during play; history resets while the new mode is prepared.");
        opt_choice("DLSS preset", &g_settings.dlss_preset, presets, DLSS_PRESET_COUNT, DLSS_PRESET_DEFAULT,
                   "Model hint for NVIDIA DLSS. Default uses K for DLAA/Quality/Balanced and M for Performance. Changes during play.");
    }
    if (*settings_temporal_mode() != DLSS_OFF) {
        float strength = (float)*settings_temporal_sharpness();
        if (opt_slider(fsr ? "FSR sharpness" : backend == TEMPORAL_XESS ? "XeSS sharpness (RCAS)" : "DLSS sharpness (RCAS)",
                       &strength, 0, 100, 10, 20, "%g%%", "Reconstruction sharpening. 0% bypasses it. Saved independently for each technique; changes at once."))
            *settings_temporal_sharpness() = (int)strength;
        opt_toggle("CAS sharpening", settings_temporal_cas(), false, "Optional CAS after reconstruction, independent of the sharpening above.");
        if (!*settings_temporal_cas()) opt_disable_next();
        float amount = (float)*settings_temporal_cas_sharpness();
        if (opt_slider("CAS sharpness", &amount, 0, 100, 10, 20, "%g%%", "Optional CAS strength, saved separately for each technique. 0% bypasses the pass."))
            *settings_temporal_cas_sharpness() = (int)amount;
    } else if (g_settings.upscaler == UPSCALE_FSR1) {
        float amount = (float)g_settings.fsr1_sharpness;
        if (opt_slider("Sharpness", &amount, 0, 100, 10, 90, "%g%%", "FSR 1 RCAS sharpening. 0% bypasses the pass."))
            g_settings.fsr1_sharpness = (int)amount;
    }
#endif
#ifndef __APPLE__  // (Vulkan: nvn/vulkan/textures.c vk_sampler, "Banding")
    opt_section("Image");
    {
        static const char *an[] = {"Game", "2x", "4x", "8x", "16x"};
        static const int an_v[] = {ANISO_GAME, 2, 4, 8, 16};
        int ai = 0;
        for (int i = 0; i < 5; i++) if (an_v[i] == g_settings.aniso) ai = i;
        char ad[400];
        snprintf(ad, sizeof ad, "Keeps ground, roads and walls seen at a shallow angle sharp into the distance. Only surface "
                                "textures are raised; shadows and screen effects keep the game's setting. Game: the level "
                                "the game asks for, as on the Switch.%s",
                 settings_aniso_at_start() != g_settings.aniso ? "\n\nApplies when the game starts again." : "");
        if (opt_choice("Anisotropic filtering", &ai, an, 5, 4, ad, false)) g_settings.aniso = an_v[ai];
    }
    opt_toggle("Smooth gradients", &g_settings.deband, true,
               "Removes the visible color steps (banding) in skies, fog and dark areas: the game's image keeps more "
               "precision until it reaches the screen and gets a fine dither there. Off: as on the Switch.");
#endif
    opt_end();
}

// Performance: frame rate, the overlays, and the shaders (stutter)
static void tab_performance(void) {
    float scale = ImGui::GetFontSize() / 34;
    opt_begin("##performance", scale);
#ifndef __APPLE__  // (Vulkan: runtime/hle/odyssey_120.c, nvn/interp.c)
    opt_section("Frame rate");
    {
        static const char *fps[FPS_COUNT] = {"60 FPS", "90 FPS (Experimental)", "120 FPS (Experimental)"};
        opt_choice("Frame rate", &g_settings.frame_rate, fps, FPS_COUNT, FPS_60,
                   "90 and 120 FPS draw up to 90 or 120 frames per second while gameplay runs at its original 60 Hz. "
                   "Use a display that refreshes at least that fast (90 Hz: Steam Deck OLED). "
                   "60 FPS matches the Switch and uses the least power. Changes at once.", false);
    }
    if (g_settings.frame_rate != FPS_60) opt_disable_next();
#ifdef _WIN32
    if (vk_temporal_enabled()) opt_disable_next();
#endif
    opt_toggle("120 FPS interpolation", &g_settings.frame_interp, false,
               "Available with Frame rate set to 60 FPS and DLSS off. Adds an image between game frames, targeting 120 FPS "
               "on a 120 Hz or faster display. Busy scenes can temporarily fall back to 60 FPS.");
#endif
    opt_section("Power");
    if (opt_action("Battery saver", "",
                   "Sets lighter settings for handhelds and laptops on battery: 75% internal resolution upscaled with FSR 1, "
                   "no SMAA, no 120 FPS or interpolation, 4x texture filtering. The game itself still runs at its normal "
                   "speed. On a Steam Deck, also lower the power limit in the Quick Access menu (Performance > TDP Limit) "
                   "for the biggest savings. Some changes apply at the next start.")) {
        settings_battery_saver();
        settings_save();
    }
    opt_section("Overlay");
    opt_toggle("Show FPS", &g_settings.show_fps, false, "Shows the frame rate in the top right corner.");
    opt_toggle("Performance overlay", &g_settings.performance_overlay, false,
               "Shows the frame rate and frame times with a graph, GPU / CPU / memory use, temperatures and power where "
               "your PC reports them, the upscaler and resolution, and Mario's position. Hidden while this menu is open. "
               "Choose its parts below.");
    {
        static const char *const corners[OVERLAY_CORNER_COUNT] = {"Top right", "Top left", "Bottom right", "Bottom left"};
        static const char *const sizes[3] = {"Small", "Medium", "Large"};
        if (!g_settings.performance_overlay) opt_disable_next();
        opt_choice("Overlay position", &g_settings.overlay_corner, corners, OVERLAY_CORNER_COUNT, OVERLAY_TOP_RIGHT,
                   "The corner of the picture the performance overlay sits in.");
        if (!g_settings.performance_overlay) opt_disable_next();
        opt_choice("Overlay size", &g_settings.overlay_size, sizes, 3, 1, "The size of the performance overlay's text.");
        float opacity = (float)g_settings.overlay_opacity;
        if (!g_settings.performance_overlay) opt_disable_next();
        if (opt_slider("Overlay background", &opacity, 0, 100, 10, 80, "%g%%",
                       "How solid the overlay's background is. 0% shows only the text."))
            g_settings.overlay_opacity = (int)(opacity + 0.5f);
        struct { const char *label; bool *on; const char *help; } parts[] = {
            {"Show GPU", &g_settings.overlay_gpu, "GPU use, temperature, power draw, video memory and clock speed. "
             "Temperature, power and clock need an NVIDIA card, or Linux with an AMD or Intel one."},
            {"Show CPU", &g_settings.overlay_cpu, "CPU use, the game's own share of it, and on Linux the CPU's temperature."},
            {"Show memory", &g_settings.overlay_ram, "The PC's memory in use, and the game's own."},
            {"Show frame time graph", &g_settings.overlay_graph, "A line of the last frames' times: flat when the game runs evenly, a spike at each hitch."},
            {"Show hardware names", &g_settings.overlay_hardware, "The names of the graphics card and the processor."},
            {"Show upscaler and resolution", &g_settings.overlay_render, "The upscaler and anti-aliasing in use, and the game's internal resolution next to its size on screen."},
            {"Show Mario's position", &g_settings.overlay_position, "Mario's position in the kingdom, in metres. Useful for bug reports."},
        };
        for (auto &part : parts) {
            if (!g_settings.performance_overlay) opt_disable_next();
            opt_toggle(part.label, part.on, true, part.help);
        }
    }
#ifndef __APPLE__  // (Vulkan: nvn/vulkan/device.c vk_prepare_before_start)
    opt_section("Shaders");
    {
        static const char *const mem[MEMORY_COUNT] = {"Auto", "Normal", "Low"};
        opt_choice("Memory use", &g_settings.memory, mem, MEMORY_COUNT, MEMORY_AUTO,
                   "Normal builds the shaders of everything an area loads ahead of time, for fewer stutters (about 9 GB of "
                   "RAM with an NVIDIA GPU). Low builds each shader the first time it is drawn: a short stutter the first "
                   "time, about 6.5 GB. Auto picks Low on a PC with less than 12 GB of RAM. The game itself needs about "
                   "3.5 GB, so 4 GB of RAM is not enough. Takes effect at the next start.", false);
    }
    opt_toggle("Preload area shaders", &g_settings.shader_preload, false,
               "Builds the shaders of everything an area loads right away, to reduce stalls and objects popping in. "
               "Uses a lot of memory (the graphics driver keeps them), so it stops at a quarter of your RAM or 8 GB. "
               "Shaders the game has drawn before are built ahead either way. Applies to areas loaded from now on.");
    {
        static const char *wn[] = {"Skip", "Wait"};
        int wi = g_settings.shader_wait;
        if (opt_choice("While a shader loads", &wi, wn, 2, 0,
                       "What happens when something is drawn before its shader is ready. Skip: the game keeps running; "
                       "the object appears a moment later (or the picture is held for a frame). Wait: nothing is ever "
                       "missing, but the game pauses for the load (a short hitch).", false)) g_settings.shader_wait = wi != 0;
    }
    {
        static const char *wn[WARMUP_COUNT] = {"Auto", "Normal", "Long"};
        opt_choice("Warm-up at start", &g_settings.warmup, wn, WARMUP_COUNT, WARMUP_AUTO,
                   "How long the game spends at start getting the pipelines it has seen ready, so effects don't stutter "
                   "the first time they appear. Normal: up to 15 s and 3 GB of memory. Long: up to 30 s and 12 GB "
                   "(at most half of this PC's memory). Auto: Long with 16 GB of memory or more. Applies at the next "
                   "start.", false);
    }
    {
        bool on = vk_prepare_requested();
        if (opt_action("Compile shaders", on ? "At next start" : "", on
                       ? "The shader preparation pass will run the next time the game starts (a window shows the "
                         "progress; a few minutes). Quit and start the game again to do it now. Select again to cancel."
                       : "Compiles the game's shader archives and pipeline states recorded during play at the next "
                         "start. A progress window shows both passes. Use after a driver update or after visiting new areas."))
            vk_prepare_request(!on);
    }
#endif
    opt_end();
}

static void tab_audio(void) {
    opt_begin("##audio", ImGui::GetFontSize() / 34);
    opt_section("Sound");
    float pct = roundf(g_settings.volume * 100);
    if (opt_slider("Volume", &pct, 0, 200, 5, 100, "%g%%",
                   "The game's volume. 100% is the game's own level; above it, louder."))
        g_settings.volume = pct / 100.0f;
    opt_toggle("Reverb", &g_settings.reverb, true,
               "The game's echo effect in caves and big rooms.");
    opt_toggle("Movie audio", &g_settings.movie_audio, true,
               "Sound in the game's videos (e.g. the control tips). Applies the next time a video starts.");
    opt_toggle("Mute in background", &g_settings.mute_unfocused, false,
               "No sound while the game's window isn't in front (another window is active). The sound comes back as "
               "soon as you switch back.");
    opt_end();
}

// Mods (runtime/mods.h): the folders in mods/ with game files, each on or off (applies the next time the game starts).
static void tab_mods(float scale) {
    const char *how = "Put each mod in its own folder in here, with its romfs folder inside - as mods for the Switch "
                      "emulators come: mods/<Mod name>/romfs/...\n\nMods made for version 1.0.0 of the game work best. "
                      "If two mods change the same file, the one higher in the list wins.\n\nCode mods (exefs: IPS / "
                      "pchtxt patches, Super Mario Odyssey: Online) can't be used: the game code is recompiled from the "
                      "original game on your PC.";
    opt_begin("##mods", scale);
    int n = mods_count();
    opt_section(mods_changed() ? "Mods - restart the game to apply" : "Mods");
    if (!n) {
        opt_disable_next();
        opt_action("No mods found", "", how);
    }
    for (int i = 0; i < n; i++) {
        const ModInfo *m = mods_get(i);
        char d[1600];
        int k = snprintf(d, sizeof d, "mods/%s\n\n", m->id);
        if (!m->romfs) {
            snprintf(d + k, sizeof d - k, "A code mod: it changes the game code, which can't be used here - the game "
                                          "code is recompiled from the original game on your PC.");
        } else {
            k += snprintf(d + k, sizeof d - k, "%d file%s", m->files, m->files == 1 ? "" : "s");
            if (m->applied)
                k += snprintf(d + k, sizeof d - k, ": %d replace%s the game's, %d %s new.", m->replaced,
                              m->replaced == 1 ? "s" : "", m->added, m->added == 1 ? "is" : "are");
            else
                k += snprintf(d + k, sizeof d - k, ".");
            if (m->applied && m->shadowed)
                k += snprintf(d + k, sizeof d - k, " %d %s also in a mod higher in this list, which wins.", m->shadowed,
                              m->shadowed == 1 ? "is" : "are");
            if (m->applied && m->skipped)
                k += snprintf(d + k, sizeof d - k, " %d %s in %s the game doesn't have, left out.", m->skipped,
                              m->skipped == 1 ? "is" : "are", m->skipped == 1 ? "a folder" : "folders");
            if (m->code) k += snprintf(d + k, sizeof d - k, " Its code patches (exefs) can't be used here.");
            snprintf(d + k, sizeof d - k, "\n\nSwitching it on or off applies the next time the game starts.");
        }
        bool on = m->romfs && m->on;
        ImGui::PushID(i);  // (two mods may have the same name)
        if (!m->romfs) opt_disable_next();
        if (opt_toggle(m->name, &on, true, d)) mods_set_on(i, on);
        ImGui::PopID();
    }
    opt_section("Folder");
    char url[1600];
    if (opt_action("Open mods folder", "", how) && mods_dir_url(url, sizeof url)) SDL_OpenURL(url);
    opt_end();
}

static void tab_updates(float scale) {
    UpdStatus s;
    updater_status(&s);
    opt_begin("##updates", scale);
    char value[96];
    opt_section("Updates");
    snprintf(value, sizeof value, "%s", s.current);
    opt_disable_next();
    opt_action("This version", value, "The version of " RT_APP_NAME " you're running.");
    const char *what = "";
    char desc[640];
    snprintf(desc, sizeof desc, "Looks for a newer release on the project's GitHub page.");
    switch (s.state) {
    case UPD_UNCONFIGURED: what = "Not set up"; snprintf(desc, sizeof desc, "This build has no update source."); break;
    case UPD_IDLE: what = "Check now"; break;
    case UPD_CHECKING: what = "Checking..."; break;
    case UPD_LATEST: what = "Up to date"; break;
    case UPD_AVAILABLE: snprintf(value, sizeof value, "%s available", s.latest); what = value; break;
    case UPD_DOWNLOADING: snprintf(value, sizeof value, "Downloading %.0f%%", s.progress * 100); what = value; break;
    case UPD_READY: snprintf(value, sizeof value, "%s installed", s.latest); what = value; break;
    case UPD_FAILED: what = "Didn't work"; snprintf(desc, sizeof desc, "%s", s.error); break;
    }
    if (s.state == UPD_UNCONFIGURED || s.state == UPD_CHECKING || s.state == UPD_DOWNLOADING) opt_disable_next();
    if (opt_action("Check for updates", what, desc) && (s.state == UPD_IDLE || s.state == UPD_LATEST || s.state == UPD_FAILED))
        updater_check();
    if (s.can_install && (s.state == UPD_AVAILABLE || s.state == UPD_FAILED)) {
        snprintf(desc, sizeof desc, "Downloads %s from GitHub and puts it over this install's program files. Your game "
                                    "files, saves, settings and mods stay. After the restart the game code is rebuilt "
                                    "for the new version if it needs to be (a few minutes, once).", s.latest);
        if (opt_action("Download and install", "", desc)) updater_install();
    }
    if (s.state == UPD_READY && opt_action("Restart now", "", "Starts the new version. Progress since your last save is lost."))
        updater_restart();
    if (s.page[0] && opt_action("What's new", "", "Opens the release's page on GitHub in your browser.")) SDL_OpenURL(s.page);
    if (s.state != UPD_UNCONFIGURED) {
        char repo[600];
        snprintf(repo, sizeof repo, "https://github.com/%s", updater_repo());
        if (opt_action("Project page", "", "Opens the project's GitHub page in your browser.")) SDL_OpenURL(repo);
        if (opt_toggle("Check at start", &g_settings.check_updates, true,
                       "Looks for a new release each time the game starts (in the background; only GitHub is contacted, "
                       "and nothing about you or your game is sent)."))
            settings_save();
    }
    opt_end();
}

static void tab_quit(float scale) {
    static char report[1200];
    static char previous[1200];
    static bool checked_previous;
    static const char *result = "Create file";
    auto copy_result = []() { int s = diagnostics_copy_status(); return s > 0 ? "Path copied" : s == 0 ? "Copy pending" : "Saved"; };
    if (!strcmp(result, "Copy pending")) result = copy_result();
    opt_begin("##quit", scale);
    opt_section("Quit");
    if (opt_action("Quit to Desktop", "", "Closes " RT_APP_NAME " after confirmation.")) g_quit_prompt = true;
    opt_section("Diagnostics");
    if (opt_action("Copy crash report", result,
                   "Creates one text file with the saved settings, log, hardware/driver and fault details. Copies its path to paste into the attachment file picker. Includes local paths; nothing is uploaded.")) {
        settings_save();
        if (diagnostics_write_report(report, sizeof report))
            result = diagnostics_copy_path(report) ? copy_result() : "Saved";
        else result = "Could not save";
    }
    if (report[0]) { opt_disable_next(); opt_action("Saved report", "", report); }
    if (!checked_previous) { diagnostics_last_crash(previous, sizeof previous); checked_previous = true; }
    if (previous[0] &&
        opt_action("Copy last crash report", "", "Copies the saved crash report's path, including after restarting the game.")) {
        if (diagnostics_last_crash(report, sizeof report)) result = diagnostics_copy_path(report) ? copy_result() : "Saved";
        else { previous[0] = 0; result = "No saved crash"; }
    }
    opt_end();
}

static void quit_dialog(float scale) {
    if (!g_quit_prompt.load()) return;
    if (!ImGui::IsPopupOpen("Quit game?")) ImGui::OpenPopup("Quit game?");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Quit game?", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Progress since your last save will be lost.");
        ImGui::Spacing();
        if (ImGui::Button("Cancel", ImVec2(136 * scale, 0)) ||
            ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            g_quit_prompt = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SetItemDefaultFocus();
        ImGui::SameLine();
        if (ImGui::Button("Quit to Desktop", ImVec2(242 * scale, 0)) && g_quit_prompt.load()) {
            settings_save();
            SDL_Event q = {};
            q.type = SDL_EVENT_QUIT;
            if (SDL_PushEvent(&q)) {
                g_quit_prompt = false;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }
}

static void tab_about(void) {
    header("System language");
    {
        // what the game is told the console's language is: its text follows this (runtime/settings.c)
        char autol[64];
        snprintf(autol, sizeof autol, "Auto (%s)", settings_language_name());
        const char *names[N_LANGUAGES];
        names[0] = autol;
        for (int i = 1; i < N_LANGUAGES; i++) names[i] = g_language_names[i];
        if (choice("Language", &g_settings.language, names, N_LANGUAGES)) settings_save();
    }
    hint("The game's text language, as its own system setting. Applies the next time the game starts.");
    ImGui::Spacing();
    header("Controller");
    toggle("Motion controls", &g_settings.motion);
    hint("The controller's gyro and accelerometer for the game's motion moves: shake it to throw Cappy at a target, "
         "spin him, or tilt where the game asks. Needs a controller with motion sensors (PlayStation, Switch Pro, "
         "Steam Deck).");
    toggle("Vibration", &g_settings.rumble);
    hint("The controller's rumble, as the game sends it (the Switch's HD rumble on the controller's motors).");
    {
        static const int pct[] = {0, 5, 10, 15, 20, 25, 30};
        static const char *const names[] = {"Off", "5%", "10%", "15%", "20%", "25%", "30%"};
        int i = 0;
        for (int k = 0; k < 7; k++) if (pct[k] <= g_settings.stick_deadzone) i = k;
        if (choice("Stick deadzone", &i, names, 7)) g_settings.stick_deadzone = pct[i];
    }
    hint("How far a controller stick moves before it counts. Raise it if Mario or the camera drifts on their own.");
    ImGui::Spacing();
    header("Mouse");
    toggle("Mouse camera", &g_settings.mouse_camera);
    hint("Moving the mouse turns the camera. While you play, the pointer stays in the window; it is free again in this "
         "menu (Esc) or when you switch to another window.");
    {
        static const int pct[] = {25, 50, 75, 100, 150, 200, 300, 400};
        static const char *const names[] = {"25%", "50%", "75%", "100%", "150%", "200%", "300%", "400%"};
        int i = 0;
        for (int k = 0; k < 8; k++) if (pct[k] <= g_settings.mouse_sensitivity) i = k;
        if (choice("Mouse sensitivity", &i, names, 8)) g_settings.mouse_sensitivity = pct[i];
        hint("How far the mouse moves for the camera to turn at full speed. A quick flick turns it faster still.");
    }
    toggle("Invert mouse up / down", &g_settings.mouse_invert_y);
    hint("Moving the mouse up turns the camera down instead.");
    {
        static const char *const names[PROMPTS_COUNT] = {"Auto", "Controller", "Keyboard and mouse", "Switch", "Xbox",
                                                         "PlayStation"};
        int i = g_settings.prompts >= 0 && g_settings.prompts < PROMPTS_COUNT ? g_settings.prompts : PROMPTS_AUTO;
        if (choice("Button prompts", &i, names, PROMPTS_COUNT)) g_settings.prompts = i;
    }
    hint("The game's button icons. Auto switches as you play: the keys you bound (and the mouse for the camera) after "
         "you use the keyboard or mouse, the controller's own buttons (Xbox, PlayStation or Switch) after you use a "
         "controller. Controller always shows the connected controller's; Switch, Xbox and PlayStation always those.");
    toggle("Skip controller tips", &g_settings.skip_intro);
    hint("Leaves out the Joy-Con instructions screen's wait before the title screen. Applies at the next start.");
    ImGui::Spacing();
    header("Controls");
    controls_bindings_draw(ImGui::GetFontSize() / 34);
    ImGui::Spacing();
    header("Menu");
    ImGui::TextUnformatted("Open / close this menu:  F1, Esc, the controller's Home / Xbox / PlayStation button, or Back + Start");
    ImGui::TextUnformatted("Switch tabs:  L / R  (Q / E)        Back:  B  (Esc)");
    ImGui::TextUnformatted("Fullscreen:  F11 / Alt+Enter");
}

// Reboot (runtime/main.c): the game is gone - menu and name dialog close; the ImGui context stays.
RT_ON_RESET(menu_reset) {
    set_open(false);
    g_quit_prompt = false;
    g_kbd = 0;
    osk_cancel();
    pthread_mutex_lock(&q_lock);
    q_events.clear();
    pthread_mutex_unlock(&q_lock);
}

static void keyboard_draw(ImGuiIO &io, float scale) {
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 120));
    float w = fminf(io.DisplaySize.x * 0.9f, 1100 * scale);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(w, 0), ImGuiCond_Always);
    ImGui::Begin("##kbd", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                    ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PushFont(g_font_big);
    ImGui::TextUnformatted("Enter your name");
    ImGui::PopFont();
    static bool was_open, was_pad;
    bool pad = g_last_pad.load(), appearing = !was_open || pad != was_pad;  // (switching device: focus the new one)
    was_open = true, was_pad = pad;
    if (!pad) {  // keyboard: type straight into the field
        if (appearing) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(-1);
        ImGui::PushFont(g_font_big);
        ImGui::InputText("##name", g_kbd_text, (size_t)(g_kbd_max * 4 + 1 < (int)sizeof g_kbd_text ? g_kbd_max * 4 + 1 : sizeof g_kbd_text),
                         ImGuiInputTextFlags_CallbackCharFilter,
                         [](ImGuiInputTextCallbackData *d) -> int { return d->EventChar < 0x20 ? 1 : 0; });
        ImGui::PopFont();
        if ((int)strlen(g_kbd_text) > g_kbd_max) g_kbd_text[g_kbd_max] = 0;
        ImGui::TextDisabled("Enter  confirm      Esc  cancel      (%d / %d)", (int)strlen(g_kbd_text), g_kbd_max);
    } else {  // controller: on-screen keyboard (osk.mm, shared with our menus' text fields)
        int r = osk_keys(g_kbd_text, sizeof g_kbd_text, g_kbd_max, appearing, scale, g_font_big);
        if (r) g_kbd = r > 0 ? 2 : 3;  // (Start / + confirms in menu_handle_event too)
    }
    ImGui::End();
    if (g_kbd.load() != 1) was_open = false;
}

static void notice_draw(ImGuiIO &io, float scale) {
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 140));
    float w = fminf(io.DisplaySize.x * 0.9f, 1000 * scale);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(w, 0), ImGuiCond_Always);
    ImGui::Begin("##note", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                     ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PushFont(g_menu_font_big);
    ImGui::TextUnformatted(g_note_title);
    ImGui::PopFont();
    ImGui::PushFont(g_menu_font);
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(g_note_text);
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    static bool was_open;
    if (!was_open) ImGui::SetKeyboardFocusHere(), ImGui::SetNavCursorVisible(true);  // (the controller's cursor on OK)
    was_open = true;
    // OK: clicked, A on it, or any face button / + (only one choice: whichever the player's layout calls A)
    if (ImGui::Button("OK", ImVec2(200 * scale, 0)) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) ||
        ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false))
        g_note = 2;
    ImGui::PopFont();
    ImGui::End();
    if (g_note.load() != 1) was_open = false;
}

// Game thread (runtime/hle/applets.c): the notice over the last frame until the player closes it.
extern "C" void menu_notice_run(const char *title, const char *text) {
    extern void backend_represent(void);
    snprintf(g_note_title, sizeof g_note_title, "%s", title);
    snprintf(g_note_text, sizeof g_note_text, "%s", text);
    input_reset();
    g_note = 1;
    while (g_note.load() == 1 && !g_rt_unwind) {  // ~60 Hz (a reboot closes it)
        uint64_t t0 = SDL_GetTicks();
        backend_represent();
        uint64_t dt = SDL_GetTicks() - t0;
        if (dt < 16) SDL_Delay((uint32_t)(16 - dt));
    }
    g_note = 0;
    input_reset();  // (the button that closed it doesn't reach the game)
}

// Game thread (runtime/hle/swkbd.c): show the dialog, re-present the last frame until it's answered.
// Returns 1 confirmed (out = text, UTF-8), 0 cancelled.
extern "C" int menu_keyboard_run(const char *initial, int max_len, char *out, int cap) {
    extern void backend_represent(void);
    snprintf(g_kbd_text, sizeof g_kbd_text, "%s", initial ? initial : "");
    g_kbd_max = max_len > 0 && max_len < (int)sizeof g_kbd_text / 4 ? max_len : 8;
    input_reset();
    g_kbd = 1;
    // (test) ODYSSEY_KBD_AUTO=<text>: answer at once
    if (getenv("ODYSSEY_KBD_AUTO")) { snprintf(g_kbd_text, sizeof g_kbd_text, "%s", getenv("ODYSSEY_KBD_AUTO")); g_kbd = 2; }
    while (g_kbd.load() == 1 && !g_rt_unwind) {  // ~60 Hz (a reboot cancels)
        uint64_t t0 = SDL_GetTicks();
        backend_represent();
        uint64_t dt = SDL_GetTicks() - t0;
        if (dt < 16) SDL_Delay((uint32_t)(16 - dt));
    }
    bool ok = g_kbd.load() == 2 && g_kbd_text[0];
    if (ok) snprintf(out, cap, "%s", g_kbd_text);
    g_kbd = 0;
    input_reset();
    return ok;
}

// Draw the settings tabs in the current window, with a Prelaunch tab when the launcher supplies one.
static void tabs_body(float scale, void (*prelaunch)(float)) {
    // (id: the page; -1 = Prelaunch)
    struct Tab { const char *name; int id; bool on; };
    Tab tabs[11];
    int ntabs = 0;
    if (prelaunch) tabs[ntabs++] = {"Prelaunch", -1, true};
    // (ids: the pages below; 5-7 were Link's Awakening's and stay unused, so ODYSSEY_MENU_TAB numbers keep meaning;
    // the old Video page is Display (0), Graphics (3) and Performance (4) now)
    static const char *names[] = {"Display", "Audio", "Mods", "Graphics", "Performance", NULL, NULL, NULL, "Controls", "Quit",
                                  "Updates"};
    static const int order[] = {0, 3, 4, 1, 8, 2, 10, 9};  // Display, Graphics, Performance, Audio, Controls, Mods, Updates, Quit
    for (int i : order)
        if (i != 9 || !prelaunch) tabs[ntabs++] = {names[i], i, true};
    int cur = 0;  // g_tab / g_tab_request hold ids
    for (int i = 0; i < ntabs; i++) if (tabs[i].id == g_tab) cur = i;
    if (!tabs[cur].on) cur = 0;
    // L/R (or Q/E) switch tabs, skipping greyed-out ones (not while typing: Q / E are letters then, and the on-screen
    // keyboard has the controller)
    if (g_tab_request_ext >= 0) { g_tab_request = g_tab_request_ext; g_tab_request_ext = -1; }
    bool any_popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId), typing = ui_text_busy();
    static int focused_tab = -99;  // the tab the controller's cursor is on (from the frame before; -99: none)
    if (!any_popup && !typing) {
        int step = ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) || ImGui::IsKeyPressed(ImGuiKey_E, false) ? 1
                 : ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) || ImGui::IsKeyPressed(ImGuiKey_Q, false) ? -1 : 0;
        if (step) {
            int i = cur;
            do i = (i + step + ntabs) % ntabs; while (!tabs[i].on);
            g_tab_request = tabs[i].id;
            // the cursor on the tab bar goes along (left behind, A on it switched straight back; user 2026-10-11)
            if (focused_tab != -99) g_focus_tab = tabs[i].id, g_focus_frames = 0;
        }
    }
    // Down with the cursor on the tab bar: the page's first row (option rows; other pages keep ImGui's own pick)
    bool down_from_tab = focused_tab != -99 && ImGui::GetCurrentContext()->NavCursorVisible && !any_popup &&
                         (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadDown, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickDown, false) ||
                          ImGui::IsKeyPressed(ImGuiKey_DownArrow, false));
    opt_focus_first(down_from_tab);
    if (ImGui::BeginTabBar("##tabs", ImGuiTabBarFlags_NoTabListScrollingButtons)) {
        for (int k = 0; k < ntabs; k++) {
            const Tab &t = tabs[k];
            ImGuiTabItemFlags f = g_tab_request == t.id && t.on ? ImGuiTabItemFlags_SetSelected : 0;
            char lbl[64];
            snprintf(lbl, sizeof lbl, "  %s  ", t.name);
            ImGui::BeginDisabled(!t.on);
            // The tabs are their names (the shown one white, the others dim) with a red line under the shown one.
            // Focus (controller / keyboard): the tab lights up a little and gets a dim line, instead of ImGui's
            // outline (it sits 4 px outside the tab and the tab bar's clipping could cut its top off on macOS)
            // (focused_tab: from the frame before - colours go in before the tab is drawn)
            bool lit = focused_tab == t.id && ImGui::GetCurrentContext()->NavCursorVisible;
            ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_Text, k == cur || lit ? kTextV : kTextDimV);
            int pushed = 2;
            if (lit) {
                for (ImGuiCol c : {ImGuiCol_Tab, ImGuiCol_TabHovered, ImGuiCol_TabSelected}) ImGui::PushStyleColor(c, ImVec4(1, 1, 1, 0.07f));
                pushed += 3;
            }
            if (g_focus_tab == -1 && !prelaunch) g_focus_tab = 0;  // (no Prelaunch in game: Display, the first tab)
            bool focus_here = g_focus_tab == t.id || (g_focus_tab == -100 && t.id == g_tab);
            bool open = ImGui::BeginTabItem(lbl, NULL, f);
            ImGui::PopStyleColor(pushed);
            if (focus_here) {  // the controller's cursor starts on this tab (SetKeyboardFocusHere skips tabs: no tab stop)
                ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
                ImGui::SetNavCursorVisible(true);
                // a few frames: the page's child window appearing makes ImGui pick its first widget once it's laid out
                if (++g_focus_frames >= 4) g_focus_tab = -99, g_focus_frames = 0;
                else g_focus_tab = t.id;
            }
            if (open || lit) {
                ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
                float h = 3 * scale > 2 ? 3 * scale : 2, m = 10 * scale;
                ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(a.x + m, b.y - h), ImVec2(b.x - m, b.y), open ? kAccent : kAccentDim);
            }
            if (ImGui::IsItemFocused()) focused_tab = t.id;
            else if (focused_tab == t.id) focused_tab = -99;
            ImGui::EndDisabled();
            if (open) {
                g_tab = t.id;
                ImGui::Spacing();
                ImGui::BeginChild("##page", ImVec2(0, 0), ImGuiChildFlags_None,  // (the rest of the window: no hint line)
                                  ImGuiWindowFlags_NavFlattened);
                // Down from the tab bar, pages that aren't option rows (Controls): their first control
                // (option-row pages: opt_focus_first)
                if (down_from_tab && (t.id == -1 || t.id == 8)) {
                    opt_focus_first(false);
                    ImGui::SetKeyboardFocusHere();
                    ImGui::SetNavCursorVisible(true);
                }
                switch (t.id) {
                case -1: prelaunch(scale); break;
                case 0: tab_display(); break;
                case 3: tab_graphics(); break;
                case 4: tab_performance(); break;
                case 1: tab_audio(); break;
                case 2: tab_mods(scale); break;
                case 8: tab_about(); break;
                case 9: tab_quit(scale); break;
                case 10: tab_updates(scale); break;
                }
                static const char *scroll_test = getenv("ODYSSEY_MENU_SCROLL");  // test hook: page scroll, 0..1
                if (scroll_test) ImGui::SetScrollY(ImGui::GetScrollMaxY() * (float)atof(scroll_test));
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
        }
        g_tab_request = -2;
        opt_focus_first(false);  // (a page without option rows: nothing took it)
        ImGui::EndTabBar();
    }
}

void menu_tabs_draw(float scale, ImFont *font, ImFont *font_big, bool, void (*prelaunch)(float)) {
    ImFont *f0 = g_font, *f1 = g_font_big;
    g_font = font, g_font_big = font_big;
    ImGuiStyle saved = ImGui::GetStyle();  // the menu's look (another UI may have its own style)
    style_init();
    ImGui::GetStyle().ScaleAllSizes(scale);
    tabs_body(scale, prelaunch);
    ImGui::GetStyle() = saved;
    g_font = f0, g_font_big = f1;
}

// ---- the performance overlay: rows of a coloured label and up to three values, the frame time graph, then the
// hardware's names, the upscaler and resolution, and Mario's position. Each part has its switch (Performance tab).
#ifdef _WIN32  // (the temporal upscalers are Windows only)
extern "C" bool vk_temporal_enabled(void);
extern "C" bool vk_fsr4_available(void);
#endif
#define PERF_COLS 18.6f  // the overlay's width in font heights

static void perf_row(const char *label, ImVec4 color, const char *a, const char *b, const char *c) {
    float em = ImGui::GetFontSize(), x0 = ImGui::GetCursorPosX();
    ImGui::TextColored(color, "%s", label);
    if (a) { ImGui::SameLine(x0 + em * 3.3f); ImGui::TextUnformatted(a); }
    if (b) { ImGui::SameLine(x0 + em * 7.5f); ImGui::TextUnformatted(b); }
    if (c) { ImGui::SameLine(x0 + em * 12.1f); ImGui::TextUnformatted(c); }
}

// "43%" / "40 C": the value with its unit, or "-" where this PC doesn't tell
static const char *perf_num(char *buf, size_t n, int v, const char *unit) {
    if (v < 0) return "-";
    snprintf(buf, n, "%d%s", v, unit);
    return buf;
}

static void draw_performance(void) {
    const ImVec4 green(0.56f, 0.86f, 0.36f, 1), blue(0.36f, 0.66f, 0.92f, 1), pink(0.82f, 0.52f, 0.72f, 1), white(1, 1, 1, 1);
    const ImVec4 dim(0.78f, 0.80f, 0.82f, 1);
    char a[48], b[48], c[48];
    float em = ImGui::GetFontSize();
    // frame times of the last 180 presents
    static float times[180];
    static int at, filled;
    float dt = ImGui::GetIO().DeltaTime * 1000.0f;
    times[at] = dt, at = (at + 1) % 180;
    if (filled < 180) filled++;
    float worst = 0, sum = 0, recent = 0;
    int n_recent = 0;
    for (int i = 0; i < filled; i++) {
        float t = times[(at - 1 - i + 360) % 180];
        if (t > worst) worst = t;
        sum += t;
        if (recent < 500.0f) recent += t, n_recent++;  // (the average over the last half second)
    }
    float ms = n_recent ? recent / n_recent : 0;
#ifndef __APPLE__
    VulkanOverlayStats s = vk_overlay_stats();
    if (g_settings.overlay_gpu) {
        char t[16], w[16];
        perf_row("GPU", green, perf_num(a, sizeof a, s.gpu_usage, "%"), perf_num(t, sizeof t, s.gpu_temp_c, " C"),
                 perf_num(w, sizeof w, s.gpu_power_w, " W"));
        if (s.vram_budget_mib) {
            snprintf(a, sizeof a, "%.2f GB", s.vram_mib / 1024.0);
            snprintf(b, sizeof b, "/ %.1f GB", s.vram_budget_mib / 1024.0);
            perf_row("VRAM", green, a, b, s.gpu_clock_mhz > 0 ? perf_num(c, sizeof c, s.gpu_clock_mhz, " MHz") : NULL);
        }
    }
    if (g_settings.overlay_cpu) {
        char t[16];
        if (s.game_cpu_usage >= 0) snprintf(c, sizeof c, "game %d%%", s.game_cpu_usage);
        perf_row("CPU", blue, perf_num(a, sizeof a, s.cpu_usage, "%"), perf_num(t, sizeof t, s.cpu_temp_c, " C"),
                 s.game_cpu_usage >= 0 ? c : NULL);
    }
    if (g_settings.overlay_ram && s.ram_total_mib) {
        snprintf(a, sizeof a, "%.1f GB", s.ram_used_mib / 1024.0);
        snprintf(b, sizeof b, "/ %.1f GB", s.ram_total_mib / 1024.0);
        snprintf(c, sizeof c, "game %.1f GB", s.game_ram_mib / 1024.0);
        perf_row("RAM", pink, a, b, s.game_ram_mib ? c : NULL);
    }
#endif
    if (ms > 0) {
        snprintf(a, sizeof a, "%.0f", 1000.0f / ms);
        snprintf(b, sizeof b, "%.1f ms", ms);
        snprintf(c, sizeof c, "max %.1f ms", worst);
        perf_row("FPS", white, a, b, c);
    } else perf_row("FPS", white, "-", NULL, NULL);
    if (g_settings.overlay_graph) {
        // the frame times as a line: flat when every frame takes as long, a spike at a hitch
        ImGui::Dummy(ImVec2(0, em * 0.25f));
        ImVec2 p = ImGui::GetCursorScreenPos(), size(em * PERF_COLS, em * 2.2f);
        ImDrawList *dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), IM_COL32(40, 44, 36, 190), em * 0.3f);
        float top = filled ? sum / filled * 2.0f : 33.4f;
        if (top < worst * 1.1f) top = worst * 1.1f;
        if (top < 1) top = 1;
        ImVec2 pts[180];
        float pad = em * 0.6f;
        for (int i = 0; i < filled; i++) {
            float t = times[(at - filled + i + 360) % 180];
            pts[i] = ImVec2(p.x + pad + (size.x - 2 * pad) * (filled > 1 ? (float)i / (filled - 1) : 0),
                            p.y + size.y - pad * 0.5f - (size.y - pad) * (t / top));
        }
        if (filled > 1) dl->AddPolyline(pts, filled, IM_COL32(235, 235, 235, 230), 0, em * 0.07f < 1 ? 1 : em * 0.07f);
        ImGui::Dummy(size);
        ImGui::Dummy(ImVec2(0, em * 0.15f));
    } else ImGui::Dummy(ImVec2(em * PERF_COLS, 0));  // (the overlay keeps its width)
#ifndef __APPLE__
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + em * PERF_COLS);
    if (g_settings.overlay_hardware) {
        if (s.gpu_name && *s.gpu_name) ImGui::TextColored(dim, "%s", s.gpu_name);
        if (s.cpu_name && *s.cpu_name) ImGui::TextColored(dim, "%s", s.cpu_name);
    }
    if (g_settings.overlay_render) {
        const char *up = g_settings.res_mode == RES_MATCH ? "Off" : g_settings.upscaler == UPSCALE_FSR1 ? "FSR 1" : "Bilinear";
        const char *aa = g_settings.antialias == AA_SMAA ? "SMAA" : "Off";
#ifdef _WIN32
        char name[48];
        if (vk_temporal_enabled()) {
            static const char *const modes[DLSS_MODE_COUNT] = {"", "Native AA", "Quality", "Balanced", "Performance"};
            int backend = g_settings.temporal_backend, mode = *settings_temporal_mode();
            const char *tech = "DLSS";
            if (backend == TEMPORAL_XESS) tech = "XeSS";
            else if (backend == TEMPORAL_FSR4) tech = vk_fsr4_available() ? "FSR 4" : "FSR 3.1";
            else if (backend == TEMPORAL_FSR3) tech = "FSR 3.1";
            if (mode < 0 || mode >= DLSS_MODE_COUNT) mode = 0;
            snprintf(name, sizeof name, "%s %s", tech, backend == TEMPORAL_DLSS && mode == DLSS_DLAA ? "DLAA" : modes[mode]);
            up = name, aa = "Temporal";
        }
#endif
        ImGui::TextColored(dim, "Upscaler: %s | Anti-aliasing: %s", up, aa);
        if (s.render_w && s.out_w) ImGui::TextColored(dim, "%u x %u -> %u x %u", s.render_w, s.render_h, s.out_w, s.out_h);
    }
    ImGui::PopTextWrapPos();
    float pos[3];
    if (g_settings.overlay_position && odyssey_player_position(pos))  // (in metres: the game's units are centimetres)
        ImGui::Text("X %.1f  Y %.1f  Z %.1f", pos[0] / 100.0f, pos[1] / 100.0f, pos[2] / 100.0f);
#endif
}
#ifdef __APPLE__
void menu_render(id<MTLDevice> dev, id<MTLCommandBuffer> cb, id<MTLTexture> target) {
    const double tw = target.width, th = target.height;
#else
void menu_render(void *cmd, uint32_t target_w, uint32_t target_h) {
    const double tw = target_w, th = target_h;
#endif
    bool open = g_open.load(), setup = g_setup.load();
    bool kbd = g_kbd.load() == 1, note = g_note.load() == 1;
    if (!open && !kbd && !note && !setup && !menu_stats_visible()) {
        if (g_inited) { pthread_mutex_lock(&q_lock); q_events.clear(); pthread_mutex_unlock(&q_lock); }
        return;
    }
#ifdef __APPLE__
    if (!g_inited) init(dev);
#else
    if (!g_inited) init();
#endif
    ImGui::SetCurrentContext(g_ctx);
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)tw, (float)th);
    io.DisplayFramebufferScale = ImVec2(1, 1);
    io.MouseDrawCursor = open && !setup && g_draw_mouse_cursor.load();
    static double last;
    double now = SDL_GetTicksNS() / 1e9;
    io.DeltaTime = last > 0 && now > last ? (float)(now - last) : 1.0f / 60;
    last = now;
    drain_events(io);
    // fonts are built at 34/56 px for a ~1080 px tall output; scale to the window. The menu and overlays at 68% of
    // that (a menu over about 40% of the game's picture); the installer page at full size.
    float scale = (float)(th / 1080.0) * (setup ? 1.0f : 0.68f);
    if (scale < 0.45f) scale = 0.45f;
    io.FontGlobalScale = scale;

#ifdef __APPLE__
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = target;
    rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
#endif
    // spacing/rounding follow the output size too (style tuned for 1080 p)
    static ImGuiStyle base;
    static float style_scale = -1;
    if (style_scale < 0) base = ImGui::GetStyle();
    if (fabsf(style_scale - scale) > 0.01f) {
        ImGui::GetStyle() = base;
        ImGui::GetStyle().ScaleAllSizes(scale);
        style_scale = scale;
    }
#ifdef __APPLE__
    ImGui_ImplMetal_NewFrame(rp);
#else
    ImGui_ImplVulkan_NewFrame();
#endif
    ImGui::NewFrame();

    if (setup) {
        setup_draw(scale, g_menu_font, g_menu_font_big);
        open = false;
    } else if (menu_stats_visible() && (!open || !g_settings.performance_overlay)) {
        bool advanced = g_settings.performance_overlay;
        float margin = 16 * scale;
        // the overlay's corner, size and background are the player's choice; the plain FPS counter stays top right
        int corner = advanced ? g_settings.overlay_corner : OVERLAY_TOP_RIGHT;
        bool right = corner == OVERLAY_TOP_RIGHT || corner == OVERLAY_BOTTOM_RIGHT;
        bool bottom = corner == OVERLAY_BOTTOM_RIGHT || corner == OVERLAY_BOTTOM_LEFT;
        static const float sizes[3] = {0.64f, 0.80f, 1.0f};
        ImGui::SetNextWindowPos(ImVec2(right ? io.DisplaySize.x - margin : margin, bottom ? io.DisplaySize.y - margin : margin),
                                ImGuiCond_Always, ImVec2(right ? 1 : 0, bottom ? 1 : 0));
        ImGui::SetNextWindowBgAlpha(advanced ? g_settings.overlay_opacity * 0.01f : 0.5f);
        if (advanced) {
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8 * scale, 3 * scale));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16 * scale, 12 * scale));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12 * scale);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.04f, 0.07f, 0.08f, 1));
        }
        ImGui::Begin("##fps", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                        ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
        ImGui::SetWindowFontScale(advanced ? sizes[g_settings.overlay_size >= 0 && g_settings.overlay_size <= 2 ? g_settings.overlay_size : 1] : 1.0f);
        if (advanced) draw_performance();
        else ImGui::Text("%.0f fps", io.Framerate);
        ImGui::End();
        if (advanced) { ImGui::PopStyleColor(); ImGui::PopStyleVar(4); }
    }

    if (!setup) {  // a new release found at start: a note in the corner for a few seconds (the menu's Updates page has it)
        static double shown_at = -1;
        UpdStatus us;
        updater_status(&us);
        if (us.state == UPD_AVAILABLE && shown_at < 0) shown_at = now;
        if (shown_at >= 0 && now - shown_at < 12 && !open) {
            float margin = 16 * scale;
            ImGui::SetNextWindowPos(ImVec2(margin, io.DisplaySize.y - margin), ImGuiCond_Always, ImVec2(0, 1));
            ImGui::SetNextWindowBgAlpha(0.85f);
            ImGui::Begin("##update_note", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                                    ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
            ImGui::PushStyleColor(ImGuiCol_Text, kAccentSoftV);
            ImGui::Text("Update %s available", us.latest);
            ImGui::PopStyleColor();
            ImGui::TextUnformatted("F1 > Updates to install it");
            ImGui::End();
        }
    }

    if (open) {
        // dim the game behind the menu
        ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 130));
        // a window in the middle with the game around it: about 55% x 70% of the picture (40% of it)
        float w = fminf(io.DisplaySize.x * 0.55f, 1560 * scale), h = fminf(io.DisplaySize.y * 0.72f, 1100 * scale);
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
        ImGui::Begin("##menu", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        ImFont *keep_big = g_font_big;  // (the window in the menu's font; header() uses g_font_big)
        g_font_big = g_menu_font_big;
        ImGui::PushFont(g_menu_font);
        // header: the crescent and the name, a close X in the corner (B / Esc too), a hairline under them
        ImGui::PushFont(g_font_big);
        {
            float lh = ImGui::GetFontSize(), sz = lh * 1.35f;
            ImVec2 lp = ImGui::GetCursorScreenPos();
            // (the icon's pipe, centred on the line)
            theme_logo(ImGui::GetWindowDrawList(), ImVec2(lp.x, lp.y + lh * 0.5f - sz * 0.5f), sz, kAccent, theme_u32(kPanelV, 1));
            ImGui::Dummy(ImVec2(sz, lh));
            ImGui::SameLine(0, lh * 0.4f);
            ImGui::TextUnformatted(RT_APP_NAME);
        }
        ImGui::PopFont();
        {
            float bx = 44 * scale;
            ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x - bx);
            ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
            bool close = ImGui::InvisibleButton("##close", ImVec2(bx, bx));
            ImGui::PopStyleColor();
            bool hot = ImGui::IsItemHovered() || ImGui::IsItemFocused();
            ImU32 col = hot ? kAccent : theme_u32(kTextV, 0.85f);
            float m = bx * 0.3f, t = (hot ? 3.0f : 2.4f) * scale * 1.5f;
            ImDrawList *dl = ImGui::GetWindowDrawList();
            if (hot) dl->AddRectFilled(p, ImVec2(p.x + bx, p.y + bx), theme_u32(kSurfaceHoverV), 4 * scale);
            dl->AddLine(ImVec2(p.x + m, p.y + m), ImVec2(p.x + bx - m, p.y + bx - m), col, t);
            dl->AddLine(ImVec2(p.x + bx - m, p.y + m), ImVec2(p.x + m, p.y + bx - m), col, t);
            if (close) set_open(false);
        }
        ImGui::Separator();

        bool any_popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId), typing = ui_text_busy();
        tabs_body(scale, NULL);
        quit_dialog(scale);
        // B with nothing focused-deeper closes the menu (not the Esc / B that ends typing: typing is from the frame
        // before, so it's still true the frame a field lets go)
        if (!any_popup && !typing && !ui_text_busy() && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
            (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
            set_open(false);
        ImGui::PopFont();
        g_font_big = keep_big;
        ImGui::End();
        osk_draw(g_font_big, scale);  // (a text field activated with the controller)
    }

    if (kbd) keyboard_draw(io, scale);
    if (note && !open) notice_draw(io, scale);  // (the menu opened over it: the notice waits behind it)
    ImGui::Render();
    g_text_busy = open && ui_text_busy();
#ifdef __APPLE__
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), cb, enc);
    [enc endEncoding];
#else
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), (VkCommandBuffer)cmd);  // inside the window image's rendering
#endif
    g_drawn = true;
}
