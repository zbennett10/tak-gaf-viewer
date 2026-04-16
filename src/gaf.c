/*
 * gaf.c -- GAF sprite file loader and RLE decoder
 *
 * Supports both TA and TAK Kingdoms GAF files. The container format is
 * identical; only the RLE compression differs:
 *
 * TA RLE (per scanline, after uint16 line_bytes):
 *   0x00:       end of line (fill remaining with transparent)
 *   0x01-0x7F:  literal N pixels
 *   0x80:       extended literal, next byte is count
 *   0x81-0xFE:  skip (byte - 0x80) transparent pixels
 *   0xFF:       extended skip, next byte is count
 *
 * TAK RLE (per scanline, after uint16 line_bytes):
 *   bit0=1:         transparent skip,  count = byte >> 1
 *   bit0=0, bit1=0: literal pixels,    count = (byte >> 2) + 1
 *   bit0=0, bit1=1: repeat color,      count = (byte >> 2) + 1
 */

#include "gaf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* File format constants */
#define GAF_HEADER_SIZE       12
#define GAF_ENTRY_HEADER_SIZE 40

/*
 * GAF_Open -- Load a GAF file from disk and validate the header.
 */
int GAF_Open(GAFFile **out, const char *path, GAFFormat format) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "Cannot open: %s\n", path);
        return -1;
    }

    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (file_size < GAF_HEADER_SIZE) {
        fprintf(stderr, "File too small: %s\n", path);
        fclose(fp);
        return -1;
    }

    uint8_t *buffer = (uint8_t *)malloc(file_size);
    if (!buffer) {
        fclose(fp);
        return -1;
    }

    fread(buffer, file_size, 1, fp);
    fclose(fp);

    if (*(uint32_t *)buffer != GAF_VERSION_MAGIC) {
        fprintf(stderr, "Invalid GAF version in: %s\n", path);
        free(buffer);
        return -1;
    }

    GAFFile *gaf = (GAFFile *)malloc(sizeof(GAFFile));
    if (!gaf) {
        free(buffer);
        return -1;
    }

    gaf->data = buffer;
    gaf->data_size = (uint32_t)file_size;
    gaf->num_entries = *(uint32_t *)(buffer + 4);
    gaf->format = (format == GAF_FORMAT_AUTO) ? GAF_FORMAT_TAK : format;

    *out = gaf;
    return 0;
}

void GAF_Close(GAFFile *gaf) {
    if (gaf) {
        free(gaf->data);
        free(gaf);
    }
}

/* ── Navigation ─────────────────────────────────────────────────── */

int GAF_GetEntryCount(GAFFile *gaf) {
    return gaf ? (int)gaf->num_entries : 0;
}

/* Get the absolute byte offset for entry i in the pointer table */
static uint32_t gaf_entry_offset(GAFFile *gaf, int entry_index) {
    return *(uint32_t *)(gaf->data + GAF_HEADER_SIZE + entry_index * 4);
}

int GAF_GetEntryInfo(GAFFile *gaf, int entry_index, GAFEntryHeader **out) {
    if (!gaf || entry_index < 0 || entry_index >= (int)gaf->num_entries)
        return -1;
    uint32_t off = gaf_entry_offset(gaf, entry_index);
    *out = (GAFEntryHeader *)(gaf->data + off);
    return 0;
}

int GAF_GetFrameInfo(GAFFile *gaf, int entry_index, int frame_index, GAFFrameHeader **out) {
    if (!gaf || entry_index < 0 || entry_index >= (int)gaf->num_entries)
        return -1;

    uint32_t entry_off = gaf_entry_offset(gaf, entry_index);
    GAFEntryHeader *entry = (GAFEntryHeader *)(gaf->data + entry_off);

    if (frame_index < 0 || frame_index >= entry->num_frames)
        return -1;

    /* Frame pointer table: 8 bytes per frame (4B offset + 4B unknown) */
    uint8_t *frame_ptr_table = gaf->data + entry_off + GAF_ENTRY_HEADER_SIZE;
    uint32_t frame_off = *(uint32_t *)(frame_ptr_table + frame_index * 8);
    *out = (GAFFrameHeader *)(gaf->data + frame_off);
    return 0;
}

/* ── RLE decoders ───────────────────────────────────────────────── */

/*
 * Decode one scanline using TAK's 2-bit flag RLE scheme.
 */
static void decode_scanline_tak(uint8_t *src, uint16_t line_bytes,
                                uint8_t *dst, uint16_t width) {
    uint8_t *end = src + line_bytes;
    uint16_t x = 0;

    while (src < end && x < width) {
        uint8_t ctrl = *src++;

        if (ctrl & 1) {
            /* TRANSPARENT: skip count pixels */
            x += ctrl >> 1;
        } else if ((ctrl & 3) == 0) {
            /* LITERAL: copy count pixels from stream */
            size_t count = (ctrl >> 2) + 1;
            if (x + count > width) count = width - x;
            memcpy(dst + x, src, count);
            src += count;
            x += (uint16_t)count;
        } else {
            /* REPEAT: fill count pixels with next byte */
            size_t count = (ctrl >> 2) + 1;
            uint8_t color = *src++;
            if (x + count > width) count = width - x;
            memset(dst + x, color, count);
            x += (uint16_t)count;
        }
    }
}

/*
 * Decode one scanline using TA's original RLE scheme.
 */
static void decode_scanline_ta(uint8_t *src, uint16_t line_bytes,
                               uint8_t *dst, uint16_t width) {
    uint8_t *end = src + line_bytes;
    uint16_t x = 0;

    while (src < end && x < width) {
        uint8_t ctrl = *src++;

        if (ctrl == 0x00) {
            /* End of line */
            break;
        } else if (ctrl <= 0x7F) {
            /* Literal run of ctrl pixels */
            size_t count = ctrl;
            if (x + count > width) count = width - x;
            memcpy(dst + x, src, count);
            src += ctrl; /* advance by original count */
            x += (uint16_t)count;
        } else if (ctrl == 0x80) {
            /* Extended literal: next byte is count */
            uint8_t count = *src++;
            if (count == 0) continue;
            size_t n = count;
            if (x + n > width) n = width - x;
            memcpy(dst + x, src, n);
            src += count;
            x += (uint16_t)n;
        } else if (ctrl == 0xFF) {
            /* Extended skip: next byte is count */
            uint8_t count = *src++;
            x += count;
        } else {
            /* Skip (ctrl - 0x80) transparent pixels */
            x += ctrl - 0x80;
        }
    }
}

uint8_t *GAF_DecodeFrame(GAFFile *gaf, const GAFFrameHeader *frame) {
    if (!gaf || !frame) return NULL;

    size_t total_pixels = (size_t)frame->width * frame->height;
    uint8_t *pixels = (uint8_t *)malloc(total_pixels);
    if (!pixels) return NULL;

    /* Pre-fill with transparency */
    memset(pixels, frame->transparency_index, total_pixels);

    uint8_t *src = gaf->data + frame->pixel_data_offset;

    /* Bounds check: pixel_data_offset must be within the file */
    if (frame->pixel_data_offset >= gaf->data_size) {
        free(pixels);
        return NULL;
    }

    if (frame->compressed == 0) {
        /* Uncompressed: raw palette indices, width*height bytes */
        size_t available = gaf->data_size - frame->pixel_data_offset;
        size_t to_copy = total_pixels;
        if (to_copy > available) to_copy = available;
        memcpy(pixels, src, to_copy);
    } else {
        /* RLE compressed: per-scanline decode */
        for (int row = 0; row < frame->height; row++) {
            /* Bounds check before reading line header */
            if (src + 2 > gaf->data + gaf->data_size) break;

            uint16_t line_bytes = *(uint16_t *)src;
            src += 2;

            if (line_bytes == 0) continue;

            /* Bounds check for line data */
            if (src + line_bytes > gaf->data + gaf->data_size) break;

            uint8_t *dst_row = pixels + row * frame->width;

            if (gaf->format == GAF_FORMAT_TAK) {
                decode_scanline_tak(src, line_bytes, dst_row, frame->width);
            } else {
                decode_scanline_ta(src, line_bytes, dst_row, frame->width);
            }

            src += line_bytes;
        }
    }

    return pixels;
}

uint32_t *GAF_DecodeFrameRGBA(GAFFile *gaf, const GAFFrameHeader *frame, const uint32_t *rgba_table) {
    if (!gaf || !frame || !rgba_table) return NULL;

    uint8_t *indices = GAF_DecodeFrame(gaf, frame);
    if (!indices) return NULL;

    size_t total_pixels = (size_t)frame->width * frame->height;
    uint32_t *rgba = (uint32_t *)malloc(total_pixels * sizeof(uint32_t));
    if (!rgba) {
        free(indices);
        return NULL;
    }

    for (size_t i = 0; i < total_pixels; i++) {
        rgba[i] = rgba_table[indices[i]];
    }

    free(indices);
    return rgba;
}
