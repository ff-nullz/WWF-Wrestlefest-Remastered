/* Named game-data tables: registry, accessors, ROM/pak backends. See tbl.h
 * and docs/adr-001-data-formats.md. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wf.h"
#include "tbl.h"
#include "pak.h"

extern int eng_dbgsel;

#define TBL_MAX 1024

typedef struct {
    const tbl_def *def;
    const uint8_t *bytes;       /* bound storage (owned: own_buf, or a pak section) */
    uint8_t *own_buf;
    uint32_t len;
    int oob_reported;
} tbl_slot;

static tbl_slot tabs[TBL_MAX];
static int n_tabs;
static const char *backend = "none";
int tbl_transition_rom;          /* set by render.c once wf.rom is loaded (until the cut-over) */

void tbl_register(const tbl_def *defs, int n)
{
    for (int i = 0; i < n; i++) {
        if (n_tabs >= TBL_MAX) { fprintf(stderr, "tbl: registry full\n"); abort(); }
        for (int j = 0; j < n_tabs; j++)
            if (!strcmp(tabs[j].def->name, defs[i].name)) {
                fprintf(stderr, "tbl: duplicate table name '%s'\n", defs[i].name);
                abort();
            }
        tabs[n_tabs].def = &defs[i];
        tabs[n_tabs].bytes = NULL;
        tabs[n_tabs].len = 0;
        n_tabs++;
    }
}

int tbl_id(const char *name)
{
    for (int j = 0; j < n_tabs; j++)
        if (!strcmp(tabs[j].def->name, name)) return j;
    if (eng_dbgsel) fprintf(stderr, "tbl: unknown table '%s'\n", name);
    return -1;
}
int tbl_count(void) { return n_tabs; }
const tbl_def *tbl_def_at(int id) { return (id >= 0 && id < n_tabs) ? tabs[id].def : NULL; }
const uint8_t *tbl_bytes(int id, uint32_t *len)
{
    if (id < 0 || id >= n_tabs || !tabs[id].bytes) { if (len) *len = 0; return NULL; }
    if (len) *len = tabs[id].len;
    return tabs[id].bytes;
}
const char *tbl_backend(void) { return backend; }

static const uint8_t *at(int id, uint32_t off, uint32_t n)
{
    tbl_slot *t;
    if (id < 0 || id >= n_tabs) return NULL;
    t = &tabs[id];
    if (!t->bytes || off + n > t->len) {
        if (!t->oob_reported) {
            t->oob_reported = 1;
            fprintf(stderr, "tbl: %s read %s at +0x%X (%u bytes) outside [0,0x%X)\n",
                    t->bytes ? "OUT-OF-RANGE" : "UNLOADED", t->def->name, off, n, t->len);
        }
        return NULL;
    }
    return t->bytes + off;
}
unsigned tbl8(int id, uint32_t off)  { const uint8_t *p = at(id, off, 1); return p ? p[0] : 0; }
unsigned tbl16(int id, uint32_t off) { const uint8_t *p = at(id, off, 2); return p ? ((unsigned)p[0] << 8) | p[1] : 0; }
uint32_t tbl32(int id, uint32_t off)
{
    const uint8_t *p = at(id, off, 4);
    return p ? ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3] : 0;
}
int tbl_s8(int id, uint32_t off)  { return (int8_t)tbl8(id, off); }
int tbl_s16(int id, uint32_t off) { return (int16_t)tbl16(id, off); }

/* ---- ROM address space ---- */
static int *ra_order;            /* table ids sorted by rom_addr */
static int ra_n = 0, ra_built_for = -1;
static int ra_cmp(const void *a, const void *b)
{
    uint32_t x = tabs[*(const int *)a].def->rom_addr, y = tabs[*(const int *)b].def->rom_addr;
    return x < y ? -1 : x > y;
}
static void ra_build(void)
{
    free(ra_order);
    ra_order = malloc(sizeof(int) * (size_t)(n_tabs ? n_tabs : 1));
    ra_n = 0;
    for (int i = 0; i < n_tabs; i++) if (tabs[i].def->rom_addr != TBL_SYNTH) ra_order[ra_n++] = i;
    qsort(ra_order, (size_t)ra_n, sizeof(int), ra_cmp);
    for (int i = 1; i < ra_n; i++) {
        const tbl_def *a = tabs[ra_order[i-1]].def, *b = tabs[ra_order[i]].def;
        if (a->len && b->len && a->rom_addr + a->len > b->rom_addr) {
            fprintf(stderr, "tbl: tables overlap: %s [0x%X,0x%X) and %s [0x%X,0x%X)\n",
                    a->name, a->rom_addr, a->rom_addr + a->len, b->name, b->rom_addr, b->rom_addr + b->len);
            abort();
        }
    }
    ra_built_for = n_tabs;
}
int tbl_ra_id(uint32_t addr, uint32_t *off)
{
    int lo = 0, hi;
    if (ra_built_for != n_tabs) ra_build();
    hi = ra_n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        const tbl_def *d = tabs[ra_order[mid]].def;
        if (addr < d->rom_addr) hi = mid - 1;
        else if (addr >= d->rom_addr + d->len) lo = mid + 1;
        else { if (off) *off = addr - d->rom_addr; return ra_order[mid]; }
    }
    return -1;
}
static struct { uint32_t addr; int n; } ra_miss[512];
static int ra_miss_n;
/* WF_TBL_RECORD=path: transition aid — an unmapped read falls back to
 * wf.rom and the touched bytes are recorded; at exit the covered ranges are
 * written to `path` (one "0xSTART 0xEND" per line) so tables can be
 * declared from real coverage. Goes away at the cut-over. */
static uint8_t *rec_map; static const char *rec_path; static int rec_on = -1;
static void rec_dump(void)
{
    FILE *f = fopen(rec_path, "w");
    if (!f) return;
    for (uint32_t a = 0; a < WF_ROM_SIZE; ) {
        if (!rec_map[a]) { a++; continue; }
        uint32_t b = a;
        while (b < WF_ROM_SIZE && rec_map[b]) b++;
        fprintf(f, "0x%05X 0x%05X %u\n", a, b, b - a);
        a = b;
    }
    fclose(f);
}
const uint8_t *tbl_ra_ptr_at(uint32_t addr, uint32_t n, const char *site)
{
    uint32_t off;
    int id = tbl_ra_id(addr, &off);
    if (id >= 0) return at(id, off, n);
    /* Unmapped: report once per 256-byte page (this IS the migration
     * inventory). TRANSITION ONLY: with the ROM backend the read still
     * comes from wf.rom (recorded when WF_TBL_RECORD is set) so the game
     * keeps running while tables are being declared; the pak backend has
     * no fallback — after the cut-over neither has. */
    {
        int k;
        for (k = 0; k < ra_miss_n; k++) if (ra_miss[k].addr == (addr & ~0xFFu)) { ra_miss[k].n++; break; }
        if (k == ra_miss_n && ra_miss_n < 512) {
            ra_miss[ra_miss_n].addr = addr & ~0xFFu; ra_miss[ra_miss_n].n = 1; ra_miss_n++;
            fprintf(stderr, "tbl: UNMAPPED ROM read at 0x%05X (%u bytes) from %s — no table covers it\n", addr, n, site);
        }
    }
    if (rec_on < 0) {
        rec_path = getenv("WF_TBL_RECORD");
        rec_on = rec_path != NULL;
        if (rec_on) { rec_map = calloc(WF_ROM_SIZE, 1); atexit(rec_dump); }
    }
    if (tbl_transition_rom && addr + n <= WF_ROM_SIZE) {   /* wf.rom is loaded: transition fallback */
        if (rec_on) for (uint32_t i = 0; i < n; i++) rec_map[addr + i] = 1;
        return wf.rom + addr;
    }
    return NULL;
}
unsigned tbl_ra8_at(uint32_t a, const char *s)  { const uint8_t *p = tbl_ra_ptr_at(a, 1, s); return p ? p[0] : 0; }
unsigned tbl_ra16_at(uint32_t a, const char *s) { const uint8_t *p = tbl_ra_ptr_at(a, 2, s); return p ? ((unsigned)p[0] << 8) | p[1] : 0; }
uint32_t tbl_ra32_at(uint32_t a, const char *s)
{
    const uint8_t *p = tbl_ra_ptr_at(a, 4, s);
    return p ? ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3] : 0;
}

void tbl_set_bytes(int id, const uint8_t *p, uint32_t len)
{
    if (id < 0 || id >= n_tabs) return;
    free(tabs[id].own_buf);
    tabs[id].own_buf = malloc(len ? len : 1);
    memcpy(tabs[id].own_buf, p, len);
    tabs[id].bytes = tabs[id].own_buf;
    tabs[id].len = len;
}

/* ---- backends ---- */
int tbl_load_rom(void)
{
    for (int j = 0; j < n_tabs; j++) {
        const tbl_def *d = tabs[j].def;
        if (d->rom_addr == TBL_SYNTH) {           /* engine-owned: compiled defaults */
            if (!d->defaults) { fprintf(stderr, "tbl: synthetic %s has no defaults\n", d->name); return -1; }
            tbl_set_bytes(j, d->defaults, d->len);
            continue;
        }
        if ((uint64_t)d->rom_addr + d->len > WF_ROM_SIZE) {
            fprintf(stderr, "tbl: %s ROM range 0x%X+0x%X exceeds the image\n", d->name, d->rom_addr, d->len);
            return -1;
        }
        tbl_set_bytes(j, wf.rom + d->rom_addr, d->len);
    }
    backend = "rom";
    return 0;
}

int tbl_load_pak(const char *path)
{
    pak *p = pak_open(path);
    int missing = 0;
    if (!p) return -1;
    for (int j = 0; j < n_tabs; j++) {
        uint32_t len;
        const uint8_t *b = pak_section(p, tabs[j].def->name, &len);
        if (!b) { fprintf(stderr, "tbl: pak lacks table '%s'\n", tabs[j].def->name); missing++; continue; }
        if (len != tabs[j].def->len)
            fprintf(stderr, "tbl: pak table '%s' is %u bytes, engine expects %u\n",
                    tabs[j].def->name, len, tabs[j].def->len);
        tabs[j].bytes = b;          /* pak memory stays mapped for the process lifetime */
        tabs[j].len = len;
    }
    if (missing) return -1;
    backend = "pak";
    return 0;
}

/* 1 = the pak carries every table the engine declares, at the declared
 * size (a pak packed before a table was added or grew is STALE even when
 * no source file changed - the engine itself did) */
int tbl_pak_matches(const char *path)
{
    pak *p = pak_open(path);
    int ok = 1;
    if (!p) return 0;
    for (int j = 0; j < n_tabs && ok; j++) {
        uint32_t len;
        const uint8_t *b = pak_section(p, tabs[j].def->name, &len);
        if (!b || len != tabs[j].def->len) ok = 0;
    }
    pak_close(p);
    return ok;
}

/* ---- misc ---- */
static const char *kind_names[] = { "u8", "s8", "u16", "s16", "u32", "s32" };
const char *tbl_kind_name(int k) { return (k >= 0 && k < 6) ? kind_names[k] : "?"; }
int tbl_kind_size(int k) { return k < 2 ? 1 : k < 4 ? 2 : 4; }

uint32_t wf_crc32(uint32_t crc, const void *buf, size_t n)
{
    static uint32_t tab[256]; static int init;
    const uint8_t *p = buf;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            tab[i] = c;
        }
        init = 1;
    }
    crc = ~crc;
    while (n--) crc = tab[(crc ^ *p++) & 0xFFu] ^ (crc >> 8);
    return ~crc;
}

int tbl_list(void)
{
    uint32_t total = 0;
    for (int j = 0; j < n_tabs; j++) {
        const tbl_def *d = tabs[j].def;
        printf("%-32s %-16s 0x%05X +0x%04X %-3s x%-3u %s\n", d->name, d->group, d->rom_addr,
               d->len, tbl_kind_name(d->kind), d->stride, d->desc ? d->desc : "");
        total += d->len;
    }
    printf("%d tables, %u bytes, backend=%s\n", n_tabs, total, backend);
    return 0;
}
