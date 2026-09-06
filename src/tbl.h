/* Named game-data tables — the data layer of docs/adr-001-data-formats.md.
 *
 * Every byte the engine used to read from wf.rom[] is a named table: a
 * byte blob with a kind (how the JSON shows it), a row stride and a ROM
 * provenance. Each source file declares the tables it owns with a static
 * tbl_def[] + TBL_REGISTER() and reads them through tbl8/tbl16/tbl32 (big-
 * endian, exactly like the old rom16()/r32() helpers, so a migrated read
 * site is byte-for-byte the same computation).
 *
 * Backends (tbl_load_*): "rom" copies each table out of wf.rom after the
 * program ROM is loaded (transition only); "pak" binds each table to a
 * section of build/base.pak (the only runtime backend after the cut-over).
 * The exporter (tools/export_tables.c) writes data/<group>/<name>.json and
 * verifies the round trip; the packer (tools/pack.c) builds the pak from
 * the JSON tree. Out-of-range reads return 0 and are reported once under
 * WF_DBGSEL — a table whose declared length is too short shows up there. */
#ifndef WF_TBL_H
#define WF_TBL_H
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

enum tbl_kind { TK_U8 = 0, TK_S8, TK_U16, TK_S16, TK_U32, TK_S32 };

typedef struct tbl_def {
    const char *name;       /* file stem, snake_case, no hex: "mash_overlay_rows" */
    const char *group;      /* directory under data/: "base/hud", "base/ai", ... */
    uint32_t rom_addr;      /* provenance: first byte in the 68k ROM image */
    uint32_t len;           /* bytes */
    uint8_t  kind;          /* enum tbl_kind — how the JSON rows are shown */
    uint16_t stride;        /* elements per JSON row (0 = one row) */
    const char *desc;       /* one line: meaning + the PCs that read it */
    const void *defaults;   /* SYNTHETIC tables only (rom_addr == TBL_SYNTH): the
                               compiled-in bytes, table byte order (big-endian
                               elements) — engine scalars that were immediates in
                               the 68k code, exported/packed like any table so a
                               mod can change them */
    const char *const *labels; /* optional row labels (rules): one per JSON row, NULL-terminated list */
} tbl_def;
#define TBL_SYNTH 0xFFFFFFFFu
#define TBL_BE16(v) (uint8_t)((unsigned)(v) >> 8), (uint8_t)(v)   /* defaults[] element */

void tbl_register(const tbl_def *defs, int n);
#define TBL_CAT_(a, b) a##b
#define TBL_CAT(a, b) TBL_CAT_(a, b)
#define TBL_REGISTER(arr) \
    static void __attribute__((constructor)) TBL_CAT(tbl_reg_, __LINE__)(void) \
    { tbl_register(arr, (int)(sizeof(arr) / sizeof((arr)[0]))); }

int            tbl_id(const char *name);          /* -1 if unknown */
int            tbl_count(void);
const tbl_def *tbl_def_at(int id);
const uint8_t *tbl_bytes(int id, uint32_t *len);  /* NULL if not loaded */

/* Call-site handle: resolved once per site. */
#define TBL(n) (__extension__({ static int tbl_id_ = -2; \
                 if (tbl_id_ == -2) tbl_id_ = tbl_id(#n); \
                 tbl_id_; }))

unsigned tbl8(int id, uint32_t off);
unsigned tbl16(int id, uint32_t off);             /* big-endian, like rom16() */
uint32_t tbl32(int id, uint32_t off);
int      tbl_s8(int id, uint32_t off);
int      tbl_s16(int id, uint32_t off);

/* ROM-address-space reads: resolve a 68k ROM address to the registered
 * table that covers it (tables are disjoint ranges, checked at register
 * time). For pointer-chased data — glyph records, streams, scene maps —
 * whose pointers stay ROM addresses in the JSON. An address no table covers
 * reads 0 and is reported once: that report IS the migration inventory. */
unsigned tbl_ra8_at(uint32_t rom_addr, const char *site);
unsigned tbl_ra16_at(uint32_t rom_addr, const char *site);
uint32_t tbl_ra32_at(uint32_t rom_addr, const char *site);
const uint8_t *tbl_ra_ptr_at(uint32_t rom_addr, uint32_t n, const char *site);   /* n bytes, or NULL */
#define TBL_STR_(x) #x
#define TBL_STR(x) TBL_STR_(x)
#define TBL_SITE __FILE__ ":" TBL_STR(__LINE__)
#define tbl_ra8(a)        tbl_ra8_at((a), TBL_SITE)        /* the report names the read site */
#define tbl_ra16(a)       tbl_ra16_at((a), TBL_SITE)
#define tbl_ra32(a)       tbl_ra32_at((a), TBL_SITE)
#define tbl_ra_ptr(a, n)  tbl_ra_ptr_at((a), (n), TBL_SITE)
int      tbl_ra_id(uint32_t rom_addr, uint32_t *off);       /* table id or -1 */

/* Backends. tbl_load_rom needs wf.rom filled; tbl_load_pak reads the pak
 * (every registered table must be present). tbl_backend() = "rom"/"pak". */
extern int tbl_transition_rom;   /* 1 while the program ROM is loaded at runtime (transition) */
int  tbl_load_rom(void);
int  tbl_load_pak(const char *path);
int  tbl_pak_matches(const char *path);   /* 1 = every declared table present at its size */
const char *tbl_backend(void);
void tbl_set_bytes(int id, const uint8_t *p, uint32_t len);   /* packer / tests */

/* Tools (tools/export_tables.c, tools/pack.c). */
int tbl_export_json(const char *data_dir);        /* wfengine --export-all */
int tbl_export_stock(const char *path);           /* wfengine --export-stock: ONE pristine-values JSON (editor stock reference) */
int tbl_pack(const char *data_dir, const char *pak_path);   /* wfengine --pack */
int tbl_list(void);                               /* wfengine --tables */
void tbl_write_json(FILE *f, const tbl_def *d, const uint8_t *b, uint32_t len);   /* one table file (editor saves) */
void tbl_write_json_sparse(FILE *f, const tbl_def *d, const uint8_t *b, uint32_t len, const uint8_t *base, uint32_t base_len);   /* rules layer: only the rows that differ from base, as {label: value} */
int  tbl_sparsify_rules(const char *mod);   /* wfengine --sparsify MOD: a mod's full rules layers -> sparse overrides */
uint8_t *tbl_merge_layers(const char *data_dir, const tbl_def *d, int nlayers, uint32_t *out_len,
                          int *nmod, int *stock_modified, char *why, size_t whylen);   /* stock + the first nlayers mods (-1 = all) */
uint8_t *tbl_json_to_bytes_file(const char *path, const tbl_def *d, uint32_t *len, char *why, size_t whylen);   /* read a table file */
int      tbl_json_byte(const char *data_dir, const char *name, uint32_t off);   /* one byte via the mod-resolved JSON, -1 on failure */

uint32_t wf_crc32(uint32_t crc, const void *p, size_t n);
const char *tbl_kind_name(int k);
int tbl_kind_size(int k);
#endif
