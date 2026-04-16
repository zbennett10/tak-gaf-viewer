/*
 * main.c -- GAF Viewer
 *
 * A cross-platform viewer for Total Annihilation and TA: Kingdoms
 * GAF sprite files. Supports both TA and TAK RLE formats.
 *
 * Usage: gaf-viewer [file.gaf] [palette.pal|palette.pcx]
 *   - If no file given, launches with an Open File prompt
 *   - Auto-loads matching .pcx palette if found alongside the GAF
 *
 * Controls:
 *   Left/Right  - Step through frames (crosses entry boundaries)
 *   Up/Down     - Jump between entries
 *   Space       - Play/pause entry animation (game-style)
 *   A           - Play/pause all-frames animation
 *   T           - Toggle between TA and TAK decode mode
 *   F           - Toggle checkerboard transparency background
 *   +/-         - Zoom in/out
 *   [/]         - Decrease/increase animation speed
 *   O           - Open GAF file
 *   P           - Open palette file
 *   Escape      - Quit
 *
 * Drag & drop .gaf, .pal, or .pcx files onto the window.
 */

#include "gaf.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#else
#include <dirent.h>
#endif

/* ── Animation modes ────────────────────────────────────────────── */
enum {
    ANIM_STOPPED,
    ANIM_ENTRIES,
    ANIM_ALL_FRAMES
};

/* ── UI button IDs ──────────────────────────────────────────────── */
enum {
    BTN_OPEN_GAF = 0,
    BTN_OPEN_PAL,
    BTN_PREV,
    BTN_PLAY_ENTRIES,
    BTN_STOP,
    BTN_PLAY_ALL,
    BTN_NEXT,
    BTN_SLOWER,
    BTN_FASTER,
    BTN_ZOOM_OUT,
    BTN_ZOOM_IN,
    BTN_CHECKER,
    BTN_MODE_TOGGLE,
    BTN_PAL_PREV,
    BTN_PAL_NEXT,
    BTN_COUNT
};

/* ── UI Constants ───────────────────────────────────────────────── */
#define TOOLBAR_HEIGHT  52
#define INFO_BAR_HEIGHT 24
#define FONT_SCALE      2
#define BTN_H          34
#define BTN_PAD         5
#define PANEL_TOP_H    28

/* Colors */
#define COL_BG_R        30
#define COL_BG_G        30
#define COL_BG_B        35

#define COL_TOOLBAR_R   22
#define COL_TOOLBAR_G   22
#define COL_TOOLBAR_B   28

#define COL_BTN_R       50
#define COL_BTN_G       50
#define COL_BTN_B       60

#define COL_BTN_HOV_R   70
#define COL_BTN_HOV_G   70
#define COL_BTN_HOV_B   85

#define COL_BTN_ACT_R   45
#define COL_BTN_ACT_G  110
#define COL_BTN_ACT_B   50

#define COL_BORDER_R    80
#define COL_BORDER_G    80
#define COL_BORDER_B    95

#define COL_TEXT_R     210
#define COL_TEXT_G     210
#define COL_TEXT_B     220

#define COL_DIM_R      120
#define COL_DIM_G      120
#define COL_DIM_B      135

#define COL_ACCENT_R   100
#define COL_ACCENT_G   180
#define COL_ACCENT_B   255

/* ── Viewer state ───────────────────────────────────────────────── */

typedef struct {
    SDL_Rect rect;
    const char *label;
    int hovered;
} UIButton;

typedef struct {
    GAFFile *gaf;
    Palette palette;
    uint32_t rgba_table[256];
    int has_palette;
    int has_gaf;

    int current_entry;
    int current_frame;
    int entry_count;

    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *frame_texture;
    SDL_Texture *checker_cache;
    int checker_w, checker_h, checker_zoom;

    int anim_mode;
    float anim_timer;
    float frame_duration;

    int show_checkerboard;
    int zoom;

    UIButton buttons[BTN_COUNT];

    char gaf_path[512];
    char pal_path[512];

    /* Discovered palette files for cycling */
    char **pal_list;       /* Array of palette file paths */
    int pal_list_count;    /* Number of discovered palettes */
    int pal_list_index;    /* Currently selected palette (-1 = default/manual) */
} ViewerState;

/* Forward declarations */
static void update_frame_texture(ViewerState *v);
static void rebuild_palette(ViewerState *v);

/* ── Palette scanning & cycling ─────────────────────────────────── */

static void free_pal_list(ViewerState *v) {
    if (v->pal_list) {
        for (int i = 0; i < v->pal_list_count; i++) free(v->pal_list[i]);
        free(v->pal_list);
        v->pal_list = NULL;
    }
    v->pal_list_count = 0;
    v->pal_list_index = -1;
}

/* Scan for .pcx palette files near the GAF's directory */
static void scan_palettes(ViewerState *v) {
    free_pal_list(v);
    if (!v->has_gaf) return;

    /* Find the directory of the GAF file */
    const char *last_slash = NULL;
    for (const char *p = v->gaf_path; *p; p++)
        if (*p == '/' || *p == '\\') last_slash = p;

    char dir[512] = {0};
    if (last_slash) {
        size_t dlen = last_slash - v->gaf_path;
        if (dlen >= sizeof(dir)) dlen = sizeof(dir) - 1;
        memcpy(dir, v->gaf_path, dlen);
    } else {
        strcpy(dir, ".");
    }

    /* Directories to scan for palette files */
    char scan_dirs[4][512];
    int num_dirs = 0;
    snprintf(scan_dirs[num_dirs++], 512, "%s", dir);
    snprintf(scan_dirs[num_dirs++], 512, "%s/../palettes", dir);
    snprintf(scan_dirs[num_dirs++], 512, "%s/palettes", dir);

    /* Collect all .pcx files from these directories */
    int capacity = 64;
    v->pal_list = (char **)malloc(capacity * sizeof(char *));
    v->pal_list_count = 0;

    for (int d = 0; d < num_dirs; d++) {
        /* Use a simple directory scan */
#ifdef _WIN32
        char pattern[512];
        snprintf(pattern, sizeof(pattern), "%s\\*.pcx", scan_dirs[d]);
        WIN32_FIND_DATAA fd;
        HANDLE hFind = FindFirstFileA(pattern, &fd);
        if (hFind == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            char full[512];
            snprintf(full, sizeof(full), "%s/%s", scan_dirs[d], fd.cFileName);
            /* Check it's a valid palette (has 0x0C marker) */
            Palette test;
            if (Palette_LoadPCX(&test, full) == 0) {
                if (v->pal_list_count >= capacity) {
                    capacity *= 2;
                    v->pal_list = (char **)realloc(v->pal_list, capacity * sizeof(char *));
                }
                v->pal_list[v->pal_list_count] = (char *)malloc(strlen(full) + 1);
                strcpy(v->pal_list[v->pal_list_count], full);
                v->pal_list_count++;
            }
        } while (FindNextFileA(hFind, &fd));
        FindClose(hFind);
#else
        /* POSIX: use opendir/readdir */
        DIR *dp = opendir(scan_dirs[d]);
        if (!dp) continue;
        struct dirent *ep;
        while ((ep = readdir(dp))) {
            size_t nlen = strlen(ep->d_name);
            if (nlen < 5) continue;
            const char *ext = ep->d_name + nlen - 4;
            if (strcmp(ext, ".pcx") != 0 && strcmp(ext, ".PCX") != 0) continue;
            char full[512];
            snprintf(full, sizeof(full), "%s/%s", scan_dirs[d], ep->d_name);
            Palette test;
            if (Palette_LoadPCX(&test, full) == 0) {
                if (v->pal_list_count >= capacity) {
                    capacity *= 2;
                    v->pal_list = (char **)realloc(v->pal_list, capacity * sizeof(char *));
                }
                v->pal_list[v->pal_list_count] = (char *)malloc(strlen(full) + 1);
                strcpy(v->pal_list[v->pal_list_count], full);
                v->pal_list_count++;
            }
        }
        closedir(dp);
#endif
    }

    /* Find current palette in the list */
    v->pal_list_index = -1;
    for (int i = 0; i < v->pal_list_count; i++) {
        if (strcmp(v->pal_list[i], v->pal_path) == 0) {
            v->pal_list_index = i;
            break;
        }
    }
}

static void cycle_palette(ViewerState *v, int direction) {
    if (v->pal_list_count == 0) return;
    v->pal_list_index += direction;
    if (v->pal_list_index >= v->pal_list_count) v->pal_list_index = 0;
    if (v->pal_list_index < 0) v->pal_list_index = v->pal_list_count - 1;

    char *path = v->pal_list[v->pal_list_index];
    if (Palette_LoadPCX(&v->palette, path) == 0) {
        strncpy(v->pal_path, path, sizeof(v->pal_path) - 1);
        v->has_palette = 1;
        rebuild_palette(v);
        update_frame_texture(v);
    }
}
static void load_gaf_file(ViewerState *v, const char *path);

/* ── Tiny bitmap font (4x6 pixels per glyph, rendered at FONT_SCALE) ─ */

static const uint8_t tiny_font[96][6] = {
    /* 0x20 space */ {0x0,0x0,0x0,0x0,0x0,0x0},
    /* ! */  {0x4,0x4,0x4,0x4,0x0,0x4},
    /* " */  {0xA,0xA,0x0,0x0,0x0,0x0},
    /* # */  {0xA,0xF,0xA,0xF,0xA,0x0},
    /* $ */  {0x4,0xE,0xC,0x6,0xE,0x4},
    /* % */  {0x9,0x2,0x4,0x2,0x9,0x0},
    /* & */  {0x4,0xA,0x4,0xA,0x5,0x0},
    /* ' */  {0x4,0x4,0x0,0x0,0x0,0x0},
    /* ( */  {0x2,0x4,0x4,0x4,0x2,0x0},
    /* ) */  {0x4,0x2,0x2,0x2,0x4,0x0},
    /* * */  {0x0,0xA,0x4,0xA,0x0,0x0},
    /* + */  {0x0,0x4,0xE,0x4,0x0,0x0},
    /* , */  {0x0,0x0,0x0,0x4,0x4,0x8},
    /* - */  {0x0,0x0,0xE,0x0,0x0,0x0},
    /* . */  {0x0,0x0,0x0,0x0,0x4,0x0},
    /* / */  {0x1,0x2,0x4,0x4,0x8,0x0},
    /* 0 */  {0x6,0x9,0x9,0x9,0x6,0x0},
    /* 1 */  {0x4,0xC,0x4,0x4,0xE,0x0},
    /* 2 */  {0x6,0x9,0x2,0x4,0xF,0x0},
    /* 3 */  {0xE,0x1,0x6,0x1,0xE,0x0},
    /* 4 */  {0x2,0x6,0xA,0xF,0x2,0x0},
    /* 5 */  {0xF,0x8,0xE,0x1,0xE,0x0},
    /* 6 */  {0x6,0x8,0xE,0x9,0x6,0x0},
    /* 7 */  {0xF,0x1,0x2,0x4,0x4,0x0},
    /* 8 */  {0x6,0x9,0x6,0x9,0x6,0x0},
    /* 9 */  {0x6,0x9,0x7,0x1,0x6,0x0},
    /* : */  {0x0,0x4,0x0,0x4,0x0,0x0},
    /* ; */  {0x0,0x4,0x0,0x4,0x8,0x0},
    /* < */  {0x2,0x4,0x8,0x4,0x2,0x0},
    /* = */  {0x0,0xE,0x0,0xE,0x0,0x0},
    /* > */  {0x8,0x4,0x2,0x4,0x8,0x0},
    /* ? */  {0x6,0x1,0x2,0x0,0x2,0x0},
    /* @ */  {0x6,0x9,0xB,0x8,0x6,0x0},
    /* A */  {0x6,0x9,0xF,0x9,0x9,0x0},
    /* B */  {0xE,0x9,0xE,0x9,0xE,0x0},
    /* C */  {0x7,0x8,0x8,0x8,0x7,0x0},
    /* D */  {0xE,0x9,0x9,0x9,0xE,0x0},
    /* E */  {0xF,0x8,0xE,0x8,0xF,0x0},
    /* F */  {0xF,0x8,0xE,0x8,0x8,0x0},
    /* G */  {0x7,0x8,0xB,0x9,0x7,0x0},
    /* H */  {0x9,0x9,0xF,0x9,0x9,0x0},
    /* I */  {0xE,0x4,0x4,0x4,0xE,0x0},
    /* J */  {0x1,0x1,0x1,0x9,0x6,0x0},
    /* K */  {0x9,0xA,0xC,0xA,0x9,0x0},
    /* L */  {0x8,0x8,0x8,0x8,0xF,0x0},
    /* M */  {0x9,0xF,0xF,0x9,0x9,0x0},
    /* N */  {0x9,0xD,0xF,0xB,0x9,0x0},
    /* O */  {0x6,0x9,0x9,0x9,0x6,0x0},
    /* P */  {0xE,0x9,0xE,0x8,0x8,0x0},
    /* Q */  {0x6,0x9,0x9,0xA,0x5,0x0},
    /* R */  {0xE,0x9,0xE,0xA,0x9,0x0},
    /* S */  {0x7,0x8,0x6,0x1,0xE,0x0},
    /* T */  {0xE,0x4,0x4,0x4,0x4,0x0},
    /* U */  {0x9,0x9,0x9,0x9,0x6,0x0},
    /* V */  {0x9,0x9,0x9,0x6,0x6,0x0},
    /* W */  {0x9,0x9,0xF,0xF,0x9,0x0},
    /* X */  {0x9,0x9,0x6,0x9,0x9,0x0},
    /* Y */  {0x9,0x9,0x6,0x4,0x4,0x0},
    /* Z */  {0xF,0x2,0x4,0x8,0xF,0x0},
    /* [ */  {0x6,0x4,0x4,0x4,0x6,0x0},
    /* \ */  {0x8,0x4,0x4,0x2,0x1,0x0},
    /* ] */  {0x6,0x2,0x2,0x2,0x6,0x0},
    /* ^ */  {0x4,0xA,0x0,0x0,0x0,0x0},
    /* _ */  {0x0,0x0,0x0,0x0,0xF,0x0},
    /* ` */  {0x8,0x4,0x0,0x0,0x0,0x0},
    /* a */  {0x0,0x6,0x9,0x9,0x7,0x0},
    /* b */  {0x8,0xE,0x9,0x9,0xE,0x0},
    /* c */  {0x0,0x7,0x8,0x8,0x7,0x0},
    /* d */  {0x1,0x7,0x9,0x9,0x7,0x0},
    /* e */  {0x0,0x6,0xF,0x8,0x7,0x0},
    /* f */  {0x3,0x4,0xE,0x4,0x4,0x0},
    /* g */  {0x0,0x7,0x9,0x7,0x1,0x6},
    /* h */  {0x8,0xE,0x9,0x9,0x9,0x0},
    /* i */  {0x4,0x0,0x4,0x4,0x4,0x0},
    /* j */  {0x2,0x0,0x2,0x2,0xA,0x4},
    /* k */  {0x8,0x9,0xE,0xA,0x9,0x0},
    /* l */  {0xC,0x4,0x4,0x4,0xE,0x0},
    /* m */  {0x0,0xF,0xF,0x9,0x9,0x0},
    /* n */  {0x0,0xE,0x9,0x9,0x9,0x0},
    /* o */  {0x0,0x6,0x9,0x9,0x6,0x0},
    /* p */  {0x0,0xE,0x9,0xE,0x8,0x8},
    /* q */  {0x0,0x7,0x9,0x7,0x1,0x1},
    /* r */  {0x0,0x7,0x8,0x8,0x8,0x0},
    /* s */  {0x0,0x7,0xC,0x3,0xE,0x0},
    /* t */  {0x4,0xE,0x4,0x4,0x3,0x0},
    /* u */  {0x0,0x9,0x9,0x9,0x7,0x0},
    /* v */  {0x0,0x9,0x9,0x6,0x6,0x0},
    /* w */  {0x0,0x9,0x9,0xF,0x6,0x0},
    /* x */  {0x0,0x9,0x6,0x6,0x9,0x0},
    /* y */  {0x0,0x9,0x9,0x7,0x1,0x6},
    /* z */  {0x0,0xF,0x2,0x4,0xF,0x0},
    /* { */  {0x2,0x4,0x8,0x4,0x2,0x0},
    /* | */  {0x4,0x4,0x4,0x4,0x4,0x0},
    /* } */  {0x8,0x4,0x2,0x4,0x8,0x0},
    /* ~ */  {0x0,0x5,0xA,0x0,0x0,0x0},
};

static void draw_char(SDL_Renderer *r, int x, int y, char ch, int scale) {
    int idx = (unsigned char)ch - 0x20;
    if (idx < 0 || idx >= 96) return;
    const uint8_t *glyph = tiny_font[idx];
    for (int row = 0; row < 6; row++) {
        for (int col = 0; col < 4; col++) {
            if (glyph[row] & (8 >> col)) {
                SDL_Rect px = { x + col * scale, y + row * scale, scale, scale };
                SDL_RenderFillRect(r, &px);
            }
        }
    }
}

static void draw_text(SDL_Renderer *r, int x, int y, const char *text, int scale) {
    for (int i = 0; text[i]; i++) {
        draw_char(r, x + i * 5 * scale, y, text[i], scale);
    }
}

static int text_width(const char *text, int scale) {
    return (int)strlen(text) * 5 * scale;
}

/* ── File dialogs (Windows native, stub on other platforms) ────── */

#ifdef _WIN32
static int open_file_dialog(char *out, int out_size, const char *filter, const char *title) {
    OPENFILENAMEA ofn = {0};
    char file[MAX_PATH] = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameA(&ofn)) {
        strncpy(out, file, out_size - 1);
        return 0;
    }
    return -1;
}
#else
static int open_file_dialog(char *out, int out_size, const char *filter, const char *title) {
    (void)out; (void)out_size; (void)filter; (void)title;
    fprintf(stderr, "File dialog not available on this platform. Use command line or drag & drop.\n");
    return -1;
}
#endif

/* ── Auto-load matching .pcx palette for a GAF file ──────────────── */

static void auto_load_palette(ViewerState *v, const char *gaf_path) {
    char try_path[512];
    strncpy(try_path, gaf_path, sizeof(try_path) - 1);
    try_path[sizeof(try_path) - 1] = '\0';

    size_t len = strlen(try_path);
    if (len > 4) {
        /* 1. Try matching .pcx next to the GAF (e.g. singlemachine.pcx) */
        strcpy(try_path + len - 4, ".pcx");
        if (Palette_LoadPCX(&v->palette, try_path) == 0) {
            strncpy(v->pal_path, try_path, sizeof(v->pal_path) - 1);
            v->has_palette = 1;
            return;
        }

        /* 2. Try matching .pal next to the GAF */
        strcpy(try_path + len - 4, ".pal");
        if (Palette_Load(&v->palette, try_path) == 0) {
            strncpy(v->pal_path, try_path, sizeof(v->pal_path) - 1);
            v->has_palette = 1;
            return;
        }
    }

    /* 3. Use the reverse-engineered lookup table to find the correct palette */
    const char *lookup_pal = GAF_LookupPalette(gaf_path);

    /* Find the directory of the GAF file */
    const char *last_slash = NULL;
    for (const char *p = gaf_path; *p; p++) {
        if (*p == '/' || *p == '\\') last_slash = p;
    }

    char dir[512] = {0};
    if (last_slash) {
        size_t dir_len = last_slash - gaf_path;
        if (dir_len >= sizeof(dir)) dir_len = sizeof(dir) - 1;
        memcpy(dir, gaf_path, dir_len);
        dir[dir_len] = '\0';
    } else {
        strcpy(dir, ".");
    }

    /* If the lookup table returned a specific palette, try it first */
    if (lookup_pal) {
        const char *search_dirs[] = { dir, "%s/../palettes", "%s/palettes" };
        char fmt_buf[512];

        /* Try same directory */
        snprintf(try_path, sizeof(try_path), "%s/%s", dir, lookup_pal);
        if (Palette_LoadPCX(&v->palette, try_path) == 0) {
            strncpy(v->pal_path, try_path, sizeof(v->pal_path) - 1);
            v->has_palette = 1;
            return;
        }

        /* Try ../palettes/ */
        snprintf(try_path, sizeof(try_path), "%s/../palettes/%s", dir, lookup_pal);
        if (Palette_LoadPCX(&v->palette, try_path) == 0) {
            strncpy(v->pal_path, try_path, sizeof(v->pal_path) - 1);
            v->has_palette = 1;
            return;
        }

        /* Try palettes/ */
        snprintf(try_path, sizeof(try_path), "%s/palettes/%s", dir, lookup_pal);
        if (Palette_LoadPCX(&v->palette, try_path) == 0) {
            strncpy(v->pal_path, try_path, sizeof(v->pal_path) - 1);
            v->has_palette = 1;
            return;
        }
    }

    /* 3b. Try the alternative palette (e.g. _textures if primary was _features) */
    const char *alt_pal = GAF_LookupPaletteAlt(gaf_path);
    if (alt_pal) {
        snprintf(try_path, sizeof(try_path), "%s/%s", dir, alt_pal);
        if (Palette_LoadPCX(&v->palette, try_path) == 0) {
            strncpy(v->pal_path, try_path, sizeof(v->pal_path) - 1);
            v->has_palette = 1;
            return;
        }
        snprintf(try_path, sizeof(try_path), "%s/../palettes/%s", dir, alt_pal);
        if (Palette_LoadPCX(&v->palette, try_path) == 0) {
            strncpy(v->pal_path, try_path, sizeof(v->pal_path) - 1);
            v->has_palette = 1;
            return;
        }
        snprintf(try_path, sizeof(try_path), "%s/palettes/%s", dir, alt_pal);
        if (Palette_LoadPCX(&v->palette, try_path) == 0) {
            strncpy(v->pal_path, try_path, sizeof(v->pal_path) - 1);
            v->has_palette = 1;
            return;
        }
    }

    /* 4. Last resort: try common shared palettes */
    const char *shared_palettes[] = {
        "guipal.pcx", "gameart.pcx", "guipal.pal", "gameart.pal", NULL
    };
    for (int i = 0; shared_palettes[i]; i++) {
        snprintf(try_path, sizeof(try_path), "%s/%s", dir, shared_palettes[i]);
        if (Palette_LoadPCX(&v->palette, try_path) == 0 ||
            Palette_Load(&v->palette, try_path) == 0) {
            strncpy(v->pal_path, try_path, sizeof(v->pal_path) - 1);
            v->has_palette = 1;
            return;
        }
    }
    for (int i = 0; shared_palettes[i]; i++) {
        snprintf(try_path, sizeof(try_path), "%s/../palettes/%s", dir, shared_palettes[i]);
        if (Palette_LoadPCX(&v->palette, try_path) == 0 ||
            Palette_Load(&v->palette, try_path) == 0) {
            strncpy(v->pal_path, try_path, sizeof(v->pal_path) - 1);
            v->has_palette = 1;
            return;
        }
    }
}

/* ── Frame navigation ───────────────────────────────────────────── */

static void advance_frame(ViewerState *v) {
    if (!v->has_gaf) return;
    GAFEntryHeader *entry = NULL;
    GAF_GetEntryInfo(v->gaf, v->current_entry, &entry);
    if (!entry) return;
    v->current_frame++;
    if (v->current_frame >= entry->num_frames) {
        v->current_frame = 0;
        v->current_entry++;
        if (v->current_entry >= v->entry_count) v->current_entry = 0;
        rebuild_palette(v);
    }
    update_frame_texture(v);
}

static void retreat_frame(ViewerState *v) {
    if (!v->has_gaf) return;
    v->current_frame--;
    if (v->current_frame < 0) {
        v->current_entry--;
        if (v->current_entry < 0) v->current_entry = v->entry_count - 1;
        GAFEntryHeader *entry = NULL;
        GAF_GetEntryInfo(v->gaf, v->current_entry, &entry);
        v->current_frame = entry ? entry->num_frames - 1 : 0;
        rebuild_palette(v);
    }
    update_frame_texture(v);
}

static void advance_entry_anim(ViewerState *v) {
    if (!v->has_gaf) return;
    v->current_entry++;
    if (v->current_entry >= v->entry_count) v->current_entry = 0;
    v->current_frame = 0;
    rebuild_palette(v);
    update_frame_texture(v);
}

static int total_frame_count(ViewerState *v) {
    int total = 0;
    for (int i = 0; i < v->entry_count; i++) {
        GAFEntryHeader *entry = NULL;
        GAF_GetEntryInfo(v->gaf, i, &entry);
        if (entry) total += entry->num_frames;
    }
    return total;
}

static int global_frame_index(ViewerState *v) {
    int idx = 0;
    for (int i = 0; i < v->current_entry; i++) {
        GAFEntryHeader *entry = NULL;
        GAF_GetEntryInfo(v->gaf, i, &entry);
        if (entry) idx += entry->num_frames;
    }
    return idx + v->current_frame;
}

/* ── Update texture ─────────────────────────────────────────────── */

static void update_frame_texture(ViewerState *v) {
    if (v->frame_texture) {
        SDL_DestroyTexture(v->frame_texture);
        v->frame_texture = NULL;
    }
    if (!v->has_gaf) return;

    GAFEntryHeader *entry = NULL;
    if (GAF_GetEntryInfo(v->gaf, v->current_entry, &entry) != 0) return;

    GAFFrameHeader *frame = NULL;
    if (GAF_GetFrameInfo(v->gaf, v->current_entry, v->current_frame, &frame) != 0) return;

    uint32_t *rgba = GAF_DecodeFrameRGBA(v->gaf, frame, v->rgba_table);
    if (!rgba) return;

    v->frame_texture = SDL_CreateTexture(v->renderer, SDL_PIXELFORMAT_RGBA32,
                                          SDL_TEXTUREACCESS_STATIC,
                                          frame->width, frame->height);
    if (v->frame_texture) {
        SDL_SetTextureBlendMode(v->frame_texture, SDL_BLENDMODE_BLEND);
        SDL_UpdateTexture(v->frame_texture, NULL, rgba, frame->width * 4);
    }

    /* Update window title */
    int fps = (int)(1.0f / v->frame_duration + 0.5f);
    char title[512];
    snprintf(title, sizeof(title), "GAF Viewer - %s  |  %s [%d/%d]  Frame %d/%d  (%dx%d)  [%s]",
             v->gaf_path,
             entry->name, v->current_entry + 1, v->entry_count,
             v->current_frame + 1, entry->num_frames,
             frame->width, frame->height,
             v->gaf->format == GAF_FORMAT_TAK ? "TAK" : "TA");
    SDL_SetWindowTitle(v->window, title);

    free(rgba);
}

static void rebuild_palette(ViewerState *v) {
    uint8_t transp = 9;
    if (v->has_gaf) {
        GAFFrameHeader *frame = NULL;
        if (GAF_GetFrameInfo(v->gaf, v->current_entry, v->current_frame, &frame) == 0)
            transp = frame->transparency_index;
    }
    Palette_BuildRGBATable(&v->palette, v->rgba_table, transp);
}

/* ── Load a GAF file with auto-palette ──────────────────────────── */

static void load_gaf_file(ViewerState *v, const char *path) {
    GAFFile *new_gaf = NULL;
    GAFFormat fmt = (v->has_gaf) ? v->gaf->format : GAF_FORMAT_TAK;
    if (GAF_Open(&new_gaf, path, fmt) != 0) return;

    if (v->has_gaf) GAF_Close(v->gaf);
    v->gaf = new_gaf;
    v->has_gaf = 1;
    v->entry_count = GAF_GetEntryCount(v->gaf);
    v->current_entry = 0;
    v->current_frame = 0;
    v->anim_mode = ANIM_STOPPED;
    strncpy(v->gaf_path, path, sizeof(v->gaf_path) - 1);

    /* Auto-load matching palette */
    v->has_palette = 0;
    auto_load_palette(v, path);
    if (!v->has_palette) {
        Palette_BuildDefault(&v->palette);
    }

    rebuild_palette(v);
    update_frame_texture(v);

    /* Scan for available palettes for cycling */
    scan_palettes(v);

    /* Invalidate checkerboard cache */
    if (v->checker_cache) {
        SDL_DestroyTexture(v->checker_cache);
        v->checker_cache = NULL;
    }
}

static void load_palette_file(ViewerState *v, const char *path) {
    if (Palette_LoadPCX(&v->palette, path) == 0 ||
        Palette_Load(&v->palette, path) == 0) {
        strncpy(v->pal_path, path, sizeof(v->pal_path) - 1);
        v->has_palette = 1;
        rebuild_palette(v);
        update_frame_texture(v);
    }
}

static int stricmp_ext(const char *a, const char *b) {
    while (*a && *b) {
        char ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return *a - *b;
}

static void handle_drop(ViewerState *v, const char *path) {
    size_t len = strlen(path);
    if (len > 4) {
        const char *ext = path + len - 4;
        if (stricmp_ext(ext, ".pal") == 0 || stricmp_ext(ext, ".pcx") == 0) {
            load_palette_file(v, path);
            return;
        }
    }
    load_gaf_file(v, path);
}

/* ── Toolbar ────────────────────────────────────────────────────── */

static int point_in_rect(int px, int py, SDL_Rect *r) {
    return px >= r->x && px < r->x + r->w && py >= r->y && py < r->y + r->h;
}

static void layout_toolbar(ViewerState *v) {
    int win_w, win_h;
    SDL_GetWindowSize(v->window, &win_w, &win_h);
    int y = win_h - TOOLBAR_HEIGHT + (TOOLBAR_HEIGHT - BTN_H) / 2;

    int bw_sm = 44;
    int bw_md = 70;
    int bw_lg = 90;

    int x = BTN_PAD + 4;

    /* File group */
    v->buttons[BTN_OPEN_GAF] = (UIButton){{x, y, bw_lg, BTN_H}, "Open GAF", 0};
    x += bw_lg + BTN_PAD;
    v->buttons[BTN_OPEN_PAL] = (UIButton){{x, y, bw_lg, BTN_H}, "Open PAL", 0};
    x += bw_lg + BTN_PAD + 14;

    /* Separator */

    /* Navigation group */
    v->buttons[BTN_PREV] = (UIButton){{x, y, bw_md, BTN_H}, "< Prev", 0};
    x += bw_md + BTN_PAD;
    v->buttons[BTN_PLAY_ENTRIES] = (UIButton){{x, y, bw_md, BTN_H}, "Anim", 0};
    x += bw_md + BTN_PAD;
    v->buttons[BTN_STOP] = (UIButton){{x, y, bw_md - 10, BTN_H}, "Stop", 0};
    x += bw_md - 10 + BTN_PAD;
    v->buttons[BTN_PLAY_ALL] = (UIButton){{x, y, bw_md, BTN_H}, "All", 0};
    x += bw_md + BTN_PAD;
    v->buttons[BTN_NEXT] = (UIButton){{x, y, bw_md, BTN_H}, "Next >", 0};
    x += bw_md + BTN_PAD + 14;

    /* Speed group */
    v->buttons[BTN_SLOWER] = (UIButton){{x, y, bw_sm, BTN_H}, "Slow", 0};
    x += bw_sm + BTN_PAD;
    v->buttons[BTN_FASTER] = (UIButton){{x, y, bw_sm, BTN_H}, "Fast", 0};
    x += bw_sm + BTN_PAD + 14;

    /* View group */
    v->buttons[BTN_ZOOM_OUT] = (UIButton){{x, y, bw_sm, BTN_H}, "Z-", 0};
    x += bw_sm + BTN_PAD;
    v->buttons[BTN_ZOOM_IN] = (UIButton){{x, y, bw_sm, BTN_H}, "Z+", 0};
    x += bw_sm + BTN_PAD;
    v->buttons[BTN_CHECKER] = (UIButton){{x, y, bw_sm + 6, BTN_H}, "BG", 0};
    x += bw_sm + 6 + BTN_PAD;
    v->buttons[BTN_MODE_TOGGLE] = (UIButton){{x, y, bw_md, BTN_H}, "TA/TAK", 0};
    x += bw_md + BTN_PAD + 14;

    /* Palette cycling group */
    v->buttons[BTN_PAL_PREV] = (UIButton){{x, y, bw_sm, BTN_H}, "P<", 0};
    x += bw_sm + BTN_PAD;
    v->buttons[BTN_PAL_NEXT] = (UIButton){{x, y, bw_sm, BTN_H}, "P>", 0};
}

static void draw_toolbar(ViewerState *v) {
    int win_w, win_h;
    SDL_GetWindowSize(v->window, &win_w, &win_h);

    /* Toolbar background */
    SDL_SetRenderDrawColor(v->renderer, COL_TOOLBAR_R, COL_TOOLBAR_G, COL_TOOLBAR_B, 245);
    SDL_Rect bar = {0, win_h - TOOLBAR_HEIGHT, win_w, TOOLBAR_HEIGHT};
    SDL_RenderFillRect(v->renderer, &bar);

    /* Top edge line */
    SDL_SetRenderDrawColor(v->renderer, COL_BORDER_R, COL_BORDER_G, COL_BORDER_B, 255);
    SDL_RenderDrawLine(v->renderer, 0, win_h - TOOLBAR_HEIGHT, win_w, win_h - TOOLBAR_HEIGHT);

    /* Buttons */
    for (int i = 0; i < BTN_COUNT; i++) {
        UIButton *btn = &v->buttons[i];
        int is_active = 0;

        if (i == BTN_PLAY_ENTRIES && v->anim_mode == ANIM_ENTRIES) is_active = 1;
        if (i == BTN_PLAY_ALL && v->anim_mode == ANIM_ALL_FRAMES) is_active = 1;
        if (i == BTN_CHECKER && v->show_checkerboard) is_active = 1;

        if (is_active) {
            SDL_SetRenderDrawColor(v->renderer, COL_BTN_ACT_R, COL_BTN_ACT_G, COL_BTN_ACT_B, 255);
        } else if (btn->hovered) {
            SDL_SetRenderDrawColor(v->renderer, COL_BTN_HOV_R, COL_BTN_HOV_G, COL_BTN_HOV_B, 255);
        } else {
            SDL_SetRenderDrawColor(v->renderer, COL_BTN_R, COL_BTN_G, COL_BTN_B, 255);
        }
        SDL_RenderFillRect(v->renderer, &btn->rect);

        /* Border */
        SDL_SetRenderDrawColor(v->renderer, COL_BORDER_R, COL_BORDER_G, COL_BORDER_B, 255);
        SDL_RenderDrawRect(v->renderer, &btn->rect);

        /* Label */
        SDL_SetRenderDrawColor(v->renderer, COL_TEXT_R, COL_TEXT_G, COL_TEXT_B, 255);
        int tw = text_width(btn->label, FONT_SCALE);
        int tx = btn->rect.x + (btn->rect.w - tw) / 2;
        int ty = btn->rect.y + (btn->rect.h - 6 * FONT_SCALE) / 2;
        draw_text(v->renderer, tx, ty, btn->label, FONT_SCALE);
    }
}

/* ── Info panel at the top ──────────────────────────────────────── */

static void draw_info_panel(ViewerState *v) {
    int win_w, win_h;
    SDL_GetWindowSize(v->window, &win_w, &win_h);

    /* Background */
    SDL_SetRenderDrawColor(v->renderer, COL_TOOLBAR_R, COL_TOOLBAR_G, COL_TOOLBAR_B, 220);
    SDL_Rect bar = {0, 0, win_w, PANEL_TOP_H};
    SDL_RenderFillRect(v->renderer, &bar);
    SDL_SetRenderDrawColor(v->renderer, COL_BORDER_R, COL_BORDER_G, COL_BORDER_B, 255);
    SDL_RenderDrawLine(v->renderer, 0, PANEL_TOP_H, win_w, PANEL_TOP_H);

    if (!v->has_gaf) {
        SDL_SetRenderDrawColor(v->renderer, COL_DIM_R, COL_DIM_G, COL_DIM_B, 255);
        draw_text(v->renderer, 8, 6, "No file loaded. Click 'Open GAF' or drag & drop a .gaf file.", FONT_SCALE);
        return;
    }

    GAFEntryHeader *entry = NULL;
    GAF_GetEntryInfo(v->gaf, v->current_entry, &entry);
    GAFFrameHeader *frame = NULL;
    GAF_GetFrameInfo(v->gaf, v->current_entry, v->current_frame, &frame);

    if (!entry || !frame) return;

    int fps = (int)(1.0f / v->frame_duration + 0.5f);
    const char *mode = v->gaf->format == GAF_FORMAT_TAK ? "TAK" : "TA";
    const char *anim = v->anim_mode == ANIM_ENTRIES ? "Entry Anim" :
                       v->anim_mode == ANIM_ALL_FRAMES ? "All Frames" : "Stopped";

    /* Extract just the filename from the palette path for display */
    const char *pal_name = v->pal_path;
    if (v->has_palette) {
        const char *p;
        for (p = v->pal_path; *p; p++)
            if (*p == '/' || *p == '\\') pal_name = p + 1;
    } else {
        pal_name = "(greyscale)";
    }

    char pal_info[64];
    if (v->pal_list_count > 0) {
        snprintf(pal_info, sizeof(pal_info), "PAL: %s [%d/%d]",
                 pal_name, v->pal_list_index + 1, v->pal_list_count);
    } else {
        snprintf(pal_info, sizeof(pal_info), "PAL: %s", pal_name);
    }

    char info[512];
    snprintf(info, sizeof(info),
             "%s  [%d/%d]  Frame %d/%d  |  %dx%d  |  %s  %d FPS  |  %s  |  %s  |  %s",
             entry->name,
             v->current_entry + 1, v->entry_count,
             v->current_frame + 1, entry->num_frames,
             frame->width, frame->height,
             anim, fps,
             pal_info,
             mode,
             v->zoom > 1 ? "Zoom" : "1x");

    SDL_SetRenderDrawColor(v->renderer, COL_TEXT_R, COL_TEXT_G, COL_TEXT_B, 255);
    draw_text(v->renderer, 8, 6, info, FONT_SCALE);
}

/* ── Welcome screen when no file loaded ─────────────────────────── */

static void draw_welcome(ViewerState *v) {
    int win_w, win_h;
    SDL_GetWindowSize(v->window, &win_w, &win_h);

    int cy = win_h / 2 - 60;
    int s = FONT_SCALE + 1;

    SDL_SetRenderDrawColor(v->renderer, COL_ACCENT_R, COL_ACCENT_G, COL_ACCENT_B, 255);
    const char *t1 = "GAF Viewer";
    draw_text(v->renderer, (win_w - text_width(t1, s)) / 2, cy, t1, s);

    SDL_SetRenderDrawColor(v->renderer, COL_TEXT_R, COL_TEXT_G, COL_TEXT_B, 255);
    const char *t2 = "For Total Annihilation & TA: Kingdoms";
    draw_text(v->renderer, (win_w - text_width(t2, FONT_SCALE)) / 2, cy + 40, t2, FONT_SCALE);

    SDL_SetRenderDrawColor(v->renderer, COL_DIM_R, COL_DIM_G, COL_DIM_B, 255);
    const char *t3 = "Click 'Open GAF' or drag a .gaf file here";
    draw_text(v->renderer, (win_w - text_width(t3, FONT_SCALE)) / 2, cy + 80, t3, FONT_SCALE);

    const char *t4 = "Palette: drag .pal or .pcx (auto-detected from GAF name)";
    draw_text(v->renderer, (win_w - text_width(t4, FONT_SCALE)) / 2, cy + 105, t4, FONT_SCALE);
}

/* ── Checkerboard ───────────────────────────────────────────────── */

static SDL_Texture *get_checkerboard(ViewerState *v, int w, int h) {
    if (v->checker_cache && v->checker_w == w && v->checker_h == h && v->checker_zoom == v->zoom)
        return v->checker_cache;

    if (v->checker_cache) SDL_DestroyTexture(v->checker_cache);

    int cell = 8 * v->zoom;
    v->checker_cache = SDL_CreateTexture(v->renderer, SDL_PIXELFORMAT_RGBA32,
                                          SDL_TEXTUREACCESS_STREAMING, w, h);
    if (!v->checker_cache) return NULL;

    uint32_t *pixels;
    int pitch;
    SDL_LockTexture(v->checker_cache, NULL, (void **)&pixels, &pitch);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int dark = ((x / cell) + (y / cell)) % 2;
            pixels[y * (pitch / 4) + x] = dark ? 0xFF999999 : 0xFFCCCCCC;
        }
    }
    SDL_UnlockTexture(v->checker_cache);
    v->checker_w = w;
    v->checker_h = h;
    v->checker_zoom = v->zoom;
    return v->checker_cache;
}

/* ── Button click handler ───────────────────────────────────────── */

static void handle_button_click(ViewerState *v, int btn_id) {
    switch (btn_id) {
    case BTN_OPEN_GAF: {
        char path[512] = {0};
        if (open_file_dialog(path, sizeof(path),
                "GAF Files (*.gaf)\0*.gaf\0All Files\0*.*\0",
                "Open GAF Sprite File") == 0) {
            load_gaf_file(v, path);
        }
        break;
    }
    case BTN_OPEN_PAL: {
        char path[512] = {0};
        if (open_file_dialog(path, sizeof(path),
                "Palette Files (*.pal;*.pcx)\0*.pal;*.pcx\0All Files\0*.*\0",
                "Open Palette File") == 0) {
            load_palette_file(v, path);
        }
        break;
    }
    case BTN_PREV:
        retreat_frame(v);
        break;
    case BTN_NEXT:
        advance_frame(v);
        break;
    case BTN_PLAY_ENTRIES:
        v->anim_mode = (v->anim_mode == ANIM_ENTRIES) ? ANIM_STOPPED : ANIM_ENTRIES;
        v->anim_timer = 0;
        break;
    case BTN_PLAY_ALL:
        v->anim_mode = (v->anim_mode == ANIM_ALL_FRAMES) ? ANIM_STOPPED : ANIM_ALL_FRAMES;
        v->anim_timer = 0;
        break;
    case BTN_STOP:
        v->anim_mode = ANIM_STOPPED;
        break;
    case BTN_SLOWER:
        if (v->frame_duration < 1.0f) v->frame_duration *= 1.5f;
        update_frame_texture(v);
        break;
    case BTN_FASTER:
        if (v->frame_duration > 0.01f) v->frame_duration /= 1.5f;
        update_frame_texture(v);
        break;
    case BTN_ZOOM_IN:
        if (v->zoom < 8) v->zoom++;
        break;
    case BTN_ZOOM_OUT:
        if (v->zoom > 1) v->zoom--;
        break;
    case BTN_CHECKER:
        v->show_checkerboard = !v->show_checkerboard;
        break;
    case BTN_MODE_TOGGLE:
        if (v->has_gaf) {
            v->gaf->format = (v->gaf->format == GAF_FORMAT_TAK) ? GAF_FORMAT_TA : GAF_FORMAT_TAK;
            update_frame_texture(v);
        }
        break;
    case BTN_PAL_PREV:
        cycle_palette(v, -1);
        break;
    case BTN_PAL_NEXT:
        cycle_palette(v, 1);
        break;
    }
}

/* ── Main ───────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    ViewerState v = {0};
    v.anim_mode = ANIM_STOPPED;
    v.frame_duration = 1.0f / 10.0f;
    v.show_checkerboard = 1;
    v.zoom = 2;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    v.window = SDL_CreateWindow("GAF Viewer",
                                SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                1024, 700,
                                SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    v.renderer = SDL_CreateRenderer(v.window, -1,
                                     SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_SetRenderDrawBlendMode(v.renderer, SDL_BLENDMODE_BLEND);
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);

    Palette_BuildDefault(&v.palette);
    Palette_BuildRGBATable(&v.palette, v.rgba_table, 9);

    layout_toolbar(&v);

    /* Load GAF from command line if provided */
    if (argc >= 2) {
        load_gaf_file(&v, argv[1]);
        /* Override auto-palette if explicit palette arg given */
        if (argc >= 3) {
            load_palette_file(&v, argv[2]);
        }
    }

    int running = 1;
    uint64_t last_time = SDL_GetPerformanceCounter();
    uint64_t freq = SDL_GetPerformanceFrequency();

    while (running) {
        uint64_t now = SDL_GetPerformanceCounter();
        float dt = (float)(now - last_time) / (float)freq;
        last_time = now;

        int mx, my;
        SDL_GetMouseState(&mx, &my);
        for (int i = 0; i < BTN_COUNT; i++)
            v.buttons[i].hovered = point_in_rect(mx, my, &v.buttons[i].rect);

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
            case SDL_QUIT:
                running = 0;
                break;
            case SDL_WINDOWEVENT:
                if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                    layout_toolbar(&v);
                    if (v.checker_cache) { SDL_DestroyTexture(v.checker_cache); v.checker_cache = NULL; }
                }
                break;
            case SDL_DROPFILE:
                handle_drop(&v, event.drop.file);
                SDL_free(event.drop.file);
                break;
            case SDL_MOUSEBUTTONDOWN:
                if (event.button.button == SDL_BUTTON_LEFT) {
                    for (int i = 0; i < BTN_COUNT; i++) {
                        if (point_in_rect(event.button.x, event.button.y, &v.buttons[i].rect)) {
                            handle_button_click(&v, i);
                            break;
                        }
                    }
                }
                break;
            case SDL_KEYDOWN:
                switch (event.key.keysym.sym) {
                case SDLK_ESCAPE: running = 0; break;
                case SDLK_o: handle_button_click(&v, BTN_OPEN_GAF); break;
                case SDLK_p: handle_button_click(&v, BTN_OPEN_PAL); break;
                case SDLK_UP:
                    if (v.has_gaf && v.current_entry > 0) {
                        v.current_entry--; v.current_frame = 0;
                        rebuild_palette(&v); update_frame_texture(&v);
                    }
                    break;
                case SDLK_DOWN:
                    if (v.has_gaf && v.current_entry < v.entry_count - 1) {
                        v.current_entry++; v.current_frame = 0;
                        rebuild_palette(&v); update_frame_texture(&v);
                    }
                    break;
                case SDLK_LEFT: retreat_frame(&v); break;
                case SDLK_RIGHT: advance_frame(&v); break;
                case SDLK_SPACE:
                    v.anim_mode = (v.anim_mode == ANIM_ENTRIES) ? ANIM_STOPPED : ANIM_ENTRIES;
                    v.anim_timer = 0;
                    break;
                case SDLK_a:
                    v.anim_mode = (v.anim_mode == ANIM_ALL_FRAMES) ? ANIM_STOPPED : ANIM_ALL_FRAMES;
                    v.anim_timer = 0;
                    break;
                case SDLK_t:
                    if (v.has_gaf) {
                        v.gaf->format = (v.gaf->format == GAF_FORMAT_TAK) ? GAF_FORMAT_TA : GAF_FORMAT_TAK;
                        update_frame_texture(&v);
                    }
                    break;
                case SDLK_f: v.show_checkerboard = !v.show_checkerboard; break;
                case SDLK_EQUALS: case SDLK_PLUS: if (v.zoom < 8) v.zoom++; break;
                case SDLK_MINUS: if (v.zoom > 1) v.zoom--; break;
                case SDLK_RIGHTBRACKET:
                    if (v.frame_duration > 0.01f) { v.frame_duration /= 1.5f; update_frame_texture(&v); }
                    break;
                case SDLK_LEFTBRACKET:
                    if (v.frame_duration < 1.0f) { v.frame_duration *= 1.5f; update_frame_texture(&v); }
                    break;
                }
                break;
            }
        }

        /* Animation */
        if (v.anim_mode != ANIM_STOPPED && v.has_gaf) {
            v.anim_timer += dt;
            if (v.anim_timer >= v.frame_duration) {
                v.anim_timer -= v.frame_duration;
                if (v.anim_mode == ANIM_ENTRIES) advance_entry_anim(&v);
                else advance_frame(&v);
            }
        }

        /* Render */
        SDL_SetRenderDrawColor(v.renderer, COL_BG_R, COL_BG_G, COL_BG_B, 255);
        SDL_RenderClear(v.renderer);

        if (v.has_gaf && v.frame_texture) {
            int tex_w, tex_h;
            SDL_QueryTexture(v.frame_texture, NULL, NULL, &tex_w, &tex_h);
            int draw_w = tex_w * v.zoom;
            int draw_h = tex_h * v.zoom;
            int win_w, win_h;
            SDL_GetWindowSize(v.window, &win_w, &win_h);
            int content_h = win_h - TOOLBAR_HEIGHT - PANEL_TOP_H;
            int draw_x = (win_w - draw_w) / 2;
            int draw_y = PANEL_TOP_H + (content_h - draw_h) / 2;
            SDL_Rect dst = {draw_x, draw_y, draw_w, draw_h};

            if (v.show_checkerboard) {
                SDL_Texture *checker = get_checkerboard(&v, draw_w, draw_h);
                if (checker) SDL_RenderCopy(v.renderer, checker, NULL, &dst);
            }
            SDL_RenderCopy(v.renderer, v.frame_texture, NULL, &dst);
        } else if (!v.has_gaf) {
            draw_welcome(&v);
        }

        draw_info_panel(&v);
        draw_toolbar(&v);
        SDL_RenderPresent(v.renderer);
    }

    if (v.frame_texture) SDL_DestroyTexture(v.frame_texture);
    if (v.checker_cache) SDL_DestroyTexture(v.checker_cache);
    SDL_DestroyRenderer(v.renderer);
    SDL_DestroyWindow(v.window);
    SDL_Quit();
    free_pal_list(&v);
    if (v.has_gaf) GAF_Close(v.gaf);

    return 0;
}
