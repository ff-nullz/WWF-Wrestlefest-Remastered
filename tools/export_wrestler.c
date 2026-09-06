/* Wrestler package exporter — ROM -> data/wrestlers/NN/{palette,poses,stats}.json
 * Invoked as `wfengine --export ID DIR` after the engine has loaded the ROM
 * (the same decoder the renderer uses, so the export is byte-faithful).
 * poses.json: one entry per pose: the decoded sprite command list (own
 * body), plus per-partner command lists for two-man (DA12) poses.
 * stats.json: the per-wrestler ROM tables the engine reads (provenance
 * = the ROM address of every field). */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "wf.h"
#include "engine.h"
#include "stream_decode.h"

#define TAB_META    0x38F14u
#define TAB_STREAM  0x38FB8u
#define ROM_BODY    0x2F22u

static uint16_t r16(uint32_t a) { return (uint16_t)((wf.rom[a] << 8) | wf.rom[a + 1]); }
static uint32_t r32(uint32_t a) { return ((uint32_t)r16(a) << 16) | r16(a + 2); }

static int decode_pose(unsigned row, unsigned pose, int flip, int prow, WfThinkerSpr *spr);
int wf_export_decode_pose(unsigned row, unsigned pose, int flip, int prow, WfThinkerSpr *spr) { return decode_pose(row, pose, flip, prow, spr); }
static int decode_pose(unsigned row, unsigned pose, int flip, int prow, WfThinkerSpr *spr)
{
    unsigned off_tab = r16(TAB_META + row * 2u);
    unsigned pose_off;
    uint32_t base;
    if (!off_tab) return -1;
    pose_off = r16(TAB_META + off_tab + pose * 2u);
    if (pose_off == 0xFFFEu || pose_off == 0xFFFFu) return -1;   /* 0xFFFE = no pose; 0xFFFF = the table terminator (agent D) */
    base = r32(TAB_STREAM + row * 4u);
    if (!base) return -1;
    wf_thinker_set_partner_row(prow);
    return wf_thinker_decode_obj(base, pose_off, flip, 0, 0, (uint16_t)row,
                                 (uint16_t)(pose | (flip ? 0x8000u : 0)), spr, WF_THINKER_MAX_SPR);
}

static void write_recs(FILE *f, const WfThinkerSpr *spr, int n)
{
    fprintf(f, "[");
    for (int i = 0; i < n; i++)
        fprintf(f, "%s{\"tile\":%u,\"x\":%d,\"y\":%d,\"flipx\":%u,\"flipy\":%u,\"chain\":%u,\"pal\":%u}",
                i ? "," : "", spr[i].tile, spr[i].x, spr[i].y, spr[i].flipx, spr[i].flipy, spr[i].chain, spr[i].pal);
    fprintf(f, "]");
}

int tool_export_sheet(unsigned id, const char *dir);
int tool_export_wrestler(unsigned id, const char *dir)
{
    char path[512];
    FILE *f;
    WfThinkerSpr spr[WF_THINKER_MAX_SPR];
    unsigned off_tab, next_tab, npose;
    if (id >= 12) { fprintf(stderr, "export: id %u out of range\n", id); return 1; }
    snprintf(path, sizeof path, "%s/%02u", dir, id);
    mkdir(dir, 0755); mkdir(path, 0755);

    /* palette: 16 pens at ROM_BODY + id*32 (RGB444 words) */
    snprintf(path, sizeof path, "%s/%02u/palette.json", dir, id);
    f = fopen(path, "w"); if (!f) return 1;
    fprintf(f, "{\"rom\":\"0x%X\",\"pens\":[", ROM_BODY + id * 32u);
    for (unsigned p = 0; p < 16; p++) fprintf(f, "%s%u", p ? "," : "", r16(ROM_BODY + id * 32u + p * 2u));
    fprintf(f, "]}\n"); fclose(f);

    /* poses */
    off_tab = r16(TAB_META + id * 2u);
    next_tab = (id + 1 < 81) ? r16(TAB_META + (id + 1) * 2u) : 0;
    npose = (next_tab > off_tab) ? (next_tab - off_tab) / 2u : 0x400u;
    if (npose > 0x400u) npose = 0x400u;
    snprintf(path, sizeof path, "%s/%02u/poses.json", dir, id);
    f = fopen(path, "w"); if (!f) return 1;
    fprintf(f, "{\"row\":%u,\"stream\":\"0x%X\",\"meta\":\"0x%X\",\"poses\":{", id, r32(TAB_STREAM + id * 4u), TAB_META + off_tab);
    {
        int first = 1, exported = 0;
        for (unsigned pose = 0; pose < npose; pose++) {
            int n = decode_pose(id, pose, 0, -1, spr);
            if (n < 0) continue;
            fprintf(f, "%s\n\"%u\":{\"own\":", first ? "" : ",", pose); first = 0;
            write_recs(f, spr, n);
            {   int nf = decode_pose(id, pose, 1, -1, spr);
                fprintf(f, ",\"own_f\":"); write_recs(f, spr, nf < 0 ? 0 : nf); }
            /* two-man art: re-decode with every possible partner row; keep
             * the partner-dependent variants that differ from own-only */
            for (int flip = 0; flip < 2; flip++) {
                int pf = 1;
                fprintf(f, flip ? ",\"with_partner_f\":{" : ",\"with_partner\":{");
                for (unsigned prow = 0; prow < 12; prow++) {
                    WfThinkerSpr sp2[WF_THINKER_MAX_SPR];
                    int n2 = decode_pose(id, pose, flip, (int)prow, sp2);
                    if (n2 <= n) continue;                 /* no partner slice */
                    fprintf(f, "%s\"%u\":", pf ? "" : ",", prow); pf = 0;
                    write_recs(f, sp2, n2);
                }
                fprintf(f, "}");
            }
            fprintf(f, "}");
            exported++;
        }
        fprintf(f, "\n},\"count\":%d}\n", exported);
        fclose(f);
        fprintf(stderr, "export: wrestler %u: %d poses -> %s/%02u\n", id, exported, dir, id);
    }

    tool_export_sheet(id, dir);

    /* stats: the per-wrestler ROM tables */
    snprintf(path, sizeof path, "%s/%02u/stats.json", dir, id);
    f = fopen(path, "w"); if (!f) return 1;
    {
        uint32_t map = r32(0xE4FEu + id * 4u);
        fprintf(f, "{\"id\":%u,\n", id);
        fprintf(f, " \"hp\":{\"rom\":\"0x%X\",\"value\":%u},\n", 0x10830u + id * 2u, r16(0x10830u + id * 2u));
        fprintf(f, " \"walk_speed\":{\"rom\":\"0x%X\",\"value\":%u},\n", 0x116AEu + id, wf.rom[0x116AEu + id]);
        fprintf(f, " \"run_speed\":{\"rom\":\"0x%X\",\"value\":%u},\n", 0x11D4Au + id, wf.rom[0x11D4Au + id]);
        fprintf(f, " \"move_map\":{\"rom\":\"0x%X\",\"rows\":[", map);
        for (unsigned cat = 0; cat < 0x15u; cat++)
            fprintf(f, "%s[%u,%u,%u]", cat ? "," : "", wf.rom[map + cat * 3u], wf.rom[map + cat * 3u + 1], wf.rom[map + cat * 3u + 2]);
        fprintf(f, "]},\n");
        fprintf(f, " \"run_matrix_cat\":{\"rom\":\"0xE070\",\"bank_band\":[");
        for (unsigned k = 0; k < 6; k++) fprintf(f, "%s%u", k ? "," : "", wf.rom[0xE070u + (k / 3u) * 0x24u + (k % 3u) * 0xCu + id]);
        fprintf(f, "]},\n");
        fprintf(f, " \"throw_matrix_cat\":{\"rom\":\"0xE232\",\"band_bank\":[");
        for (unsigned k = 0; k < 6; k++) fprintf(f, "%s%u", k ? "," : "", wf.rom[0xE232u + (k / 2u) * 0x18u + (k % 2u) * 0xCu + id]);
        fprintf(f, "]},\n");
        fprintf(f, " \"drop_move\":{\"rom\":\"0x%X\",\"value\":%u},\n", 0xEFFEu + id, wf.rom[0xEFFEu + id]);
        fprintf(f, " \"pin_intent\":{\"rom\":\"0x%X\",\"value\":%u},\n", 0x110C8u + id, wf.rom[0x110C8u + id]);
        fprintf(f, " \"portrait\":{\"rom\":\"0x%X\",\"value\":%u},\n", 0x7814u + id * 2u, r16(0x7814u + id * 2u));
        fprintf(f, " \"ai_fallback_move\":{\"rom\":\"0x%X\",\"value\":%u},\n", 0x1F150u + id, wf.rom[0x1F150u + id]);
        fprintf(f, " \"ai_policy\":{\"rom\":\"0x%X\",\"contexts\":[", r32(0x219B4u + id * 4u));
        {
            uint32_t base = r32(0x219B4u + id * 4u);
            for (unsigned ctx = 0; ctx < 11; ctx++) {
                uint32_t rec = r32(base + ctx * 4u);
                unsigned n = (rec > 0x219B4u && rec < 0x24060u) ? wf.rom[rec] : 0;
                fprintf(f, "%s{\"rom\":\"0x%X\",\"ids\":[", ctx ? "," : "", rec);
                for (unsigned k = 0; k < n && n < 32; k++) fprintf(f, "%s%u", k ? "," : "", wf.rom[rec + 1 + k]);
                fprintf(f, "],\"w\":[");
                for (unsigned b = 0; b < 3 && n < 32; b++) {
                    fprintf(f, "%s[", b ? "," : "");
                    for (unsigned k = 0; k < n; k++) fprintf(f, "%s%u", k ? "," : "", wf.rom[rec + 1 + n + b * n + k]);
                    fprintf(f, "]");
                }
                fprintf(f, "]}");
            }
        }
        fprintf(f, "]}\n}\n");
        fclose(f);
    }
    return 0;
}

/* ---- per-wrestler tile sheet: the tiles this wrestler's own records use
 * (pal == id), chains expanded, as an editable indexed PNG + tiles.json. */
#include <png.h>
#include <stdlib.h>
const uint8_t *wf_video_tile_pens(unsigned t);
static uint16_t sheet_tiles[8192]; static int sheet_n;
static void sheet_add(unsigned t)
{
    if (sheet_n >= 8192) return;
    for (int i = 0; i < sheet_n; i++) if (sheet_tiles[i] == t) return;
    sheet_tiles[sheet_n++] = (uint16_t)t;
}
static void sheet_collect(const WfThinkerSpr *spr, int n, unsigned id)
{
    for (int i = 0; i < n; i++) {
        unsigned chain = spr[i].chain, base = spr[i].tile;
        if ((spr[i].pal & 0x0Fu) != id) continue;          /* partner slice */
        for (unsigned k = 0; k <= chain; k++) sheet_add((base + k) & 0xFFFFu);
    }
}
static int cmp_u16(const void *a, const void *b)
{ return (int)*(const uint16_t *)a - (int)*(const uint16_t *)b; }

int tool_export_sheet(unsigned id, const char *dir)
{
    char path[512];
    WfThinkerSpr spr[WF_THINKER_MAX_SPR];
    unsigned off_tab = r16(TAB_META + id * 2u);
    unsigned next_tab = (id + 1 < 81) ? r16(TAB_META + (id + 1) * 2u) : 0;
    unsigned npose = (next_tab > off_tab) ? (next_tab - off_tab) / 2u : 0x400u;
    if (npose > 0x400u) npose = 0x400u;
    sheet_n = 0;
    for (unsigned pose = 0; pose < npose; pose++)
        for (int flip = 0; flip < 2; flip++) {
            int n = decode_pose(id, pose, flip, -1, spr);
            if (n > 0) sheet_collect(spr, n, id);
            for (unsigned prow = 0; prow < 12; prow++) {
                int n2 = decode_pose(id, pose, flip, (int)prow, spr);
                if (n2 > 0) sheet_collect(spr, n2, id);
            }
        }
    qsort(sheet_tiles, (size_t)sheet_n, sizeof(uint16_t), cmp_u16);
    {   /* tiles.json */
        FILE *f;
        snprintf(path, sizeof path, "%s/%02u/tiles.json", dir, id);
        f = fopen(path, "w"); if (!f) return 1;
        fprintf(f, "{\"count\":%d,\"tiles\":[", sheet_n);
        for (int i = 0; i < sheet_n; i++) fprintf(f, "%s%u", i ? "," : "", sheet_tiles[i]);
        fprintf(f, "]}\n"); fclose(f);
    }
    {   /* sheet.png: 16 tiles per row, 8-bit indexed, wrestler palette */
        int cols = 16, rows = (sheet_n + cols - 1) / cols;
        int W = cols * 16, H = rows * 16;
        FILE *f; png_structp png; png_infop info; png_color pal16[16];
        uint8_t *img = calloc((size_t)W * H, 1); png_bytep *rp = malloc(sizeof(png_bytep) * (size_t)H);
        if (!img || !rp) return 1;
        for (int i = 0; i < sheet_n; i++) {
            const uint8_t *pens = wf_video_tile_pens(sheet_tiles[i]);
            int ox = (i % cols) * 16, oy = (i / cols) * 16;
            for (int y = 0; y < 16; y++)
                memcpy(img + (size_t)(oy + y) * W + ox, pens + y * 16, 16);
        }
        for (int p = 0; p < 16; p++) {
            uint16_t w = r16(ROM_BODY + id * 32u + (unsigned)p * 2u);
            pal16[p].red = (png_byte)((w & 0x0F) * 17);
            pal16[p].green = (png_byte)(((w >> 4) & 0x0F) * 17);
            pal16[p].blue = (png_byte)(((w >> 8) & 0x0F) * 17);
        }
        snprintf(path, sizeof path, "%s/%02u/sheet.png", dir, id);
        f = fopen(path, "wb"); if (!f) return 1;
        png = png_create_write_struct(PNG_LIBPNG_VER_STRING, 0, 0, 0);
        info = png_create_info_struct(png);
        if (setjmp(png_jmpbuf(png))) { fclose(f); return 1; }
        png_init_io(png, f);
        png_set_IHDR(png, info, (png_uint_32)W, (png_uint_32)H, 8, PNG_COLOR_TYPE_PALETTE,
                     PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
        png_set_PLTE(png, info, pal16, 16);
        for (int y = 0; y < H; y++) rp[y] = img + (size_t)y * W;
        png_set_rows(png, info, rp);
        png_write_png(png, info, PNG_TRANSFORM_IDENTITY, 0);
        png_destroy_write_struct(&png, &info);
        fclose(f); free(img); free(rp);
        fprintf(stderr, "export: wrestler %u sheet: %d tiles (%dx%d)\n", id, sheet_n, W, H);
    }
    return 0;
}
