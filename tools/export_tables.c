/* Table exporter + packer — `wfengine --export-all DIR` and
 * `wfengine --pack DIR PAK` (docs/adr-001-data-formats.md).
 *
 * Export: for every registered table (tbl.h registry, bytes already bound
 * from wf.rom) write DIR/<group>/<name>.json:
 *   { "name", "group", "kind", "stride", "desc",
 *     "prov": { "rom": "0x....", "len", "crc32", "exporter", "verified": true },
 *     "rows": [[e, e, ...], ...] }          // big-endian elements by kind
 * then re-read the file through json.c, re-encode, and compare with the
 * ROM bytes — a mismatch deletes the file and fails the run (ADR rule 2).
 * DIR/manifest.json lists every file with its crc32.
 *
 * Pack: for every registered table read the JSON (mods/<name>/ layers from
 * mods/order.txt first, then DIR), encode, add as a pak section named after
 * the table; pak_writer_save re-opens and verifies. A table whose bytes no
 * longer match the export-time crc is reported as "modified" (a mod), not
 * an error. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>
#include "wf.h"
#include "tbl.h"
#include "pak.h"
#include "json.h"
#include "profile.h"
#include "version.h"

static int mkdir_p(const char *path)
{
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') { *p = 0; if (mkdir(tmp, 0775) && errno != EEXIST) return -1; *p = '/'; }
    return (mkdir(tmp, 0775) && errno != EEXIST) ? -1 : 0;
}

static int64_t elem_at(const uint8_t *b, int kind, uint32_t i)
{
    switch (kind) {
    case TK_U8:  return b[i];
    case TK_S8:  return (int8_t)b[i];
    case TK_U16: return (b[i*2] << 8) | b[i*2+1];
    case TK_S16: return (int16_t)((b[i*2] << 8) | b[i*2+1]);
    case TK_U32: return ((uint32_t)b[i*4] << 24) | ((uint32_t)b[i*4+1] << 16) | (b[i*4+2] << 8) | b[i*4+3];
    default:     return (int32_t)(((uint32_t)b[i*4] << 24) | ((uint32_t)b[i*4+1] << 16) | (b[i*4+2] << 8) | b[i*4+3]);
    }
}
static void elem_put(uint8_t *b, int kind, uint32_t i, int64_t v)
{
    switch (tbl_kind_size(kind)) {
    case 1: b[i] = (uint8_t)v; break;
    case 2: b[i*2] = (uint8_t)(v >> 8); b[i*2+1] = (uint8_t)v; break;
    default: b[i*4] = (uint8_t)(v >> 24); b[i*4+1] = (uint8_t)(v >> 16); b[i*4+2] = (uint8_t)(v >> 8); b[i*4+3] = (uint8_t)v;
    }
}
static int kind_from_name(const char *s)
{
    for (int k = 0; k < 6; k++) if (!strcmp(s, tbl_kind_name(k))) return k;
    return -1;
}

/* Rules tables carry row labels; a SPARSE mod layer names the rows it sets:
 *     { "name": "game_rules", "group": "rules", "kind": "u16",
 *       "overrides": { "turbo": 3, "parasitic_pct": 100 } }
 * and the packer merges every active mod's layer IN PROFILE ORDER on top of
 * the stock table (a full-rows layer replaces the table, an overrides
 * layer patches it), so rule mods STACK (user 2026-09-05: parasitic +
 * turbo in one profile). `wfengine --sparsify MOD` rewrites a mod's full
 * rules layers into this form (rows that differ from the engine defaults). */
static int label_index(const tbl_def *d, const char *label)
{
    if (!d->labels) return -1;
    for (int i = 0; d->labels[i]; i++) if (!strcmp(d->labels[i], label)) return i;
    return -1;
}

/* apply doc's "overrides" onto b (len bytes); 0 = ok */
static int json_apply_overrides(const json_val *doc, const tbl_def *d, uint8_t *b, uint32_t len, char *why, size_t whylen)
{
    const json_val *ov = json_get(doc, "overrides");
    int ksz = tbl_kind_size(d->kind);
    if (!ov) return 0;
    if (ov->type != JSON_OBJECT) { snprintf(why, whylen, "overrides is not an object"); return 1; }
    if (!d->labels || (d->stride > 1)) { snprintf(why, whylen, "%s has no row labels: overrides need a rules table", d->name); return 1; }
    for (const json_val *m = ov->child; m; m = m->next) {
        int i = m->key ? label_index(d, m->key) : -1;
        if (i < 0) { snprintf(why, whylen, "unknown rule '%s'", m->key ? m->key : "?"); return 1; }
        if ((uint32_t)(i + 1) * (uint32_t)ksz > len) { snprintf(why, whylen, "rule '%s' is past the table (%u bytes)", m->key, len); return 1; }
        if (m->type != JSON_NUMBER && m->type != JSON_STRING) { snprintf(why, whylen, "rule '%s': not a number", m->key); return 1; }
        elem_put(b, d->kind, (uint32_t)i, json_int(m, 0));
    }
    return 0;
}

/* write a rules table as a sparse layer: the rows that differ from `base`
 * (the engine defaults) as {label: value} */
void tbl_write_json_sparse(FILE *f, const tbl_def *d, const uint8_t *b, uint32_t len, const uint8_t *base, uint32_t base_len)
{
    int ksz = tbl_kind_size(d->kind), first = 1;
    fprintf(f, "{\n  \"name\": "); json_write_string(f, d->name);
    fprintf(f, ",\n  \"group\": "); json_write_string(f, d->group);
    fprintf(f, ",\n  \"kind\": \"%s\",\n  \"desc\": \"sparse mod layer of %s: only the rules it sets, merged over the stock table in profile order (wfengine --sparsify)\",\n  \"overrides\": {", tbl_kind_name(d->kind), d->name);
    for (uint32_t i = 0; d->labels && d->labels[i] && (i + 1) * (uint32_t)ksz <= len; i++) {
        int64_t v = elem_at(b, d->kind, i);
        int64_t s = (i + 1) * (uint32_t)ksz <= base_len ? elem_at(base, d->kind, i) : v;
        if (v == s) continue;
        fprintf(f, "%s\n    ", first ? "" : ","); json_write_string(f, d->labels[i]); fprintf(f, ": %lld", (long long)v);
        first = 0;
    }
    fprintf(f, "%s}\n}\n", first ? "" : "\n  ");
}

/* wfengine --sparsify MOD: every rules layer under mods/MOD/tables/ becomes
 * a sparse layer (rows that differ from the engine defaults). 0 = ok. */
int tbl_sparsify_rules(const char *mod)
{
    int n = tbl_count(), done = 0, fails = 0;
    char path[1024], why[128];
    for (int i = 0; i < n; i++) {
        const tbl_def *d = tbl_def_at(i);
        uint8_t *b; uint32_t len; FILE *f; int diff = 0;
        if (d->rom_addr != TBL_SYNTH || !d->defaults || !d->labels) continue;
        snprintf(path, sizeof path, "mods/%s/tables/%s/%s.json", mod, d->group, d->name);
        if (access(path, R_OK)) continue;
        b = tbl_json_to_bytes_file(path, d, &len, why, sizeof why);
        if (!b) { fprintf(stderr, "sparsify: %s: %s\n", path, why); fails++; continue; }
        {   /* the base: with a profile active (--profile X --sparsify MOD), the
             * table the mods BENEATH MOD produce; otherwise the engine defaults */
            const uint8_t *base = (const uint8_t *)d->defaults; uint32_t blen = d->len; uint8_t *mb = NULL;
            int below = -1;
            for (int k = 0; k < wf_profile_nmods(); k++) if (!strcmp(wf_profile_mod(k), mod)) { below = k; break; }
            if (below >= 0) { int nm, sm; char w2[256]; uint32_t ml, el; uint8_t *eb;
                mb = tbl_merge_layers("data/tables", d, below, &ml, &nm, &sm, w2, sizeof w2);
                eb = tbl_merge_layers("data/tables", d, below + 1, &el, &nm, &sm, w2, sizeof w2);   /* the EFFECTIVE table
                                          through MOD (a sparse layer only patches, so its own file is not the table) */
                if (mb && eb) { base = mb; blen = ml; free(b); b = eb; len = el; } else free(eb); }
            for (uint32_t k = 0; k < len && k < blen; k++) if (b[k] != base[k]) diff++;
            if (!diff) { fprintf(stderr, "sparsify: %s sets nothing - removed\n", path); remove(path); free(b); free(mb); done++; continue; }
            f = fopen(path, "w");
            if (!f) { fprintf(stderr, "sparsify: cannot write %s\n", path); fails++; free(b); free(mb); continue; }
            tbl_write_json_sparse(f, d, b, len, base, blen);
            fclose(f); free(b); free(mb); done++;
        }
        fprintf(stderr, "sparsify: %s -> overrides\n", path);
    }
    fprintf(stderr, "sparsify: %s: %d layers rewritten, %d failed\n", mod, done, fails);
    return fails ? 1 : 0;
}

/* Decode a table JSON into bytes. Returns malloc'd buffer (len out) or NULL. */
static uint8_t *json_to_bytes(const json_val *doc, const tbl_def *d, uint32_t *out_len, char *why, size_t whylen)
{
    const json_val *rows = json_get(doc, "rows");
    const char *kname = json_str(json_get(doc, "kind"), "");
    int kind = kind_from_name(kname), ksz;
    uint32_t n = 0, cap;
    uint8_t *buf;
    if (strcmp(json_str(json_get(doc, "name"), ""), d->name)) { snprintf(why, whylen, "name mismatch"); return NULL; }
    if (kind < 0) { snprintf(why, whylen, "bad kind '%s'", kname); return NULL; }
    if (kind != d->kind) { snprintf(why, whylen, "kind %s, engine expects %s", kname, tbl_kind_name(d->kind)); return NULL; }
    ksz = tbl_kind_size(kind);
    if (!rows || rows->type != JSON_ARRAY) {
        /* a SPARSE layer: no rows, only "overrides" {label: value} on top of
         * the engine defaults (rules tables) */
        if (json_get(doc, "overrides") && d->rom_addr == TBL_SYNTH && d->defaults && d->len) {
            buf = malloc(d->len);
            memcpy(buf, d->defaults, d->len);
            if (json_apply_overrides(doc, d, buf, d->len, why, whylen)) { free(buf); return NULL; }
            *out_len = d->len;
            return buf;
        }
        snprintf(why, whylen, "no rows"); return NULL;
    }
    cap = d->len ? d->len : 16;
    buf = malloc(cap);
    for (const json_val *r = rows->child; r; r = r->next) {
        const json_val *e = (r->type == JSON_ARRAY) ? r->child : r;
        int single = r->type != JSON_ARRAY;
        for (; e; e = single ? NULL : e->next) {
            if (e->type != JSON_NUMBER && e->type != JSON_STRING) { free(buf); snprintf(why, whylen, "non-numeric element"); return NULL; }
            if ((n + 1) * (uint32_t)ksz > cap) { cap *= 2; buf = realloc(buf, cap); }
            elem_put(buf, kind, n, json_int(e, 0));
            n++;
        }
    }
    *out_len = n * (uint32_t)ksz;
    if (json_get(doc, "overrides")) {          /* rows AND overrides: the overrides win */
        if (*out_len < d->len && d->rom_addr == TBL_SYNTH && d->defaults) {
            buf = realloc(buf, d->len); memcpy(buf + *out_len, (const uint8_t *)d->defaults + *out_len, d->len - *out_len); *out_len = d->len;
        }
        if (json_apply_overrides(doc, d, buf, *out_len, why, whylen)) { free(buf); return NULL; }
    }
    return buf;
}

void tbl_write_json(FILE *f, const tbl_def *d, const uint8_t *b, uint32_t len)
{
    int ksz = tbl_kind_size(d->kind);
    uint32_t ne = len / (uint32_t)ksz, stride = d->stride ? d->stride : ne;
    fprintf(f, "{\n  \"name\": "); json_write_string(f, d->name);
    fprintf(f, ",\n  \"group\": "); json_write_string(f, d->group);
    fprintf(f, ",\n  \"kind\": \"%s\",\n  \"stride\": %u,\n  \"desc\": ", tbl_kind_name(d->kind), d->stride);
    json_write_string(f, d->desc ? d->desc : "");
    if (d->rom_addr == TBL_SYNTH)
        fprintf(f, ",\n  \"prov\": { \"rom\": \"engine\", \"len\": %u, \"crc32\": \"%08x\", \"verified\": true },\n",
                len, wf_crc32(0, b, len));
    else
        fprintf(f, ",\n  \"prov\": { \"rom\": \"0x%05X\", \"len\": %u, \"crc32\": \"%08x\", \"verified\": true },\n",
                d->rom_addr, len, wf_crc32(0, b, len));   /* exporter build: manifest.json only (no per-file churn) */
    if (d->labels) {
        fprintf(f, "  \"labels\": [");
        for (int i = 0; d->labels[i]; i++) { fprintf(f, "%s", i ? ", " : ""); json_write_string(f, d->labels[i]); }
        fprintf(f, "],\n");
    }
    fprintf(f, "  \"rows\": [\n");
    for (uint32_t i = 0; i < ne; i += stride) {
        fprintf(f, "    [");
        for (uint32_t j = i; j < i + stride && j < ne; j++) {
            if (d->kind == TK_U32) fprintf(f, "%s\"0x%08llX\"", j == i ? "" : ", ", (unsigned long long)(uint32_t)elem_at(b, d->kind, j));
            else fprintf(f, "%s%lld", j == i ? "" : ", ", (long long)elem_at(b, d->kind, j));
        }
        fprintf(f, "]%s\n", i + stride < ne ? "," : "");
    }
    fprintf(f, "  ]\n}\n");
}

int tbl_export_json(const char *data_dir)
{
    int n = tbl_count(), fails = 0;
    char path[1024], err[256], why[128];
    FILE *mf;
    snprintf(path, sizeof path, "%s", data_dir);
    if (mkdir_p(path)) { fprintf(stderr, "export: cannot create %s\n", path); return 1; }
    snprintf(path, sizeof path, "%s/manifest.json", data_dir);
    mf = fopen(path, "w");
    if (!mf) { fprintf(stderr, "export: cannot write %s\n", path); return 1; }
    fprintf(mf, "{\n  \"schema\": 1,\n  \"exporter\": %d,\n  \"files\": [\n", WF_VERSION);
    for (int i = 0; i < n; i++) {
        const tbl_def *d = tbl_def_at(i);
        uint32_t len, len2;
        const uint8_t *b = tbl_bytes(i, &len);
        FILE *f; json_val *doc; uint8_t *back;
        if (!b) { fprintf(stderr, "export: %s not loaded\n", d->name); fails++; continue; }
        if (len % (uint32_t)tbl_kind_size(d->kind)) { fprintf(stderr, "export: %s len 0x%X not a multiple of %s\n", d->name, len, tbl_kind_name(d->kind)); fails++; continue; }
        snprintf(path, sizeof path, "%s/%s", data_dir, d->group);
        if (mkdir_p(path)) { fprintf(stderr, "export: cannot create %s\n", path); fails++; continue; }
        snprintf(path, sizeof path, "%s/%s/%s.json", data_dir, d->group, d->name);
        f = fopen(path, "w");
        if (!f) { fprintf(stderr, "export: cannot write %s\n", path); fails++; continue; }
        tbl_write_json(f, d, b, len);
        fclose(f);
        /* ADR rule 2: re-read and compare */
        doc = json_parse_file(path, err, sizeof err);
        back = doc ? json_to_bytes(doc, d, &len2, why, sizeof why) : NULL;
        if (!doc || !back || len2 != len || memcmp(back, b, len)) {
            fprintf(stderr, "export: VERIFY FAILED for %s (%s) — file removed\n", d->name, doc ? (back ? "bytes differ" : why) : err);
            remove(path); fails++;
        } else {
            fprintf(mf, "    { \"path\": \"%s/%s.json\", \"crc32\": \"%08x\" }%s\n", d->group, d->name, wf_crc32(0, b, len), i + 1 < n ? "," : "");
        }
        free(back); json_free(doc);
    }
    fprintf(mf, "  ]\n}\n");
    fclose(mf);
    fprintf(stderr, "export: %d tables -> %s (%d failed)\n", n - fails, data_dir, fails);
    return fails ? 1 : 0;
}

/* Resolve a table file through the ACTIVE PROFILE's mod layers (later
 * mods win; stock = no profile = no mods — profile.h). mods/order.txt is
 * retired. */
static int resolve_table_path(const char *data_dir, const tbl_def *d, char *out, size_t outlen)
{
    char rel[512];
    snprintf(rel, sizeof rel, "tables/%s/%s.json", d->group, d->name);
    if (wf_mod_resolve(rel, out, outlen)) return 1;
    /* legacy mod trees kept tables at the layer root (mods/<m>/<group>/) */
    snprintf(rel, sizeof rel, "%s/%s.json", d->group, d->name);
    if (wf_mod_resolve(rel, out, outlen)) return 1;
    snprintf(out, outlen, "%s/%s/%s.json", data_dir, d->group, d->name);
    return 0;
}

/* The table as the first `nlayers` mods of the active profile see it (-1 =
 * all of them): the stock file, then every mod's layer lowest first (later
 * wins) - a full-rows layer REPLACES the table, an overrides layer PATCHES
 * it, so rule mods stack (a layer can be both). Rules layers shorter than
 * the engine's table are padded with the defaults (rows are appended).
 * malloc'd bytes or NULL (why); *nmod = how many mod layers applied,
 * *stock_modified = the stock file no longer matches its export crc. */
uint8_t *tbl_merge_layers(const char *data_dir, const tbl_def *d, int nlayers, uint32_t *out_len,
                          int *nmod, int *stock_modified, char *why, size_t whylen)
{
    char path[1024], err[256];
    json_val *doc; uint8_t *b; uint32_t len;
    int from_mod = 0;
    if (nmod) *nmod = 0;
    if (stock_modified) *stock_modified = 0;
    snprintf(path, sizeof path, "%s/%s/%s.json", data_dir, d->group, d->name);
    doc = json_parse_file(path, err, sizeof err);
    if (!doc) { snprintf(why, whylen, "%s: %s", path, err); return NULL; }
    b = json_to_bytes(doc, d, &len, why, whylen);
    if (!b) { json_free(doc); return NULL; }
    {
        const json_val *prov = json_get(doc, "prov");
        char crc[16]; snprintf(crc, sizeof crc, "%08x", wf_crc32(0, b, len));
        if (prov && strcmp(json_str(json_get(prov, "crc32"), ""), crc) && stock_modified) *stock_modified = 1;
    }
    json_free(doc);
    if (nlayers < 0 || nlayers > wf_profile_nmods()) nlayers = wf_profile_nmods();
    for (int m = 0; m < nlayers; m++) {
        static const char *const rel[2] = { "tables/%s/%s.json", "%s/%s.json" };   /* + the legacy root layout */
        for (int r = 0; r < 2; r++) {
            char sub[512]; uint8_t *nb; uint32_t nlen;
            snprintf(sub, sizeof sub, rel[r], d->group, d->name);
            snprintf(path, sizeof path, "mods/%s/%s", wf_profile_mod(m), sub);
            if (access(path, R_OK)) continue;
            doc = json_parse_file(path, err, sizeof err);
            if (!doc) { snprintf(why, whylen, "%s: %s", path, err); free(b); return NULL; }
            if (json_get(doc, "rows")) {          /* replace (json_to_bytes applies its own overrides too) */
                nb = json_to_bytes(doc, d, &nlen, why, whylen);
                if (!nb) { json_free(doc); free(b); return NULL; }
                free(b); b = nb; len = nlen;
            } else {                              /* sparse: patch the accumulated table */
                if (len < d->len && d->rom_addr == TBL_SYNTH && d->defaults) {
                    b = realloc(b, d->len); memcpy(b + len, (const uint8_t *)d->defaults + len, d->len - len); len = d->len;
                }
                if (json_apply_overrides(doc, d, b, len, why, whylen)) { json_free(doc); free(b); return NULL; }
            }
            json_free(doc);
            from_mod++;
            break;                                /* one layout per mod */
        }
    }
    if (len < d->len && d->rom_addr == TBL_SYNTH && d->defaults) {   /* a rules layer written before new rows were
                                                                          added (they are appended): pad with the defaults */
        uint8_t *nb = realloc(b, d->len);
        if (nb) { memcpy(nb + len, (const uint8_t *)d->defaults + len, d->len - len); b = nb; len = d->len; }
    }
    if (nmod) *nmod = from_mod;
    *out_len = len;
    return b;
}

int tbl_pack(const char *data_dir, const char *pak_path)
{
    int n = tbl_count(), fails = 0, modded = 0;
    pak_writer *w = pak_writer_new();
    char why[256];
    for (int i = 0; i < n; i++) {
        const tbl_def *d = tbl_def_at(i);
        uint8_t *b; uint32_t len; int from_mod = 0, stock_mod = 0;
        b = tbl_merge_layers(data_dir, d, -1, &len, &from_mod, &stock_mod, why, sizeof why);
        if (!b) { fprintf(stderr, "pack: %s: %s\n", d->name, why); fails++; continue; }
        if (stock_mod) { modded++; fprintf(stderr, "pack: %s modified since export\n", d->name); }
        if (from_mod) { modded++; fprintf(stderr, "pack: %s from %d mod layer%s\n", d->name, from_mod, from_mod > 1 ? "s (stacked)" : ""); }
        if (len != d->len) fprintf(stderr, "pack: %s is %u bytes, engine declares %u\n", d->name, len, d->len);
        if (pak_writer_add(w, d->name, b, len)) fails++;
        free(b);
    }
    if (!fails && pak_writer_save(w, pak_path)) fails++;
    pak_writer_free(w);
    fprintf(stderr, "pack: %d tables -> %s (%d modified, %d failed)\n", n - fails, pak_path, modded, fails);
    return fails ? 1 : 0;
}

/* One byte of a table straight from its (mod-resolved) JSON — for pack-time
 * checks that run before any pak is loaded. -1 if unreadable/out of range. */
int tbl_json_byte(const char *data_dir, const char *name, uint32_t off)
{
    int id = tbl_id(name);
    const tbl_def *d = id >= 0 ? tbl_def_at(id) : NULL;
    char path[1024], why[128]; uint32_t len; uint8_t *b; int v = -1;
    if (!d) return -1;
    resolve_table_path(data_dir, d, path, sizeof path);
    b = tbl_json_to_bytes_file(path, d, &len, why, sizeof why);
    if (b && off < len) v = b[off];
    free(b);
    return v;
}

uint8_t *tbl_json_to_bytes_file(const char *path, const tbl_def *d, uint32_t *len, char *why, size_t whylen)
{
    char err[256];
    json_val *doc = json_parse_file(path, err, sizeof err);
    uint8_t *b;
    if (!doc) { snprintf(why, whylen, "%s", err); return NULL; }
    b = json_to_bytes(doc, d, len, why, whylen);
    json_free(doc);
    return b;
}

/* ---------------------------------------------------------------- stock */
/* `wfengine --export-stock <file>` (forced ROM backend, like every export
 * tool): ONE JSON holding every table's pristine values — the editor's
 * "stock" reference for change-highlighting and per-table revert. ADR
 * rule 2 applies: the file is re-read and byte-compared before we keep it. */
int tbl_export_stock(const char *path)
{
    int n = tbl_count(), fails = 0;
    FILE *f = fopen(path, "w");
    char err[256];
    json_val *doc;
    if (!f) { fprintf(stderr, "stock: cannot write %s\n", path); return 1; }
    fprintf(f, "{\n  \"schema\": 1,\n  \"exporter\": %d,\n", WF_VERSION);
    fprintf(f, "  \"comment\": \"pristine ROM/engine-default table values (the wfeditor 'stock' reference). Regenerate: ./wfengine --export-stock %s\",\n", path);
    fprintf(f, "  \"tables\": {\n");
    for (int i = 0; i < n; i++) {
        const tbl_def *d = tbl_def_at(i);
        uint32_t len; const uint8_t *b = tbl_bytes(i, &len);
        uint32_t ne;
        if (!b) { fprintf(stderr, "stock: %s not loaded\n", d->name); fails++; continue; }
        ne = len / (uint32_t)tbl_kind_size(d->kind);
        fprintf(f, "    \"%s/%s\": { \"kind\": \"%s\", \"values\": [", d->group, d->name, tbl_kind_name(d->kind));
        for (uint32_t j = 0; j < ne; j++) {
            if (d->kind == TK_U32) fprintf(f, "%s\"0x%08llX\"", j ? "," : "", (unsigned long long)(uint32_t)elem_at(b, d->kind, j));
            else fprintf(f, "%s%lld", j ? "," : "", (long long)elem_at(b, d->kind, j));
        }
        fprintf(f, "] }%s\n", i + 1 < n ? "," : "");
    }
    fprintf(f, "  }\n}\n");
    fclose(f);
    /* verify: re-read, compare every element */
    doc = json_parse_file(path, err, sizeof err);
    if (!doc) { fprintf(stderr, "stock: VERIFY FAILED (%s) — file removed\n", err); remove(path); return 1; }
    {
        const json_val *tabs = json_get(doc, "tables");
        for (int i = 0; i < n; i++) {
            const tbl_def *d = tbl_def_at(i);
            char key[128]; const json_val *t, *vals; const json_val *e; uint32_t j = 0;
            uint32_t len; const uint8_t *b = tbl_bytes(i, &len);
            if (!b) continue;
            snprintf(key, sizeof key, "%s/%s", d->group, d->name);
            t = json_get(tabs, key); vals = t ? json_get(t, "values") : NULL;
            if (!vals || vals->type != JSON_ARRAY) { fprintf(stderr, "stock: VERIFY %s missing\n", key); fails++; continue; }
            for (e = vals->child; e; e = e->next, j++)
                if (elem_at(b, d->kind, j) != (int64_t)(d->kind == TK_U32 ? (int64_t)(uint32_t)json_int(e, 0) : json_int(e, 0)))
                    { fprintf(stderr, "stock: VERIFY %s[%u] differs\n", key, j); fails++; break; }
            if (j * (uint32_t)tbl_kind_size(d->kind) != len && e == NULL && !fails)
                { fprintf(stderr, "stock: VERIFY %s length differs\n", key); fails++; }
        }
    }
    json_free(doc);
    if (fails) { fprintf(stderr, "stock: VERIFY FAILED — file removed\n"); remove(path); return 1; }
    fprintf(stderr, "stock: %d tables -> %s (verified)\n", n, path);
    return 0;
}
