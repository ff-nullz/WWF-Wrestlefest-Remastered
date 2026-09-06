# wfengine modding guide

The original game is untouchable by design: `wfengine` with no arguments always
launches pure stock, `data/` is read-only to every editor surface, and `rom/` is
never written. Everything below layers on top.

## Profiles — named versions of the game

A **profile** is a launchable variant: its own rules, its own paks, its own name.

    wfengine --profiles            # list them
    wfengine --profile hardcore    # play one (packs automatically when stale)
    wfengine --editor              # the editor (games launch from its Profiles tab)

**Creating one (the whole workflow):** open the editor → Profiles tab → type a
name, press *Create profile* → click the profile's name → the **settings page**
opens: every game rule, with stock values marked. Change what you want → *Save*
→ *Launch*. Your edits live in `mods/<name>/` (created automatically); the
profile file is `profiles/<name>.json`.

The header dropdown ("editing:") picks which profile the Tables/Rules/Wrestlers
panels operate on — pick **stock** there to compare values (read-only), flip
back to keep editing. *Quick save* = apply + write + repack in one click;
*Quick launch* starts the selected profile.

## The settings (mode_rules + game_rules)

MATCH MODE — what kind of match this is:

| row | meaning |
|---|---|
| team_size / team_size_b | 1 = singles (tournament card shows), 2 = stock tag, 3 = trios; side B may differ (handicap) |
| pin_enabled / ref_enabled / countout_enabled | referee behavior a la carte |
| weapons_mode | 0 none · 1 stock (ringside only) · 2 HARDCORE (carry into the ring and back out; the weapon survives ring-outs; the CPU uses it inside) |
| time_limit_min | BCD minutes; 0 = no clock |
| rumble_bell / rumble_live_cap / rumble_spawn_frames | rumble seeding: men at the bell (2..12), live cap (up to 12), entrant interval. 12 at the bell = every stock wrestler, nobody left to walk in = a battle royale (`profiles/battle-royale-12.json`); with all twelve in play every sprite palette bank is taken, so weapons, badges and clones have no bank to borrow there |
| runin_enabled | tag partners run in on pins |
| elimination | SURVIVOR rules: a fall eliminates the pinned man until a side is empty |
| mirror_picks | the select screen accepts duplicate picks (second copy = auto alternate palette) |
| cage_escape_win | in the cage, climbing out wins: hold DOWN into the bottom wall of the screen for ~2 s, climb it hand over hand (pose 277 mirrored), over the top (poses 37/33/35), drop down the outside |

GENERAL MODS — gameplay switches (all neutral in stock). The canonical list with stock values and the editor's help text is `wfengine --rules-doc` (from `src/rules.def`, the one file a new rule is added to):

| row | meaning |
|---|---|
| unlimited_energy | 1 = humans never drain, 2 = nobody does |
| unlimited_time | the clock never ticks |
| difficulty_offset | 128 = stock; higher = the CPU wins tie-ups like a later stage |
| speed_pct_human / speed_pct_cpu | walk/run speed %, 100 = stock |
| cpu_kickout_delay | 128 = stock; higher = pinned CPUs stay down longer |
| ref_knockdown / ref_down_frames | strikes floor the referee ("OH NO"); no counts until he recovers (default 30s). His knocked-out art is EXTERNAL: `data/badges/ref_down_0/1/2.png` (falling, mid-fall, flat) — replace the PNGs (codex from the in-game ref frame, installed with `--art-key IN OUT 92 92`, then `--pack`) |
| weapon_dq | a landed weapon swing disqualifies the swinger's team |
| cpu_energy_meters | the CPU men's HUD rows show gauge + portrait |
| grapple_gauge | the tug-of-war bar under a human grapple (blinks = throw window) |
| exit_ring | hold into a rope to leave the ring (tag) |
| exit_ring_hold / exit_ring_ropes / exit_ring_climb | frames the stick is held (32 = ~0.5s, 172 = 3s); which ropes (right 1, left 2, top 4, bottom 8; 0 = any); 1 = walk to the near rope and climb out through the ropes (the rumble elimination climb) instead of hopping over |
| backup_avg_secs / backup_max / backup_stay_frames | random run-ins: a wrestler runs down the aisle, climbs in and brawls (AI-driven, obeys the ring laws, climbs out/in across ring-outs). He LEAVES only when thrown out over the top rope — then, once free, he climbs out and walks up the aisle. `backup_stay_frames` 0 (default) = no timer; >0 adds a timer that sets the same goal |
| camera_zoom_pct | <100 zooms OUT (panning), down to 63 |
| grapple_pick | deterministic throws: held direction picks the row, buttons the column |
| camera_fixed | LOCKED per-scene view — the whole ring on screen, no panning |
| extended_moves | strikes knock climbers off the buckle (the CPU learns to do it too) |
| video_smooth | 1 (default) = bilinear-filtered scaling like MAME's default `-filter`; 0 = crisp nearest-neighbour pixels. `WF_SMOOTH=0|1` overrides |
| human_hit_mult | damage multiplier on everything a HUMAN lands - strikes, knees, throws (1 = stock) |
| badge_human / badge_cpu | a 1P..4P / CPU chip floats over the men (`data/badges/*.png`, replaceable art) |
| dt_stomp | default ON: the entering partner's double-team stomp at the corner hold (stock behaviour restored) |
| throw_out | bodyslams, press slams and backdrops near the ropes throw the victim OUT over the top in a real arc (Hogan's suplex and the gorilla press excluded) |
| toprope_out | with extended_moves, a man knocked off the buckle goes out of the ring instead of onto the mat |
| pin_anyone | anyone can pin anyone in a brawl (non-legal men included) |
| pin_outside | covers count at ringside too — the referee joins the pile outside |
| rumble_endless | the entrant pool re-opens once everyone has walked in; the rumble runs until you are eliminated |
| ko_dq | energy 0 = OUT: eliminated in the rumble, his team loses a match — humans too |
| rumble_rotate | an eliminated human seat is not GAME OVER: you take over the next entrant to walk in |
| parasitic_pct | PARASITIC energy: that percent of the energy every hit takes is given to the hitter (100 = the full amount, capped at his gauge); humans and CPU alike |
| turbo | 1..5: the wrestlers walk, run and animate at 120..200% - the clock, counts and referee stay at real time; stacks with speed_pct_* |
| ai_commit | experimental: n = a CPU keeps a rolled move for up to n frames while the situation holds. Measured (`tools/ai_bench.sh`, WF_AISTATS=1): stock re-rolls only ~7 times per 1000 CPU frames, so this is NOT where the flip-flopping comes from - leave 0 |

The **RUN key**: bindable per player in the Input panel (default unbound); it
acts as both buttons held. P3/P4 keys are under the pair toggle there.

## Mod layers stack (sparse rules layers)

A rules layer names only the rows it sets:

    mods/turbo/tables/rules/game_rules.json
    { "name": "game_rules", "group": "rules", "kind": "u16",
      "overrides": { "turbo": 3 } }

The packer merges every mod of the profile IN ORDER on top of the stock
table, so two rule mods in one profile both apply (`profiles/vampire-turbo.json`
= parasitic + turbo). A layer with full `rows` still replaces the whole table
(then its own `overrides`, if any, patch it) - that is what an old full layer
does, and why two of those used to shadow each other. `wfengine --sparsify MOD`
rewrites a mod's full rules layers into the sparse form; with a profile active
(`--profile X --sparsify MOD`) the rows are those that differ from the table the
mods BENEATH it produce, otherwise from the engine defaults. The editor saves a
rules layer the same way - against the layers beneath it - so an edited mod
never absorbs its neighbours' rows.

## Clone wrestlers (ids 12..15)

A clone plays exactly like its base wrestler but carries its own name, stats and
palette. A re-palette clone is three small files in a mod:

    mods/<mymod>/wrestlers/12/wrestler.json   {"clone_of": 0, "name": "HOLLYWOOD"}
    mods/<mymod>/wrestlers/12/palette.json    (16 pens, arcade xBGR words)
    mods/<mymod>/wrestlers/12/stats.json      (hp / walk / run)

Pack the profile and the clone exists (`build/.../wrestlers/12.pak`, ~300
bytes). His aisle card shows his own name; the announcer skips the base's name
call. Duplicate seats (mirror picks, auto-filled trios) become unnamed
alternate-palette clones automatically. Test hook: `WF_CLONE=12` seats a clone
as the CPU leader.

## Music and sound

The engine plays the game's ORIGINAL sound hardware, emulated: the Z80
sound program from the ROM drives a YM2151 (music) and an OKI M6295
(samples), so every tune is full-length and every SFX is the real thing.
The two sound ROM images live in `data/sound/z80.bin` + `data/sound/oki.bin`
(created once by `./wfengine --export-sound`; the game never opens `rom/`).

**Replacing a tune** is a plain-file job:

1. In wfeditor's **Sounds** tab, pick the tune and press **Render to mod**
   (or run `./wfengine --render-music 0x05 90 mods/mymod/music/cmd_05.wav`).
   That writes the REAL full-length tune as a 48 kHz WAV into your mod.
2. Edit the WAV in any audio editor — or replace it with any WAV at all.
3. Done. When a profile stacks that mod, the engine streams your WAV for
   that music command instead of the emulated tune (the file loops whole).
   Delete the file to get the emulated original back.

Commands: music is `0x01..0x12` (`0x00` stops). The Sounds tab lists every
command with its label and plays it through the board. A mod can also
replace the whole sound program or sample set by shipping its own
`sound/z80.bin` / `sound/oki.bin`.

`WF_SOUND=wav` forces the old excerpt-WAV player (fallback mode).

## The art stage (AI or artist reskins)

Full new-art wrestlers without touching a pixel editor:

1. `./wfengine --art-job 8 /path/job` — renders every pose of the base
   (id 8 here) into `job/ref/` (RGBA frames, fixed origin) + a manifest
   contract and a `prompt.txt` to edit.
2. Fill `job/out/` with restyled frames — ANY generator works:
   hand an artist the ref frames. Contract: same size, same origin, same
   silhouette, transparent background (the ingest restores alpha from
   the ref mask automatically).
3. `./wfengine --art-ingest job SLOT mods/<m>/wrestlers/<SLOT>` — builds
   the package (auto palette, tiles, poses); keep/add wrestler.json,
   pack the profile. Poses missing from out/ fall back to the base.

Start with `--limit 5` + ingest + a `--poses SLOT` strip to judge the
model's consistency before paying for all ~380 frames.

## The Forge (build-a-wrestler)

wfeditor's **Forge** tab composes a new wrestler into any of the **32
clone slots (ids 12..43)**: pick a BASE (his art, AI and animations),
name him, tune hp/walk/run, remap his **move map** (all 21 situations x
3 buttons, choosing from the unified move catalog - every move any stock
wrestler routes, punch/kick basics through the signature throws), and
recolour his 16-pen palette. **Create wrestler** writes the clone into
your editing profile's mod and packs it; Launch, and he has his own cell
on the select grid (the grid grows columns rightward, page-scrolling
past the LOD column). **Load slot** reads a forged wrestler back.

Per-slot files (what Create writes, hand-editable):
`mods/<m>/wrestlers/NN/wrestler.json` ({"clone_of", "name"}),
`stats.json` (hp/speeds + "move_map" rows), `palette.json`; optional
extras: `select/NN.png` face (80x80 cell format), `palettes/NN.json`
outfits, own sheet/tiles/poses via `--retile` (each slot owns a
6144-tile art arena). Headless: WF_FORGE="slot,base,NAME[,cat.col=move]".

## Palette select (outfits)

A mod can ship alternate OUTFITS per wrestler: `palettes/NN.json` with up
to 8 named 16-pen palettes. On the select screen, **B2 on that wrestler's
cell cycles his outfit** (the name tags under the cell); the choice
recolors him in the match, the intro banner, interludes and the ceremony.
Without the mod, B2 keeps its stock behaviour. Pens 1-6 are the skin ramp
in every stock palette - keep them to change only the uniform.

`mods/wardrobe` ships three generated outfits for all 12 stock wrestlers
(`--profile wardrobe`, or stack "wardrobe" into any profile's mod list).
Author refined palettes in wfeditor's Wrestlers panel and paste the pens
into the JSON. `WF_PALSEL="id:k,..."` pre-picks outfits (harness).

## New-art wrestlers (the clone-art arena)

A clone slot (12..43) can carry its **own sprite sheet** — a genuinely new
wrestler as a pure art project:

1. Export the base whose moveset fits:
   `./wfengine --export 7 /tmp/exp` (368 poses, ~4.7k tiles).
2. Rebase it into the slot's tile arena:
   `./wfengine --retile /tmp/exp/07 mods/mymod/wrestlers/12 0x10000`
   (arena bases: slot 12 = 0x10000, 13 = 0x11800, 14 = 0x13000, 15 = 0x14800).
3. Add `mods/mymod/wrestlers/12/wrestler.json`:
   `{ "clone_of": 7, "name": "MYGUY" }` — the clone keeps base 7's moveset/AI.
4. **Repaint `sheet.png`** in any image editor (indexed PNG, keep the
   palette indices; `palette.json` holds the body colours). Every pixel is
   yours — the slot draws from its own tile space, nothing else changes.
5. Put the mod in a profile and launch. `WF_CLONE=12` forces the CPU to
   use the slot for a quick look.

`profiles/newart.json` + `mods/newart/` ship as a working example (slot 12
carrying its own copy of id 7's art — repaint it). Pose tile refs outside
the sheet (grapple cells showing the OTHER man) stay global by design.

## Layers (advanced)

A profile is really an ordered list of **mod layers** (`mods/<m>/`, later wins);
the settings page maintains the profile's own top layer for you. Stack extra
layers from the popup's *advanced: mod layers* section — e.g. one shared
"fixedview" layer reused by several profiles. Layer layout:
`tables/<group>/<name>.json`, `wrestlers/NN/<file>`, `gfx-edit/...`,
`keymap.json`.

## Safety net

`data/stock.json` is the pristine reference (editor highlights every deviation
amber; *Stock* buttons revert). `data/` regenerates from the ROM with
`wfengine --export-all data/tables` — verified byte-for-byte. Nothing you do in
profiles or mods can touch the original game.

## Where the rules hook into the engine

Every rule above is read through `eng_mod_rule()` (src/modrules.c). The rules
that change what the wrestlers DO (launches, landings, damage, the hit scan,
the exit-ring pad gate, the pin gate) are implemented in **src/modhooks.c**
behind named hooks; the transcribed handlers in anim.c / hit.c / core.c call
one hook per site (`mod_fall_launch`, `mod_fall_landed`, `mod_hit_scan_extras`,
`mod_damage_taken`, `mod_exit_ring_gate`, ...). Add a new gameplay rule there,
not inside a handler, and give it a line in `tools/mod_gate.sh` (one headless
drive + the effect it must produce; `tools/trace_gate.sh` proves stock did not
change, the mod gate proves the mods still work). Presentation rules (HUD, camera, chips) and the AI /
picker branches stay where they are; the file header lists them.
