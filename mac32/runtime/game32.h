// mac32: per-game settings and the hooks the generic Cocoa/OpenGL host (host.m) calls. A game project defines
// `const Rt32Game rt32_game` and implements the game32_* functions, usually as calls into the engine's own C API
// (the functions the original app's Cocoa glue called).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "rt.h"

typedef struct {
    const char *name;           // window title / display name, e.g. "My Game"
    const char *app_bundle;     // the original Mac app, e.g. "MyGame.app"
    const char *exe_name;       // its executable: <app_bundle>/Contents/MacOS/<exe_name>
    const char *steam_folder;   // steamapps/common/<steam_folder>/<app_bundle> (NULL: no Steam lookup)
    const char *prefs_domain;   // CFPreferences domain, e.g. the original bundle ID, so settings are shared with it
    const char *support_dir;    // ~/Library/Application Support/<support_dir>: offline Steam stats and achievements
    const char *log_name;       // ~/Library/Logs/<log_name>.log
    int width, height;          // initial window size in points
} Rt32Game;

extern const Rt32Game rt32_game;

// ---- hooks implemented by the game (c is the main guest CPU) ----
// After the static constructors ran, with the OpenGL context current: start the engine. Returns 0 on failure.
int  game32_start(CPU *c);
// The engine's backbuffer size in pixels and whether it wants a window (vs. fullscreen); 0 x 0 if unknown.
void game32_window_size(CPU *c, float *w, float *h);
int  game32_windowed(CPU *c);
// One frame (a 60 Hz timer drives the view). Called with the context current.
void game32_frame(CPU *c);
int  game32_quit_requested(CPU *c);
// Input. Coordinates are engine pixels (origin top-left). mods are NSEventModifierFlags.
void game32_mouse_move(CPU *c, float x, float y);
void game32_mouse_button(CPU *c, int button, int down, int clicks);   // 0 left, 1 right, 2 middle
void game32_wheel(CPU *c, int delta);
void game32_key(CPU *c, int down, uint32_t ch, uint32_t mods);         // ch: first character, ignoring modifiers
void game32_modifier(CPU *c, int which, int down);                    // 0 shift, 1 control, 2 option

// ---- provided by the host for the game and the runtime ----
void host_present(void);            // swap buffers (call from the engine's present)
void host_show_cursor(int show);    // the game shows/hides the system pointer over its view
const char *host_resources_dir(void);   // <app_bundle>/Contents/Resources of the user's copy (also the cwd)
