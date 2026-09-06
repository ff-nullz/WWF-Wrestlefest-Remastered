# wfeditor — the in-engine deep-dive editor

`./wfengine --editor` (or `WF_EDITOR=1`) opens a second window, **wfeditor**,
beside the running game. It is written in C inside the engine (Nuklear UI over
SDL2, `src/editor.c`, vendored `src/ext/nuklear*.h`), so it works on the very
same in-memory data the match is using: the table registry, the wrestler
packages, the live `eng_state`. It is a hack/mod test bench, not a polished
app: expose everything, apply it live, save it to the data tree.

Keys: game keys are ignored while the editor window has focus; `P` pauses /
resumes the game (when no text field is active), `F11` toggles borderless
fullscreen. `Esc` in the game window quits.

UI (V404 pass): the window opens filling the desktop work area; dark theme
with gold accents; a header bar carries the nav tabs, live match status and
the PAUSE button, a status bar the active mod layer / dirty flag / last log
line. Hover ANY control for an explanation tooltip; table cells report their
row/col, value and ROM address on hover. The **Rules** panel renders each
scalar as `name — value — explanation` (`rule_help[]` in `src/editor.c`);
the **Match** panel decodes every object into a card (name, decoded state,
energy bar, labelled x/y/z with coordinate-system hints, dimmed raw hex
line); the **Wrestlers** panel names the roster and edits pens with a colour
picker.

## Panels

| panel | what you can do |
|---|---|
| **Tables** | every registered table (164) by group. Grid of rows × stride cells (`hex` toggle for display). **Apply live** writes the bytes into the running engine (immediate effect for tables read per frame; init-time copies need a new match). **Save JSON** writes `data/tables/<group>/<name>.json` — or `mods/<layer>/<group>/<name>.json` when a mod layer is active. **Load JSON** pulls the file back. **Revert** = the engine's current bytes. |
| **Rules** | the `rules` group: engine scalars that used to be hard-coded (hold / tag / ring-out), with row labels, plus **`dips`** — the cabinet DIP switches (raw MAME values; see `docs/dip-switches.md` for every switch, its values and its consumers). Same buttons. |
| **Input** | the SDL keyboard map for both players (`src/keymap.c`). Click a key, press the new one (`Esc` cancels); rebinds apply to the game window at once. **Save** writes `data/keymap.json` — or `mods/<layer>/keymap.json` when a mod layer is active (load order: compiled defaults ← `data/keymap.json` ← each mod in `mods/order.txt`, later wins). Values are SDL key names (`SDL_GetKeyFromName`); a missing or partial file falls back per entry. Entries: `p1_right/left/up/down/b1/b2/start/coin`, `p2_...`. |
| **Wrestlers** | per package: hp / walk / run (Apply live, Save `stats.json`), the 16-pen body palette (swatches, R/G/B 0-15, Apply live = reinstall, Save `palette.json`), and a **pose browser** (pose id, flip, partner row) rendered from the package's command lists and the loaded tiles — what the game draws. |
| **Engine** | pause / step / +credit; stage and clock edits; every live object: state, move, react, flags, opponent/partner, facing; editable hp, x, y, z. |
| **Mods** | choose the active layer (base = `data/` itself, or a dir under `mods/`), create a mod (mkdir + `mods/order.txt`). Saves go to the active layer; `Tools > Pack` bakes it into the paks; restart the game to load. |
| **Tools** | run the C tools and see their output: Pack, Export all, Verify gfx, List tables, `make regress`, `cov_drives.sh`, git status. |

## Notes

- Nothing is written unless you press a Save button; the paks are rebuilt
  only by Pack / `./build.sh`. Live edits die with the process.
- Rule tables are synthetic (`TBL_SYNTH`): their defaults are compiled in,
  the JSON under `data/tables/rules/` is what the pak carries.
- Debug: `WF_EDITOR_NAV=n` starts on panel n (0 Tables, 1 Rules, 2 Input,
  3 Wrestlers, 4 Engine, 5 Mods, 6 Tools),
  `WF_EDITOR_SHOT=file.ppm` dumps the editor window every frame
  (works with `SDL_VIDEODRIVER=dummy` for headless screenshots).

## Next

More engine scalars into `rules` (every hard-coded constant the user wants to
tweak), a JSON file browser for the wrestler stats tree, tile/sheet viewers,
and "reload paks without restart".
