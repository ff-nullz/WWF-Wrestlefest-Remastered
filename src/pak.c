/* Pak reader/writer — see pak.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pak.h"
#include "tbl.h"          /* wf_crc32 */

typedef struct { char name[PAK_NAME_MAX]; uint32_t off, len, crc, pad; } sec_rec;
typedef struct { char magic[4]; uint32_t version, nsec, hdr_crc; } pak_hdr;

struct pak { uint8_t *buf; size_t size; uint32_t nsec; const sec_rec *secs; };

static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static void wr32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

pak *pak_open(const char *path)
{
    FILE *f = fopen(path, "rb");
    pak *p;
    long sz;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < (long)sizeof(pak_hdr)) { fclose(f); fprintf(stderr, "pak: %s too short\n", path); return NULL; }
    p = calloc(1, sizeof *p);
    p->buf = malloc((size_t)sz); p->size = (size_t)sz;
    if (fread(p->buf, 1, p->size, f) != p->size) { fclose(f); fprintf(stderr, "pak: %s short read\n", path); pak_close(p); return NULL; }
    fclose(f);
    if (memcmp(p->buf, "WFPK", 4)) { fprintf(stderr, "pak: %s bad magic\n", path); pak_close(p); return NULL; }
    if (rd32(p->buf + 4) != PAK_VERSION) { fprintf(stderr, "pak: %s version %u, want %u\n", path, rd32(p->buf + 4), PAK_VERSION); pak_close(p); return NULL; }
    p->nsec = rd32(p->buf + 8);
    if (sizeof(pak_hdr) + (size_t)p->nsec * sizeof(sec_rec) > p->size) { fprintf(stderr, "pak: %s truncated section table\n", path); pak_close(p); return NULL; }
    if (rd32(p->buf + 12) != wf_crc32(0, p->buf + sizeof(pak_hdr), p->nsec * sizeof(sec_rec))) {
        fprintf(stderr, "pak: %s section-table CRC mismatch\n", path); pak_close(p); return NULL;
    }
    p->secs = (const sec_rec *)(p->buf + sizeof(pak_hdr));
    for (uint32_t i = 0; i < p->nsec; i++) {
        const sec_rec *s = &p->secs[i];
        if ((size_t)s->off + s->len > p->size) { fprintf(stderr, "pak: %s section %s out of file\n", path, s->name); pak_close(p); return NULL; }
        if (wf_crc32(0, p->buf + s->off, s->len) != s->crc) { fprintf(stderr, "pak: %s section %s CRC mismatch\n", path, s->name); pak_close(p); return NULL; }
    }
    return p;
}

const uint8_t *pak_section(const pak *p, const char *name, uint32_t *len)
{
    for (uint32_t i = 0; i < p->nsec; i++)
        if (!strncmp(p->secs[i].name, name, PAK_NAME_MAX)) { if (len) *len = p->secs[i].len; return p->buf + p->secs[i].off; }
    if (len) *len = 0;
    return NULL;
}
int pak_section_count(const pak *p) { return (int)p->nsec; }
const char *pak_section_name(const pak *p, int i) { return p->secs[i].name; }
void pak_close(pak *p) { if (p) { free(p->buf); free(p); } }

/* ---- writer ---- */
struct pak_writer { sec_rec *secs; uint8_t **data; int n, cap; };

pak_writer *pak_writer_new(void) { return calloc(1, sizeof(pak_writer)); }

int pak_writer_add(pak_writer *w, const char *name, const uint8_t *bytes, uint32_t len)
{
    if (strlen(name) >= PAK_NAME_MAX) { fprintf(stderr, "pak: name too long: %s\n", name); return -1; }
    for (int i = 0; i < w->n; i++)
        if (!strcmp(w->secs[i].name, name)) { fprintf(stderr, "pak: duplicate section %s\n", name); return -1; }
    if (w->n == w->cap) {
        w->cap = w->cap ? w->cap * 2 : 64;
        w->secs = realloc(w->secs, (size_t)w->cap * sizeof *w->secs);
        w->data = realloc(w->data, (size_t)w->cap * sizeof *w->data);
    }
    memset(&w->secs[w->n], 0, sizeof(sec_rec));
    strncpy(w->secs[w->n].name, name, PAK_NAME_MAX - 1);
    w->secs[w->n].len = len;
    w->secs[w->n].crc = wf_crc32(0, bytes, len);
    w->data[w->n] = malloc(len ? len : 1);
    memcpy(w->data[w->n], bytes, len);
    w->n++;
    return 0;
}

int pak_writer_save(pak_writer *w, const char *path)
{
    size_t off = sizeof(pak_hdr) + (size_t)w->n * sizeof(sec_rec);
    uint8_t hdr[sizeof(pak_hdr)];
    FILE *f;
    pak *chk;
    for (int i = 0; i < w->n; i++) {
        off = (off + 15) & ~(size_t)15;
        w->secs[i].off = (uint32_t)off;
        off += w->secs[i].len;
    }
    f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "pak: cannot write %s\n", path); return -1; }
    memcpy(hdr, "WFPK", 4); wr32(hdr + 4, PAK_VERSION); wr32(hdr + 8, (uint32_t)w->n);
    wr32(hdr + 12, wf_crc32(0, w->secs, (size_t)w->n * sizeof(sec_rec)));
    fwrite(hdr, 1, sizeof hdr, f);
    fwrite(w->secs, sizeof(sec_rec), (size_t)w->n, f);
    for (int i = 0; i < w->n; i++) {
        static const uint8_t zero[16];
        long cur = ftell(f);
        if (cur < (long)w->secs[i].off) fwrite(zero, 1, (size_t)(w->secs[i].off - (uint32_t)cur), f);
        fwrite(w->data[i], 1, w->secs[i].len, f);
    }
    if (fclose(f) != 0) { fprintf(stderr, "pak: write error on %s\n", path); return -1; }
    chk = pak_open(path);                        /* verify by re-reading */
    if (!chk) { remove(path); return -1; }
    for (int i = 0; i < w->n; i++) {
        uint32_t len; const uint8_t *b = pak_section(chk, w->secs[i].name, &len);
        if (!b || len != w->secs[i].len || memcmp(b, w->data[i], len)) {
            fprintf(stderr, "pak: verify failed on section %s\n", w->secs[i].name);
            pak_close(chk); remove(path); return -1;
        }
    }
    pak_close(chk);
    return 0;
}

void pak_writer_free(pak_writer *w)
{
    if (!w) return;
    for (int i = 0; i < w->n; i++) free(w->data[i]);
    free(w->secs); free(w->data); free(w);
}
