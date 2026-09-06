# Mods — backlog / ideas (discussed 2026-08-30, none started)

Rules of the road: stock stays byte-identical (regress baseline); every
deviation is a `game_rules` / `mode_rules` row with an editor description,
off by default. Mod men (backups, managers) are driven by AI + the ring
laws — never state overrides. New tools in C. Shipped rules are listed in
`src/modrules.c` (labels) and the editor's Rules tab; this file is what is
NOT built yet.

## Next up (cheap, discussed in detail)

- **turbo_pct** (100-200): the main-loop pacer runs `57.44 x pct/100` engine
  frames per real second. Everything scales (walk, anims, ring-out count,
  match clock). Deterministic, replay/netplay-safe, ~a dozen lines. Audio
  stays in lockstep with engine frames for a first pass (pitch/tempo rise
  like a wrong crystal; MAME's throttle does the same); decoupling the
  Z80/YM/OKI to real time is a later option and touches the lockstep code
  (2-master-clocks-per-OPM_Clock trap). Stacks multiplicatively with
  `speed_pct_human/cpu` - say so in the description.
  Variants NOT chosen: action-turbo (wrestlers faster, clocks real-time:
  scale CELL_DUR + AI timers, changes hit windows = design work);
  skip-frame turbo (pointless on a modern machine).

## New behaviour (not reachable by combining existing rules)

1. **Manager at ringside** - a spawned man who never enters: trips a runner
   along the ropes, slides a weapon in (hardcore), pulls the ref out of a
   count. Reuses the backup spawner + outside AI + weapon hand-over +
   ref-KO paths; new AI role. Recommended after the table.
2. **Table spot** - a breakable ringside object: slam/throw arc onto it ->
   breaks (badge-style art frame like the ref KO art), big damage, stays
   broken. All pieces exist (arc, landing check, external art). Best
   impact per effort.
3. **Weapon respawn / wear** - hardcore weapons come back after N seconds;
   chairs break after N hits, stairs never.
4. **Momentum meter** - near-falls, reversals, crowd-pleasers fill a bar;
   full = one-shot finisher into an unkickable pin. Distinct from
   comeback / on-fire.
5. **Reversals** - a timed press by the throw VICTIM reverses it (stock has
   none outside the tie-up). New victim-side input window.
6. **Submission tap-out** - holds can END the match when the victim's mash
   fails at zero energy (stock holds only drain).
7. **Blind tag / tag from anywhere on the apron** - a tag-law change.
8. **Final-four rumble rules** - pins/submissions legal once 4 remain, the
   ring-out count returns (pin_anyone + census).
9. **Rope break** - a downed man near the ropes escapes a pin/hold by
   grabbing them (new escape branch + ref break).
10. **Injury** - a limb targeted N times (leg drops, arm bars) slows
    walking/running or weakens grapples for the rest of the match.
11. **Cage** - climbing the wall as a real climb with the opponent able to
    pull you down; door escape needing the ref.
12. **Replay camera** - after a fall, re-run the last ~4 s from state with a
    slow zoomed camera (the engine is deterministic; needs the replay
    machinery from docs/todo-netplay.md).
13. **Career persistence** - win/loss record and streak across sessions
    feeding stats/difficulty (small JSON).

## Picked by the user (2026-08-30) - build these

- **Guest referee** - a PLAYER controls the referee: walk, count at your own
  tempo, get knocked down, hit people. The ref state machine (referee.c
  SM0-SM10) exists; the new part is giving it a port and human-driven
  transitions (count on a press, a strike move).
- **Tornado tag** - both partners LEGAL at once: no usher, no illegal-man
  rules, pins/rescues by either man. A tag-LAW change (legality is baked
  into the ROM's pin/rescue tests), not a preset.
- **Ironman with stats** - most falls in N minutes, a running scoreboard on
  the HUD (the fall counter exists; the scoreboard is new), decision at the
  bell (draw handling).
- **Stamina + SHOWOFF** - a second bar drained by running and big moves;
  empty = slow walk, no run, weaker grapple odds; regenerates standing. A
  button/combo plays the existing crowd-pose animations ("showoff") and
  RECHARGES energy/stamina - but leaves the man wide open to be hit for
  the duration (risk/reward).
- **Tag-team finishers** - a double-team move needing both partners in
  position. LOD have one in the STOCK ROM that was never ported - port
  that first (records/art exist), then generalise per team (paired art
  via the pipeline for others).
- **AI / meta** - smarter, cleverer CPU: per-wrestler personalities
  (brawler / high-flyer / cheat) as a table, situational play (rope-break
  awareness, corner spots, weapon use, tag timing), adaptive difficulty
  on the session's win/loss streak, and SOME CHEATING (an illegal double
  team while the ref is turned, a low blow, holding the ropes) gated by a
  rule. Needs work beyond the ROM's id x stage x band tables.
- **Practice mode** - a dummy that stands/lies/runs/blocks on command,
  hit-frame + hitbox overlay, move list, save/restore state. Also the
  tuning bench for every mod above (and the replay/netplay determinism
  check).

## Other new-behaviour ideas (not picked yet)

- Last man standing (ref counts 10 over a downed man - the ring-out count
  transplanted in-ring). Ladder/belt match (ladder object + climb state -
  big). Lumberjack match (permanent ringside backups throwing men back
  in). Weight classes (heavy men can't be lifted until worn down; the
  fumble roll). Dives to the outside (throw arc + outside landing).
  Corner spots (post shot, ten-punch mash). Slow-motion finisher (the
  turbo pacer at 1/4). Visible kick-out meter on the HUD. Random events
  (lights out, chair run-in, second referee). Moveset editor (swap the
  grapple-category move ids per wrestler - data exists, UI doesn't).

## Parked elsewhere

- Netplay (peer-to-peer lockstep, `enable_net`): docs/todo-netplay.md.

## Playtest follow-ups on shipped mods

- throw_out arc size (V1.52: 25% more lift, lands 0x38 past the rope).
- ref KO frames: 8 ticks per falling frame; flat frame y offset.
- rumble_rotate hand-over feel in a live rumble.
- tag elimination = existing `mode_rules.elimination` (survivor): playtest; its walk-off is a forced 0x7A start (fine at the pin release, else the V1.57 AI-goal shape); with ko_dq a KO ends the match rather than eliminating the man - one branch if wanted.
- backup: leaving as an AI goal (V1.57) - watch for a man who never gets
  "free" (held forever) and so never leaves.
