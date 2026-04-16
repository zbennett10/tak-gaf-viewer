/*
 * pal_lookup.c -- TAK GAF-to-palette mapping table
 *
 * Reverse-engineered from the TAK Kingdoms game binary. The original engine
 * loads specific palettes for specific GAF files via UnitTypeMap_InsertNode()
 * and Palette_LoadFromPCX(). This table captures those associations so the
 * viewer can auto-select the correct palette.
 *
 * Mapping sources:
 *   gameart.pcx    -- Default for all unit/animation GAFs
 *   fx.pcx         -- Effects: smoke, fire, death, shadows, damage flames
 *   cursors.pcx    -- All cursor sprites
 *   colorlogos.pcx -- Faction logos, team logos
 *   guipal.pcx     -- GUI elements: buttons, scrollbars, fonts, HUD
 *   modalbuttons.pcx -- In-game modal dialog buttons
 *   Per-faction     -- aramon.pcx, taros.pcx, veruna.pcx, zhon.pcx for unit sprites
 *
 * GAF files not in this table fall back to the auto-detection chain
 * (matching .pcx next to the GAF, then guipal, then gameart).
 */

#include "gaf.h"
#include <string.h>

/* Case-insensitive string comparison */
static int istrcmp(const char *a, const char *b) {
    while (*a && *b) {
        char ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

/* Check if haystack ends with needle (case-insensitive) */
static int ends_with(const char *haystack, const char *needle) {
    size_t hlen = strlen(haystack);
    size_t nlen = strlen(needle);
    if (nlen > hlen) return 0;
    return istrcmp(haystack + hlen - nlen, needle) == 0;
}

/* Extract just the filename without path or extension */
static void basename_no_ext(const char *path, char *out, int out_size) {
    const char *name = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\') name = p + 1;

    int i = 0;
    while (name[i] && name[i] != '.' && i < out_size - 1) {
        out[i] = name[i];
        i++;
    }
    out[i] = '\0';
}

/*
 * GAF files that use fx.pcx palette (effects, shadows, damage)
 */
static const char *fx_gafs[] = {
    "smoke", "radiated", "nobuild", "transportfx", "deathmagic",
    "activity_fires", "minifire", "oldfx",
    NULL
};

/*
 * GAF files that use cursors.pcx palette
 */
static const char *cursor_gafs[] = {
    "cursors", "cursors_tex",
    NULL
};

/*
 * GAF files that use colorlogos.pcx palette
 */
static const char *logo_gafs[] = {
    "colorlogos", "colorlogos2", "teamlogos", "logos",
    NULL
};

/*
 * GAF files that use guipal.pcx palette (GUI, HUD, fonts, menus)
 */
static const char *gui_gafs[] = {
    "commongui", "commongui_french", "commongui_german",
    "gui", "buildbuttons",
    "scrollbars", "modalbuttons",
    "byhelpbutton", "byhelpcrusadesbutton",
    "bymessagemarquee", "bynewsconsole",
    "byc_icons", "byc_primary", "byc_secondary", "byc_shadows",
    "byfakemetamapbg", "bywarroombutton", "bynewsninfobutton",
    "bynewsninfocrusadesbutton", "byiconlookup",
    "byiconsgrouped", "byiconslatency",
    "bymetagameforming", "byhouseterror", "byhousehonor",
    "metamap_font", "font48",
    "f2menu", "battlemenu", "battlemenuoptions",
    "multiplayerexitmenu", "singleplayerexitmenu",
    "knighterrant", "strategicpoint",
    "main_console",
    NULL
};

/*
 * GAF files that use the matching per-sprite .pcx palette
 * (these have their own palette file alongside them)
 */
static const char *self_palette_gafs[] = {
    "singlemachine", "bodgirl", "multiknight",
    "mainscreen", "mainmenu", "mainmenuconflict",
    "singleplayerwin", "singleplayerlose", "singleplaywin",
    "battlescreens",
    NULL
};

/*
 * Determine the best palette filename for a given GAF file.
 * Returns the palette name (e.g. "guipal.pcx") or NULL if unknown.
 * The caller should search for this file in the palettes directory.
 */
const char *GAF_LookupPalette(const char *gaf_path) {
    char name[64];
    basename_no_ext(gaf_path, name, sizeof(name));

    /* Check self-palette GAFs (matching .pcx next to the GAF) */
    for (int i = 0; self_palette_gafs[i]; i++) {
        if (istrcmp(name, self_palette_gafs[i]) == 0)
            return NULL; /* auto_load_palette will find the matching .pcx */
    }

    /* Check fx palette */
    for (int i = 0; fx_gafs[i]; i++) {
        if (istrcmp(name, fx_gafs[i]) == 0)
            return "fx.pcx";
    }

    /* Check cursor palette */
    for (int i = 0; cursor_gafs[i]; i++) {
        if (istrcmp(name, cursor_gafs[i]) == 0)
            return "cursors.pcx";
    }

    /* Check logo palette */
    for (int i = 0; logo_gafs[i]; i++) {
        if (istrcmp(name, logo_gafs[i]) == 0)
            return "colorlogos.pcx";
    }

    /* Check GUI palette */
    for (int i = 0; gui_gafs[i]; i++) {
        if (istrcmp(name, gui_gafs[i]) == 0)
            return "guipal.pcx";
    }

    /* Check faction-specific patterns based on filename prefix.
     * The original engine loads per-faction palettes from sidedata.tdf:
     *   ara* -> ara_textures (Aramon)
     *   tar* -> tar_textures (Taros)
     *   ver* -> ver_textures (Veruna)
     *   zon* -> zon_textures (Zhon)
     *   aid* -> aid_textures (Aiden, expansion)
     */
    /*
     * Faction-prefixed GAFs: could be a unit (uses _textures palette) or
     * a map feature (uses _features palette). We return the _features palette
     * since auto_load_palette tries multiple paths. The caller should also
     * try the _textures variant as a fallback.
     *
     * Sidedata mappings from sidedata.tdf:
     *   ara* -> aramon_features.pcx / ara_textures.pcx
     *   tar* -> taros_features.pcx  / tar_textures.pcx
     *   ver* -> veruna_features.pcx / ver_textures.pcx
     *   zon* -> zhon_features.pcx   / zon_textures.pcx
     */
    if (name[0] == 'a' || name[0] == 'A') {
        if ((name[1] == 'r' || name[1] == 'R') && (name[2] == 'a' || name[2] == 'A'))
            return "aramon_features.pcx";
        if ((name[1] == 'i' || name[1] == 'I') && (name[2] == 'd' || name[2] == 'D'))
            return "aiden_features.pcx";
    }
    if ((name[0] == 't' || name[0] == 'T') &&
        (name[1] == 'a' || name[1] == 'A') &&
        (name[2] == 'r' || name[2] == 'R'))
        return "taros_features.pcx";
    if ((name[0] == 'v' || name[0] == 'V') &&
        (name[1] == 'e' || name[1] == 'E') &&
        (name[2] == 'r' || name[2] == 'R'))
        return "veruna_features.pcx";
    if ((name[0] == 'z' || name[0] == 'Z') &&
        (name[1] == 'o' || name[1] == 'O') &&
        (name[2] == 'n' || name[2] == 'N'))
        return "zhon_features.pcx";
    if ((name[0] == 'z' || name[0] == 'Z') &&
        (name[1] == 'h' || name[1] == 'H') &&
        (name[2] == 'o' || name[2] == 'O'))
        return "zhon_features.pcx";
    /* npc/monster units */
    if ((name[0] == 'n' || name[0] == 'N') &&
        (name[1] == 'p' || name[1] == 'P') &&
        (name[2] == 'c' || name[2] == 'C'))
        return "npc_textures.pcx";
    if ((name[0] == 'm' || name[0] == 'M') &&
        (name[1] == 'o' || name[1] == 'O') &&
        (name[2] == 'n' || name[2] == 'N'))
        return "mon_textures.pcx";

    /* bipal files use guipal */
    if (ends_with(name, "bipal")) return "guipal.pcx";

    /* Shadow GAFs use fx palette */
    if (ends_with(name, "shadow") || ends_with(name, "shadows"))
        return "fx.pcx";

    /* Font GAFs use guipal */
    if (strstr(name, "font") || strstr(name, "Font") ||
        strstr(name, "times") || strstr(name, "Times") ||
        strstr(name, "lombardic") || strstr(name, "Lombardic"))
        return "guipal.pcx";

    /* Default: gameart.pcx for everything else */
    return "gameart.pcx";
}

/*
 * For faction-prefixed GAFs, return the alternative palette to try.
 * The primary lookup returns _features; this returns _textures (or vice versa).
 * Returns NULL if there's no alternative to try.
 */
const char *GAF_LookupPaletteAlt(const char *gaf_path) {
    char name[64];
    basename_no_ext(gaf_path, name, sizeof(name));

    if (name[0] == 'a' || name[0] == 'A') {
        if ((name[1] == 'r' || name[1] == 'R') && (name[2] == 'a' || name[2] == 'A'))
            return "ara_textures.pcx";
        if ((name[1] == 'i' || name[1] == 'I') && (name[2] == 'd' || name[2] == 'D'))
            return "aid_textures.pcx";
    }
    if ((name[0] == 't' || name[0] == 'T') &&
        (name[1] == 'a' || name[1] == 'A') &&
        (name[2] == 'r' || name[2] == 'R'))
        return "tar_textures.pcx";
    if ((name[0] == 'v' || name[0] == 'V') &&
        (name[1] == 'e' || name[1] == 'E') &&
        (name[2] == 'r' || name[2] == 'R'))
        return "ver_textures.pcx";
    if ((name[0] == 'z' || name[0] == 'Z') &&
        ((name[1] == 'o' || name[1] == 'O') || (name[1] == 'h' || name[1] == 'H')))
        return "zon_textures.pcx";

    return NULL;
}
