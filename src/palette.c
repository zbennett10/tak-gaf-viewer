/*
 * palette.c -- PAL file loader and RGBA conversion
 *
 * PAL files are 1024 bytes: 256 entries of (R, G, B, padding).
 * Used to convert 8-bit GAF palette indices into displayable colors.
 */

#include "gaf.h"
#include <stdio.h>
#include <string.h>

int Palette_Load(Palette *pal, const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;

    fseek(fp, 0, SEEK_END);
    if (ftell(fp) != 1024) {
        fclose(fp);
        return -1;
    }

    fseek(fp, 0, SEEK_SET);
    fread(pal->entries, 1024, 1, fp);
    fclose(fp);
    return 0;
}

void Palette_BuildRGBATable(const Palette *pal, uint32_t *table_out, uint8_t transparent_index) {
    for (int i = 0; i < 256; i++) {
        PaletteEntry e = pal->entries[i];
        if (i == transparent_index) {
            /* Fully transparent (RGBA: 0,0,0,0) */
            table_out[i] = 0x00000000;
        } else {
            /* Pack as ABGR (SDL_PIXELFORMAT_RGBA32 on little-endian) */
            table_out[i] = ((uint32_t)0xFF << 24) |
                           ((uint32_t)e.b << 16) |
                           ((uint32_t)e.g << 8) |
                           ((uint32_t)e.r);
        }
    }
}

int Palette_LoadPCX(Palette *pal, const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;

    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);

    /* Need at least 769 bytes for palette (1 marker + 256*3 RGB) */
    if (file_size < 769) {
        fclose(fp);
        return -1;
    }

    /* Check for 0x0C palette marker at file_size - 769 */
    fseek(fp, file_size - 769, SEEK_SET);
    uint8_t marker;
    fread(&marker, 1, 1, fp);
    if (marker != 0x0C) {
        fclose(fp);
        return -1;
    }

    /* Read 256 RGB triplets (3 bytes each, no padding) */
    uint8_t rgb[768];
    fread(rgb, 768, 1, fp);
    fclose(fp);

    for (int i = 0; i < 256; i++) {
        pal->entries[i].r = rgb[i * 3];
        pal->entries[i].g = rgb[i * 3 + 1];
        pal->entries[i].b = rgb[i * 3 + 2];
        pal->entries[i].pad = 0;
    }

    return 0;
}

void Palette_BuildDefault(Palette *pal) {
    for (int i = 0; i < 256; i++) {
        pal->entries[i].r = (uint8_t)i;
        pal->entries[i].g = (uint8_t)i;
        pal->entries[i].b = (uint8_t)i;
        pal->entries[i].pad = 0;
    }
}
