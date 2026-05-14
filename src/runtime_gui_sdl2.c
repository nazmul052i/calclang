/*
   SDL2-backed GUI builtins for the native runtime.

   Compiled and linked alongside `runtime_x64.c` whenever CalcLang
   programs use any `gui_*` function. Adds a small set of immediate-mode
   primitives — window/clear/rect/line/poll/key/mouse — and a simple
   widget kit lives on top in CalcLang at `lib/gui.calc`.

   This file is in addition to (not in place of) `runtime_x64.c`. It
   shares the Value model and helpers from there via the public
   declarations in `include/runtime.h`. Each function below is a
   `Value cl_builtin_<name>(...)` callable from CalcLang.

   Linking: needs `-lSDL2 -lSDL2main -lmingw32` on Windows mingw, with
   SDL2.dll copied alongside the produced executable. The calcnat
   driver handles both automatically when it detects a vendored SDL2
   tree under third_party (see tools/setup_sdl2.sh). */

#include "runtime.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* All GUI state lives in a single static struct. There's only one
   window per program — opening a second one isn't supported. That's
   fine for the educational use cases and saves an awful lot of
   plumbing. */
typedef struct {
    int           inited;
    SDL_Window   *win;
    SDL_Renderer *ren;
    int           width;
    int           height;
    int           should_close;
    /* Current draw color, set by gui_set_color and used by every draw call. */
    Uint8         r, g, b, a;
    /* Pressed-this-frame key set; cleared on each gui_poll_events. */
    int           keys_pressed_count;
    SDL_Keycode   keys_pressed[64];
    /* Mouse state. */
    int           mouse_x, mouse_y;
    int           mouse_button_down[5];        /* SDL has up to ~5 buttons */
    int           mouse_button_clicked[5];     /* one-shot, cleared by poll */
} GuiState;

static GuiState G;

/* Forward — exported by runtime_x64.c. */
extern void cl_die_rt(const char *msg);
extern void require_num(Value v, const char *where);
extern void require_str(Value v, const char *where);

/* Helper: numeric-coerce a Value to int. */
static int as_int(Value v, const char *where) {
    require_num(v, where);
    return (int)cl_as_num(v);
}

/* Helper: string -> SDL_Keycode. We accept friendly names so CalcLang
   code doesn't need to know SDL's integer constants. Returns 0
   (SDLK_UNKNOWN) for anything unmatched, so callers always get a
   defined-but-unmatched answer. */
static SDL_Keycode key_from_name(const char *name) {
    if (!name) return 0;
    /* Arrows + common keys. */
    if (strcmp(name, "left")    == 0) return SDLK_LEFT;
    if (strcmp(name, "right")   == 0) return SDLK_RIGHT;
    if (strcmp(name, "up")      == 0) return SDLK_UP;
    if (strcmp(name, "down")    == 0) return SDLK_DOWN;
    if (strcmp(name, "space")   == 0) return SDLK_SPACE;
    if (strcmp(name, "enter")   == 0) return SDLK_RETURN;
    if (strcmp(name, "esc")     == 0 || strcmp(name, "escape") == 0) return SDLK_ESCAPE;
    if (strcmp(name, "tab")     == 0) return SDLK_TAB;
    if (strcmp(name, "back")    == 0 || strcmp(name, "backspace") == 0) return SDLK_BACKSPACE;
    if (strcmp(name, "shift")   == 0) return SDLK_LSHIFT;
    if (strcmp(name, "ctrl")    == 0) return SDLK_LCTRL;
    if (strcmp(name, "alt")     == 0) return SDLK_LALT;
    /* Single-character keys: pass through as the SDLK_<char> value
       (matches the lowercase ASCII codepoint for letters/digits). */
    if (name[0] && name[1] == '\0') {
        char c = name[0];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        return (SDL_Keycode)(unsigned char)c;
    }
    return 0;
}

/* gui_init(w, h, title) — open the window. Returns 1 on success, 0
   on failure. Safe to call only once; later calls are no-ops. */
Value cl_builtin_gui_init(Value w_v, Value h_v, Value title_v) {
    if (G.inited) return cl_from_num(1.0);
    int w = as_int(w_v, "gui_init");
    int h = as_int(h_v, "gui_init");
    require_str(title_v, "gui_init");
    const char *title = cl_as_str(title_v)->data;

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "gui_init: SDL_Init failed: %s\n", SDL_GetError());
        return cl_from_num(0.0);
    }
    G.win = SDL_CreateWindow(title,
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        w, h, SDL_WINDOW_SHOWN);
    if (!G.win) {
        fprintf(stderr, "gui_init: SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return cl_from_num(0.0);
    }
    G.ren = SDL_CreateRenderer(G.win, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!G.ren) {
        /* Fallback to software rendering if the GPU path is unavailable. */
        G.ren = SDL_CreateRenderer(G.win, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!G.ren) {
        fprintf(stderr, "gui_init: SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(G.win);
        SDL_Quit();
        return cl_from_num(0.0);
    }
    SDL_SetRenderDrawBlendMode(G.ren, SDL_BLENDMODE_BLEND);
    G.width = w;
    G.height = h;
    G.r = 255; G.g = 255; G.b = 255; G.a = 255;
    G.inited = 1;
    return cl_from_num(1.0);
}

Value cl_builtin_gui_close(void) {
    if (!G.inited) return cl_from_num(0.0);
    if (G.ren) SDL_DestroyRenderer(G.ren);
    if (G.win) SDL_DestroyWindow(G.win);
    SDL_Quit();
    G.ren = NULL; G.win = NULL; G.inited = 0;
    return cl_from_num(0.0);
}

Value cl_builtin_gui_should_close(void) {
    return cl_from_num(G.should_close ? 1.0 : 0.0);
}

/* Pump SDL's event queue. Updates mouse state, fires the per-frame
   "key was pressed" and "mouse was clicked" flags, and watches for
   SDL_QUIT (window-close button, alt-F4). Must be called every frame
   or the OS will mark the window unresponsive. */
Value cl_builtin_gui_poll_events(void) {
    if (!G.inited) return cl_from_num(0.0);
    G.keys_pressed_count = 0;
    for (int i = 0; i < 5; i++) G.mouse_button_clicked[i] = 0;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
            case SDL_QUIT:
                G.should_close = 1;
                break;
            case SDL_KEYDOWN:
                if (!e.key.repeat && G.keys_pressed_count < 64) {
                    G.keys_pressed[G.keys_pressed_count++] = e.key.keysym.sym;
                }
                if (e.key.keysym.sym == SDLK_ESCAPE) G.should_close = 1;
                break;
            case SDL_MOUSEMOTION:
                G.mouse_x = e.motion.x;
                G.mouse_y = e.motion.y;
                break;
            case SDL_MOUSEBUTTONDOWN: {
                int b = e.button.button - 1;
                if (b >= 0 && b < 5) {
                    G.mouse_button_down[b] = 1;
                    G.mouse_button_clicked[b] = 1;
                }
                break;
            }
            case SDL_MOUSEBUTTONUP: {
                int b = e.button.button - 1;
                if (b >= 0 && b < 5) G.mouse_button_down[b] = 0;
                break;
            }
        }
    }
    return cl_from_num(0.0);
}

/* gui_set_color(r, g, b) — components 0..255. Affects every draw call
   until the next set_color. Alpha defaults to 255 (opaque). */
Value cl_builtin_gui_set_color(Value rv, Value gv, Value bv) {
    int r = as_int(rv, "gui_set_color");
    int g = as_int(gv, "gui_set_color");
    int b = as_int(bv, "gui_set_color");
    if (r < 0) r = 0; if (r > 255) r = 255;
    if (g < 0) g = 0; if (g > 255) g = 255;
    if (b < 0) b = 0; if (b > 255) b = 255;
    G.r = (Uint8)r; G.g = (Uint8)g; G.b = (Uint8)b;
    if (G.inited) SDL_SetRenderDrawColor(G.ren, G.r, G.g, G.b, G.a);
    return cl_from_num(0.0);
}

Value cl_builtin_gui_clear(Value rv, Value gv, Value bv) {
    if (!G.inited) return cl_from_num(0.0);
    int r = as_int(rv, "gui_clear");
    int g = as_int(gv, "gui_clear");
    int b = as_int(bv, "gui_clear");
    SDL_SetRenderDrawColor(G.ren, (Uint8)r, (Uint8)g, (Uint8)b, 255);
    SDL_RenderClear(G.ren);
    SDL_SetRenderDrawColor(G.ren, G.r, G.g, G.b, G.a);   /* restore */
    return cl_from_num(0.0);
}

Value cl_builtin_gui_rect(Value xv, Value yv, Value wv, Value hv) {
    if (!G.inited) return cl_from_num(0.0);
    SDL_Rect r = {
        as_int(xv, "gui_rect"), as_int(yv, "gui_rect"),
        as_int(wv, "gui_rect"), as_int(hv, "gui_rect")
    };
    SDL_RenderFillRect(G.ren, &r);
    return cl_from_num(0.0);
}

Value cl_builtin_gui_rect_outline(Value xv, Value yv, Value wv, Value hv) {
    if (!G.inited) return cl_from_num(0.0);
    SDL_Rect r = {
        as_int(xv, "gui_rect_outline"), as_int(yv, "gui_rect_outline"),
        as_int(wv, "gui_rect_outline"), as_int(hv, "gui_rect_outline")
    };
    SDL_RenderDrawRect(G.ren, &r);
    return cl_from_num(0.0);
}

Value cl_builtin_gui_line(Value x1v, Value y1v, Value x2v, Value y2v) {
    if (!G.inited) return cl_from_num(0.0);
    SDL_RenderDrawLine(G.ren,
        as_int(x1v, "gui_line"), as_int(y1v, "gui_line"),
        as_int(x2v, "gui_line"), as_int(y2v, "gui_line"));
    return cl_from_num(0.0);
}

Value cl_builtin_gui_pixel(Value xv, Value yv) {
    if (!G.inited) return cl_from_num(0.0);
    SDL_RenderDrawPoint(G.ren,
        as_int(xv, "gui_pixel"), as_int(yv, "gui_pixel"));
    return cl_from_num(0.0);
}

Value cl_builtin_gui_present(void) {
    if (!G.inited) return cl_from_num(0.0);
    SDL_RenderPresent(G.ren);
    return cl_from_num(0.0);
}

Value cl_builtin_gui_set_title(Value title_v) {
    if (!G.inited) return cl_from_num(0.0);
    require_str(title_v, "gui_set_title");
    SDL_SetWindowTitle(G.win, cl_as_str(title_v)->data);
    return cl_from_num(0.0);
}

/* gui_key_down(name) — currently-held? Uses SDL's persistent keyboard
   state, so this works even between events. */
Value cl_builtin_gui_key_down(Value name_v) {
    if (!G.inited) return cl_from_num(0.0);
    require_str(name_v, "gui_key_down");
    SDL_Keycode kc = key_from_name(cl_as_str(name_v)->data);
    if (kc == 0) return cl_from_num(0.0);
    SDL_Scancode sc = SDL_GetScancodeFromKey(kc);
    const Uint8 *state = SDL_GetKeyboardState(NULL);
    return cl_from_num(state[sc] ? 1.0 : 0.0);
}

/* gui_key_pressed(name) — just pressed this frame (since the last
   gui_poll_events)? Useful for menu/UI toggling that should fire
   exactly once per press. */
Value cl_builtin_gui_key_pressed(Value name_v) {
    if (!G.inited) return cl_from_num(0.0);
    require_str(name_v, "gui_key_pressed");
    SDL_Keycode kc = key_from_name(cl_as_str(name_v)->data);
    if (kc == 0) return cl_from_num(0.0);
    for (int i = 0; i < G.keys_pressed_count; i++) {
        if (G.keys_pressed[i] == kc) return cl_from_num(1.0);
    }
    return cl_from_num(0.0);
}

Value cl_builtin_gui_mouse_x(void) {
    return cl_from_num((double)G.mouse_x);
}
Value cl_builtin_gui_mouse_y(void) {
    return cl_from_num((double)G.mouse_y);
}

/* gui_mouse_down(idx) — currently-pressed mouse button? idx 0 = left,
   1 = middle, 2 = right. */
Value cl_builtin_gui_mouse_down(Value idx_v) {
    int idx = as_int(idx_v, "gui_mouse_down");
    if (idx < 0 || idx >= 5) return cl_from_num(0.0);
    return cl_from_num(G.mouse_button_down[idx] ? 1.0 : 0.0);
}

/* gui_mouse_clicked(idx) — was clicked this frame? One-shot,
   cleared on the next gui_poll_events. */
Value cl_builtin_gui_mouse_clicked(Value idx_v) {
    int idx = as_int(idx_v, "gui_mouse_clicked");
    if (idx < 0 || idx >= 5) return cl_from_num(0.0);
    return cl_from_num(G.mouse_button_clicked[idx] ? 1.0 : 0.0);
}
