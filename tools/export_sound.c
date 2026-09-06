/* --export-sound: the sound ROMs leave the ROM (ADR-001).
 *
 *   rom/31a11-2.ic42 (64K Z80 sound program) -> data/sound/z80.bin
 *   rom/31j10.ic73  (512K OKI ADPCM samples) -> data/sound/oki.bin
 *
 * Straight byte copies (these are not decoded tables — the emulated board
 * IS the decoder), self-verified by re-reading and comparing. The engine
 * loads only data/sound/*.bin (mod-resolvable); rom/ stays tool-only. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static unsigned char *slurp(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    unsigned char *b; long sz;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    b = malloc((size_t)sz);
    if (!b || fread(b, 1, (size_t)sz, f) != (size_t)sz) { free(b); fclose(f); return NULL; }
    fclose(f); *n = (size_t)sz;
    return b;
}

static int copy_verified(const char *src, const char *dst, size_t want)
{
    size_t n = 0, m = 0;
    unsigned char *a = slurp(src, &n), *b;
    FILE *f;
    if (!a) { fprintf(stderr, "export-sound: cannot read %s\n", src); return -1; }
    if (want && n != want)
        fprintf(stderr, "export-sound: NOTE %s is %zu bytes (expected %zu)\n", src, n, want);
    f = fopen(dst, "wb");
    if (!f || fwrite(a, 1, n, f) != n) { fprintf(stderr, "export-sound: cannot write %s\n", dst); free(a); if (f) fclose(f); return -1; }
    fclose(f);
    b = slurp(dst, &m);                       /* self-verify: re-read + compare */
    if (!b || m != n || memcmp(a, b, n) != 0) {
        fprintf(stderr, "export-sound: VERIFY FAILED %s\n", dst);
        free(a); free(b);
        return -1;
    }
    printf("export-sound: %s -> %s (%zu bytes, verified)\n", src, dst, n);
    free(a); free(b);
    return 0;
}

int tool_export_sound(const char *rom_dir)
{
    char src[512];
    mkdir("data", 0775); mkdir("data/sound", 0775);
    snprintf(src, sizeof src, "%s/31a11-2.ic42", rom_dir);
    if (copy_verified(src, "data/sound/z80.bin", 0x10000)) return 1;
    snprintf(src, sizeof src, "%s/31j10.ic73", rom_dir);
    if (copy_verified(src, "data/sound/oki.bin", 0x80000)) return 1;
    return 0;
}
