#ifndef LAYER_H
#define LAYER_H

/* Play layers. Musashi always stays linked.
 *
 *   arcade — the original 68k. ROM gameplay, untouched 68k rendering.
 *            The visual and behavioral oracle.
 *   stock  — the C port of the original, no mods. C select and native
 *            filesystem rendering; no extras, no sandbox.
 *   mods   — the C port with mods: extras, new modes, C menu.
 *   native — C match from assets + SDL. Not arcade-exact. Musashi is
 *            idle after reset (linked, never executed). Same renderer
 *            and 0xD1FC decoder as --port.
 *
 *   --68k            arcade
 *   --port           stock
 *   --mods           mods
 *   --native         native
 *   (no flag, live)  mods
 * Scenarios with no layer flag stay arcade so exact stays green.
 *
 * --rom-select is orthogonal: it pins the 68k character-select back on for
 * any layer, so the comparison harnesses can drive a --port run into a match
 * by the same route the arcade side takes. It is not a layer.
 *
 * The enum still spells the middle layer STOCK while its flag is --port.
 * Same layer, older internal name.
 */

typedef enum {
    WF_LAYER_ARCADE = 0,
    WF_LAYER_STOCK  = 1,
    WF_LAYER_MODS   = 2,
    WF_LAYER_NATIVE = 3
} wf_layer;

void wf_layer_set(wf_layer layer);
wf_layer wf_layer_get(void);
const char *wf_layer_name(void);

int wf_layer_arcade(void);
int wf_layer_stock(void);
int wf_layer_mods(void);
int wf_layer_native(void);

/* 1 if this mode id is offered on the current layer. */
int wf_layer_mode_ok(const char *id);

#endif
