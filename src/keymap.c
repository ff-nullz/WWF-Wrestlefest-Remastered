/* Keyboard mapping — see keymap.h. The compiled defaults are the keys
 * main.c always used (MAME-style: P1 arrows + A/Z + 1/5, P2 DGRF + Q/W +
 * 2/6); data/keymap.json and the mod layers override by entry name. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "keymap.h"
#include "profile.h"
#include "json.h"

static const SDL_Scancode def_sc[KM_PLAYERS][KM_N] = {
    { SDL_SCANCODE_RIGHT, SDL_SCANCODE_LEFT, SDL_SCANCODE_UP, SDL_SCANCODE_DOWN,
      SDL_SCANCODE_A, SDL_SCANCODE_Z, SDL_SCANCODE_1, SDL_SCANCODE_5, SDL_SCANCODE_UNKNOWN },
    { SDL_SCANCODE_G, SDL_SCANCODE_D, SDL_SCANCODE_R, SDL_SCANCODE_F,
      SDL_SCANCODE_Q, SDL_SCANCODE_W, SDL_SCANCODE_2, SDL_SCANCODE_6, SDL_SCANCODE_UNKNOWN },
    /* P3/P4: mid-game buy-in seats (0x18C4/0x6E5E). MAME-style spread on
     * keys the engine leaves free: P3 = IJKL + Y/U, P4 = numpad + 0/Enter. */
    { SDL_SCANCODE_L, SDL_SCANCODE_J, SDL_SCANCODE_I, SDL_SCANCODE_K,
      SDL_SCANCODE_Y, SDL_SCANCODE_U, SDL_SCANCODE_3, SDL_SCANCODE_7, SDL_SCANCODE_UNKNOWN },
    { SDL_SCANCODE_KP_6, SDL_SCANCODE_KP_4, SDL_SCANCODE_KP_8, SDL_SCANCODE_KP_2,
      SDL_SCANCODE_KP_0, SDL_SCANCODE_KP_ENTER, SDL_SCANCODE_4, SDL_SCANCODE_8, SDL_SCANCODE_UNKNOWN },
};
static const char *const bit_names[KM_N] =
    { "right", "left", "up", "down", "b1", "b2", "start", "coin", "run" };

static SDL_Scancode map[KM_PLAYERS][KM_N];
static int loaded;

const char *keymap_entry_name(int player, int bit)
{
    static char nm[KM_PLAYERS][KM_N][16];
    if (player < 0 || player >= KM_PLAYERS || bit < 0 || bit >= KM_N) return "";
    if (!nm[player][bit][0])
        snprintf(nm[player][bit], sizeof nm[0][0], "p%d_%s", player + 1, bit_names[bit]);
    return nm[player][bit];
}

void keymap_reset_defaults(void)
{
    memcpy(map, def_sc, sizeof map);
    loaded = 1;
}

SDL_Scancode keymap_sc(int player, int bit)
{
    if (!loaded) keymap_reset_defaults();
    if (player < 0 || player >= KM_PLAYERS || bit < 0 || bit >= KM_N) return SDL_SCANCODE_UNKNOWN;
    return map[player][bit];
}

void keymap_set(int player, int bit, SDL_Scancode sc)
{
    if (!loaded) keymap_reset_defaults();
    if (player < 0 || player >= KM_PLAYERS || bit < 0 || bit >= KM_N) return;
    map[player][bit] = sc;
}

const char *keymap_keyname(int player, int bit)
{
    SDL_Keycode kc = SDL_GetKeyFromScancode(keymap_sc(player, bit));
    const char *n = SDL_GetKeyName(kc);
    return (n && n[0]) ? n : "(none)";
}

/* One JSON file over the current map; unknown names are warned and kept. */
static void overlay_file(const char *path)
{
    char err[128];
    json_val *root = json_parse_file(path, err, sizeof err);
    if (!root) return;                       /* missing file = no overlay */
    if (root->type == JSON_OBJECT) {
        for (int p = 0; p < KM_PLAYERS; p++)
            for (int b = 0; b < KM_N; b++) {
                const json_val *v = json_get(root, keymap_entry_name(p, b));
                const char *s = v ? json_str(v, NULL) : NULL;
                if (!s) continue;
                {
                    SDL_Keycode kc = SDL_GetKeyFromName(s);
                    if (kc == SDLK_UNKNOWN) {
                        fprintf(stderr, "keymap: %s: unknown key \"%s\" for %s (kept %s)\n",
                                path, s, keymap_entry_name(p, b), keymap_keyname(p, b));
                        continue;
                    }
                    map[p][b] = SDL_GetScancodeFromKey(kc);
                }
            }
    } else {
        fprintf(stderr, "keymap: %s: not a JSON object, ignored\n", path);
    }
    json_free(root);
}

void keymap_load(void)
{
    keymap_reset_defaults();
    overlay_file("data/keymap.json");
    {   /* the ACTIVE PROFILE's mod layers, in order (later wins; stock =
         * none). mods/order.txt is retired — profile.h. */
        for (int i = 0; i < wf_profile_nmods(); i++) {
            char path[320];
            snprintf(path, sizeof path, "mods/%s/keymap.json", wf_profile_mod(i));
            overlay_file(path);
        }
    }
    if (getenv("WF_DBGSEL"))
        for (int p = 0; p < KM_PLAYERS; p++) {
            /* one call per entry: SDL_GetKeyName reuses a static buffer */
            fprintf(stderr, "keymap: P%d", p + 1);
            for (int b = 0; b < KM_N; b++)
                fprintf(stderr, " %s=%s", keymap_entry_name(p, b), keymap_keyname(p, b));
            fprintf(stderr, "\n");
        }
}

int keymap_save(const char *path)
{
    FILE *f;
    if (!path) path = "data/keymap.json";
    f = fopen(path, "w");
    if (!f) { fprintf(stderr, "keymap: cannot write %s\n", path); return -1; }
    fprintf(f, "{\n");
    for (int p = 0; p < KM_PLAYERS; p++)
        for (int b = 0; b < KM_N; b++) {
            fprintf(f, "  ");
            json_write_string(f, keymap_entry_name(p, b));
            fprintf(f, ": ");
            json_write_string(f, keymap_keyname(p, b));
            fprintf(f, "%s\n", (p == KM_PLAYERS - 1 && b == KM_N - 1) ? "" : ",");
        }
    fprintf(f, "}\n");
    fclose(f);
    return 0;
}
