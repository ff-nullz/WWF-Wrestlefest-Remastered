/* art_run — the AI art PRODUCTION BATCH in C (user 2026-08-25: "write it
 * in C, so it uses existing code, and can easily be ported to a
 * tab/screen in wfeditor").
 *
 *   wfengine --art-run <jobdir> <anchor.png> <chardesc.txt>
 *
 * For every unfinished pose in <jobdir>/ref (skipping partial/overlay
 * refs), builds 4x4 gray-mannequin sheets on SOLID MAGENTA, hands each
 * to `codex exec` with the anchor, cuts the magenta key (a colour test
 * — no masks, no floods), slices the largest component per cell,
 * NEAREST-scales to the ref size at the ref's feet line, and QCs by
 * silhouette IoU against the ref. Misses re-roll as single frames;
 * final failures fall back to base art (absent out file).
 *
 * chardesc.txt: line 1.. = the character description; a line starting
 * with "OUTLINES:" switches to the outline rules. Progress -> stderr +
 * <jobdir>/run.log + <jobdir>/run_state.json (the wfeditor tab reads
 * those). Everything image-side reuses the engine's PNG io.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include "../src/engine.h"
#include "../src/json.h"
#include <dirent.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <stdarg.h>

int wf_video_load_rgba_png(const char *path, uint8_t **out, int *w, int *h);
int wf_art_write_rgba_png(const char *path, const uint8_t *rgba, int W, int H);

#define CANVAS 256                 /* the generator's WINDOW: square; the canvas is 256x320 */
#define CANVAS_H 320
#define GRID   4
#define TMO    540
#define IOU_OK 0.42

static const char *jobdir;
static FILE *runlog;

static void logf_(const char *fmt, ...)
{
    va_list ap; char line[512]; time_t t = time(0); struct tm tm;
    localtime_r(&t, &tm);
    va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
    fprintf(stderr, "art-run: %s\n", line);
    if (runlog) {
        fprintf(runlog, "%02d:%02d:%02d %s\n", tm.tm_hour, tm.tm_min, tm.tm_sec, line);
        fflush(runlog);
    }
}

static void state_json(int done, int todo, int ok, int miss, const char *phase)
{
    char p[512]; FILE *f;
    snprintf(p, sizeof p, "%s/run_state.json", jobdir);
    f = fopen(p, "w");
    if (!f) return;
    fprintf(f, "{\"phase\":\"%s\",\"done\":%d,\"todo\":%d,\"ok\":%d,\"miss\":%d}\n",
            phase, done, todo, ok, miss);
    fclose(f);
}

/* ---- codex ---- */
static char newest_path[1024];
static double newest_gen(void)
{
    char base[512]; DIR *d; struct dirent *e; double best = 0;
    snprintf(base, sizeof base, "%s/.codex/generated_images", getenv("HOME"));
    d = opendir(base);
    if (!d) return 0;
    while ((e = readdir(d))) {
        char sub[768]; DIR *sd; struct dirent *se;
        if (e->d_name[0] == '.') continue;
        snprintf(sub, sizeof sub, "%s/%s", base, e->d_name);
        sd = opendir(sub);
        if (!sd) continue;
        while ((se = readdir(sd))) {
            char f[1200]; struct stat st;
            if (strncmp(se->d_name, "exec-", 5)) continue;
            snprintf(f, sizeof f, "%s/%s", sub, se->d_name);
            if (stat(f, &st) == 0 && (double)st.st_mtime > best) {
                best = (double)st.st_mtime;
                snprintf(newest_path, sizeof newest_path, "%s", f);
            }
        }
        closedir(sd);
    }
    closedir(d);
    return best;
}

/* fork/exec, no shell — the prompt carries quotes freely */
/* ONE codex call at a time, machine-wide (jobs/.codex.lock): results are
 * claimed as "the newest file in ~/.codex/generated_images", so two
 * concurrent calls — a batch and a manual re-roll from the editor, or two
 * batches — swap each other's images (Honky frames landed in the class-2
 * template and a Perfect in Honky's, 2026-08-25). A caller finding the
 * lock held by a live pid WAITS (polls) instead of failing. */
static void codex_lock(void)
{
    for (int waited = 0; waited < 3600; waited += 2) {
        FILE *g = fopen("jobs/.codex.lock", "r"); long pid = 0;
        if (g) { if (fscanf(g, "%ld", &pid) != 1) pid = 0; fclose(g); }
        if (pid > 0 && pid != (long)getpid() && kill((pid_t)pid, 0) == 0) {
            if (!waited) logf_("codex busy (pid %ld) - waiting", pid);
            sleep(2); continue;
        }
        g = fopen("jobs/.codex.lock", "w");
        if (g) { fprintf(g, "%d\n", (int)getpid()); fclose(g); }
        return;
    }
}
static void codex_unlock(void) { unlink("jobs/.codex.lock"); }
static int codex_exec_locked(const char *img1, const char *img2, const char *prompt, char *out_path, size_t out_n);
/* AI PROVIDERS (user 2026-08-26): the generation backend is chosen per skin
 * (WF_ART_PROVIDER, from skin.json "provider"); every request goes through
 * this one dispatch. codex is the only backend (qwen-edit and grok were
 * tried 2026-08-26/28 and dropped: not usable); a new one = a row here
 * (name + an exec taking two input images and a prompt, returning a PNG). */
typedef int (*art_exec_fn)(const char *img1, const char *img2, const char *prompt, char *out_path, size_t out_n);
static const char *art_provider_used = "codex";   /* the row that ran (art_stamp) */
static int codex_provider(const char *img1, const char *img2, const char *prompt, char *out_path, size_t out_n)
{
    int r;
    codex_lock();
    r = codex_exec_locked(img1, img2, prompt, out_path, out_n);
    codex_unlock();
    return r;
}
static const struct { const char *name; art_exec_fn exec; } art_providers[] = {
    { "codex", codex_provider },
};
const char *wf_art_provider_names[] = { "codex", NULL };   /* for the editor's dropdown */
static int codex_exec(const char *img1, const char *img2, const char *prompt,
                      char *out_path, size_t out_n)
{
    const char *want = getenv("WF_ART_PROVIDER");
    static int said;
    for (size_t k = 0; k < sizeof art_providers / sizeof art_providers[0]; k++)
        if (!want || !want[0] || !strcmp(want, art_providers[k].name)) {
            if (!said) { logf_("provider: %s", art_providers[k].name); said = 1; }
            art_provider_used = art_providers[k].name;
            return art_providers[k].exec(img1, img2, prompt, out_path, out_n);
        }
    logf_("provider '%s' unknown - no such backend (codex)", want);
    return -1;
}
static int codex_exec_locked(const char *img1, const char *img2, const char *prompt,
                             char *out_path, size_t out_n)
{
    for (int t = 0; t < 2; t++) {
        double before = newest_gen();
        pid_t pid = fork();
        if (pid == 0) {
            const char *argv[12]; int n = 0;
            freopen("/dev/null", "w", stdout);
            freopen("/dev/null", "w", stderr);
            argv[n++] = "codex"; argv[n++] = "exec"; argv[n++] = "--skip-git-repo-check";
            argv[n++] = "-i"; argv[n++] = img1;
            argv[n++] = "-i"; argv[n++] = img2;
            argv[n++] = "--"; argv[n++] = prompt; argv[n] = NULL;
            execvp("codex", (char *const *)argv);
            _exit(127);
        }
        if (pid < 0) return -1;
        for (int s = 0; s < TMO; s++) {
            int st;
            if (waitpid(pid, &st, WNOHANG) == pid) { pid = 0; break; }
            sleep(1);
        }
        if (pid) { kill(pid, SIGKILL); waitpid(pid, NULL, 0); logf_("codex timeout"); }
        if (newest_gen() > before) {
            snprintf(out_path, out_n, "%s", newest_path);
            return 0;
        }
        logf_("codex try %d: nothing came back", t + 1);
    }
    return -1;
}

/* ---- image ops (RGBA byte buffers, engine PNG io) ---- */
typedef struct { uint8_t *px; int w, h; } img;

static int iload(const char *path, img *m)
{
    return wf_video_load_rgba_png(path, &m->px, &m->w, &m->h) == 0 ? 0 : -1;
}


/* WINDOW (2026-08-26): refs/outs are 256x320 canvases (origin row 176, 144 px
 * below the feet line); the square generator works on rows
 * [win_y, win_y+256) - win_y per pose from the job's manifest.json. Refs
 * are cropped to the window on load, outputs padded back on write. */
static short winy[2048]; static char winy_dir[520];
static void winy_load(const char *dir)
{
    char mp[760], err[128]; json_val *doc;
    if (!dir || !strcmp(winy_dir, dir)) return;
    snprintf(winy_dir, sizeof winy_dir, "%s", dir);
    memset(winy, 0, sizeof winy);
    snprintf(mp, sizeof mp, "%s/manifest.json", dir);
    doc = json_parse_file(mp, err, sizeof err);
    if (!doc) return;
    for (int sec = 0; sec < 2; sec++) {
        const json_val *m = json_get(doc, sec ? "victims" : "poses");
        for (const json_val *e = m ? m->child : NULL; e; e = e->next) {
            unsigned pz = (unsigned)atoi(e->key);
            if (pz < 2048) winy[pz] = (short)json_int(json_get(e, "win_y"), 0);
        }
    }
    json_free(doc);
}
static int winy_of_path(const char *path)
{
    const char *q = strstr(path, "pose_"); int pz;
    if (!q || sscanf(q, "pose_%d", &pz) != 1 || pz < 0 || pz >= 2048) return 0;
    if (jobdir) winy_load(jobdir);
    return winy[pz];
}
/* a ref or out frame in WINDOW space (256 square) */
static int iload_ref(const char *path, img *m)
{
    if (iload(path, m)) return -1;
    if (m->w == CANVAS && m->h == CANVAS_H) {
        int wy = winy_of_path(path); uint8_t *c = malloc((size_t)CANVAS * CANVAS * 4);
        if (!c) { free(m->px); return -1; }
        memcpy(c, m->px + (size_t)wy * CANVAS * 4, (size_t)CANVAS * CANVAS * 4);
        free(m->px); m->px = c; m->h = CANVAS;
    }
    return 0;
}
/* write a window-space canvas as a full 256x320 frame at its window */
static int write_canvas(const char *path, const uint8_t *cv, int w, int h)
{
    if (w == CANVAS && h == CANVAS) {
        int wy = winy_of_path(path), rc; uint8_t *full = calloc((size_t)CANVAS * CANVAS_H, 4);
        if (!full) return -1;
        memcpy(full + (size_t)wy * CANVAS * 4, cv, (size_t)CANVAS * CANVAS * 4);
        rc = wf_art_write_rgba_png(path, full, CANVAS, CANVAS_H);
        free(full);
        return rc;
    }
    return wf_art_write_rgba_png(path, cv, w, h);
}
/* the loose key test (any magenta-ish pixel) and the STRICT one (the flat
 * #FF00FF the prompt asks for - a purple costume highlight is never this
 * saturated: Undertaker's tights sit around (120,60,150)) */
#define ISKEY(p)  ((p)[0] > 140 && (p)[2] > 140 && (p)[1] < ((p)[0] + (p)[2]) / 4)
#define ISPURE(p) ((p)[0] >= 200 && (p)[2] >= 200 && (p)[1] <= 60)
/* ENCLOSED KEY (user 2026-09-05, Duggan: "purple bits where they should be
 * transparent" between the legs): the border flood cannot reach a gap the
 * figure closes off (legs + touching boots, an arm against the body), so
 * the flat magenta the model painted there survived as costume. Each
 * 4-connected component of opaque key-coloured pixels that is NOT border
 * background is judged as a whole: mostly PURE #FF00FF = the key, cleared
 * (its anti-aliased rim goes with it); a purple costume highlight is a
 * shaded mix and never passes the strict test. `cleared` (w*h, may be
 * NULL) is marked for every pixel dropped. Returns the count. */
static int key_enclosed(img *m, uint8_t *cleared)
{
    int w = m->w, h = m->h, n = 0, *stk = malloc(sizeof(int) * (size_t)w * h), *comp = malloc(sizeof(int) * (size_t)w * h);
    uint8_t *seen = calloc((size_t)w * h, 1);
    if (!stk || !comp || !seen) { free(stk); free(comp); free(seen); return 0; }
    for (int s = 0; s < w * h; s++) {
        int sp = 0, nc = 0, pure = 0; long sr = 0, sg = 0, sb = 0, lo;
        if (seen[s] || m->px[s*4+3] < 128 || !ISKEY(m->px + s*4)) continue;
        seen[s] = 1; stk[sp++] = s;
        while (sp) {
            int i = stk[--sp], x = i % w, y = i / w;
            static const int dx4[4] = { 1, -1, 0, 0 }, dy4[4] = { 0, 0, 1, -1 };
            comp[nc++] = i;
            if (ISPURE(m->px + i*4)) pure++;
            sr += m->px[i*4]; sg += m->px[i*4+1]; sb += m->px[i*4+2];
            for (int d = 0; d < 4; d++) {
                int nx = x + dx4[d], ny = y + dy4[d], j;
                if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                j = ny * w + nx;
                if (seen[j] || m->px[j*4+3] < 128 || !ISKEY(m->px + j*4)) continue;
                seen[j] = 1; stk[sp++] = j;
            }
        }
        /* the key: a fair share of flat #FF00FF, or a pocket so small the
         * rim mix is all there is - judged by the component MEAN's magenta
         * saturation min(r,b) - g (measured 2026-09-05: Duggan's leftover
         * pockets >= 108, every Undertaker costume patch <= 81 with no
         * pure pixel at all) */
        lo = sr < sb ? sr : sb;
        if (!(pure * 10 >= nc * 3 || (lo - sg) / nc >= 100)) continue;   /* a shaded purple patch: costume */
        for (int k = 0; k < nc; k++) {
            int i = comp[k];
            m->px[i*4] = m->px[i*4+1] = m->px[i*4+2] = m->px[i*4+3] = 0;
            if (cleared) cleared[i] = 1;
        }
        n += nc;
    }
    free(stk); free(comp); free(seen);
    return n;
}
/* FRINGE (user 2026-08-26: "purple pixels on the edge"): the model
 * anti-aliases the figure into the magenta, so the outermost ring is a
 * half-magenta mix the key test keeps. Two erosion passes drop edge
 * pixels that are magenta-TINTED (r and b well above g, r ~ b) - a pink
 * or red costume has r >> b and stays. `near` (w*h, may be NULL) limits
 * the erosion to pixels touching a MARKED transparent pixel (the freshly
 * keyed ground of a frame that already had alpha: its other edges were
 * settled long ago and a purple costume's rim must stay); killed pixels
 * join the mark so the second pass can follow the rim. */
static void key_fringe(img *m, uint8_t *near)
{
    for (int pass = 0; pass < 2; pass++) {
        int w = m->w, h = m->h, n = 0;
        uint8_t *kill = calloc((size_t)w * h, 1);
        if (!kill) return;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                size_t i = (size_t)y*w + x;
                uint8_t *p = m->px + i*4;
                int r = p[0], g = p[1], b = p[2], edge;
                if (!p[3]) continue;
#define TR(j) (!m->px[(j)*4+3] && (!near || near[j]))
                edge = (x > 0 && TR(i-1)) || (x < w-1 && TR(i+1)) ||
                       (y > 0 && TR(i-w)) || (y < h-1 && TR(i+w));
#undef TR
                if (edge && r > 90 && b > 90 && r * 10 > g * 14 && b * 10 > g * 14 && abs(r - b) < 50)
                    { kill[i] = 1; n++; }
            }
        for (int i = 0; i < w * h; i++)
            if (kill[i]) { m->px[i*4] = m->px[i*4+1] = m->px[i*4+2] = m->px[i*4+3] = 0; if (near) near[i] = 1; }
        free(kill);
        if (!n) break;
    }
}
/* key the magenta ground of a generated picture. An image that already
 * carries alpha keeps it (its ground was keyed before) and only loses
 * enclosed pure-magenta pockets; returns the pixels dropped (0 = untouched). */
static int key_magenta(img *m)
{
    int opaque = 1, w = m->w, h = m->h, n = 0;
    uint8_t *cleared = calloc((size_t)w * h, 1);
    if (!cleared) return 0;
    for (int i = 0; i < w * h && opaque; i++)
        if (m->px[i * 4 + 3] != 255) opaque = 0;
    if (opaque) {
        /* BACKGROUND ONLY (user 2026-08-29, Undertaker's "transparent
         * tights"): the colour test also matches a purple costume highlight
         * INSIDE the figure, and the fringe erosion then grew each such
         * pixel into a hole. Key the magenta that is 4-connected to the
         * image border; the enclosed pockets go through the strict
         * component test below - a costume pixel can never be keyed. */
        int *stk = malloc(sizeof(int) * (size_t)w * h), sp = 0;
        if (!stk) { free(cleared); return 0; }
#define PUSH(j) do { if (!cleared[j] && ISKEY(m->px + (j)*4)) { cleared[j] = 1; stk[sp++] = (j); } } while (0)
        for (int x = 0; x < w; x++) { PUSH(x); PUSH((h - 1) * w + x); }
        for (int y = 0; y < h; y++) { PUSH(y * w); PUSH(y * w + w - 1); }
        while (sp) {                              /* each pixel enters the stack once */
            int i = stk[--sp], x = i % w, y = i / w;
            if (x > 0) PUSH(i - 1);
            if (x < w - 1) PUSH(i + 1);
            if (y > 0) PUSH(i - w);
            if (y < h - 1) PUSH(i + w);
        }
#undef PUSH
        for (int i = 0; i < w * h; i++)
            if (cleared[i]) { m->px[i*4] = m->px[i*4+1] = m->px[i*4+2] = m->px[i*4+3] = 0; n++; }
        free(stk);
    }
    n += key_enclosed(m, cleared);
    if (opaque) key_fringe(m, NULL);              /* the whole rim is fresh */
    else if (n) key_fringe(m, cleared);           /* only the rims of the pockets just opened */
    free(cleared);
    return n;
}
#undef ISKEY
#undef ISPURE

/* ENCLOSED HOLES: transparent pixels NOT 4-connected to the image border
 * are the keyer's old interior bites (or a generator's own gaps); fill each
 * from the mean of its opaque 4-neighbours, growing inward until none is
 * left. Returns the pixel count filled. */
static int fill_holes(img *m)
{
    int w = m->w, h = m->h, filled = 0, *stk = malloc(sizeof(int) * 2 * (size_t)w * h), sp = 0;
    uint8_t *bg = calloc((size_t)w * h, 1);
    if (!stk || !bg) { free(stk); free(bg); return 0; }
#define PUSH(j) do { if (!bg[j] && m->px[(j)*4+3] < 128) { bg[j] = 1; stk[sp++] = (j); } } while (0)
    for (int x = 0; x < w; x++) { PUSH(x); PUSH((h - 1) * w + x); }
    for (int y = 0; y < h; y++) { PUSH(y * w); PUSH(y * w + w - 1); }
    while (sp) {
        int i = stk[--sp], x = i % w, y = i / w;
        if (x > 0) PUSH(i - 1);
        if (x < w - 1) PUSH(i + 1);
        if (y > 0) PUSH(i - w);
        if (y < h - 1) PUSH(i + w);
    }
#undef PUSH
    for (;;) {
        int n = 0;
        for (int i = 0; i < w * h; i++) {
            int x = i % w, y = i / w, sr = 0, sg = 0, sb = 0, c = 0;
            static const int dx4[4] = { 1, -1, 0, 0 }, dy4[4] = { 0, 0, 1, -1 };
            if (bg[i] || m->px[i*4+3] >= 128) continue;
            for (int d = 0; d < 4; d++) {
                int nx = x + dx4[d], ny = y + dy4[d], j;
                if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                j = ny * w + nx;
                if (m->px[j*4+3] < 128) continue;
                sr += m->px[j*4]; sg += m->px[j*4+1]; sb += m->px[j*4+2]; c++;
            }
            if (!c) continue;
            stk[n++] = i; stk[n++] = (sr / c) | ((sg / c) << 8) | ((sb / c) << 16);
        }
        if (!n) break;
        for (int k = 0; k < n; k += 2) {
            int i = stk[k], v = stk[k + 1];
            m->px[i*4] = (uint8_t)(v & 255); m->px[i*4+1] = (uint8_t)((v >> 8) & 255);
            m->px[i*4+2] = (uint8_t)((v >> 16) & 255); m->px[i*4+3] = 255;
        }
        filled += n / 2;
    }
    free(stk); free(bg);
    return filled;
}

/* --art-fill-holes JOBDIR: repair out/ frames keyed before the
 * background-only keyer (interior bites in purple costumes). */
int tool_art_fill_holes(const char *dir)
{
    static const char *sub[2] = { "out", "out_hi" };   /* the ingest quantises out_hi when present */
    for (int s = 0; s < 2; s++) {
        char p[512]; DIR *d; struct dirent *e; int fixed = 0, seen = 0, px = 0;
        snprintf(p, sizeof p, "%s/%s", dir, sub[s]);
        d = opendir(p);
        if (!d) { if (!s) { fprintf(stderr, "art-fill-holes: no %s\n", p); return 1; } continue; }
        while ((e = readdir(d))) {
            int pose, n; img of; char op[560];
            if (sscanf(e->d_name, "pose_%d.png", &pose) != 1) continue;
            snprintf(op, sizeof op, "%s/%s/pose_%04d.png", dir, sub[s], pose);
            if (iload(op, &of)) continue;
            seen++;
            n = fill_holes(&of);
            if (n > 0 && wf_art_write_rgba_png(op, of.px, of.w, of.h) == 0) {
                fixed++; px += n;
                fprintf(stderr, "art-fill-holes: %s pose %04d filled %d px\n", sub[s], pose, n);
            }
            free(of.px);
        }
        closedir(d);
        fprintf(stderr, "art-fill-holes: %d of %d frames repaired (%d px) -> %s/%s\n", fixed, seen, px, dir, sub[s]);
    }
    return 0;
}

/* --art-rekey JOBDIR: repair out/ + out_hi/ frames keyed before the enclosed
 * pocket test (flat magenta left between the legs / under an arm). */
int tool_art_rekey(const char *dir)
{
    static const char *sub[2] = { "out", "out_hi" };   /* the ingest quantises out_hi when present */
    for (int s = 0; s < 2; s++) {
        char p[512]; DIR *d; struct dirent *e; int fixed = 0, seen = 0, px = 0;
        snprintf(p, sizeof p, "%s/%s", dir, sub[s]);
        d = opendir(p);
        if (!d) { if (!s) { fprintf(stderr, "art-rekey: no %s\n", p); return 1; } continue; }
        while ((e = readdir(d))) {
            int pose, n; img of; char op[560];
            if (sscanf(e->d_name, "pose_%d.png", &pose) != 1) continue;
            snprintf(op, sizeof op, "%s/%s/pose_%04d.png", dir, sub[s], pose);
            if (iload(op, &of)) continue;
            seen++;
            n = key_magenta(&of);
            if (n > 0 && wf_art_write_rgba_png(op, of.px, of.w, of.h) == 0) {
                fixed++; px += n;
                fprintf(stderr, "art-rekey: %s pose %04d keyed %d px\n", sub[s], pose, n);
            }
            free(of.px);
        }
        closedir(d);
        fprintf(stderr, "art-rekey: %d of %d frames repaired (%d px) -> %s/%s\n", fixed, seen, px, dir, sub[s]);
    }
    return 0;
}

/* provenance sidecar (user 2026-08-26: when + which model): <png>.meta */
static void art_stamp(const char *png)
{
    char p[1100]; FILE *f; time_t t = time(0); struct tm tm;
    snprintf(p, sizeof p, "%s.meta", png);
    f = fopen(p, "w");
    if (!f) return;
    localtime_r(&t, &tm);
    fprintf(f, "provider=%s\nwhen=%04d-%02d-%02d %02d:%02d\n", art_provider_used,
            tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
    fclose(f);
}

/* largest 4-connected alpha component inside a region -> bbox; 0 = none */
static int main_component(const img *m, int rx0, int ry0, int rx1, int ry1,
                          int bb[4])
{
    int w = m->w, h = m->h, bestn = 0;
    uint8_t *lab = calloc((size_t)w * h, 1);
    int *stk = malloc(sizeof(int) * (size_t)w * h);
    if (!lab || !stk) { free(lab); free(stk); return 0; }
    for (int sy = ry0; sy < ry1; sy += 3)
        for (int sx = rx0; sx < rx1; sx += 3) {
            int sp = 0, n = 0, x0 = sx, y0 = sy, x1 = sx, y1 = sy;
            if (m->px[(sy*w+sx)*4+3] < 64 || lab[sy*w+sx]) continue;
            stk[sp++] = sy*w+sx;
            while (sp) {
                int q = stk[--sp], x = q % w, y = q / w;
                if (lab[q] || m->px[q*4+3] < 64) continue;
                lab[q] = 1; n++;
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
                if (y < y0) y0 = y;
                if (y > y1) y1 = y;
                if (x > 0) stk[sp++] = q-1;
                if (x < w-1) stk[sp++] = q+1;
                if (y > 0) stk[sp++] = q-w;
                if (y < h-1) stk[sp++] = q+w;
            }
            if (n > bestn) { bestn = n; bb[0]=x0; bb[1]=y0; bb[2]=x1+1; bb[3]=y1+1; }
        }
    free(lab); free(stk);
    return bestn >= 400;
}

static void ref_bbox(const img *m, int bb[4])
{
    bb[0] = m->w; bb[1] = m->h; bb[2] = 0; bb[3] = 0;
    for (int y = 0; y < m->h; y++)
        for (int x = 0; x < m->w; x++)
            if (m->px[(y*m->w+x)*4+3]) {
                if (x < bb[0]) bb[0] = x;
                if (x+1 > bb[2]) bb[2] = x+1;
                if (y < bb[1]) bb[1] = y;
                if (y+1 > bb[3]) bb[3] = y+1;
            }
}

/* centroid x of the alpha in the BOTTOM strip of a bbox (the feet): the
 * hair/pompadour widens the bbox asymmetrically, so bbox-centre alignment
 * skewed every figure a few px — visible as a consistent offset in the
 * composed grapples (playtest 2026-08-25). Feet don't lie. */
static int feet_cx(const uint8_t *px, int w, int stride4, const int bb[4])
{
    int strip = (bb[3]-bb[1]) / 6; long sum = 0, n = 0;
    if (strip < 4) strip = 4;
    if (bb[2]-bb[0] > bb[3]-bb[1]) {   /* WIDE (lying) pose: the bottom strip
                                          is the body's side — use the whole
                                          figure's centroid instead (playtest:
                                          downed honky sat offset on the canvas) */
        for (int y = bb[1]; y < bb[3]; y++)
            for (int x = bb[0]; x < bb[2]; x++)
                if (px[((size_t)y*w + x)*4 + 3] >= 64) { sum += x; n++; }
        (void)stride4;
        return n ? (int)(sum / n) : (bb[0]+bb[2])/2;
    }
    for (int y = bb[3]-strip; y < bb[3]; y++)
        for (int x = bb[0]; x < bb[2]; x++)
            if (px[((size_t)y*w + x)*4 + 3] >= 64) { sum += x; n++; }
    (void)stride4;
    return n ? (int)(sum / n) : (bb[0]+bb[2])/2;
}

/* NEAREST-scale src bbox onto a 256 canvas: feet LINE from the ref's
 * bbox bottom, feet CENTROID for x (not bbox centre) */
static int place_ref_feet_cx = -1;   /* set per slice from the REF image */
static int art_height_pct = 100, art_width_pct = 100;   /* body scale, see body_load() */
/* place_s: scale the slice's bbox onto the (CANVAS*S)^2 canvas, feet LINE
 * from the ref's bbox bottom, feet CENTROID for x. BOX-FILTERED (user
 * 2026-08-26: "bridge the gap to stock" - nearest sampling dropped half the
 * source pixels: broken outlines, speckled trim): every destination pixel
 * averages its source footprint, alpha = coverage. S=4 writes the hi-res
 * archive (out_hi/) the ingest quantises at full resolution. */
static void place_s(const img *src, const int sb[4], const int rb[4], uint8_t *cv, int S)
{
    int W = CANVAS * S;
    int sw = sb[2]-sb[0], sh = sb[3]-sb[1];
    int rh = (rb[3]-rb[1]) * art_height_pct / 100 * S;   /* body scale: feet stay put, he grows upward */
    int nw = sw * rh / (sh ? sh : 1) * art_width_pct / 100; if (nw < 1) nw = 1;
    int sfc = feet_cx(src->px, src->w, 0, sb);
    int scaled_fc = (sfc - sb[0]) * nw / (sw ? sw : 1);
    int ox, oy = rb[3] * S - rh;
    ox = (rb[0]+rb[2])/2 * S - nw/2;   /* fallback */
    if (place_ref_feet_cx >= 0) ox = place_ref_feet_cx * S - scaled_fc;
    memset(cv, 0, (size_t)W*W*4);
    for (int y = 0; y < rh; y++)
        for (int x = 0; x < nw; x++) {
            int X0 = sb[0] + x * sw / nw, X1 = sb[0] + (x + 1) * sw / nw;
            int Y0 = sb[1] + y * sh / rh, Y1 = sb[1] + (y + 1) * sh / rh;
            long r = 0, g = 0, b = 0, a = 0, n = 0;
            int dx = ox + x, dy = oy + y;
            if (dx < 0 || dx >= W || dy < 0 || dy >= W) continue;
            if (X1 <= X0) X1 = X0 + 1; if (Y1 <= Y0) Y1 = Y0 + 1;
            for (int yy = Y0; yy < Y1 && yy < src->h; yy++)
                for (int xx = X0; xx < X1 && xx < src->w; xx++) {
                    const uint8_t *p = src->px + ((size_t)yy*src->w + xx)*4;
                    n++;
                    if (p[3] < 128) continue;
                    r += p[0]; g += p[1]; b += p[2]; a++;
                }
            if (!n || a * 2 < n) continue;             /* coverage < 50%: transparent */
            {
                uint8_t *q = cv + ((size_t)dy*W + dx)*4;
                q[0] = (uint8_t)(r / a); q[1] = (uint8_t)(g / a); q[2] = (uint8_t)(b / a); q[3] = 255;
            }
        }
}
static void place(const img *src, const int sb[4], const int rb[4], uint8_t *cv) { place_s(src, sb, rb, cv, 1); }
#define HI 4
static void write_hi(const char *dir, int pose, const img *src, const int sb[4], const int rb[4])
{
    static uint8_t hi[CANVAS*HI*CANVAS*HI*4]; char p[600];
    place_s(src, sb, rb, hi, HI);
    snprintf(p, sizeof p, "%s/out_hi", dir); mkdir(p, 0775);
    snprintf(p, sizeof p, "%s/out_hi/pose_%04d.png", dir, pose);
    wf_art_write_rgba_png(p, hi, CANVAS*HI, CANVAS*HI);
}

static double iou_vs_ref(const uint8_t *cv, const img *ref)
{
    long inter = 0, uni = 0;
    for (int y = 0; y < CANVAS; y++)
        for (int x = 0; x < CANVAS; x++) {
            int a = cv[((size_t)y*CANVAS+x)*4+3] >= 128;
            int b = ref->px[((size_t)y*ref->w+x)*4+3] >= 128;
            if (a && b) inter++;
            if (a || b) uni++;
        }
    return uni ? (double)inter / (double)uni : 0.0;
}

/* REFERENCE STYLE (WF_ART_REF_STYLE, 2026-08-26 experiment for the generic):
 *   luminance  - the base frame in grey (the original "mannequin": every
 *                drawn feature survives, so the base's identity can leak)
 *   posterised - 4 grey tones from luminance: volumes and limb shading
 *                without the fine features (moustache, face, boot detail)
 *   silhouette - one flat grey with a 1-px darker edge: pose only */
enum { REF_LUMINANCE, REF_POSTERISED, REF_SILHOUETTE };
static int ref_style_get(void)
{
    static int style = -1;
    if (style < 0) {
        const char *e = getenv("WF_ART_REF_STYLE");
        style = REF_LUMINANCE;
        if (e && !strcmp(e, "posterised")) style = REF_POSTERISED;
        else if (e && !strcmp(e, "silhouette")) style = REF_SILHOUETTE;
    }
    return style;
}
static int ref_opaque(const img *ref, int x, int y)
{
    if (x < 0 || y < 0 || x >= ref->w || y >= ref->h) return 0;
    return ref->px[((size_t)y*ref->w + x)*4 + 3] != 0;
}
/* RANKED GREY (user 2026-08-28: "palette issues mess up the contrast and
 * how the model views the pose"): a frame is <=16 palette pens, and plain
 * luminance lands a pastel singlet and the skin on the same grey. Rank the
 * frame's distinct opaque colours by luminance and spread the ranks evenly
 * over 40..235 - every pen stays separable, darker stays darker. Shared by
 * the sheets/singles sent to the model and the editor's grey tiles. */
void wf_art_gray_ranked(uint8_t *rgba, int w, int h)
{
    uint32_t col[256]; int lum[256], n = 0;
    for (int i = 0; i < w * h; i++) {
        const uint8_t *p = rgba + (size_t)i * 4; uint32_t c; int k;
        if (!p[3]) continue;
        c = (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
        for (k = 0; k < n; k++) if (col[k] == c) break;
        if (k == n) { if (n < 256) { col[n] = c; lum[n] = (299*p[0] + 587*p[1] + 114*p[2]) / 1000; } n++; if (n > 32) break; }   /* > 32 distinct: not palette art, stop scanning */
    }
    if (!n) return;
    if (n > 32) {                      /* NOT palette art (a converted / generated frame with thousands of
                                          shades): ranking collapses the body to black - plain luminance
                                          (user 2026-08-28: undertaker's 'converted' refs drew as silhouettes) */
        for (int i = 0; i < w * h; i++) { uint8_t *p = rgba + (size_t)i * 4; if (!p[3]) continue; uint8_t l = (uint8_t)((299*p[0] + 587*p[1] + 114*p[2]) / 1000); p[0] = p[1] = p[2] = l; }
        return;
    }
    for (int i = 0; i < w * h; i++) {
        uint8_t *p = rgba + (size_t)i * 4; uint32_t c; int rank = 0, ties = 0, l0, k;
        if (!p[3]) continue;
        c = (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
        for (k = 0; k < n; k++) if (col[k] == c) break;
        l0 = lum[k];
        for (int j = 0; j < n; j++) { if (lum[j] < l0) rank++; else if (lum[j] == l0 && j < k) ties++; }
        p[0] = p[1] = p[2] = (uint8_t)(n > 1 ? 40 + (rank + ties) * 195 / (n - 1) : 150);
    }
}
/* gray mannequin of a ref pasted into an RGB sheet at (ox,oy), cell px */
static void paste_gray(uint8_t *sheet, int sw, const img *ref, int ox, int oy, int cell)
{
    int style = ref_style_get();
    uint8_t *rk = malloc((size_t)ref->w * ref->h * 4);   /* the ranked-grey copy */
    if (rk) { memcpy(rk, ref->px, (size_t)ref->w * ref->h * 4); wf_art_gray_ranked(rk, ref->w, ref->h); }
    for (int y = 0; y < cell; y++)
        for (int x = 0; x < cell; x++) {
            int sx = x * ref->w / cell, sy = y * ref->h / cell;
            const uint8_t *p = (rk ? rk : ref->px) + ((size_t)sy*ref->w + sx)*4;
            uint8_t *q = sheet + ((size_t)(oy+y)*sw + (ox+x))*3;
            if (p[3]) {
                uint8_t l = rk ? p[0] : (uint8_t)((299*p[0] + 587*p[1] + 114*p[2]) / 1000);
                if (style == REF_POSTERISED) {
                    static const uint8_t tone[4] = { 56, 112, 168, 224 };
                    l = tone[l >> 6];
                } else if (style == REF_SILHOUETTE) {
                    int edge = !ref_opaque(ref, sx - 1, sy) || !ref_opaque(ref, sx + 1, sy)
                            || !ref_opaque(ref, sx, sy - 1) || !ref_opaque(ref, sx, sy + 1);
                    l = edge ? 40 : 160;
                }
                q[0] = q[1] = q[2] = l;
            }
        }
    free(rk);
}

static int write_rgb_png(const char *path, const uint8_t *rgb, int W, int H)
{
    uint8_t *tmp = malloc((size_t)W*H*4);
    int rc;
    if (!tmp) return -1;
    for (int i = 0; i < W*H; i++) {
        tmp[i*4] = rgb[i*3]; tmp[i*4+1] = rgb[i*3+1];
        tmp[i*4+2] = rgb[i*3+2]; tmp[i*4+3] = 255;
    }
    rc = wf_art_write_rgba_png(path, tmp, W, H);
    free(tmp);
    return rc;
}

/* ---- prompts ---- */
static char chardesc[1024], outldesc[1024];
static char prompt[4096];

/* WF_ART_BUILD=anchor: the figure keeps the TARGET's build, not the
 * mannequin's — for class STOCK templates, where the mannequin is a
 * borrowed body from another class (Perfect drawn from a Boss Man ref) */
/* BODY settings (user 2026-08-26: "Andre on Earthquake's template, slightly
 * taller"): skin.json "height_pct"/"width_pct" scale the accepted figure at
 * placement (feet anchored, so he grows upward), "build": "anchor" keeps the
 * target's own proportions in the prompt. Loaded next to character.txt. */
static int art_build_anchor = 0;
static void body_load(const char *dirp)
{
    char jp[700], err[96]; json_val *doc;
    snprintf(jp, sizeof jp, "%s/skin.json", dirp);
    doc = json_parse_file(jp, err, sizeof err);
    if (!doc) return;
    art_height_pct = (int)json_int(json_get(doc, "height_pct"), 100);
    art_width_pct  = (int)json_int(json_get(doc, "width_pct"), 100);
    art_build_anchor = !strcmp(json_str(json_get(doc, "build"), "mannequin"), "anchor");
    if (art_height_pct < 80) art_height_pct = 80; if (art_height_pct > 120) art_height_pct = 120;
    if (art_width_pct < 80) art_width_pct = 80;   if (art_width_pct > 120) art_width_pct = 120;
    json_free(doc);
    if (art_height_pct != 100 || art_width_pct != 100 || art_build_anchor)
        logf_("body: height %d%% width %d%% build=%s", art_height_pct, art_width_pct, art_build_anchor ? "anchor" : "mannequin");
}
static const char *build_phrase(void)
{
    const char *b = getenv("WF_ART_BUILD");
    return (b && !strcmp(b, "anchor")) || art_build_anchor
        ? "the TARGET CHARACTER's OWN build and proportions exactly as in the second image (his frame, chest and waist width, limb thickness) - NOT the mannequin's, which is a different body"
        : "EXACT same body proportions as the mannequin";
}

/* PROMPT TEMPLATES (user 2026-08-25: each recipe editable per skin). Slots:
 * {CHAR} the character line, {OUTL} the outlines line, {BUILD} the build
 * phrase (WF_ART_BUILD), {N} cells on the sheet, {GRID} the grid size,
 * {EXTRA} a re-roll direction. Defaults below; a skin's prompts.json (next
 * to its character.txt) overrides any key. --art-prompt-defaults dumps them. */
enum { PK_ANCHOR, PK_SELECT, PK_SHEET, PK_FRAG_SHEET, PK_SINGLE, PK_FRAG_SINGLE, PK_CARDS, PK_CONT, PK_TITLE, PK_N };
static const char *pk_name[PK_N] = { "anchor", "select", "sheet", "frag_sheet", "single", "frag_single", "cards", "cont", "title" };
static const char *pk_title[PK_N] = {
    "2 Anchor (inputs: the grey base ref twice)", "3 Portrait - select cell (inputs: stock select cell + anchor)",
    "4 GO - pose sheet 4x4 / 2x2 (inputs: grey mannequin sheet + anchor)", "4 GO - fragment sheet",
    "re-roll: single pose", "re-roll: single fragment",
    "5 Animation - the six continue cards as one 3x2 sheet (inputs: stock cards sheet + anchor)",
    "re-roll: one continue card", "6 Title card (inputs: stock title card + anchor)" };
static const char *pk_default[PK_N] = {
    /* anchor = the single-pose recipe */
    "Use your image generation tool once, immediately, before anything else. "
    "The first image is a grayscale POSE REFERENCE mannequin. The second image is the "
    "TARGET CHARACTER: {CHAR}. Draw the target character in the mannequin's pose. Copy the "
    "mannequin's pose EXACTLY, limb by limb: the exact leg positions and stride (which "
    "leg is forward, the bend of each knee, foot placement), the forward lean of the "
    "torso, the exact arm and hand positions. {BUILD}, same framing. {OUTL} Background: SOLID PURE MAGENTA (#FF00FF) everywhere "
    "outside the figure, including the enclosed gaps between limbs - one flat colour, no "
    "checkerboard. Generate now, then stop.",
    "Use your image generation tool once, immediately, before anything else. "
    "The first image is a STYLE EXAMPLE: an 80x80 character-select portrait from a "
    "16-bit arcade wrestling game - it looks like a DIGITIZED PHOTOGRAPH of a real "
    "man, realistic facial structure, gritty photographic shading, crunched to a "
    "limited arcade palette. The second image is the TARGET CHARACTER: {CHAR}. Paint a "
    "NEW select portrait of the target character in that digitized-photo arcade "
    "style: realistic photographic face, head and shoulders filling the frame, "
    "facing slightly left like the example, tight crop. The BACKGROUND must be "
    "SOLID BRIGHT CYAN (#00EEFF) - one flat colour everywhere behind the figure, "
    "with a CLEAN SHARP silhouette edge around the hair, no black between hair and "
    "background, no gradient. Square image. Generate now, then stop.",
    "Use your image generation tool once, immediately, before anything else. "
    "The first image is a {GRID}x{GRID} SPRITE SHEET of {N} grayscale pose-reference mannequins, "
    "one per cell (empty cells stay empty). The second image is the TARGET CHARACTER: {CHAR}. "
    "Redraw the sheet: EVERY mannequin cell, each mannequin replaced by the target "
    "character in that mannequin's EXACT pose, copied limb by limb (exact leg positions "
    "and stride, bend of each knee, foot placement, torso lean, exact arm and hand "
    "positions), {BUILD}, same position and framing "
    "within its cell. Keep the {GRID}x{GRID} grid layout exactly: same cell positions, no figures "
    "merged, added, or dropped, and no figure touching the image edges. {OUTL} "
    "Background: SOLID PURE MAGENTA (#FF00FF) everywhere outside the figures, including "
    "the enclosed gaps between the legs and between arms and body - one flat colour, no "
    "gradient, no pattern, no checkerboard. Generate now, then stop.",
    "Use your image generation tool once, immediately, before anything else. "
    "The first image is a {GRID}x{GRID} sheet of {N} grayscale BODY-PART FRAGMENTS (a boot, an "
    "arm, a leg, a torso piece) - overlay pieces a wrestling game composites onto other "
    "frames. The second image is the TARGET CHARACTER these parts belong to: {CHAR}. Redraw "
    "EVERY fragment cell in the target character's style: his boots, his suit, his skin "
    "- EXACT same shape, same position and framing within its cell, same proportions, "
    "same perspective. Keep the {GRID}x{GRID} grid exactly; no fragments merged, added, dropped "
    "or turned into whole figures. {OUTL} Background: SOLID PURE MAGENTA (#FF00FF) - one "
    "flat colour, no pattern. Generate now, then stop.",
    "Use your image generation tool once, immediately, before anything else. "
    "The first image is a grayscale POSE REFERENCE mannequin. The second image is the "
    "TARGET CHARACTER: {CHAR}. Draw the target character in the mannequin's pose. Copy the "
    "mannequin's pose EXACTLY, limb by limb: the exact leg positions and stride (which "
    "leg is forward, the bend of each knee, foot placement), the forward lean of the "
    "torso, the exact arm and hand positions. EXACT same body proportions as the "
    "mannequin, same framing. {OUTL} Background: SOLID PURE MAGENTA (#FF00FF) everywhere "
    "outside the figure, including the enclosed gaps between limbs - one flat colour, no "
    "checkerboard.{EXTRA} Generate now, then stop.",
    "Use your image generation tool once, immediately, before anything else. "
    "The first image is a grayscale BODY-PART FRAGMENT (a boot, an arm, a leg, a torso "
    "piece) - an overlay piece a wrestling game composites onto other frames. The second "
    "image is the TARGET CHARACTER this part belongs to: {CHAR}. Redraw the fragment in the "
    "target character's style: his boots, his suit, his skin - EXACT same shape, same "
    "framing, same proportions, same perspective. Do NOT turn it into a whole figure. "
    "{OUTL} Background: SOLID PURE MAGENTA (#FF00FF) - one flat colour.{EXTRA} Generate now, then "
    "stop.",
    "Use your image generation tool once, immediately, before anything else. "
    "The first image is a 3x2 grid of six CONTINUE-SCREEN portrait cards from a 16-bit arcade wrestling game: "
    "the same digitized-photo face in six frames of a small reaction animation (six expressions / head angles), "
    "in front of a wall of WWF logos, each with a decorative border. The second image is the TARGET CHARACTER: {CHAR}. "
    "Repaint the whole grid with the TARGET CHARACTER's face in the SAME digitized-photograph arcade style, in "
    "FULL COLOUR: keep the 3x2 grid layout exactly (six equal cells, same order), keep each cell's framing, "
    "wall-of-logos background and border exactly, and keep each cell's EXPRESSION and head angle - it must be "
    "the SAME man in all six cells, with the same haircut, skin and features. Landscape image 3:2, the grid fills "
    "the whole image, no margins, no gaps, no captions. Generate now, then stop.",
    "Use your image generation tool once, immediately, before anything else. "
    "The first image is an 80x80 CONTINUE-SCREEN portrait card from a 16-bit arcade wrestling game: a "
    "digitized-photo face in front of a wall of WWF logos, with a decorative border. The second image is the "
    "TARGET CHARACTER: {CHAR}. Repaint the card with the TARGET CHARACTER's face in the SAME digitized-photograph "
    "arcade style, in FULL COLOUR: keep the exact framing and crop, keep the wall-of-logos background and the "
    "border exactly, and keep the SAME EXPRESSION and head angle as the card (this is one frame of a small "
    "reaction animation). Square image, the card fills the whole image, no transparent areas, no margins.{EXTRA} "
    "Generate now, then stop.",
    "Use your image generation tool once, immediately, before anything else. "
    "The first image is a TITLE-CARD illustration from a 16-bit arcade wrestling game: the wrestler from the "
    "waist up, one arm raised pointing upward in victory, digitized-photo arcade style, full colour. The second "
    "image is the TARGET CHARACTER: {CHAR}. Redraw it with the TARGET CHARACTER in the SAME pose, framing and style, "
    "in FULL COLOUR. The BACKGROUND must be SOLID PURE MAGENTA (#FF00FF) everywhere outside the figure - one "
    "flat colour, no gradient, no checkerboard.{EXTRA} Generate now, then stop.",
};
static char *pk_over[PK_N];            /* the skin's prompts.json overrides */
static void pk_load(const char *descfile)
{
    char dirp[600], jp[700], err[96]; const char *sl; json_val *doc;
    for (int k = 0; k < PK_N; k++) { free(pk_over[k]); pk_over[k] = NULL; }
    snprintf(dirp, sizeof dirp, "%s", descfile);
    sl = strrchr(dirp, '/');
    if (sl) *(char *)sl = 0; else snprintf(dirp, sizeof dirp, ".");
    body_load(dirp);
    snprintf(jp, sizeof jp, "%s/prompts.json", dirp);
    doc = json_parse_file(jp, err, sizeof err);
    if (!doc) return;
    for (int k = 0; k < PK_N; k++) {
        const char *t = json_str(json_get(doc, pk_name[k]), NULL);
        if (t && t[0]) pk_over[k] = strdup(t);
    }
    json_free(doc);
    logf_("prompts.json: %s (custom prompts in use)", jp);
}
static const char *pk_expand(int k, int n, int grid, const char *extra)
{
    const char *t = pk_over[k] ? pk_over[k] : pk_default[k];
    size_t o = 0; char ex[1200];
    snprintf(ex, sizeof ex, "%s%s", extra && extra[0] ? " Direction: " : "", extra && extra[0] ? extra : "");
    while (*t && o < sizeof prompt - 1) {
        const char *rep = NULL; int skip = 0; char num[16];
        if (*t == '{') {
            if (!strncmp(t, "{CHAR}", 6)) { rep = chardesc; skip = 6; }
            else if (!strncmp(t, "{OUTL}", 6)) { rep = outldesc; skip = 6; }
            else if (!strncmp(t, "{BUILD}", 7)) { rep = build_phrase(); skip = 7; }
            else if (!strncmp(t, "{EXTRA}", 7)) { rep = ex; skip = 7; }
            else if (!strncmp(t, "{N}", 3)) { snprintf(num, sizeof num, "%d", n); rep = num; skip = 3; }
            else if (!strncmp(t, "{GRID}", 6)) { snprintf(num, sizeof num, "%d", grid); rep = num; skip = 6; }
        }
        if (rep) { o += (size_t)snprintf(prompt + o, sizeof prompt - o, "%s", rep); t += skip; }
        else prompt[o++] = *t++;
    }
    prompt[o < sizeof prompt ? o : sizeof prompt - 1] = 0;
    return prompt;
}
static const char *sheet_prompt(int n, int grid)   { return pk_expand(PK_SHEET, n, grid, NULL); }
static const char *frag_sheet_prompt(int n)        { return pk_expand(PK_FRAG_SHEET, n, GRID, NULL); }
static const char *frag_single_prompt(void)        { return pk_expand(PK_FRAG_SINGLE, 1, 1, NULL); }
static const char *single_prompt(void)             { return pk_expand(PK_SINGLE, 1, 1, NULL); }
static const char *anchor_prompt(void)             { return pk_expand(PK_ANCHOR, 1, 1, NULL); }
static const char *select_prompt(void)             { return pk_expand(PK_SELECT, 1, 1, NULL); }
static const char *cont_prompt(const char *extra)  { return pk_expand(PK_CONT, 1, 1, extra); }
static const char *title_prompt(const char *extra) { return pk_expand(PK_TITLE, 1, 1, extra); }
static const char *cards_prompt(void)              { return pk_expand(PK_CARDS, 6, 3, NULL); }

int tool_art_prompt_defaults(const char *out)   /* --art-prompt-defaults OUT.json */
{
    FILE *f = fopen(out, "w");
    if (!f) return 1;
    fprintf(f, "{\n");
    for (int k = 0; k < PK_N; k++) {
        fprintf(f, "  \"%s\": ", pk_name[k]); json_write_string(f, pk_default[k]);
        fprintf(f, ",\n  \"%s_title\": ", pk_name[k]); json_write_string(f, pk_title[k]);
        fprintf(f, k + 1 < PK_N ? ",\n" : "\n");
    }
    fprintf(f, "}\n");
    fclose(f);
    return 0;
}
static int load_desc(const char *descfile)
{
    /* character description: plain lines; "OUTLINES:" switches target */
    FILE *f = fopen(descfile, "r");
    char line[512]; char *dst = chardesc; size_t cap = sizeof chardesc, len = 0;
    if (!f) { fprintf(stderr, "art-run: cannot read %s\n", descfile); return 1; }
    outldesc[0] = chardesc[0] = 0;
    while (fgets(line, sizeof line, f)) {
        size_t l;
        if (!strncmp(line, "OUTLINES:", 9)) {
            dst = outldesc; cap = sizeof outldesc; len = strlen(outldesc);
            memmove(line, line + 9, strlen(line + 9) + 1);
        }
        l = strlen(line);
        while (l && (line[l-1] == '\n' || line[l-1] == '\r')) line[--l] = 0;
        if (!l) continue;
        if (len && len + 1 < cap) { dst[len++] = ' '; dst[len] = 0; }
        if (l + len + 1 < cap) { memcpy(dst + len, line, l + 1); len += l; }
    }
    fclose(f);
    if (!chardesc[0]) { fprintf(stderr, "art-run: %s is empty\n", descfile); return 1; }
    pk_load(descfile);
    return 0;
}

/* magenta single-frame input from one ref png */
static int make_single_input(const char *refpng, const char *outpng)
{
    static uint8_t flat[1024*1024*3];
    img ref;
    if (iload_ref(refpng, &ref)) return -1;
    for (int k = 0; k < 1024*1024; k++) {
        flat[k*3] = 255; flat[k*3+1] = 0; flat[k*3+2] = 255;
    }
    paste_gray(flat, 1024, &ref, 0, 0, 1024);
    free(ref.px);
    return write_rgb_png(outpng, flat, 1024, 1024);
}

/* ---- the run ---- */
static int poses[1024], nposes;
static int frags[256], nfrags;

static int slice_accept2(const img *out, int rx0, int ry0, int rx1, int ry1,
                         int pose, double iou_min, int minpx)
{
    char p[512]; img ref; int sb[4], rb[4];
    static uint8_t cv[CANVAS*CANVAS*4];
    double q;
    snprintf(p, sizeof p, "%s/ref/pose_%04d.png", jobdir, pose);
    if (iload_ref(p, &ref)) return 0;
    if (!main_component(out, rx0, ry0, rx1, ry1, sb)) { free(ref.px); return 0; }
    {   /* fragment cells are small: honour the caller's floor */
        int n = (sb[2]-sb[0]) * (sb[3]-sb[1]);
        if (n < minpx) { free(ref.px); return 0; }
    }
    ref_bbox(&ref, rb);
    place_ref_feet_cx = feet_cx(ref.px, ref.w, 0, rb);
    place(out, sb, rb, cv);
    q = iou_vs_ref(cv, &ref);
    free(ref.px);
    q = q * ((art_height_pct > 100 ? art_height_pct : 100) / 100.0) * ((art_width_pct > 100 ? art_width_pct : 100) / 100.0);   /* a scaled body: compare at its own size */
    if (q < iou_min) { logf_("pose %d: IoU %.2f -> re-roll", pose, q); return 0; }
    {   /* COLOUR gate (user 2026-08-25: "sometimes it returns in black and
           white"): the refs are grey mannequins on purpose, and codex now and
           then copies the tone instead of the anchor's colours. A figure
           whose mean chroma is near zero is a grey copy -> re-roll queue. */
        long chroma = 0, n = 0;
        for (int i = 0; i < CANVAS*CANVAS; i++) if (cv[i*4+3]) {
            int r = cv[i*4], g = cv[i*4+1], b = cv[i*4+2];
            int mx = r > g ? (r > b ? r : b) : (g > b ? g : b), mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
            chroma += mx - mn; n++;
        }
        if (n && chroma / n < 12) { logf_("pose %d: GRAYSCALE (mean chroma %ld) -> re-roll", pose, chroma / n); return 0; }
    }
    write_hi(jobdir, pose, out, sb, rb);   /* the hi-res archive (feet ctx still set) */
    place_ref_feet_cx = -1;
    snprintf(p, sizeof p, "%s/out/pose_%04d.png", jobdir, pose);
    return write_canvas(p, cv, CANVAS, CANVAS) == 0;
}
static int slice_accept(const img *out, int rx0, int ry0, int rx1, int ry1, int pose)
{
    return slice_accept2(out, rx0, ry0, rx1, ry1, pose, IOU_OK, 400);
}

/* PORTRAIT surface (user 2026-08-25: part of a skin's generation list): one
 * request, the ROM card as the composition/style example + the anchor; the
 * result is box-downscaled to the card's size and pasted at the card's
 * place on the canvas. Continue faces (800+cell): keep the backdrop, border
 * and EXPRESSION, replace the man. Title card (810): the pointing victory
 * pose on solid magenta, keyed. `extra` = a per-roll direction (re-roll). */
static int portrait_one(const char *dir, const char *anchor, int pose, const char *extra)
{
    img ref, out; int rb[4]; char rp[600], gen[1024], op[600];
    static char pp2[4096];
    snprintf(rp, sizeof rp, "%s/ref/pose_%04d.png", dir, pose);
    if (iload_ref(rp, &ref)) return -1;
    ref_bbox(&ref, rb);
    snprintf(pp2, sizeof pp2, "%s", pose < 810 ? cont_prompt(extra) : title_prompt(extra));
    logf_("portrait %d (%s)", pose, pose < 810 ? "continue face" : "title card");
    if (codex_exec(rp, anchor, pp2, gen, sizeof gen) || iload(gen, &out)) { free(ref.px); logf_("portrait %d: nothing came back", pose); return -1; }
    if (pose >= 810) key_magenta(&out);
    {   /* box-downscale the whole image (continue) or its opaque bbox (title) onto the card's bbox */
        int bw = rb[2] - rb[0] + 1, bh = rb[3] - rb[1] + 1;
        int sx0 = 0, sy0 = 0, sx1 = out.w, sy1 = out.h, rc = -1;
        uint8_t *canvas = calloc((size_t)ref.w * ref.h, 4);
        if (pose >= 810) { int ob[4]; ref_bbox(&out, ob); if (ob[2] > ob[0]) { sx0 = ob[0]; sy0 = ob[1]; sx1 = ob[2] + 1; sy1 = ob[3] + 1; } }
        for (int y = 0; y < bh; y++) for (int x = 0; x < bw; x++) {
            int X0 = sx0 + (sx1 - sx0) * x / bw, X1 = sx0 + (sx1 - sx0) * (x + 1) / bw;
            int Y0 = sy0 + (sy1 - sy0) * y / bh, Y1 = sy0 + (sy1 - sy0) * (y + 1) / bh;
            long r = 0, g = 0, b = 0, a = 0, n = 0;
            if (X1 <= X0) X1 = X0 + 1; if (Y1 <= Y0) Y1 = Y0 + 1;
            for (int yy = Y0; yy < Y1 && yy < out.h; yy++) for (int xx = X0; xx < X1 && xx < out.w; xx++) {
                const uint8_t *q = out.px + ((size_t)yy * out.w + xx) * 4;
                r += q[0]; g += q[1]; b += q[2]; a += q[3]; n++;
            }
            if (n && (pose < 810 || a / n >= 128)) {
                uint8_t *d = canvas + ((size_t)(rb[1] + y) * ref.w + rb[0] + x) * 4;
                d[0] = (uint8_t)(r / n); d[1] = (uint8_t)(g / n); d[2] = (uint8_t)(b / n); d[3] = 255;
            }
        }
        snprintf(op, sizeof op, "%s/out/pose_%04d.png", dir, pose);
        if (write_canvas(op, canvas, ref.w, ref.h) == 0) { logf_("portrait %d: done", pose); rc = 0; }
        free(canvas); free(out.px); free(ref.px);
        return rc;
    }
}
/* CONTINUE ANIMATION (user 2026-08-25: "shouldn't it be an animation"): the
 * six faces 800-805 are frames of ONE reaction animation, so they go to codex
 * as ONE 3x2 sheet of the ROM cards (in colour) and come back as one sheet:
 * one face, six expressions, one request. Returns the number of cards written. */
/* the six continue cards sit in ROM cells 0,1,2,4,5,6 of row 0x30+rep (3 and 7 are empty) */
static const int cont_cells[6] = { 0, 1, 2, 4, 5, 6 };
static int cards_sheet(const char *dir, const char *anchor, const int *pend, int npend)
{
    enum { COLS = 3, ROWS = 2, CELL = 340 };
    static uint8_t sheet[COLS*CELL * ROWS*CELL * 3];
    static char pp[4096];
    char sp[600], gen[1024]; img out; int wrote = 0;
    memset(sheet, 0, sizeof sheet);
    for (int k = 0; k < 6; k++) {
        img ref; char rp[600];
        snprintf(rp, sizeof rp, "%s/ref/pose_%04d.png", dir, ENG_CONT_POSE0 + cont_cells[k]);
        if (iload_ref(rp, &ref)) return 0;
        int rb[4]; ref_bbox(&ref, rb);
        int bw = rb[2] - rb[0] + 1, bh = rb[3] - rb[1] + 1, ox = (k % COLS) * CELL, oy = (k / COLS) * CELL;
        for (int y = 0; y < CELL; y++) for (int x = 0; x < CELL; x++) {
            const uint8_t *q = ref.px + ((size_t)(rb[1] + y * bh / CELL) * ref.w + rb[0] + x * bw / CELL) * 4;
            uint8_t *d = sheet + ((size_t)(oy + y) * (COLS*CELL) + ox + x) * 3;
            d[0] = q[0]; d[1] = q[1]; d[2] = q[2];
        }
        free(ref.px);
    }
    snprintf(sp, sizeof sp, "%s/work_cards.png", dir);
    write_rgb_png(sp, sheet, COLS*CELL, ROWS*CELL);
    snprintf(pp, sizeof pp, "%s", cards_prompt());
    logf_("continue animation: one 3x2 sheet of the six faces");
    if (codex_exec(sp, anchor, pp, gen, sizeof gen) || iload(gen, &out)) { logf_("continue animation: nothing came back"); return 0; }
    {
        int cw = out.w / COLS, ch = out.h / ROWS;
        for (int k = 0; k < 6; k++) {
            int want = 0; for (int i = 0; i < npend; i++) if (pend[i] == ENG_CONT_POSE0 + cont_cells[k]) want = 1;
            if (!want) continue;
            img ref; char rp[600], op[600]; int rb[4];
            snprintf(rp, sizeof rp, "%s/ref/pose_%04d.png", dir, ENG_CONT_POSE0 + cont_cells[k]);
            if (iload_ref(rp, &ref)) continue;
            ref_bbox(&ref, rb);
            int bw = rb[2] - rb[0] + 1, bh = rb[3] - rb[1] + 1, sx0 = (k % COLS) * cw, sy0 = (k / COLS) * ch;
            uint8_t *canvas = calloc((size_t)ref.w * ref.h, 4);
            for (int y = 0; y < bh; y++) for (int x = 0; x < bw; x++) {
                int X0 = sx0 + cw * x / bw, X1 = sx0 + cw * (x + 1) / bw, Y0 = sy0 + ch * y / bh, Y1 = sy0 + ch * (y + 1) / bh;
                long r = 0, g = 0, b = 0, n = 0;
                if (X1 <= X0) X1 = X0 + 1; if (Y1 <= Y0) Y1 = Y0 + 1;
                for (int yy = Y0; yy < Y1 && yy < out.h; yy++) for (int xx = X0; xx < X1 && xx < out.w; xx++) {
                    const uint8_t *q = out.px + ((size_t)yy * out.w + xx) * 4; r += q[0]; g += q[1]; b += q[2]; n++;
                }
                if (n) { uint8_t *d = canvas + ((size_t)(rb[1] + y) * ref.w + rb[0] + x) * 4;
                         d[0] = (uint8_t)(r / n); d[1] = (uint8_t)(g / n); d[2] = (uint8_t)(b / n); d[3] = 255; }
            }
            snprintf(op, sizeof op, "%s/out/pose_%04d.png", dir, ENG_CONT_POSE0 + cont_cells[k]);
            if (write_canvas(op, canvas, ref.w, ref.h) == 0) { wrote++; logf_("card %d: done", ENG_CONT_POSE0 + cont_cells[k]); }
            free(canvas); free(ref.px);
        }
        free(out.px);
    }
    return wrote;
}
int tool_art_run(const char *dir, const char *anchor, const char *descfile)
{
    char p[512]; FILE *f; DIR *d; struct dirent *e;
    int ok = 0, sheets_done = 0;
    int miss[1024], nmiss = 0;
    static int low[1024]; int nlow = 0;   /* lying/low figures: 2x2 sheets */
    static int ports[64]; int nports = 0;  /* portrait surfaces 800+: continue faces, title card */
    jobdir = dir;
    signal(SIGPIPE, SIG_IGN);          /* the editor closing must NOT kill the
                                          batch: orphaned runs keep grinding
                                          headless (run.log + run_state carry
                                          on; stdout just goes nowhere) */
    {   /* GLOBAL single-instance lock (jobs/.art-run.lock): every run claims
           its results from the one ~/.codex/generated_images cache, so two
           runs on DIFFERENT jobs can cross-claim each other's images (a
           Perfect landed in Honky's batch, 2026-08-25) — one art-run per
           machine, whichever job dir */
        FILE *g = fopen("jobs/.art-run.lock", "r");
        if (g) {
            long gpid = 0; char gdir[512] = "";
            if (fscanf(g, "%ld %511s", &gpid, gdir) >= 1 && gpid > 0 && kill((pid_t)gpid, 0) == 0) {
                fclose(g);
                fprintf(stderr, "art-run: another run (pid %ld, %s) is active - only one art-run at a time (shared codex image cache)\n", gpid, gdir);
                return 1;
            }
            fclose(g);
        }
        g = fopen("jobs/.art-run.lock", "w");
        if (g) { fprintf(g, "%d %s\n", (int)getpid(), dir); fclose(g); }
    }
    {   /* per-job lock: a fresh GO while an orphaned run is still
           alive would double-write out/ and cross-claim codex results */
        struct stat st_;
        snprintf(p, sizeof p, "%s/run.lock", dir);
        f = fopen(p, "r");
        if (f) {
            long oldpid = 0;
            if (fscanf(f, "%ld", &oldpid) == 1 && oldpid > 0 && kill((pid_t)oldpid, 0) == 0) {
                fclose(f);
                fprintf(stderr, "art-run: another run (pid %ld) is still active on %s - "
                                "it survived an editor close; Cancel it or let it finish\n",
                        oldpid, dir);
                return 1;
            }
            fclose(f);
        }
        (void)st_;
        f = fopen(p, "w");
        if (f) { fprintf(f, "%d\n", (int)getpid()); fclose(f); }
    }

    if (load_desc(descfile)) return 1;

    snprintf(p, sizeof p, "%s/out", dir); mkdir(p, 0775);
    snprintf(p, sizeof p, "%s/run.log", dir);
    runlog = fopen(p, "a");

    /* worklist: refs minus finished minus partial */
    snprintf(p, sizeof p, "%s/ref", dir);
    d = opendir(p);
    if (!d) { fprintf(stderr, "art-run: no %s\n", p); return 1; }
    while ((e = readdir(d))) {
        int pose; struct stat st; img ref; int rb[4];
        if (sscanf(e->d_name, "pose_%d.png", &pose) != 1) continue;
        snprintf(p, sizeof p, "%s/out/pose_%04d.png", dir, pose);
        if (stat(p, &st) == 0) continue;
        snprintf(p, sizeof p, "%s/ref/pose_%04d.png", dir, pose);
        if (iload_ref(p, &ref)) continue;
        if (pose >= 800 && pose < 1024) {   /* PORTRAIT surface: its own pass (portrait recipe) */
            free(ref.px);
            if (nports < 64) ports[nports++] = pose;
            continue;
        }
        ref_bbox(&ref, rb);
        {   /* fragment = FEW OPAQUE PIXELS, not a short bbox: a lying body
               is under 60px tall but very much a full figure (playtest:
               downed poses came out as base fallbacks) */
            long alpha_n = 0;
            for (int i2 = 0; i2 < ref.w * ref.h; i2++)
                if (ref.px[(size_t)i2*4+3]) alpha_n++;
            free(ref.px);
            if (alpha_n < 2500) {
            /* partial/overlay piece (boot, limbs): its OWN pass with the
               fragment prompt (user 2026-08-25: no base-art pieces popping
               up amid the new skin) */
                if (nfrags < 256) frags[nfrags++] = pose;
                continue;
            }
            if (rb[3] - rb[1] < 70) {   /* LYING / low figure: 4x4 cells lose it
                                           (stock-class2 trial 2026-08-25) — its
                                           own 2x2 pass */
                if (nlow < 1024) low[nlow++] = pose;
                continue;
            }
        }
        if (nposes < 1024) poses[nposes++] = pose;
    }
    closedir(d);
    {   /* sort ascending */
        for (int i = 1; i < nposes; i++) {
            int v = poses[i], j = i - 1;
            while (j >= 0 && poses[j] > v) { poses[j+1] = poses[j]; j--; }
            poses[j+1] = v;
        }
    }
    {   /* sort the fragments too */
        for (int i = 1; i < nfrags; i++) {
            int v = frags[i], j = i - 1;
            while (j >= 0 && frags[j] > v) { frags[j+1] = frags[j]; j--; }
            frags[j+1] = v;
        }
    }
    logf_("todo %d poses + %d low/lying (2x2) + %d fragments + %d cards (%s)", nposes, nlow, nfrags, nports, dir);
    if (getenv("WF_ART_CARDS")) {   /* "4 Animation": the cards ONLY (own button, own row) */
        const char *which = getenv("WF_ART_CARDS");   /* faces | title | anything = both */
        int faces = 0;
        for (int k = 0; k < nports; k++) if (ports[k] < ENG_CONT_POSE0 + 8) faces++;
        if (faces && strcmp(which, "title")) ok += cards_sheet(dir, anchor, ports, nports);
        if (strcmp(which, "faces"))
            for (int k = 0; k < nports; k++)
                if (ports[k] >= ENG_CONT_POSE0 + 8 && portrait_one(dir, anchor, ports[k], NULL) == 0) ok++;
        logf_("cards done: %d written (%d were pending)", ok, nports);
        state_json(ok, nports, ok, nports - ok, "done");
        if (runlog) fclose(runlog);
        return 0;
    }
    if (nports) logf_("%d cards pending - they are the 4 Animation button's job, not this run's", nports);

    /* sheets (WF_ART_SHEETS=N stops after N sheets — a trial batch to eye):
       tall figures 4x4, lying/low figures 2x2 (512px cells) */
    for (int pass = 0; pass < 2; pass++) {
    /* WF_ART_GRID=2|3|4 (user 2026-08-27): the tall-figure sheet size - 4 =
       256 px per figure (the pixelated conversions), 3 = 341 px, 2 = 512 px
       (~150 calls for a 600-frame skin); lying/low figures stay 2x2 */
    int main_grid = getenv("WF_ART_GRID") ? atoi(getenv("WF_ART_GRID")) : GRID;
    if (main_grid < 1 || main_grid > 4) main_grid = GRID;   /* 1 = one pose per call (the single prompt) */
    int *list = pass ? low : poses, ln = pass ? nlow : nposes, grid = pass ? 2 : main_grid;
    for (int c0 = 0; c0 < ln; c0 += grid*grid) {
        if (getenv("WF_ART_SHEETS") && sheets_done >= atoi(getenv("WF_ART_SHEETS"))) { logf_("stopping after %d sheet(s) (WF_ART_SHEETS)", sheets_done); break; }
        int n = ln - c0 < grid*grid ? ln - c0 : grid*grid;
        int cell = 1024 / grid;
        static uint8_t sheet[1024*1024*3];
        char sp[512], gen[1024]; img out;
        for (int i = 0; i < 1024*1024; i++) {   /* magenta ground */
            sheet[i*3] = 255; sheet[i*3+1] = 0; sheet[i*3+2] = 255;
        }
        for (int i = 0; i < n; i++) {
            img ref;
            snprintf(p, sizeof p, "%s/ref/pose_%04d.png", dir, list[c0+i]);
            if (iload_ref(p, &ref)) continue;
            paste_gray(sheet, 1024, &ref, (i % grid)*cell, (i / grid)*cell, cell);
            free(ref.px);
        }
        snprintf(sp, sizeof sp, "%s/work_sheet.png", dir);
        write_rgb_png(sp, sheet, 1024, 1024);
        logf_("%ssheet %d/%d (%d poses from %d)", pass ? "low 2x2 " : "", c0/(grid*grid)+1,
              (ln + grid*grid - 1)/(grid*grid), n, list[c0]);
        state_json(ok, nposes + nlow, ok, nmiss, "sheets");
        if (codex_exec(sp, anchor, grid == 1 ? single_prompt() : sheet_prompt(n, grid), gen, sizeof gen)) {
            for (int i = 0; i < n && nmiss < 1024; i++) miss[nmiss++] = list[c0+i];
            continue;
        }
        if (iload(gen, &out)) continue;
        key_magenta(&out);
        for (int i = 0; i < n; i++) {
            int cs = out.w / grid;
            if (slice_accept(&out, (i % grid)*cs, (i / grid)*cs,
                             (i % grid)*cs + cs, (i / grid)*cs + cs, list[c0+i]))
                ok++;
            else if (nmiss < 1024) miss[nmiss++] = list[c0+i];
            state_json(ok, nposes + nlow, ok, nmiss, "sheets");   /* live meter per pose */
        }
        free(out.px);
        sheets_done++;
        logf_("sheet done: ok %d miss %d", ok, nmiss);
    }
    }

    /* PORTRAIT surfaces (user 2026-08-25: part of a skin's generation list):
       one request each, the ROM card as the composition/style example + the
       anchor; the result is box-downscaled to the card's size and pasted at
       the card's place on the canvas. Continue faces (800+cell): keep the
       backdrop, border and EXPRESSION, replace the man. Title card (810): the
       pointing victory pose on solid magenta, keyed. */

    /* fragment sheets: same machinery, fragment prompt, loose gates */
    for (int c0 = 0; c0 < nfrags; c0 += GRID*GRID) {
        int n = nfrags - c0 < GRID*GRID ? nfrags - c0 : GRID*GRID;
        int cell = 1024 / GRID;
        static uint8_t sheet[1024*1024*3];
        char sp[512], gen[1024]; img out;
        for (int i = 0; i < 1024*1024; i++) {
            sheet[i*3] = 255; sheet[i*3+1] = 0; sheet[i*3+2] = 255;
        }
        for (int i = 0; i < n; i++) {
            img ref;
            snprintf(p, sizeof p, "%s/ref/pose_%04d.png", dir, frags[c0+i]);
            if (iload_ref(p, &ref)) continue;
            paste_gray(sheet, 1024, &ref, (i % GRID)*cell, (i / GRID)*cell, cell);
            free(ref.px);
        }
        snprintf(sp, sizeof sp, "%s/work_sheet.png", dir);
        write_rgb_png(sp, sheet, 1024, 1024);
        logf_("fragment sheet %d/%d (%d pieces)", c0/(GRID*GRID)+1,
              (nfrags + GRID*GRID - 1)/(GRID*GRID), n);
        state_json(ok, nposes + nfrags, ok, nmiss, "fragments");
        if (codex_exec(sp, anchor, frag_sheet_prompt(n), gen, sizeof gen)) {
            for (int i = 0; i < n && nmiss < 1024; i++) miss[nmiss++] = frags[c0+i];
            continue;
        }
        if (iload(gen, &out)) continue;
        key_magenta(&out);
        for (int i = 0; i < n; i++) {
            int cs = out.w / GRID;
            if (slice_accept2(&out, (i % GRID)*cs, (i / GRID)*cs,
                              (i % GRID)*cs + cs, (i / GRID)*cs + cs,
                              frags[c0+i], 0.20, 60))
                ok++;
            else if (nmiss < 1024) miss[nmiss++] = frags[c0+i];
            state_json(ok, nposes + nfrags, ok, nmiss, "fragments");
        }
    }

    /* re-rolls as singles */
    logf_("re-roll pass: %d", nmiss);
    state_json(ok, nposes, ok, nmiss, "re-rolls");
    for (int i = 0; i < nmiss; i++) {
        char sp[512], gen[1024]; img ref, out;
        static uint8_t flat[1024*1024*3];
        snprintf(p, sizeof p, "%s/ref/pose_%04d.png", dir, miss[i]);
        if (iload_ref(p, &ref)) continue;
        for (int k = 0; k < 1024*1024; k++) {
            flat[k*3] = 255; flat[k*3+1] = 0; flat[k*3+2] = 255;
        }
        paste_gray(flat, 1024, &ref, 0, 0, 1024);
        free(ref.px);
        snprintf(sp, sizeof sp, "%s/work_single.png", dir);
        write_rgb_png(sp, flat, 1024, 1024);
        {
            int isfrag = 0;
            for (int k = 0; k < nfrags; k++) if (frags[k] == miss[i]) { isfrag = 1; break; }
            if (codex_exec(sp, anchor, isfrag ? frag_single_prompt() : single_prompt(),
                           gen, sizeof gen)) continue;
            if (iload(gen, &out)) continue;
            key_magenta(&out);
            if (slice_accept2(&out, 0, 0, out.w, out.h, miss[i],
                              isfrag ? 0.20 : IOU_OK, isfrag ? 60 : 400)) ok++;
            else logf_("pose %d: base fallback", miss[i]);
        }
        free(out.px);
        state_json(ok, nposes, ok, nmiss - i - 1, "re-rolls");
    }
    state_json(ok, nposes, ok, 0, "done");
    logf_("RUN COMPLETE: ok %d of %d", ok, nposes);
    if (runlog) fclose(runlog);
    snprintf(p, sizeof p, "%s/run.lock", dir);
    unlink(p);
    return 0;
}

/* ---- wfeditor Skins tab verbs (docs/ai-art-pipeline.md) ---- */

/* --art-anchor JOBDIR CHARDESC OUT.png: one codex call — the target
 * character in the base's pose-0 mannequin. Review, Accept, repeat. */
int tool_art_anchor(const char *dir, const char *descfile, const char *outpng)
{
    char p[512], sp[512], gen[1024];
    jobdir = dir;
    if (load_desc(descfile)) return 1;
    snprintf(p, sizeof p, "%s/ref/pose_0000.png", dir);
    {   /* the class's GENERIC base ref (user 2026-08-26: "set them as the BASE
         * reference for each class") - the job's manifest names its class;
         * data/generics/<class>/base_ref.png replaces grey <stock base> at
         * the identity step. WF_ART_BASE_REF overrides; missing = old path. */
        char mp[600], gp[256], err[128]; json_val *doc; int cls = -1;
        snprintf(mp, sizeof mp, "%s/manifest.json", dir);
        doc = json_parse_file(mp, err, sizeof err);
        if (doc) { cls = (int)json_int(json_get(doc, "class"), -1); json_free(doc); }
        if (cls < 0) { extern int eng_ws_body_class(int); int base = -1;
            snprintf(mp, sizeof mp, "%s/skin.json", dir);
            doc = json_parse_file(mp, err, sizeof err);
            if (doc) { base = (int)json_int(json_get(doc, "base"), -1); json_free(doc); }
            if (base >= 0) cls = eng_ws_body_class(base); }
        if (cls >= 0) { snprintf(gp, sizeof gp, "data/generics/%d/base_ref.png", cls); if (access(gp, R_OK) == 0) snprintf(p, sizeof p, "%s", gp); }
        if (getenv("WF_ART_BASE_REF") && getenv("WF_ART_BASE_REF")[0])
            snprintf(p, sizeof p, "%s", getenv("WF_ART_BASE_REF"));
        fprintf(stderr, "art-anchor: base ref %s\n", p);
    }
    snprintf(sp, sizeof sp, "%s/work_anchor_in.png", dir);
    if (make_single_input(p, sp)) { fprintf(stderr, "art-anchor: no %s\n", p); return 1; }
    if (codex_exec(sp, sp, anchor_prompt(), gen, sizeof gen)) {
        fprintf(stderr, "art-anchor: codex produced nothing\n"); return 1;
    }
    {
        img m;
        if (iload(gen, &m)) return 1;
        key_magenta(&m);
        if (wf_art_write_rgba_png(outpng, m.px, m.w, m.h)) { free(m.px); return 1; }
        free(m.px);
        art_stamp(outpng);
    }
    fprintf(stderr, "art-anchor: candidate -> %s\n", outpng);
    return 0;
}

/* --art-portrait STYLE.png ANCHOR.png CHARDESC OUT.png: one codex call —
 * the digitized-photo select portrait on solid cyan. RAW result. */
int tool_art_portrait(const char *style, const char *anchor,
                      const char *descfile, const char *outpng)
{
    char gen[1024];
    static char pp[4096];
    if (load_desc(descfile)) return 1;
    snprintf(pp, sizeof pp, "%s", select_prompt());
    if (codex_exec(style, anchor, pp, gen, sizeof gen)) {
        fprintf(stderr, "art-portrait: codex produced nothing\n"); return 1;
    }
    {
        img m;
        if (iload(gen, &m)) return 1;
        if (wf_art_write_rgba_png(outpng, m.px, m.w, m.h)) { free(m.px); return 1; }
        free(m.px);
        art_stamp(outpng);
    }
    fprintf(stderr, "art-portrait: candidate -> %s\n", outpng);
    return 0;
}

/* --art-portrait-install RAW.png DST.png: BOX-downscale to the 80x80
 * select-cell format and quantize to 14 colours + the cyan panel
 * (charselect cell_png_load: corner colour = pen 15, <= 14 others). */
int tool_art_portrait_install(const char *rawpng, const char *dst)
{
    img m;
    static uint8_t cell[80*80*4];
    uint32_t cols[14]; int ncol = 0;
    if (iload(rawpng, &m)) { fprintf(stderr, "portrait-install: cannot read %s\n", rawpng); return 1; }
    /* BOX downscale to 80x80 */
    for (int y = 0; y < 80; y++)
        for (int x = 0; x < 80; x++) {
            long r = 0, g = 0, b = 0, n = 0;
            int x0 = x * m.w / 80, x1 = (x+1) * m.w / 80;
            int y0 = y * m.h / 80, y1 = (y+1) * m.h / 80;
            for (int sy = y0; sy < (y1 > y0 ? y1 : y0+1); sy++)
                for (int sx = x0; sx < (x1 > x0 ? x1 : x0+1); sx++) {
                    const uint8_t *p = m.px + ((size_t)sy*m.w + sx)*4;
                    r += p[0]; g += p[1]; b += p[2]; n++;
                }
            cell[((size_t)y*80+x)*4] = (uint8_t)(r/n);
            cell[((size_t)y*80+x)*4+1] = (uint8_t)(g/n);
            cell[((size_t)y*80+x)*4+2] = (uint8_t)(b/n);
            cell[((size_t)y*80+x)*4+3] = 255;
        }
    free(m.px);
    /* snap the model's cyan to the exact panel colour, then a
     * diversity-floor 14-colour palette over the rest (ingest recipe) */
    for (int i = 0; i < 80*80; i++) {
        uint8_t *p = cell + (size_t)i*4;
        if (p[1] > 150 && p[2] > 150 && p[0] < 120) { p[0]=0; p[1]=238; p[2]=255; }
    }
    {
        static uint32_t hist[4096];
        memset(hist, 0, sizeof hist);
        for (int i = 0; i < 80*80; i++) {
            uint8_t *p = cell + (size_t)i*4;
            if (p[0]==0 && p[1]==238 && p[2]==255) continue;
            hist[((p[0]>>4)<<8) | ((p[1]>>4)<<4) | (p[2]>>4)]++;
        }
        for (int pass = 8; pass >= 0 && ncol < 14; pass--) {   /* diversity floor relaxes */
            for (int c = 0; c < 4096 && ncol < 14; c++) {
                int r = (c>>8)<<4, g = ((c>>4)&15)<<4, b = (c&15)<<4, far = 1;
                if (!hist[c]) continue;
                for (int k = 0; k < ncol; k++) {
                    int dr = r-(int)((cols[k]>>16)&0xFF), dg = g-(int)((cols[k]>>8)&0xFF), db = b-(int)(cols[k]&0xFF);
                    if (dr*dr + dg*dg + db*db < pass*pass*64) { far = 0; break; }
                }
                if (!far) continue;
                {   /* take the most popular far bucket this pass */
                    uint32_t bestn = 0; int bestc = -1;
                    for (int c2 = c; c2 < 4096; c2++) {
                        int r2=(c2>>8)<<4, g2=((c2>>4)&15)<<4, b2=(c2&15)<<4, far2=1;
                        if (hist[c2] <= bestn) continue;
                        for (int k = 0; k < ncol; k++) {
                            int dr=r2-(int)((cols[k]>>16)&0xFF), dg=g2-(int)((cols[k]>>8)&0xFF), db=b2-(int)(cols[k]&0xFF);
                            if (dr*dr+dg*dg+db*db < pass*pass*64) { far2=0; break; }
                        }
                        if (far2) { bestn = hist[c2]; bestc = c2; }
                    }
                    if (bestc < 0) break;
                    cols[ncol++] = ((uint32_t)(((bestc>>8)<<4)|8) << 16)
                                 | ((uint32_t)((((bestc>>4)&15)<<4)|8) << 8)
                                 |  (uint32_t)(((bestc&15)<<4)|8);
                }
            }
        }
        for (int i = 0; i < 80*80; i++) {   /* nearest palette colour */
            uint8_t *p = cell + (size_t)i*4;
            int bd = 1<<30, bk = -1;
            if (p[0]==0 && p[1]==238 && p[2]==255) continue;
            for (int k = 0; k < ncol; k++) {
                int dr = p[0]-(int)((cols[k]>>16)&0xFF), dg = p[1]-(int)((cols[k]>>8)&0xFF), db = p[2]-(int)(cols[k]&0xFF);
                int d = dr*dr + dg*dg + db*db;
                if (d < bd) { bd = d; bk = k; }
            }
            if (bk >= 0) { p[0]=(cols[bk]>>16)&0xFF; p[1]=(cols[bk]>>8)&0xFF; p[2]=cols[bk]&0xFF; }
        }
    }
    cell[0]=0; cell[1]=238; cell[2]=255;      /* corner = panel pen */
    if (wf_art_write_rgba_png(dst, cell, 80, 80)) return 1;
    fprintf(stderr, "portrait-install: %s (%d colours + panel)\n", dst, ncol);
    return 0;
}

/* --art-pose JOBDIR ANCHOR CHARDESC POSE: manually regenerate ONE pose
 * as a single frame (review browser: "a botched image the QC did not
 * catch"). The human is the judge — a very loose IoU only fences off
 * outright garbage; success REPLACES the out frame. */
int tool_art_pose(const char *dir, const char *anchor, const char *descfile, int pose)
{
    extern const char *wf_art_pose_extra;   /* main.c: optional per-roll direction */
    char p[512], sp[512], gen[1024];
    img ref, out; int sb[4], rb[4];
    static uint8_t cv[CANVAS*CANVAS*4];
    double q;
    jobdir = dir;
    if (load_desc(descfile)) return 1;
    snprintf(p, sizeof p, "%s/run.log", dir);
    runlog = fopen(p, "a");
    if (pose >= 800 && pose < 1024) {   /* a CARD (continue face / title card): the portrait recipe, not the mannequin one */
        int rc = portrait_one(dir, anchor, pose, wf_art_pose_extra);
        if (rc == 0) logf_("pose %d: REPLACED (portrait) — judge it in the review browser", pose);
        if (runlog) fclose(runlog);
        return rc ? 1 : 0;
    }
    int isfrag = 0;
    snprintf(p, sizeof p, "%s/ref/pose_%04d.png", dir, pose);
    {   /* fragment refs get the fragment prompt — classified by opaque
           pixel count (a lying body is short but a full figure) */
        img rf;
        if (iload(p, &rf) == 0) {
            long alpha_n = 0;
            for (int i2 = 0; i2 < rf.w * rf.h; i2++)
                if (rf.px[(size_t)i2*4+3]) alpha_n++;
            free(rf.px);
            if (alpha_n < 2500) isfrag = 1;
        }
    }
    snprintf(sp, sizeof sp, "%s/work_single.png", dir);
    if (make_single_input(p, sp)) { logf_("pose %d: no ref", pose); return 1; }
    logf_("manual re-roll: pose %d%s%s", pose,
          wf_art_pose_extra ? " — " : "", wf_art_pose_extra ? wf_art_pose_extra : "");
    {
        const char *pp;
        if (wf_art_pose_extra && wf_art_pose_extra[0] == '=') {
            /* REPLACE mode (editor checkbox unchecked): the artist's text IS
               the whole prompt — the C scaffold steps aside. The magenta
               chroma key still runs downstream, so the prompt had better
               ask for a magenta background. */
            snprintf(prompt, sizeof prompt, "%s", wf_art_pose_extra + 1);
            pp = prompt;
        } else {
            pp = pk_expand(isfrag ? PK_FRAG_SINGLE : PK_SINGLE, 1, 1, wf_art_pose_extra);   /* {EXTRA} = the artist's direction */
        }
        if (codex_exec(sp, anchor, pp, gen, sizeof gen)) {
            logf_("pose %d: codex produced nothing (old frame kept)", pose); return 1;
        }
    }
    if (iload(gen, &out)) return 1;
    key_magenta(&out);
    if (!main_component(&out, 0, 0, out.w, out.h, sb)) {
        logf_("pose %d: no figure in the result (old frame kept)", pose);
        free(out.px); return 1;
    }
    if (iload_ref(p, &ref)) { free(out.px); return 1; }
    ref_bbox(&ref, rb);
    place_ref_feet_cx = feet_cx(ref.px, ref.w, 0, rb);
    place(&out, sb, rb, cv);
    q = iou_vs_ref(cv, &ref);
    if (q >= 0.20) write_hi(dir, pose, &out, sb, rb);
    place_ref_feet_cx = -1;
    free(ref.px); free(out.px);
    if (q < 0.20) { logf_("pose %d: result is garbage (IoU %.2f, old frame kept)", pose, q); return 1; }
    snprintf(p, sizeof p, "%s/out/pose_%04d.png", dir, pose);
    if (write_canvas(p, cv, CANVAS, CANVAS)) return 1;
    logf_("pose %d: REPLACED (IoU %.2f) — judge it in the review browser", pose, q);
    if (runlog) fclose(runlog);
    return 0;
}

/* --art-base-ref CLASS IN.png OUT.png (user 2026-08-26): install a GENERIC
 * BASE REFERENCE for a body class — a generated neutral man in the stance
 * (an --art-anchor result made from the class's stock stance with a plain
 * description) placed onto the pose canvas exactly like a pose result:
 * feet line and feet centroid from the class's stock stance ref, body
 * height scaled to it, IoU QC. From then on the anchor step reads this
 * (WF_ART_BASE_REF) instead of grey <stock base>, so no stock identity
 * reaches the model at the identity step. */
int tool_art_base_ref(int cls, const char *in, const char *out)
{
    char p[512]; img src, ref; int sb[4], rb[4]; double q;
    static uint8_t cv[CANVAS*CANVAS*4];
    snprintf(p, sizeof p, "jobs/stock-class%d/ref/pose_0000.png", cls);
    if (iload_ref(p, &ref)) { fprintf(stderr, "art-base-ref: no class stance %s (run --class-template %d first)\n", p, cls); return 1; }
    if (iload(in, &src)) { free(ref.px); fprintf(stderr, "art-base-ref: cannot read %s\n", in); return 1; }
    key_magenta(&src);
    if (!main_component(&src, 0, 0, src.w, src.h, sb)) { fprintf(stderr, "art-base-ref: no figure in %s\n", in); free(src.px); free(ref.px); return 1; }
    ref_bbox(&ref, rb);
    {   /* match the stance's WIDTH too (height is matched by place()): the
         * model draws the build a few px off; a per-axis scale of that size
         * is invisible in pixel art and keeps every class's proportions
         * exactly the stock's (user 2026-08-26) */
        int sw = sb[2]-sb[0], sh = sb[3]-sb[1], rh = rb[3]-rb[1], rw = rb[2]-rb[0];
        int nw = sh ? sw * rh / sh : sw, saved = art_width_pct;
        art_width_pct = nw ? (rw * 100 + nw - 1) / nw : 100;   /* ceil: place() truncates */
        if (art_width_pct < 80) art_width_pct = 80; if (art_width_pct > 120) art_width_pct = 120;
        place_ref_feet_cx = feet_cx(ref.px, ref.w, 0, rb);
        place(&src, sb, rb, cv);
        place_ref_feet_cx = -1;
        fprintf(stderr, "art-base-ref: class %d: width scale %d%% (drawn %d px at stance height, stance %d px)\n", cls, art_width_pct, nw, rw);
        art_width_pct = saved;
    }
    q = iou_vs_ref(cv, &ref);
    free(src.px); free(ref.px);
    if (q < 0.5) { fprintf(stderr, "art-base-ref: class %d: silhouette IoU %.2f vs the stock stance - too far off, not installed\n", cls, q); return 1; }
    if (write_canvas(out, cv, CANVAS, CANVAS)) return 1;
    fprintf(stderr, "art-base-ref: class %d: %s -> %s (IoU %.2f vs the stock stance, height matched)\n", cls, in, out, q);
    return 0;
}

/* --art-align JOBDIR: re-align every existing out frame's FEET CENTROID
 * to its ref's (pure horizontal shift, no regeneration) — repairs the
 * consistent bbox-centre skew in already-generated skins. */
/* SILHOUETTE alignment (user playtest 2026-08-25: "sometimes too close,
 * sometimes too far" in holds): the feet centroid is wrong for leaning /
 * crouched / arms-out poses, so search a small window for the shift that
 * maximises mask IoU with the mannequin — x and y. Returns the best IoU;
 * *bdx/*bdy = shift to apply to the out frame. */
static double best_shift(const img *of, const img *rf, int *bdx, int *bdy)
{
    double best = -1; int W = of->w, H = of->h;
    *bdx = *bdy = 0;
    for (int dy = -8; dy <= 8; dy++)
        for (int dx = -16; dx <= 16; dx++) {
            long inter = 0, uni = 0;
            for (int y = 0; y < H; y++) {
                int sy = y - dy; if (sy < 0 || sy >= H) { for (int x = 0; x < W; x++) uni += rf->px[((size_t)y*W+x)*4+3] > 0; continue; }
                for (int x = 0; x < W; x++) {
                    int sx = x - dx, a = rf->px[((size_t)y*W+x)*4+3] > 0, b;
                    b = sx >= 0 && sx < W && of->px[((size_t)sy*W+sx)*4+3] > 0;
                    inter += a & b; uni += a | b;
                }
            }
            if (uni && (double)inter / uni > best) { best = (double)inter / uni; *bdx = dx; *bdy = dy; }
        }
    return best;
}
int tool_art_align_iou(const char *dir)
{
    char p[512]; DIR *d; struct dirent *e; int fixed = 0, seen = 0;
    jobdir = dir;
    snprintf(p, sizeof p, "%s/out", dir);
    d = opendir(p);
    if (!d) { fprintf(stderr, "art-align: no %s\n", p); return 1; }
    while ((e = readdir(d))) {
        int pose; img of, rf; int dx, dy; double iou;
        char op[560], rp[560];
        if (sscanf(e->d_name, "pose_%d.png", &pose) != 1) continue;
        snprintf(op, sizeof op, "%s/out/pose_%04d.png", dir, pose);
        snprintf(rp, sizeof rp, "%s/ref/pose_%04d.png", dir, pose);
        if (iload_ref(op, &of)) continue;
        if (iload_ref(rp, &rf) || rf.w != of.w || rf.h != of.h) { free(of.px); continue; }
        seen++;
        iou = best_shift(&of, &rf, &dx, &dy);
        free(rf.px);
        if ((dx || dy) && iou > 0.3) {
            uint8_t *nw2 = calloc((size_t)of.w * of.h, 4);
            if (nw2) {
                for (int y = 0; y < of.h; y++)
                    for (int x = 0; x < of.w; x++) {
                        int tx = x + dx, ty = y + dy;
                        if (tx >= 0 && tx < of.w && ty >= 0 && ty < of.h)
                            memcpy(nw2 + (((size_t)ty*of.w)+tx)*4, of.px + (((size_t)y*of.w)+x)*4, 4);
                    }
                if (write_canvas(op, nw2, of.w, of.h) == 0) {
                    fixed++;
                    fprintf(stderr, "art-align: pose %04d shifted %+d,%+d px (IoU %.2f)\n", pose, dx, dy, iou);
                }
                free(nw2);
            }
        }
        free(of.px);
    }
    closedir(d);
    fprintf(stderr, "art-align: %d of %d frames re-aligned by silhouette -> %s/out\n", fixed, seen, dir);
    return 0;
}
int tool_art_align(const char *dir)
{
    char p[512]; DIR *d; struct dirent *e; int fixed = 0, seen = 0;
    jobdir = dir;
    snprintf(p, sizeof p, "%s/out", dir);
    d = opendir(p);
    if (!d) { fprintf(stderr, "art-align: no %s\n", p); return 1; }
    while ((e = readdir(d))) {
        int pose; img of, rf; int ob[4], rb[4], dx;
        char op[560], rp[560];
        if (sscanf(e->d_name, "pose_%d.png", &pose) != 1) continue;
        snprintf(op, sizeof op, "%s/out/pose_%04d.png", dir, pose);
        snprintf(rp, sizeof rp, "%s/ref/pose_%04d.png", dir, pose);
        if (iload(op, &of)) continue;
        if (iload_ref(rp, &rf)) { free(of.px); continue; }
        seen++;
        ref_bbox(&of, ob); ref_bbox(&rf, rb);
        dx = feet_cx(rf.px, rf.w, 0, rb) - feet_cx(of.px, of.w, 0, ob);
        free(rf.px);
        if (dx != 0 && dx > -32 && dx < 32) {
            uint8_t *nw2 = calloc((size_t)of.w * of.h, 4);
            if (nw2) {
                for (int y = 0; y < of.h; y++)
                    for (int x = 0; x < of.w; x++) {
                        int tx = x + dx;
                        if (tx >= 0 && tx < of.w)
                            memcpy(nw2 + (((size_t)y*of.w)+tx)*4,
                                   of.px + (((size_t)y*of.w)+x)*4, 4);
                    }
                if (write_canvas(op, nw2, of.w, of.h) == 0) {
                    fixed++;
                    fprintf(stderr, "art-align: pose %04d shifted %+d px\n", pose, dx);
                }
                free(nw2);
            }
        }
        free(of.px);
    }
    closedir(d);
    fprintf(stderr, "art-align: %d of %d frames re-aligned -> %s/out\n", fixed, seen, dir);
    return 0;
}

/* --art-prompts CHARDESC OUT.txt: every recipe with the prompt variables
 * filled in, for the Skins tab's prompt page (user 2026-08-25: "I want to
 * see ALL the prompting data"). */
int tool_art_prompts(const char *descfile, const char *out)
{
    FILE *f;
    if (load_desc(descfile)) return 1;
    f = fopen(out, "w");
    if (!f) return 1;
    for (int k = 0; k < PK_N; k++)
        fprintf(f, "== %s ==\n%s\n\n", pk_title[k], pk_expand(k, k == PK_SHEET ? 16 : k == PK_CARDS ? 6 : 4, k == PK_CARDS ? 3 : 4, ""));
    fclose(f);
    return 0;
}

/* --art-key IN.png OUT.png MAXW MAXH: key the magenta background (border-
 * connected only), crop to the opaque box and BOX-downscale to fit MAXW x
 * MAXH keeping the aspect - one PNG in, an alpha PNG out, e.g. a codex
 * image -> a badge (data/badges: the knocked-out referee, 2026-08-30). */
int tool_art_key(const char *in, const char *out, int maxw, int maxh)
{
    img m; int x0, y0, x1, y1, bw, bh, ow, oh; uint8_t *o;
    if (iload(in, &m)) { fprintf(stderr, "art-key: cannot read %s\n", in); return 1; }
    key_magenta(&m);
    x0 = m.w; y0 = m.h; x1 = -1; y1 = -1;
    for (int y = 0; y < m.h; y++) for (int x = 0; x < m.w; x++)
        if (m.px[((size_t)y * m.w + x) * 4 + 3] >= 128) { if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y; }
    if (x1 < 0) { fprintf(stderr, "art-key: %s: nothing opaque after the key\n", in); free(m.px); return 1; }
    bw = x1 - x0 + 1; bh = y1 - y0 + 1;
    ow = bw; oh = bh;
    if (maxw > 0 && ow > maxw) { oh = oh * maxw / ow; ow = maxw; }
    if (maxh > 0 && oh > maxh) { ow = ow * maxh / oh; oh = maxh; }
    if (ow < 1) ow = 1; if (oh < 1) oh = 1;
    o = calloc((size_t)ow * oh, 4);
    for (int y = 0; y < oh; y++) for (int x = 0; x < ow; x++) {
        long r = 0, g = 0, b = 0, n = 0, na = 0;
        int sx0 = x0 + x * bw / ow, sx1 = x0 + (x + 1) * bw / ow, sy0 = y0 + y * bh / oh, sy1 = y0 + (y + 1) * bh / oh;
        if (sx1 <= sx0) sx1 = sx0 + 1; if (sy1 <= sy0) sy1 = sy0 + 1;
        for (int sy = sy0; sy < sy1; sy++) for (int sx = sx0; sx < sx1; sx++) {
            const uint8_t *p = m.px + ((size_t)sy * m.w + sx) * 4; n++;
            if (p[3] < 128) continue;
            r += p[0]; g += p[1]; b += p[2]; na++;
        }
        if (na * 2 >= n && na) { uint8_t *d = o + ((size_t)y * ow + x) * 4; d[0] = (uint8_t)(r / na); d[1] = (uint8_t)(g / na); d[2] = (uint8_t)(b / na); d[3] = 255; }
    }
    {   /* 15 colours with a DIVERSITY floor (the badge packer keeps the 15 most
         * frequent: a white-shirted figure lost its skin and hair = "black and
         * white", user 2026-08-30): frequency order, but a candidate must sit at
         * least `pass` 4-bit steps from every colour already chosen, relaxing */
        static uint32_t hist[4096]; uint32_t chosen[15]; int nc = 0;
        memset(hist, 0, sizeof hist);
        for (int i = 0; i < ow * oh; i++) { const uint8_t *d = o + (size_t)i * 4; if (d[3]) hist[((d[0] >> 4) << 8) | ((d[1] >> 4) << 4) | (d[2] >> 4)]++; }
        for (int pass = 6; pass >= 0 && nc < 15; pass--) {
            for (;;) {
                int best = -1; uint32_t bc = 0;
                for (int c = 0; c < 4096; c++) {
                    int far = 1;
                    if (!hist[c] || hist[c] <= bc) continue;
                    for (int k = 0; k < nc && far; k++) {
                        int dr = (int)(c >> 8) - (int)(chosen[k] >> 8), dg = (int)((c >> 4) & 15) - (int)((chosen[k] >> 4) & 15), db = (int)(c & 15) - (int)(chosen[k] & 15);
                        if (dr < 0) dr = -dr; if (dg < 0) dg = -dg; if (db < 0) db = -db;
                        if (dr < pass && dg < pass && db < pass) far = 0;
                    }
                    if (far) { best = c; bc = hist[c]; }
                }
                if (best < 0 || nc >= 15) break;
                chosen[nc++] = (uint32_t)best; hist[best] = 0;
            }
        }
        for (int i = 0; i < ow * oh; i++) {          /* snap every pixel to the nearest chosen colour */
            uint8_t *d = o + (size_t)i * 4; int bk = 0, bd = 1 << 30;
            if (!d[3]) continue;
            for (int k = 0; k < nc; k++) {
                int dr = (int)(d[0] >> 4) - (int)(chosen[k] >> 8), dg = (int)(d[1] >> 4) - (int)((chosen[k] >> 4) & 15), db = (int)(d[2] >> 4) - (int)(chosen[k] & 15);
                int dd = dr * dr + dg * dg + db * db; if (dd < bd) { bd = dd; bk = k; }
            }
            d[0] = (uint8_t)((chosen[bk] >> 8) * 17); d[1] = (uint8_t)(((chosen[bk] >> 4) & 15) * 17); d[2] = (uint8_t)((chosen[bk] & 15) * 17);
        }
    }
    if (wf_art_write_rgba_png(out, o, ow, oh)) { free(o); free(m.px); return 1; }
    fprintf(stderr, "art-key: %s -> %s (%dx%d from a %dx%d box, 15 colours)\n", in, out, ow, oh, bw, bh);
    free(o); free(m.px);
    return 0;
}

/* ---------------------------------------------------------------- ARENA RECIPE
 * --art-arena NAME (user 2026-08-30, the Arenas > AI recipe tab): every
 * picture of the arena's two views is re-drawn by codex in a THEME.
 *   arenas/NAME/recipe.json  {"theme": "...", "prompt": "<template or empty>"}
 *   arenas/NAME/_gen/<view>/<file>.png   the results (resumable: done = skipped)
 *   arenas/NAME/_gen/run_state.json / run.log / last.txt (newest result)
 * Per picture: image 1 = the picture as it is now with its transparent
 * area painted magenta (the model sees the shape), image 2 = the STYLE
 * ANCHOR (the first picture of the run that came back, so one look holds
 * over the whole arena; crowd steps 1..N anchor on the theme's step 0);
 * the result is box-scaled back to the exact size and MASKED with the
 * original's alpha (a plane's transparent cells decide which plane shows -
 * the model may not paint into them).  The rope pictures and the side-rope
 * sprites are skipped (thin lines the model wrecks; they stay stock). */
static void box_scale(const img *s, uint8_t *d, int dw, int dh)
{
    for (int y = 0; y < dh; y++) {
        int sy0 = y * s->h / dh, sy1 = (y + 1) * s->h / dh; if (sy1 <= sy0) sy1 = sy0 + 1;
        for (int x = 0; x < dw; x++) {
            int sx0 = x * s->w / dw, sx1 = (x + 1) * s->w / dw, acc[4] = { 0, 0, 0, 0 }, n = 0;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            for (int yy = sy0; yy < sy1 && yy < s->h; yy++)
                for (int xx = sx0; xx < sx1 && xx < s->w; xx++) {
                    const uint8_t *p = s->px + ((size_t)yy * s->w + xx) * 4;
                    acc[0] += p[0]; acc[1] += p[1]; acc[2] += p[2]; acc[3] += p[3]; n++;
                }
            if (!n) n = 1;
            for (int c = 0; c < 4; c++) d[((size_t)y * dw + x) * 4 + c] = (uint8_t)(acc[c] / n);
        }
    }
}
static const char *arena_default_prompt =
    "Image 1 is a pixel-art picture from a 1991 arcade wrestling game, %dx%d pixels: %s. "
    "Repaint it in this theme: %s. "
    "Keep the exact composition and geometry - everything stays in the same place at the same size "
    "(ring, ropes, posts, rails, floor, crowd tiers are gameplay geometry); only the look changes. "
    "Pure magenta (#FF00FF) areas are transparent: leave them exactly magenta and paint nothing into them. "
    "Output one picture with the same %dx%d proportions, crisp 16-colour arcade pixel art, no text, no border, no frame.";
static const char *arena_anchor_note =
    " Image 2 is a picture of the same arena already repainted in this theme: match its style, palette and lighting exactly.";
static const char *arena_crowd_note =
    " Image 2 is the same crowd already repainted in this theme (animation step 0); image 1 is another step of the "
    "same crowd animation: reproduce image 2's look, changing the crowd figures only where they move between the steps.";
typedef struct { char file[64]; char what[160]; int is_crowd; } arena_pic;
static int arena_list(const char *vdir, int ringside, arena_pic *out, int max)
{
    char p[600], err[128]; json_val *doc; const json_val *L; int n = 0;
    snprintf(p, sizeof p, "%s/arena.json", vdir);
    doc = json_parse_file(p, err, sizeof err);
    if (!doc) return 0;
    L = json_get(json_get(doc, "layers"), "under");
    if (L) {
        const char *ov = json_str(json_get(L, "overlay"), NULL);
        if (ov && n < max) { snprintf(out[n].file, 64, "%s", ov); snprintf(out[n].what, 160, "%s", ringside ? "the ringside floor, the rails and the ring seen from outside" : "the static ring: mat, apron, posts and the floor around it (no crowd in it)"); out[n].is_crowd = 0; n++; }
        { int k = 0; for (const json_val *q = json_get(L, "frames") ? json_get(L, "frames")->child : NULL; q && n < max; q = q->next, k++) {
              snprintf(out[n].file, 64, "%s", json_str(q, "")); snprintf(out[n].what, 160, "the crowd tiers around the ring, crowd animation step %d", k); out[n].is_crowd = k > 0; n++; } }
    }
    L = json_get(json_get(doc, "layers"), "over");
    if (L && ringside) {
        int k = 0;
        for (const json_val *q = json_get(L, "frames") ? json_get(L, "frames")->child : NULL; q && n < max; q = q->next, k++) {
            snprintf(out[n].file, 64, "%s", json_str(q, "")); snprintf(out[n].what, 160, "the far crowd in front of the ringside floor, crowd animation step %d", k); out[n].is_crowd = k > 0; n++; }
    }
    json_free(doc);
    return n;
}
/* --art-arena NAME --scene VIEW (user 2026-08-30: "export the entire scene to
 * codex and ask it to edit it down"): image 1 = the packer's step-0 COMPOSITE
 * of the view (WF_ART_SRC_ARENA's, else the arena's own: build/arena-preview/
 * <arena>/<view>_0.png), ONE codex call, the result scaled to the view and
 * SLICED into every picture of the view by that picture's own alpha (the
 * layers are cut-outs of one composite) -> _gen/<view>/<file>.  Rope pictures
 * are not written (stock ropes). */
static int arena_scene(const char *name, const char *view, const char *theme, const char *tmpl)
{
    char base[300], gen[400], src[600], send[700], out_path[1024], prompt[8192], p[700]; img m, o; uint8_t *scaled;
    const char *srca = getenv("WF_ART_SRC_ARENA") && getenv("WF_ART_SRC_ARENA")[0] ? getenv("WF_ART_SRC_ARENA") : name;
    arena_pic pics[24]; int np, ringside = !strcmp(view, "ringside");
    snprintf(base, sizeof base, "arenas/%s", name); snprintf(gen, sizeof gen, "%s/_gen", base);
    mkdir(gen, 0775); snprintf(p, sizeof p, "%s/%s", gen, view); mkdir(p, 0775);
    jobdir = gen; snprintf(p, sizeof p, "%s/run.log", gen); runlog = fopen(p, "a");
    snprintf(src, sizeof src, "build/arena-preview/%s/%s_0.png", srca, view);
    if (iload(src, &o)) { logf_("scene: no composite %s (Pack the arena first)", src); return 1; }
    snprintf(p, sizeof p, "%s/%s", base, view); np = arena_list(p, ringside, pics, 24);
    /* the composite is opaque everywhere the planes cover; the rest (outside
       the world) goes magenta so the model leaves it alone */
    for (int k = 0; k < o.w * o.h; k++) if (o.px[k * 4 + 3] < 128) { o.px[k * 4] = 255; o.px[k * 4 + 1] = 0; o.px[k * 4 + 2] = 255; o.px[k * 4 + 3] = 255; }
    snprintf(send, sizeof send, "%s/_send.png", gen); wf_art_write_rgba_png(send, o.px, o.w, o.h);
    snprintf(prompt, sizeof prompt, tmpl[0] ? tmpl : arena_default_prompt, o.w, o.h, ringside ? "the whole ringside view of the arena as the game draws it: floor, rails, the ring seen from outside, the near and far crowd" : "the whole in-ring view of the arena as the game draws it: crowd tiers, ring, posts, ropes, rails and floor", theme, o.w, o.h);
    state_json(0, 1, 0, 0, view);
    logf_("scene %s: %s -> codex (%dx%d)", view, src, o.w, o.h);
    if (codex_exec(send, send, prompt, out_path, sizeof out_path) || iload(out_path, &m)) { logf_("scene %s: nothing came back", view); state_json(1, 1, 0, 1, "done"); free(o.px); return 2; }
    scaled = malloc((size_t)o.w * o.h * 4);
    if (m.w == o.w && m.h == o.h) memcpy(scaled, m.px, (size_t)o.w * o.h * 4); else { box_scale(&m, scaled, o.w, o.h); logf_("scene %s: %dx%d scaled to %dx%d", view, m.w, m.h, o.w, o.h); }
    snprintf(p, sizeof p, "%s/%s/_scene.png", gen, view); wf_art_write_rgba_png(p, scaled, o.w, o.h);   /* the whole result, for the eye */
    for (int i = 0; i < np; i++) {                         /* slice by each picture's alpha */
        char lp[700], dp[700]; img L;
        snprintf(lp, sizeof lp, "%s/%s/%s", base, view, pics[i].file); snprintf(dp, sizeof dp, "%s/%s/%s", gen, view, pics[i].file);
        if (iload(lp, &L) || L.w != o.w || L.h != o.h) { logf_("scene %s: %s skipped (size)", view, pics[i].file); continue; }
        for (int k = 0; k < o.w * o.h; k++) {
            uint8_t *q = L.px + (size_t)k * 4; const uint8_t *r = scaled + (size_t)k * 4;
            if (q[3] < 128) continue;
            if (r[0] > 150 && r[2] > 150 && r[1] < 110 && r[0] - r[1] > 80) continue;   /* residual magenta: keep the original pixel */
            q[0] = r[0]; q[1] = r[1]; q[2] = r[2]; q[3] = 255;
        }
        wf_art_write_rgba_png(dp, L.px, o.w, o.h); free(L.px);
        logf_("scene %s: %s sliced", view, pics[i].file);
    }
    { FILE *lf; snprintf(p, sizeof p, "%s/last.txt", gen); lf = fopen(p, "w"); if (lf) { fprintf(lf, "%s/%s/_scene.png\n", gen, view); fclose(lf); } }
    unlink(send); free(scaled); free(m.px); free(o.px);
    state_json(1, 1, 1, 0, "done");
    if (runlog) fclose(runlog);
    return 0;
}
/* only: "view/file.png" = redo that ONE picture (removed first, so it is not
 * skipped as done) - the manual, one-at-a-time review flow (user 2026-08-30) */
const char *wf_art_arena_only;
int tool_art_arena(const char *name)
{
    char base[300], gen[400], p[700], err[128], theme[2048] = "", tmpl[4096] = "", anchor[700] = "";
    json_val *doc; int todo = 0, done = 0, ok = 0, miss = 0;
    static const char *views[2] = { "ring", "ringside" };
    arena_pic pics[2][24]; int np[2];
    snprintf(base, sizeof base, "arenas/%s", name);
    snprintf(gen, sizeof gen, "%s/_gen", base);
    snprintf(p, sizeof p, "%s/recipe.json", base);
    doc = json_parse_file(p, err, sizeof err);
    if (!doc) { fprintf(stderr, "art-arena: no %s (%s)\n", p, err); return 1; }
    snprintf(theme, sizeof theme, "%s", json_str(json_get(doc, "theme"), ""));
    snprintf(tmpl, sizeof tmpl, "%s", json_str(json_get(doc, "prompt"), ""));
    json_free(doc);
    if (!theme[0]) { fprintf(stderr, "art-arena: recipe.json has no theme\n"); return 1; }
    if (wf_art_arena_only && !strncmp(wf_art_arena_only, "scene:", 6)) return arena_scene(name, wf_art_arena_only + 6, theme, tmpl);
    mkdir(gen, 0775);
    for (int v = 0; v < 2; v++) { snprintf(p, sizeof p, "%s/%s", gen, views[v]); mkdir(p, 0775); }
    jobdir = gen;
    snprintf(p, sizeof p, "%s/run.log", gen); runlog = fopen(p, "a");
    for (int v = 0; v < 2; v++) { snprintf(p, sizeof p, "%s/%s", base, views[v]); np[v] = arena_list(p, v, pics[v], 24); todo += np[v]; }
    logf_("arena %s: %d pictures, theme: %s", name, todo, theme);
    state_json(0, todo, 0, 0, "start");
    if (wf_art_arena_only && wf_art_arena_only[0]) { snprintf(p, sizeof p, "%s/%s", gen, wf_art_arena_only); unlink(p); }   /* redo: gone before the anchor search picks it */
    /* a previous run's first result is the anchor again (resume keeps one look) */
    for (int v = 0; v < 2 && !anchor[0]; v++) for (int i = 0; i < np[v] && !anchor[0]; i++) {
        snprintf(p, sizeof p, "%s/%s/%s", gen, views[v], pics[v][i].file);
        if (access(p, R_OK) == 0) snprintf(anchor, sizeof anchor, "%s", p);
    }
    for (int v = 0; v < 2; v++) for (int i = 0; i < np[v]; i++) {
        const arena_pic *pc = &pics[v][i];
        char src[700], dst[700], send[700], out_path[1024], prompt[8192], ref[700], rel[200]; img m, o; uint8_t *scaled;
        snprintf(src, sizeof src, "%s/%s/%s", base, views[v], pc->file);
        if (getenv("WF_ART_SRC_ARENA") && getenv("WF_ART_SRC_ARENA")[0])   /* image 1 from ANOTHER arena's same file (a clean stock picture
                                                                              to redraw plainly - the template's own pads were ragged) */
            snprintf(src, sizeof src, "arenas/%s/%s/%s", getenv("WF_ART_SRC_ARENA"), views[v], pc->file);
        if (getenv("WF_ART_SRC_FILE") && getenv("WF_ART_SRC_FILE")[0]) snprintf(src, sizeof src, "%s", getenv("WF_ART_SRC_FILE"));   /* image 1 = this exact file (a hand-edited picture) */
        snprintf(dst, sizeof dst, "%s/%s/%s", gen, views[v], pc->file);
        snprintf(rel, sizeof rel, "%s/%s", views[v], pc->file);
        if (wf_art_arena_only && wf_art_arena_only[0]) { if (strcmp(rel, wf_art_arena_only)) { todo--; continue; } unlink(dst); }
        if (access(dst, R_OK) == 0) { done++; ok++; state_json(done, todo, ok, miss, pc->file); continue; }
        state_json(done, todo, ok, miss, pc->file);
        if (iload(src, &m)) { logf_("%s: cannot read", src); miss++; done++; continue; }
        for (int k = 0; k < m.w * m.h; k++) if (m.px[k * 4 + 3] < 128) { m.px[k * 4] = 255; m.px[k * 4 + 1] = 0; m.px[k * 4 + 2] = 255; m.px[k * 4 + 3] = 255; }
        snprintf(send, sizeof send, "%s/_send.png", gen);
        wf_art_write_rgba_png(send, m.px, m.w, m.h);
        free(m.px);
        /* the reference: crowd steps > 0 anchor on the theme's step 0 of this view */
        ref[0] = 0;
        if (pc->is_crowd) {        /* crowd_3.png -> the theme's crowd_0.png of this view */
            char stem[64]; const char *us; snprintf(stem, sizeof stem, "%s", pc->file); us = strrchr(stem, '_'); if (us) stem[us - stem] = 0;
            snprintf(ref, sizeof ref, "%s/%s/%s_0.png", gen, views[v], stem);
            if (access(ref, R_OK) != 0) ref[0] = 0; }
        if (!ref[0] && anchor[0] && !getenv("WF_ART_NO_ANCHOR")) snprintf(ref, sizeof ref, "%s", anchor);   /* WF_ART_NO_ANCHOR=1: judge this picture on its own */
        iload(src, &o);            /* the original again: size + alpha mask */
        snprintf(prompt, sizeof prompt, tmpl[0] ? tmpl : arena_default_prompt, o.w, o.h, pc->what, theme, o.w, o.h);
        if (ref[0]) strncat(prompt, pc->is_crowd ? arena_crowd_note : arena_anchor_note, sizeof prompt - strlen(prompt) - 1);
        logf_("%s/%s: %s%s", views[v], pc->file, ref[0] ? "with anchor " : "first picture (becomes the anchor)", ref[0] ? ref : "");
        if (codex_exec(send, ref[0] ? ref : send, prompt, out_path, sizeof out_path) || iload(out_path, &m)) {
            logf_("%s/%s: nothing came back", views[v], pc->file); miss++; done++; free(o.px); state_json(done, todo, ok, miss, pc->file); continue;
        }
        scaled = malloc((size_t)o.w * o.h * 4);
        if (m.w == o.w && m.h == o.h) memcpy(scaled, m.px, (size_t)o.w * o.h * 4); else { box_scale(&m, scaled, o.w, o.h); logf_("%s/%s: %dx%d scaled to %dx%d", views[v], pc->file, m.w, m.h, o.w, o.h); }
        for (int k = 0; k < o.w * o.h; k++) {
            uint8_t *q = scaled + (size_t)k * 4;
            q[3] = o.px[k * 4 + 3] < 128 ? 0 : 255;
            if (!q[3]) { q[0] = q[1] = q[2] = 0; continue; }
            /* residual magenta on an opaque pixel (the model painted the key into
               a rail gap, or its geometry slid a pixel): the original's pixel */
            if (q[0] > 150 && q[2] > 150 && q[1] < 110 && q[0] - q[1] > 80) memcpy(q, o.px + (size_t)k * 4, 3);
        }
        wf_art_write_rgba_png(dst, scaled, o.w, o.h);
        free(scaled); free(m.px); free(o.px);
        if (!anchor[0]) snprintf(anchor, sizeof anchor, "%s", dst);
        { FILE *lf; snprintf(p, sizeof p, "%s/last.txt", gen); lf = fopen(p, "w"); if (lf) { fprintf(lf, "%s\n", dst); fclose(lf); } }
        ok++; done++;
        state_json(done, todo, ok, miss, pc->file);
        logf_("%s/%s: ok (%d / %d)", views[v], pc->file, ok, todo);
    }
    snprintf(p, sizeof p, "%s/_send.png", gen); unlink(p);
    state_json(done, todo, ok, miss, "done");
    logf_("arena %s: done %d ok %d missed %d", name, done, ok, miss);
    if (runlog) fclose(runlog);
    return miss ? 2 : 0;
}

/* ---------------------------------------------------------------- TEMPLATE BATCH
 * --art-template STYLE OUTDIR   (user 2026-08-30: "run the codex generations
 * yourself so you can check sizes")  STYLE = greyscale | color.  Eight codex
 * calls from the stock scene inputs (exports/arenas/wwf/scene_ring.png,
 * scene_ringside.png, scene_ringside-frontcrowd.png), results saved RAW (no
 * scaling - the slicer fits and shift=auto registers them) under OUTDIR with
 * the names --arena-slice expects; resumable (a result that exists is kept);
 * a result without any transparent pixel gets pure magenta keyed. */
static const char *tpl_style_text(const char *style)
{
    return !strcmp(style, "greyscale")
        ? "STRICTLY GREYSCALE (black, white and greys only), crisp flat-shaded arcade pixel art, clean hard edges, no gradients"
        : "the ORIGINAL COLOURS kept (blue mat, blue floor, warm crowd), crisp flat-shaded arcade pixel art, clean hard edges, no gradients";
}
static int tpl_has_alpha(const img *m) { for (int i = 0; i < m->w * m->h; i++) if (m->px[(size_t)i * 4 + 3] < 128) return 1; return 0; }
static int tpl_one(const char *in, const char *out, const char *prompt, const char *what)
{
    char gen[1024]; img m;
    if (access(out, R_OK) == 0) { logf_("template: %s kept", out); return 0; }
    if (access(in, R_OK) != 0) { logf_("template: input %s missing", in); return -1; }
    logf_("template: %s -> codex", what);
    if (codex_exec(in, in, prompt, gen, sizeof gen) || iload(gen, &m)) { logf_("template: %s: nothing came back", what); return -1; }
    if (!tpl_has_alpha(&m)) key_magenta(&m);
    wf_art_write_rgba_png(out, m.px, m.w, m.h);
    logf_("template: %s: %dx%d -> %s", what, m.w, m.h, out);
    free(m.px);
    return 0;
}
int tool_art_template(const char *style, const char *outdir)
{
    char p[700], in[300], prompt[4096]; const char *st = tpl_style_text(style); int miss = 0, done = 0;
    static const char *base_ring =
        "Edit this pixel-art wrestling arena picture (the in-ring view). Output the SAME picture at exactly 640x512, same composition, nothing moved, cropped or resized - every rail, post, tier and floor line stays at the same pixel position. %s. Remove every logo, word and sponsor art: plain flat mat, plain apron skirt. REMOVE ALL ROPES - the three near rope lines at the bottom of the ring and the three far rope strands behind it - leave no rope pixels anywhere. REMOVE the two NEAR ring posts and their turnbuckle pads (bottom-left and bottom-right of the ring); the mat corner continues underneath. KEEP the two FAR ring posts with their turnbuckle pads exactly where they are, drawn clean and simple. Keep the guard rails, floor lines, crowd and lighting rig exactly in place. Pure magenta (#FF00FF) areas are transparent: keep them exactly magenta.";
    static const char *base_ringside =
        "Edit this pixel-art wrestling arena picture (the ring seen from outside, ringside). Output the SAME picture at exactly 960x256, same composition, nothing moved, cropped or resized - keep the black strip at the bottom. %s. Remove every logo, word and sponsor art: plain flat mat, plain apron skirt. REMOVE ALL ROPE STRANDS on the ring - leave no rope pixels between the posts. KEEP all the ring posts with their turnbuckle pads exactly where they are. Keep the guard rails, the floor and its lines, the seating tiers and the crowd exactly in place. Pure magenta (#FF00FF) areas are transparent: keep them exactly magenta.";
    static const char *crowd_add = " Then output ONLY THE AUDIENCE: the same picture with everything except the crowd figures replaced by pure magenta (#FF00FF).";
    static const char *crowd2_add = " Then output ONLY THE AUDIENCE in a second animation pose (arms raised where they were lowered and vice versa, heads turned), everything else replaced by pure magenta (#FF00FF); the figures stay in the same places.";
    static const char *front =
        "Edit this pixel-art picture: the front-row crowd seen from behind, standing in front of a wrestling ring (960x256, transparent = pure magenta #FF00FF). Output the SAME picture at exactly 960x256, figures in the same places and the same size. %s. Keep the magenta transparent area exactly magenta.%s";
    snprintf(p, sizeof p, "mkdir -p \"%s\"", outdir); if (system(p)) return 1;
    jobdir = outdir; snprintf(p, sizeof p, "%s/run.log", outdir); runlog = fopen(p, "a");
    state_json(0, 8, 0, 0, "start");
#define TPL(inp, name, fmt, extra, what) do { snprintf(in, sizeof in, "exports/arenas/wwf/%s", inp); snprintf(p, sizeof p, "%s/%s", outdir, name); \
        snprintf(prompt, sizeof prompt, fmt, st); if (extra) strncat(prompt, extra, sizeof prompt - strlen(prompt) - 1); \
        if (tpl_one(in, p, prompt, what)) miss++; done++; state_json(done, 8, done - miss, miss, what); } while (0)
    TPL("scene_ring.png", "scene_ring-empty-no-ropes.png", base_ring, NULL, "ring empty");
    TPL("scene_ring.png", "scene_ring-crowd-only.png", base_ring, crowd_add, "ring crowd 1");
    TPL("scene_ring.png", "scene_ring-crowd-only-frame-2.png", base_ring, crowd2_add, "ring crowd 2");
    TPL("scene_ringside.png", "scene_ringside-empty-no-ropes.png", base_ringside, NULL, "ringside empty");
    TPL("scene_ringside.png", "scene_ringside-crowd-only.png", base_ringside, crowd_add, "ringside crowd 1");
    TPL("scene_ringside.png", "scene_ringside-crowd-only-frame-2.png", base_ringside, crowd2_add, "ringside crowd 2");
    { char f1[4096]; snprintf(f1, sizeof f1, front, "%s", ""); snprintf(prompt, sizeof prompt, f1, st);
      snprintf(in, sizeof in, "exports/arenas/wwf/scene_ringside-frontcrowd.png"); snprintf(p, sizeof p, "%s/scene_ringside-frontcrowd-frame-1.png", outdir);
      if (tpl_one(in, p, prompt, "front crowd 1")) miss++; done++; state_json(done, 8, done - miss, miss, "front crowd 1");
      snprintf(f1, sizeof f1, front, "%s", " Draw the figures in a second animation pose: arms raised where they were lowered and vice versa, heads turned; same places."); snprintf(prompt, sizeof prompt, f1, st);
      snprintf(p, sizeof p, "%s/scene_ringside-frontcrowd-frame-2.png", outdir);
      if (tpl_one(in, p, prompt, "front crowd 2")) miss++; done++; state_json(done, 8, done - miss, miss, "front crowd 2"); }
#undef TPL
    state_json(done, 8, done - miss, miss, "done");
    logf_("template %s: %d / 8 ok", style, done - miss);
    if (runlog) fclose(runlog);
    return miss ? 2 : 0;
}
