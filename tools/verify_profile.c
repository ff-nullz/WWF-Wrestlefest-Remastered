/* --verify-profile P: the FIDELITY GATE for a profile's stock men. Since
 * 2026-08-26 EVERY profile (stock included) packs the 12 stock wrestlers
 * from frames (data/stockskins/NN/frames -> ingest -> pak); the yardstick
 * is the ROM render (WF_DATA=rom: tables and sprites from the ROM export).
 *
 *   wfengine --verify-profile stock
 *   wfengine --verify-profile superstars
 *
 * For each stock id 0..11 and each scenario frame (WF_VERIFY_FRAMES
 * overrides) the same scripted match is run twice in child processes — in
 * ROM mode and with the profile's paks — and the two --shot frames are compared inside the
 * ring area (rows 40..199: the HUD above carries the name plate, which
 * legitimately differs). A frame passes when the differing pixels are at or
 * under WF_VERIFY_MAX (default 64: the residue of overlap draw-order edge
 * cases measured at 10-32 per frame on 2026-08-26). Exit 1 on any failure.
 * Ropes: the scripted match stays mid-ring, so no rope strip is in frame. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/wait.h>

/* SCENARIOS: the grapple choreography (tie-up -> punch / throw) and the
 * DRAG (walking face lock: the tie-up win held with a direction, poses
 * 67-70 composed with the hidden victim - the case that exposed the
 * flip-unaware ownership mask, 2026-08-26). */
/* scripts: jab chain (34..95) wins the tie-up and knocks P2 down; then
 *   grapple: keep pressing (a second tie-up / throw in progress at f100-200)
 *   drag:    hold the stick from f98 (walking headlock)
 *   pin:     press B1 at f100 over the downed man -> the cover, the count
 *   throw:   B2 (0x20) at f100 in the tie-up -> a throw, its landing frames */
/*   weapon:  ringside (WF_OUT2/WF_WOUT1 pokes), pick up the box at f40, walk,
 *            swing at f100 - the overlay cells; compared over the WHOLE frame
 *            (the ringside scene, no HUD name plate in play) */
static const struct { const char *name, *script, *frames; char key; const char *env; int full; } vp_scen[] = {
    { "grapple", "0-30:1,34-35:10,40-41:10,46-47:10,52-53:10,58-59:10,64-65:10,70-71:10,76-77:10,82-83:10,88-89:10,94-95:10,100-101:10,110-111:10", "60,100,140,200", 'f', NULL, 0 },
    { "drag",    "0-30:1,34-35:10,40-41:10,46-47:10,52-53:10,58-59:10,64-65:10,70-71:10,76-77:10,82-83:10,88-89:10,94-95:10,98-240:1", "150,180", 'd', NULL, 0 },
    { "pin",     "0-30:1,34-35:10,40-41:10,46-47:10,52-53:10,58-59:10,64-65:10,70-71:10,76-77:10,82-83:10,88-89:10,94-95:10,100-101:10,150-212:1,218-219:10,226-227:10", "235,262,290", 'p', NULL, 0 },
    { "throw",   "0-30:1,34-35:10,40-41:10,46-47:10,52-53:10,58-59:10,64-65:10,70-71:10,76-77:10,82-83:10,88-89:10,94-95:10,100-101:20,110-111:20,120-121:20", "115,130,150,175", 't', NULL, 0 },
    { "weapon",  "40-41:10,60-70:1,100-101:10", "60,80,104,120", 'w', "WF_NOINTRO=1 WF_OUT2=20 WF_WOUT1=30", 1 },
};
static const char *VP_SCRIPT, *VP_ENV; static int VP_FULL;

/* rom = the reference render (WF_DATA=rom: tables and sprites straight from
 * the ROM export); profile NULL = the stock profile's paks */
static int vp_run(int id, int frames, const char *profile, int rom, const char *shot)
{
    pid_t pid;
    fflush(stdout); fflush(stderr);   /* a forked child must not re-flush the parent's report */
    pid = fork();
    if (pid == 0) {
        char w1[8], fr[16]; const char *argv[16]; int n = 0;
        snprintf(w1, sizeof w1, "%d", id); snprintf(fr, sizeof fr, "%d", frames);
        setenv("WF_W1", w1, 1); setenv("WF_P1", VP_SCRIPT, 1);
        if (VP_ENV) { char *e = strdup(VP_ENV), *tok = strtok(e, " "); for (; tok; tok = strtok(NULL, " ")) { char *eq = strchr(tok, '='); if (eq) { *eq = 0; setenv(tok, eq + 1, 1); } } }
        unsetenv("WF_PAKDIR"); unsetenv("WF_GFXPAK"); unsetenv("WF_PAK");   /* the child picks its own data */
        if (rom) setenv("WF_DATA", "rom", 1); else unsetenv("WF_DATA");
        if (!freopen("/dev/null", "w", stdout) || !freopen("/dev/null", "w", stderr)) _exit(127);
        argv[n++] = "./wfengine";
        if (profile) { argv[n++] = "--profile"; argv[n++] = profile; }
        argv[n++] = "--headless"; argv[n++] = "--no-front"; argv[n++] = "--frames"; argv[n++] = fr;
        argv[n++] = "--drive"; argv[n++] = "script"; argv[n++] = "--shot"; argv[n++] = shot; argv[n] = NULL;
        execv("./wfengine", (char *const *)argv);
        _exit(127);
    }
    if (pid < 0) return -1;
    { int st; waitpid(pid, &st, 0); return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1; }
}

static uint8_t *vp_ppm(const char *path, int *w, int *h)
{
    FILE *f = fopen(path, "rb"); int maxv; uint8_t *px;
    if (!f) return NULL;
    if (fscanf(f, "P6 %d %d %d", w, h, &maxv) != 3 || *w <= 0 || *h <= 0) { fclose(f); return NULL; }
    fgetc(f);
    px = malloc((size_t)*w * (size_t)*h * 3);
    if (!px || fread(px, 3, (size_t)*w * (size_t)*h, f) != (size_t)*w * (size_t)*h) { free(px); fclose(f); return NULL; }
    fclose(f);
    return px;
}

int tool_verify_profile(const char *profile)
{
    int frames[16], nf = 0, fails = 0, maxdiff = getenv("WF_VERIFY_MAX") ? atoi(getenv("WF_VERIFY_MAX")) : 64;
    char a[128], b[128];
    const char *prof = (!profile || !profile[0] || !strcmp(profile, "stock")) ? NULL : profile;   /* stock = its own paks */
    snprintf(a, sizeof a, "/tmp/wf-verify-%d-a.ppm", (int)getpid());
    snprintf(b, sizeof b, "/tmp/wf-verify-%d-b.ppm", (int)getpid());
    printf("verify-profile %s vs the ROM render: 12 stock men x 5 scenarios (grapple f60/100/140/200, drag d150/180, pin p235/262/290, throw t115/130/150/175, weapon w60/80/104/120 full frame), ring area rows 40..199, pass <= %d differing pixels\n", prof ? prof : "stock", maxdiff);
    for (int id = 0; id < 12; id++) {
        int worst = 0, bad = 0; char line[256] = "";
        for (unsigned sc = 0; sc < sizeof vp_scen / sizeof vp_scen[0]; sc++) {
        VP_SCRIPT = vp_scen[sc].script; VP_ENV = vp_scen[sc].env; VP_FULL = vp_scen[sc].full;
        nf = 0;
        for (const char *p = getenv("WF_VERIFY_FRAMES") ? getenv("WF_VERIFY_FRAMES") : vp_scen[sc].frames; *p && nf < 16; ) { frames[nf++] = atoi(p); while (*p && *p != ',') p++; if (*p) p++; }
        for (int k = 0; k < nf; k++) {
            uint8_t *pa, *pb; int wa, ha, wb, hb, diff = 0; char one[32];
            if (vp_run(id, frames[k], NULL, 1, a) || vp_run(id, frames[k], prof, 0, b)) { snprintf(one, sizeof one, " f%d:RUN-FAIL", frames[k]); strncat(line, one, sizeof line - strlen(line) - 1); bad++; continue; }
            pa = vp_ppm(a, &wa, &ha); pb = vp_ppm(b, &wb, &hb);
            if (!pa || !pb || wa != wb || ha != hb) { free(pa); free(pb); snprintf(one, sizeof one, " f%d:SHOT-FAIL", frames[k]); strncat(line, one, sizeof line - strlen(line) - 1); bad++; continue; }
            for (int y = VP_FULL ? 0 : 40; y < (VP_FULL ? ha : 200) && y < ha; y++)
                for (int x = 0; x < wa; x++)
                    if (memcmp(pa + ((size_t)y * wa + x) * 3, pb + ((size_t)y * wa + x) * 3, 3)) diff++;
            free(pa); free(pb);
            if (diff > worst) worst = diff;
            if (diff > maxdiff) bad++;
            snprintf(one, sizeof one, " %c%d:%d", vp_scen[sc].key, frames[k], diff);
            strncat(line, one, sizeof line - strlen(line) - 1);
        }
        }
        printf("  %s wrestler %02d  worst %4d px %s\n", bad ? "FAIL" : "ok  ", id, worst, line);
        if (bad) fails++;
    }
    unlink(a); unlink(b);
    printf("verify-profile %s: %s (%d of 12 failed)\n", prof ? prof : "stock", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
