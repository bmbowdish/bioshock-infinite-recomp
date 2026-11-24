// A 32-bit Mac game on Recomp-Framework (mac32): settings and the hooks the host calls (see runtime/game32.h).
// The hooks usually call the engine's own C entry points (the ones the original app's Cocoa code called), found
// by name with guest_func("_Symbol") and called with guest_call(c, addr, nargs, args).
#include "game32.h"
#include "tables.h"

const Rt32Game rt32_game = {
    .name = "My Game",
    .app_bundle = "MyGame.app",
    .exe_name = "MyGame",
    .steam_folder = "MyGame",                // steamapps/common/MyGame/MyGame.app
    .prefs_domain = "com.example.MyGame",    // the original app's bundle ID: settings are shared with it
    .support_dir = "MyGame-recomp",
    .log_name = "MyGame-recomp",
    .width = 1024, .height = 768,
};

static uint32_t call0(CPU *c, const char *sym) { return guest_call(c, guest_func(sym), 0, NULL); }

int game32_start(CPU *c) { return (call0(c, "_Engine_Start") & 0xFF) != 0; }
void game32_window_size(CPU *c, float *w, float *h) { *w = 1024; *h = 768; }   // ask the engine
int game32_windowed(CPU *c) { return 1; }
void game32_frame(CPU *c) { call0(c, "_Engine_Frame"); }
int game32_quit_requested(CPU *c) { return 0; }
void game32_mouse_move(CPU *c, float x, float y) {}
void game32_mouse_button(CPU *c, int button, int down, int clicks) {}
void game32_wheel(CPU *c, int delta) {}
void game32_key(CPU *c, int down, uint32_t ch, uint32_t mods) {}
void game32_modifier(CPU *c, int which, int down) {}

// Engine functions replaced natively are listed in hle.txt and defined here as H_<name>(CPU *c), for example the
// engine's present call:  void H_RenderPresent(CPU *c) { host_present(); }
