#ifndef GAF_H
#define GAF_H

#include <stdint.h>
#include <stddef.h>

/*
 * GAF/TAF sprite format definitions.
 *
 * Supports both Total Annihilation (TA) and TA: Kingdoms (TAK) GAF files.
 * These use different RLE compression schemes despite sharing the same
 * container format.
 */

#define GAF_VERSION_MAGIC 0x00010100

typedef enum {
    GAF_FORMAT_TA,   /* Original TA RLE scheme */
    GAF_FORMAT_TAK,  /* TAK Kingdoms 2-bit flag RLE scheme */
    GAF_FORMAT_AUTO  /* Auto-detect (try TAK first, fall back to TA) */
} GAFFormat;

typedef struct {
    uint16_t version;
    uint16_t subversion;
    uint32_t num_entries;
    uint32_t reserved;
} GAFHeader;

typedef struct {
    uint16_t num_frames;
    uint16_t reserved1;
    uint32_t reserved2;
    char name[32];
} GAFEntryHeader;

typedef struct {
    uint16_t width;
    uint16_t height;
    int16_t  offset_x;
    int16_t  offset_y;
    uint8_t  transparency_index;
    uint8_t  compressed;
    uint16_t subframes;
    uint32_t unknown;
    uint32_t pixel_data_offset;
} GAFFrameHeader;

typedef struct {
    uint8_t *data;
    uint32_t data_size;
    uint32_t num_entries;
    GAFFormat format;
} GAFFile;

typedef struct {
    uint8_t r, g, b, pad;
} PaletteEntry;

typedef struct {
    PaletteEntry entries[256];
} Palette;

/* File operations */
int GAF_Open(GAFFile **out, const char *path, GAFFormat format);
void GAF_Close(GAFFile *gaf);

/* Sequence/frame navigation */
int GAF_GetEntryCount(GAFFile *gaf);
int GAF_GetEntryInfo(GAFFile *gaf, int entry_index, GAFEntryHeader **out);
int GAF_GetFrameInfo(GAFFile *gaf, int entry_index, int frame_index, GAFFrameHeader **out);

/* Decoding */
uint8_t *GAF_DecodeFrame(GAFFile *gaf, const GAFFrameHeader *frame);
uint32_t *GAF_DecodeFrameRGBA(GAFFile *gaf, const GAFFrameHeader *frame, const uint32_t *rgba_table);

/* Palette */
int Palette_Load(Palette *pal, const char *path);
void Palette_BuildRGBATable(const Palette *pal, uint32_t *table_out, uint8_t transparent_index);

/* Load palette from a PCX file (reads the 256-color palette at the end) */
int Palette_LoadPCX(Palette *pal, const char *path);

/* Built-in default palette (greyscale ramp for when no .pal file is available) */
void Palette_BuildDefault(Palette *pal);

/* Look up the correct palette filename for a TAK GAF file.
 * Returns a palette name like "guipal.pcx" or NULL if the GAF has
 * a matching .pcx next to it (auto-detect will find it). */
const char *GAF_LookupPalette(const char *gaf_path);

/* For faction GAFs, return an alternative palette to try.
 * Primary returns _features.pcx, alt returns _textures.pcx.
 * Returns NULL if no alternative exists. */
const char *GAF_LookupPaletteAlt(const char *gaf_path);

#endif /* GAF_H */
