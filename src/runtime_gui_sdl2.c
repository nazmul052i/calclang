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

/* --- 8x8 bitmap font + gui_text ---------------------------------- */

/* Public-domain 8x8 ASCII font (dhepper/font8x8_basic, in the public
   domain per its README). Only the printable ASCII range 32..126 is
   used by gui_text; we keep all 128 slots for trivial indexing.
   Each glyph is 8 bytes, one byte per row. Bit 0 = leftmost pixel.
   So bit pattern 0x42 = . X X . . . . . */
static const unsigned char GUI_FONT8x8[128][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},  /* U+0000-001F: non-printable, all zero */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},  /* U+0020 ' ' */
    {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00},  /* '!' */
    {0x36,0x36,0x00,0x00,0x00,0x00,0x00,0x00},  /* '"' */
    {0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00},  /* '#' */
    {0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0x00},  /* '$' */
    {0x00,0x63,0x33,0x18,0x0C,0x66,0x63,0x00},  /* '%' */
    {0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0x00},  /* '&' */
    {0x06,0x06,0x03,0x00,0x00,0x00,0x00,0x00},  /* '\'' */
    {0x18,0x0C,0x06,0x06,0x06,0x0C,0x18,0x00},  /* '(' */
    {0x06,0x0C,0x18,0x18,0x18,0x0C,0x06,0x00},  /* ')' */
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00},  /* '*' */
    {0x00,0x0C,0x0C,0x3F,0x0C,0x0C,0x00,0x00},  /* '+' */
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x06},  /* ',' */
    {0x00,0x00,0x00,0x3F,0x00,0x00,0x00,0x00},  /* '-' */
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x00},  /* '.' */
    {0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0x00},  /* '/' */
    {0x3E,0x63,0x73,0x7B,0x6F,0x67,0x3E,0x00},  /* '0' */
    {0x0C,0x0E,0x0C,0x0C,0x0C,0x0C,0x3F,0x00},  /* '1' */
    {0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0x00},  /* '2' */
    {0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0x00},  /* '3' */
    {0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0x00},  /* '4' */
    {0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0x00},  /* '5' */
    {0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0x00},  /* '6' */
    {0x3F,0x33,0x30,0x18,0x0C,0x0C,0x0C,0x00},  /* '7' */
    {0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0x00},  /* '8' */
    {0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0x00},  /* '9' */
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x00},  /* ':' */
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x06},  /* ';' */
    {0x18,0x0C,0x06,0x03,0x06,0x0C,0x18,0x00},  /* '<' */
    {0x00,0x00,0x3F,0x00,0x00,0x3F,0x00,0x00},  /* '=' */
    {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00},  /* '>' */
    {0x1E,0x33,0x30,0x18,0x0C,0x00,0x0C,0x00},  /* '?' */
    {0x3E,0x63,0x7B,0x7B,0x7B,0x03,0x1E,0x00},  /* '@' */
    {0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0x00},  /* 'A' */
    {0x3F,0x66,0x66,0x3E,0x66,0x66,0x3F,0x00},  /* 'B' */
    {0x3C,0x66,0x03,0x03,0x03,0x66,0x3C,0x00},  /* 'C' */
    {0x1F,0x36,0x66,0x66,0x66,0x36,0x1F,0x00},  /* 'D' */
    {0x7F,0x46,0x16,0x1E,0x16,0x46,0x7F,0x00},  /* 'E' */
    {0x7F,0x46,0x16,0x1E,0x16,0x06,0x0F,0x00},  /* 'F' */
    {0x3C,0x66,0x03,0x03,0x73,0x66,0x7C,0x00},  /* 'G' */
    {0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0x00},  /* 'H' */
    {0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00},  /* 'I' */
    {0x78,0x30,0x30,0x30,0x33,0x33,0x1E,0x00},  /* 'J' */
    {0x67,0x66,0x36,0x1E,0x36,0x66,0x67,0x00},  /* 'K' */
    {0x0F,0x06,0x06,0x06,0x46,0x66,0x7F,0x00},  /* 'L' */
    {0x63,0x77,0x7F,0x7F,0x6B,0x63,0x63,0x00},  /* 'M' */
    {0x63,0x67,0x6F,0x7B,0x73,0x63,0x63,0x00},  /* 'N' */
    {0x1C,0x36,0x63,0x63,0x63,0x36,0x1C,0x00},  /* 'O' */
    {0x3F,0x66,0x66,0x3E,0x06,0x06,0x0F,0x00},  /* 'P' */
    {0x1E,0x33,0x33,0x33,0x3B,0x1E,0x38,0x00},  /* 'Q' */
    {0x3F,0x66,0x66,0x3E,0x36,0x66,0x67,0x00},  /* 'R' */
    {0x1E,0x33,0x07,0x0E,0x38,0x33,0x1E,0x00},  /* 'S' */
    {0x3F,0x2D,0x0C,0x0C,0x0C,0x0C,0x1E,0x00},  /* 'T' */
    {0x33,0x33,0x33,0x33,0x33,0x33,0x3F,0x00},  /* 'U' */
    {0x33,0x33,0x33,0x33,0x33,0x1E,0x0C,0x00},  /* 'V' */
    {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00},  /* 'W' */
    {0x63,0x63,0x36,0x1C,0x1C,0x36,0x63,0x00},  /* 'X' */
    {0x33,0x33,0x33,0x1E,0x0C,0x0C,0x1E,0x00},  /* 'Y' */
    {0x7F,0x63,0x31,0x18,0x4C,0x66,0x7F,0x00},  /* 'Z' */
    {0x1E,0x06,0x06,0x06,0x06,0x06,0x1E,0x00},  /* '[' */
    {0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0x00},  /* '\\' */
    {0x1E,0x18,0x18,0x18,0x18,0x18,0x1E,0x00},  /* ']' */
    {0x08,0x1C,0x36,0x63,0x00,0x00,0x00,0x00},  /* '^' */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF},  /* '_' */
    {0x0C,0x0C,0x18,0x00,0x00,0x00,0x00,0x00},  /* '`' */
    {0x00,0x00,0x1E,0x30,0x3E,0x33,0x6E,0x00},  /* 'a' */
    {0x07,0x06,0x06,0x3E,0x66,0x66,0x3B,0x00},  /* 'b' */
    {0x00,0x00,0x1E,0x33,0x03,0x33,0x1E,0x00},  /* 'c' */
    {0x38,0x30,0x30,0x3e,0x33,0x33,0x6E,0x00},  /* 'd' */
    {0x00,0x00,0x1E,0x33,0x3f,0x03,0x1E,0x00},  /* 'e' */
    {0x1C,0x36,0x06,0x0f,0x06,0x06,0x0F,0x00},  /* 'f' */
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x1F},  /* 'g' */
    {0x07,0x06,0x36,0x6E,0x66,0x66,0x67,0x00},  /* 'h' */
    {0x0C,0x00,0x0E,0x0C,0x0C,0x0C,0x1E,0x00},  /* 'i' */
    {0x30,0x00,0x30,0x30,0x30,0x33,0x33,0x1E},  /* 'j' */
    {0x07,0x06,0x66,0x36,0x1E,0x36,0x67,0x00},  /* 'k' */
    {0x0E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00},  /* 'l' */
    {0x00,0x00,0x33,0x7F,0x7F,0x6B,0x63,0x00},  /* 'm' */
    {0x00,0x00,0x1F,0x33,0x33,0x33,0x33,0x00},  /* 'n' */
    {0x00,0x00,0x1E,0x33,0x33,0x33,0x1E,0x00},  /* 'o' */
    {0x00,0x00,0x3B,0x66,0x66,0x3E,0x06,0x0F},  /* 'p' */
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x78},  /* 'q' */
    {0x00,0x00,0x3B,0x6E,0x66,0x06,0x0F,0x00},  /* 'r' */
    {0x00,0x00,0x3E,0x03,0x1E,0x30,0x1F,0x00},  /* 's' */
    {0x08,0x0C,0x3E,0x0C,0x0C,0x2C,0x18,0x00},  /* 't' */
    {0x00,0x00,0x33,0x33,0x33,0x33,0x6E,0x00},  /* 'u' */
    {0x00,0x00,0x33,0x33,0x33,0x1E,0x0C,0x00},  /* 'v' */
    {0x00,0x00,0x63,0x6B,0x7F,0x7F,0x36,0x00},  /* 'w' */
    {0x00,0x00,0x63,0x36,0x1C,0x36,0x63,0x00},  /* 'x' */
    {0x00,0x00,0x33,0x33,0x33,0x3E,0x30,0x1F},  /* 'y' */
    {0x00,0x00,0x3F,0x19,0x0C,0x26,0x3F,0x00},  /* 'z' */
    {0x38,0x0C,0x0C,0x07,0x0C,0x0C,0x38,0x00},  /* '{' */
    {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00},  /* '|' */
    {0x07,0x0C,0x0C,0x38,0x0C,0x0C,0x07,0x00},  /* '}' */
    {0x6E,0x3B,0x00,0x00,0x00,0x00,0x00,0x00},  /* '~' */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},  /* DEL (unused) */
};

/* Text-render state. Scale defaults to 1 (a glyph is 8×8 px). At
   scale 2, each glyph is 16×16 etc. Changing the scale doesn't
   reflow anything — gui_text just multiplies its step size. */
static int gui_text_scale = 1;

Value cl_builtin_gui_set_text_scale(Value sv) {
    int s = as_int(sv, "gui_set_text_scale");
    if (s < 1) s = 1;
    if (s > 32) s = 32;
    gui_text_scale = s;
    return cl_from_num(0.0);
}

/* Draw one character at (x, y) using the current draw color. Returns
   the glyph width in pixels (always 8 * scale) so callers can advance. */
static int draw_char(int x, int y, char ch) {
    int s = gui_text_scale;
    if ((unsigned char)ch >= 128) ch = '?';
    const unsigned char *g = GUI_FONT8x8[(unsigned char)ch];
    for (int row = 0; row < 8; row++) {
        unsigned char bits = g[row];
        if (!bits) continue;
        for (int col = 0; col < 8; col++) {
            if (bits & (1u << col)) {
                if (s == 1) {
                    SDL_RenderDrawPoint(G.ren, x + col, y + row);
                } else {
                    SDL_Rect r = { x + col * s, y + row * s, s, s };
                    SDL_RenderFillRect(G.ren, &r);
                }
            }
        }
    }
    return 8 * s;
}

/* gui_text(x, y, str) — draw str starting at (x, y) using the
   current draw color and current text scale. Each character is
   8 px wide at scale 1; newlines advance one row down. */
Value cl_builtin_gui_text(Value xv, Value yv, Value sv) {
    if (!G.inited) return cl_from_num(0.0);
    require_str(sv, "gui_text");
    int x0 = as_int(xv, "gui_text");
    int y0 = as_int(yv, "gui_text");
    CalcStr *cs = cl_as_str(sv);
    int x = x0, y = y0;
    int step_x = 8 * gui_text_scale;
    int step_y = 8 * gui_text_scale;
    for (uint64_t i = 0; i < cs->len; i++) {
        char ch = cs->data[i];
        if (ch == '\n') { x = x0; y += step_y; continue; }
        if (ch == '\r') continue;
        draw_char(x, y, ch);
        x += step_x;
    }
    return cl_from_num(0.0);
}

/* --- Circle (filled + outline) ----------------------------------- */

/* Midpoint circle algorithm — single sweep, 8-way symmetry. Filled
   variant writes horizontal spans; outline writes individual pixels.
   Uses the current draw color set by gui_set_color. */
Value cl_builtin_gui_circle(Value xv, Value yv, Value rv) {
    if (!G.inited) return cl_from_num(0.0);
    int cx = as_int(xv, "gui_circle");
    int cy = as_int(yv, "gui_circle");
    int r  = as_int(rv, "gui_circle");
    if (r <= 0) return cl_from_num(0.0);
    int x = r, y = 0;
    int err = 0;
    while (x >= y) {
        SDL_RenderDrawLine(G.ren, cx - x, cy + y, cx + x, cy + y);
        SDL_RenderDrawLine(G.ren, cx - x, cy - y, cx + x, cy - y);
        SDL_RenderDrawLine(G.ren, cx - y, cy + x, cx + y, cy + x);
        SDL_RenderDrawLine(G.ren, cx - y, cy - x, cx + y, cy - x);
        y++;
        if (err <= 0) { err += 2 * y + 1; }
        else          { x--; err += 2 * (y - x) + 1; }
    }
    return cl_from_num(0.0);
}

Value cl_builtin_gui_circle_outline(Value xv, Value yv, Value rv) {
    if (!G.inited) return cl_from_num(0.0);
    int cx = as_int(xv, "gui_circle_outline");
    int cy = as_int(yv, "gui_circle_outline");
    int r  = as_int(rv, "gui_circle_outline");
    if (r <= 0) return cl_from_num(0.0);
    int x = r, y = 0;
    int err = 0;
    while (x >= y) {
        SDL_RenderDrawPoint(G.ren, cx + x, cy + y);
        SDL_RenderDrawPoint(G.ren, cx + y, cy + x);
        SDL_RenderDrawPoint(G.ren, cx - y, cy + x);
        SDL_RenderDrawPoint(G.ren, cx - x, cy + y);
        SDL_RenderDrawPoint(G.ren, cx - x, cy - y);
        SDL_RenderDrawPoint(G.ren, cx - y, cy - x);
        SDL_RenderDrawPoint(G.ren, cx + y, cy - x);
        SDL_RenderDrawPoint(G.ren, cx + x, cy - y);
        y++;
        if (err <= 0) { err += 2 * y + 1; }
        else          { x--; err += 2 * (y - x) + 1; }
    }
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
