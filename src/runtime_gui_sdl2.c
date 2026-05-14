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

#ifdef _WIN32
  /* Pull in commdlg.h for GetOpenFileNameA / GetSaveFileNameA. We
     don't pull all of windows.h to keep the namespace clean — just
     the bare-minimum OPENFILENAMEA + entry points. */
  #include <windows.h>
  #include <commdlg.h>
  /* GetActiveWindow is normally in user32; pull it via windows.h too. */
#endif

/* stb_truetype — single-header TTF rasterizer (Sean Barrett, public
   domain). Used for crisp scalable text in gui_text. We disable the
   built-in stbtt_assert dependency and let it use the C library's
   assert. */
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "../third_party/stb/stb_truetype.h"

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
    /* Text input buffer. SDL_TEXTINPUT events accumulate here; the
       gui_text_typed() builtin drains it. Capped at 1024 bytes per
       frame — anything beyond is dropped silently (real typing rates
       are nowhere near that). */
    char          typed_buf[1024];
    int           typed_len;
    /* Currently-focused widget ID. A short string set by the widget
       library (gui_set_focus) and read each frame (gui_get_focus).
       Used by text_input/dropdown to know which one has the typing
       cursor and which dropdown panel is open. */
    char          focus_buf[128];
    int           focus_len;
    /* TTF font state. font_loaded is 1 iff we successfully read a
       font file at init time. When 0, gui_text falls back to the
       embedded 8x8 bitmap font. */
    int           font_loaded;
    stbtt_fontinfo font_info;
    unsigned char *font_buf;
    /* Current font pixel size — set by gui_set_text_scale, used by
       gui_text. Scale 1 → 14px (body), 2 → 22px (sub-headings),
       3 → 32px (large numbers). Maps cleanly to the existing
       widget code that calls gui_set_text_scale(1..3). */
    int           font_size;
} GuiState;

static GuiState G;

/* TTF glyph cache (one entry per rasterized (codepoint, size) pair).
   Declared up front so cleanup helpers can reference them; the
   build/lookup functions live further down with the text-render code. */
typedef struct {
    int          codepoint;
    int          size;
    SDL_Texture *tex;
    int          w;
    int          h;
    int          xoff;
    int          yoff;
    int          advance;
} GlyphEntry;

#define GLYPH_CACHE_MAX 1024
static GlyphEntry  GL_CACHE[GLYPH_CACHE_MAX];
static int         GL_CACHE_N = 0;

/* Forward — exported by runtime_x64.c. */
extern void cl_die_rt(const char *msg);
extern void require_num(Value v, const char *where);
extern void require_str(Value v, const char *where);
extern Value cl_new_str(const char *bytes, uint64_t len);

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
    G.font_size = 14;
    G.inited = 1;
    /* Enable SDL's text-input mode so SDL_TEXTINPUT events fire for
       typed characters. Without this, all we'd get is raw SDL_KEYDOWN
       events that don't account for keyboard layout / Shift / dead
       keys — fine for game-key polling, useless for typing into a
       text field. */
    SDL_StartTextInput();

    /* Try to load a system TTF font for crisp text rendering. We
       walk a small fallback chain — Segoe UI is the default Windows
       UI font (present on Win7+); macOS / Linux paths added when
       those backends land. If everything fails we silently fall
       back to the embedded 8x8 bitmap font in gui_text. */
    const char *font_candidates[] = {
        /* Windows */
        "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/arial.ttf",
        "C:/Windows/Fonts/consola.ttf",
        /* macOS — system fonts. */
        "/System/Library/Fonts/Helvetica.ttc",
        "/System/Library/Fonts/SFNS.ttf",
        /* Linux — DejaVu is on most distros. */
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        NULL,
    };
    for (int i = 0; font_candidates[i]; i++) {
        FILE *ff = fopen(font_candidates[i], "rb");
        if (!ff) continue;
        fseek(ff, 0, SEEK_END);
        long sz = ftell(ff);
        fseek(ff, 0, SEEK_SET);
        if (sz <= 0 || sz > 16 * 1024 * 1024) { fclose(ff); continue; }
        G.font_buf = (unsigned char *)malloc((size_t)sz);
        if (!G.font_buf) { fclose(ff); continue; }
        if (fread(G.font_buf, 1, (size_t)sz, ff) != (size_t)sz) {
            free(G.font_buf); G.font_buf = NULL; fclose(ff); continue;
        }
        fclose(ff);
        if (stbtt_InitFont(&G.font_info, G.font_buf,
                stbtt_GetFontOffsetForIndex(G.font_buf, 0)) == 0) {
            free(G.font_buf); G.font_buf = NULL; continue;
        }
        G.font_loaded = 1;
        break;
    }
    return cl_from_num(1.0);
}

Value cl_builtin_gui_close(void) {
    if (!G.inited) return cl_from_num(0.0);
    /* Free cached glyph textures first — they're tied to the
       renderer, so they must die before SDL_DestroyRenderer. */
    for (int i = 0; i < GL_CACHE_N; i++) {
        if (GL_CACHE[i].tex) SDL_DestroyTexture(GL_CACHE[i].tex);
    }
    GL_CACHE_N = 0;
    if (G.font_buf) { free(G.font_buf); G.font_buf = NULL; }
    G.font_loaded = 0;
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
            case SDL_TEXTINPUT: {
                /* Append the typed bytes to the per-frame buffer. We
                   keep it as raw bytes — non-ASCII multi-byte UTF-8
                   passes through cleanly even though our 8x8 font
                   only renders ASCII. */
                const char *t = e.text.text;
                int tl = (int)strlen(t);
                if (G.typed_len + tl < (int)sizeof G.typed_buf) {
                    memcpy(G.typed_buf + G.typed_len, t, (size_t)tl);
                    G.typed_len += tl;
                }
                break;
            }
        }
    }
    return cl_from_num(0.0);
}

/* gui_text_typed() — returns and clears the per-frame typed-character
   buffer. Returns an empty string when no text input fired. */
Value cl_builtin_gui_text_typed(void) {
    Value v = cl_new_str(G.typed_buf, (uint64_t)G.typed_len);
    G.typed_len = 0;
    return v;
}

/* gui_get_focus() — returns the current focused widget id, or "". */
Value cl_builtin_gui_get_focus(void) {
    return cl_new_str(G.focus_buf, (uint64_t)G.focus_len);
}

/* gui_set_focus(id) — record a string id as focused. The widget
   library uses this to know which text input gets typed text and
   which dropdown is open. Pass "" to clear focus. */
Value cl_builtin_gui_set_focus(Value id_v) {
    require_str(id_v, "gui_set_focus");
    CalcStr *s = cl_as_str(id_v);
    int n = (int)s->len;
    if (n >= (int)sizeof G.focus_buf) n = (int)sizeof G.focus_buf - 1;
    memcpy(G.focus_buf, s->data, (size_t)n);
    G.focus_buf[n] = '\0';
    G.focus_len = n;
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

/* Map the legacy gui_set_text_scale(1..n) integer scale onto a TTF
   pixel size. Scale 1 (the widget default) gets a comfortable body
   size; larger scales jump to sub-heading and heading sizes. This
   keeps existing widget code calling `gui_set_text_scale(1)` /
   `(2)` / `(3)` working untouched, but the visual result is crisp
   anti-aliased text instead of a pixelated 8x8 bitmap. */
static int gui_font_size_for_scale(int s) {
    if (s <= 1) return 14;
    if (s == 2) return 22;
    if (s == 3) return 32;
    /* Past scale 3, linear-extrapolate. */
    return 32 + (s - 3) * 10;
}

Value cl_builtin_gui_set_text_scale(Value sv) {
    int s = as_int(sv, "gui_set_text_scale");
    if (s < 1) s = 1;
    if (s > 32) s = 32;
    G.font_size = gui_font_size_for_scale(s);
    return cl_from_num(0.0);
}

/* --- TTF rendering ---------------------------------------------- */
/* Each glyph-cache entry is one (codepoint, size) → rasterized
   SDL_Texture. We pre-multiply with white-with-alpha so
   SDL_SetTextureColorMod can recolor each glyph at draw time. */

static GlyphEntry *find_or_build_glyph(int codepoint, int size) {
    for (int i = 0; i < GL_CACHE_N; i++) {
        if (GL_CACHE[i].codepoint == codepoint && GL_CACHE[i].size == size) {
            return &GL_CACHE[i];
        }
    }
    if (GL_CACHE_N >= GLYPH_CACHE_MAX) {
        /* Cache full — drop the first half (oldest by insertion). */
        for (int i = 0; i < GL_CACHE_N / 2; i++) {
            if (GL_CACHE[i].tex) SDL_DestroyTexture(GL_CACHE[i].tex);
        }
        memmove(&GL_CACHE[0], &GL_CACHE[GL_CACHE_N / 2],
                sizeof(GlyphEntry) * (size_t)(GL_CACHE_N - GL_CACHE_N / 2));
        GL_CACHE_N = GL_CACHE_N - GL_CACHE_N / 2;
    }
    float scale = stbtt_ScaleForPixelHeight(&G.font_info, (float)size);
    int gw, gh, xoff, yoff;
    unsigned char *bm = stbtt_GetCodepointBitmap(&G.font_info, 0, scale,
        codepoint, &gw, &gh, &xoff, &yoff);
    int adv, lsb;
    stbtt_GetCodepointHMetrics(&G.font_info, codepoint, &adv, &lsb);
    int advance_px = (int)((float)adv * scale + 0.5f);

    SDL_Texture *tex = NULL;
    if (bm && gw > 0 && gh > 0) {
        /* Expand the 8-bit alpha bitmap into an RGBA8888 buffer where
           every pixel is white-with-alpha. SDL_SetTextureColorMod
           will recolor it at draw time. */
        Uint32 *rgba = (Uint32 *)malloc((size_t)(gw * gh) * sizeof(Uint32));
        if (rgba) {
            for (int i = 0; i < gw * gh; i++) {
                Uint8 a = bm[i];
                rgba[i] = ((Uint32)a << 24) | 0x00FFFFFF;
            }
            SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormatFrom(
                rgba, gw, gh, 32, gw * 4, SDL_PIXELFORMAT_ABGR8888);
            if (surf) {
                tex = SDL_CreateTextureFromSurface(G.ren, surf);
                SDL_FreeSurface(surf);
            }
            free(rgba);
        }
    }
    if (bm) stbtt_FreeBitmap(bm, NULL);

    GlyphEntry *e = &GL_CACHE[GL_CACHE_N++];
    e->codepoint = codepoint;
    e->size      = size;
    e->tex       = tex;
    e->w         = gw;
    e->h         = gh;
    e->xoff      = xoff;
    e->yoff      = yoff;
    e->advance   = advance_px;
    return e;
}

/* Draw one character via the TTF cache. Returns the horizontal advance
   in pixels. (x, y) is the top-left of the line box; glyph offsets
   are added internally so descenders/etc. land in the right place. */
static int draw_char_ttf(int x, int y, int size, int codepoint) {
    GlyphEntry *e = find_or_build_glyph(codepoint, size);
    /* Empty advance — nothing visible, just space. */
    if (!e || !e->tex) {
        return e ? e->advance : (size * 5 / 8);
    }
    /* The baseline is `ascent * scale` below the top of the line box.
       Use ascent from font metrics. */
    int asc, desc, line_gap;
    stbtt_GetFontVMetrics(&G.font_info, &asc, &desc, &line_gap);
    float fscale = stbtt_ScaleForPixelHeight(&G.font_info, (float)size);
    int baseline = (int)((float)asc * fscale);
    SDL_SetTextureColorMod(e->tex, G.r, G.g, G.b);
    SDL_SetTextureAlphaMod(e->tex, G.a);
    SDL_Rect dst = { x + e->xoff, y + baseline + e->yoff, e->w, e->h };
    SDL_RenderCopy(G.ren, e->tex, NULL, &dst);
    return e->advance;
}

/* Fallback bitmap-font drawer for when no TTF was loaded. Keeps
   programs running on systems that have no system font. */
static int draw_char_bitmap(int x, int y, int size, char ch) {
    int s = size < 8 ? 1 : size / 8;
    if (s < 1) s = 1;
    if ((unsigned char)ch >= 128) ch = '?';
    const unsigned char *g = GUI_FONT8x8[(unsigned char)ch];
    for (int row = 0; row < 8; row++) {
        unsigned char bits = g[row];
        if (!bits) continue;
        for (int col = 0; col < 8; col++) {
            if (bits & (1u << col)) {
                SDL_Rect r = { x + col * s, y + row * s, s, s };
                SDL_RenderFillRect(G.ren, &r);
            }
        }
    }
    return 8 * s;
}

/* gui_text(x, y, str) — draw str starting at (x, y) using the
   current draw color and current text scale. With a TTF loaded,
   each glyph is anti-aliased and properly proportional; without
   one, we fall back to the embedded 8×8 bitmap font. Newlines
   advance one line height. */
Value cl_builtin_gui_text(Value xv, Value yv, Value sv) {
    if (!G.inited) return cl_from_num(0.0);
    require_str(sv, "gui_text");
    int x0 = as_int(xv, "gui_text");
    int y0 = as_int(yv, "gui_text");
    CalcStr *cs = cl_as_str(sv);
    int size = G.font_size;
    int line_h = size + (size / 4);
    int x = x0, y = y0;
    for (uint64_t i = 0; i < cs->len; i++) {
        unsigned char ch = (unsigned char)cs->data[i];
        if (ch == '\n') { x = x0; y += line_h; continue; }
        if (ch == '\r') continue;
        if (G.font_loaded) {
            x += draw_char_ttf(x, y, size, (int)ch);
        } else {
            x += draw_char_bitmap(x, y, size, (char)ch);
        }
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

/* --- Clipping / scrollable regions ------------------------------- */

/* gui_set_clip(x, y, w, h) — restrict subsequent draws to this rect.
   Anything outside is discarded. Used by scrollable containers to
   hide content that's scrolled out of view. */
Value cl_builtin_gui_set_clip(Value xv, Value yv, Value wv, Value hv) {
    if (!G.inited) return cl_from_num(0.0);
    SDL_Rect r = {
        as_int(xv, "gui_set_clip"), as_int(yv, "gui_set_clip"),
        as_int(wv, "gui_set_clip"), as_int(hv, "gui_set_clip")
    };
    SDL_RenderSetClipRect(G.ren, &r);
    return cl_from_num(0.0);
}

Value cl_builtin_gui_clear_clip(void) {
    if (!G.inited) return cl_from_num(0.0);
    SDL_RenderSetClipRect(G.ren, NULL);
    return cl_from_num(0.0);
}

/* --- Modal dialogs (OS-native) ----------------------------------- */

/* gui_message_box(title, msg) — show an OS-native modal alert. Blocks
   until the user dismisses. Returns 0. */
Value cl_builtin_gui_message_box(Value title_v, Value msg_v) {
    require_str(title_v, "gui_message_box");
    require_str(msg_v,   "gui_message_box");
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION,
        cl_as_str(title_v)->data,
        cl_as_str(msg_v)->data,
        G.inited ? G.win : NULL);
    return cl_from_num(0.0);
}

/* gui_confirm(title, msg) — OK / Cancel dialog. Returns 1 for OK,
   0 for Cancel (or for any failure to show). */
Value cl_builtin_gui_confirm(Value title_v, Value msg_v) {
    require_str(title_v, "gui_confirm");
    require_str(msg_v,   "gui_confirm");
    const SDL_MessageBoxButtonData buttons[2] = {
        { SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Cancel" },
        { SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "OK"     },
    };
    SDL_MessageBoxData mb = {0};
    mb.flags       = SDL_MESSAGEBOX_INFORMATION;
    mb.window      = G.inited ? G.win : NULL;
    mb.title       = cl_as_str(title_v)->data;
    mb.message     = cl_as_str(msg_v)->data;
    mb.numbuttons  = 2;
    mb.buttons     = buttons;
    mb.colorScheme = NULL;
    int btn = 0;
    if (SDL_ShowMessageBox(&mb, &btn) != 0) return cl_from_num(0.0);
    return cl_from_num((double)btn);
}

/* --- Native file open / save dialogs ----------------------------- */

#ifdef _WIN32
/* Convert "Text files|*.txt|All files|*.*" (the cross-platform form
   the CalcLang side expects) into the Win32 \0-separated form
   "Text files\0*.txt\0All files\0*.*\0\0" in place. Returns the
   total length including all NULs. */
static int build_win_filter(const char *src, char *dst, int dst_max) {
    int i = 0;
    while (src[i] && i < dst_max - 2) {
        dst[i] = src[i] == '|' ? '\0' : src[i];
        i++;
    }
    dst[i++] = '\0';
    dst[i] = '\0';
    return i + 1;
}
#endif

/* gui_open_file(filter) — show OS-native Open dialog. Filter is
   "Description|pattern|Description|pattern|..." (pipe-separated).
   Returns the chosen path as a string, or "" if cancelled. */
Value cl_builtin_gui_open_file(Value filter_v) {
    require_str(filter_v, "gui_open_file");
#ifdef _WIN32
    CalcStr *f = cl_as_str(filter_v);
    char filter[512];
    int  flen = (int)f->len;
    if (flen >= (int)sizeof filter - 2) flen = (int)sizeof filter - 3;
    memcpy(filter, f->data, (size_t)flen);
    filter[flen] = '\0';
    build_win_filter(filter, filter, (int)sizeof filter);

    OPENFILENAMEA ofn;
    char path[1024];
    memset(&ofn, 0, sizeof ofn);
    memset(path, 0, sizeof path);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner   = NULL;            /* keeping things simple */
    ofn.lpstrFilter = filter;
    ofn.lpstrFile   = path;
    ofn.nMaxFile    = sizeof path;
    ofn.Flags       = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameA(&ofn)) {
        return cl_new_str(path, strlen(path));
    }
    return cl_new_str("", 0);
#else
    /* TODO: macOS NSOpenPanel via Cocoa, Linux GTK or zenity fallback.
       For now we just return "" so portable code paths still work. */
    return cl_new_str("", 0);
#endif
}

/* gui_save_file(filter) — Save dialog. Same filter format as
   gui_open_file. Returns chosen path or "". */
Value cl_builtin_gui_save_file(Value filter_v) {
    require_str(filter_v, "gui_save_file");
#ifdef _WIN32
    CalcStr *f = cl_as_str(filter_v);
    char filter[512];
    int  flen = (int)f->len;
    if (flen >= (int)sizeof filter - 2) flen = (int)sizeof filter - 3;
    memcpy(filter, f->data, (size_t)flen);
    filter[flen] = '\0';
    build_win_filter(filter, filter, (int)sizeof filter);

    OPENFILENAMEA ofn;
    char path[1024];
    memset(&ofn, 0, sizeof ofn);
    memset(path, 0, sizeof path);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner   = NULL;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile   = path;
    ofn.nMaxFile    = sizeof path;
    ofn.Flags       = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    if (GetSaveFileNameA(&ofn)) {
        return cl_new_str(path, strlen(path));
    }
    return cl_new_str("", 0);
#else
    return cl_new_str("", 0);
#endif
}
