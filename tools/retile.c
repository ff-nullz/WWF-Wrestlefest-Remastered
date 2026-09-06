/* --retile: rebase an exported wrestler's sprite tiles into the CLONE-ART
 * ARENA so a clone slot can carry its OWN art instead of borrowing the
 * base's global tiles.
 *
 *   wfengine --retile <srcdir> <dstdir> <newbase>
 *   e.g. --retile export/07 mods/newart/wrestlers/12 0x10000
 *
 * Arena bases (video.c): slot N = 0x10000 + (N-12)*0x2000, slots 12..43
 * (0x2000 tiles each, the top 32 reserved for the select portrait;
 * a full wrestler is ~4.7-5.8k).
 *
 * tiles.json ids become newbase+index IN ORDER (sheet.png rows stay
 * aligned); every "tile" value in poses.json is remapped through that
 * table; sheet.png / palette.json / stats.json are copied verbatim.
 * The modder then edits sheet.png freely and adds wrestler.json
 * ({"clone_of": N, "name": "..."}) to bind the moveset. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>

static char *slurp2(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb"); char *b; long sz;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    b = malloc((size_t)sz + 1);
    if (!b || fread(b, 1, (size_t)sz, f) != (size_t)sz) { free(b); fclose(f); return NULL; }
    fclose(f); b[sz] = 0; if (n) *n = (size_t)sz;
    return b;
}

static int copy_file(const char *src, const char *dst)
{
    size_t n; char *b = slurp2(src, &n); FILE *f;
    if (!b) { fprintf(stderr, "retile: cannot read %s\n", src); return -1; }
    f = fopen(dst, "wb");
    if (!f || fwrite(b, 1, n, f) != n) { fprintf(stderr, "retile: cannot write %s\n", dst); free(b); if (f) fclose(f); return -1; }
    fclose(f); free(b);
    return 0;
}

int tool_retile(const char *src, const char *dst, unsigned newbase)
{
    char path[512];
    static int map[0x10000];        /* old id -> new id, -1 = unmapped */
    int nmap = 0, rc = 0;
    char *buf; FILE *out;

    for (int i = 0; i < 0x10000; i++) map[i] = -1;
    mkdir(dst, 0775);

    /* tiles.json: ordered old ids -> newbase+index */
    snprintf(path, sizeof path, "%s/tiles.json", src);
    buf = slurp2(path, NULL);
    if (!buf) { fprintf(stderr, "retile: cannot read %s\n", path); return 1; }
    {
        const char *p = strstr(buf, "\"tiles\"");
        snprintf(path, sizeof path, "%s/tiles.json", dst);
        out = fopen(path, "w");
        if (!p || !out) { fprintf(stderr, "retile: bad tiles.json / cannot write\n"); free(buf); if (out) fclose(out); return 1; }
        fprintf(out, "{ \"tiles\": [");
        while ((p = strpbrk(p, "0123456789]")) && *p != ']') {
            long v = strtol(p, (char **)&p, 10);
            if (v >= 0 && v < 0x10000 && map[v] < 0) map[v] = (int)(newbase + (unsigned)nmap);
            fprintf(out, "%s%u", nmap ? "," : "", newbase + (unsigned)nmap);
            nmap++;
        }
        fprintf(out, "] }\n");
        fclose(out);
    }
    free(buf);
    fprintf(stderr, "retile: %d tiles -> 0x%X..0x%X\n", nmap, newbase, newbase + (unsigned)nmap - 1);

    /* poses.json: rewrite every "tile": N through the map */
    snprintf(path, sizeof path, "%s/poses.json", src);
    buf = slurp2(path, NULL);
    if (!buf) { fprintf(stderr, "retile: cannot read %s\n", path); return 1; }
    snprintf(path, sizeof path, "%s/poses.json", dst);
    out = fopen(path, "w");
    if (!out) { fprintf(stderr, "retile: cannot write %s\n", path); free(buf); return 1; }
    {
        const char *p = buf; int missed = 0;
        for (;;) {
            const char *k = strstr(p, "\"tile\"");
            const char *q;
            long v;
            if (!k) { fputs(p, out); break; }
            q = k + 6;
            while (*q == ' ' || *q == '\t') q++;
            if (*q != ':') { fwrite(p, 1, (size_t)(k + 6 - p), out); p = k + 6; continue; }
            q++;
            while (*q == ' ' || *q == '\t') q++;
            if (!isdigit((unsigned char)*q)) { fwrite(p, 1, (size_t)(q - p), out); p = q; continue; }
            v = strtol(q, (char **)&q, 10);
            fwrite(p, 1, (size_t)(k - p), out);
            if (v >= 0 && v < 0x10000 && map[v] >= 0)
                fprintf(out, "\"tile\": %d", map[v]);
            else { fprintf(out, "\"tile\": %ld", v); missed++; }
            p = q;
        }
        if (missed) fprintf(stderr, "retile: %d pose tile ref(s) outside the sheet stay at their global ids (shared/partner art - expected)\n", missed);
    }
    fclose(out); free(buf);

    /* verbatim copies */
    {
        static const char *cp[] = { "sheet.png", "palette.json", "stats.json" };
        for (int i = 0; i < 3; i++) {
            char a[512], b2[512];
            snprintf(a, sizeof a, "%s/%s", src, cp[i]);
            snprintf(b2, sizeof b2, "%s/%s", dst, cp[i]);
            if (copy_file(a, b2)) rc = 1;
        }
    }
    if (!rc)
        fprintf(stderr, "retile: done. Add %s/wrestler.json {\"clone_of\": N, \"name\": \"...\"} and put the dir in a mod's wrestlers/<slot>/.\n", dst);
    return rc;
}
