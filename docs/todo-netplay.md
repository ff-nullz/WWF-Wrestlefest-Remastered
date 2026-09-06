# Netplay — TODO / later (discussed 2026-08-30, not started)

Status: **design notes only**. Nothing is implemented. Parked because
"there's a lot of detail to work out"; revisit after the mod backlog.

## Shape agreed

- **Peer-to-peer lockstep**, no server, no authority. Both machines run the
  identical simulation; only INPUTS cross the wire. Works because the engine
  is a deterministic fixed-step (57.44 Hz) integer simulation — the regress
  digest and the scripted drives already depend on that.
- Set up from **wfeditor → Tools**, enabled by a mod rule `enable_net`
  (game_rules row, off by default; stock never sees it). The editor page
  holds the peer address/port, the seat split (which ports this machine
  owns), and the input delay.

## Prerequisite: input record / replay (do this first, on its own merits)

- Write every frame's 4 port input words to a file; `--replay FILE` re-runs
  the match. Header carries version, pak hashes, profile, rule tables, RNG
  seed, and any WF_* pokes that were set.
- Proves determinism across runs and machines and doubles as a bug-report
  format ("here's the replay of the float").
- Expected leaks to fix here, not in a net session: static match state in
  the mod layers (`eng_backup_tick` spawned/stay/slot, rumble `rot`, the
  WF_MODRULES parse cache) — wants one explicit match-init reset; runtime
  `getenv` pokes; anything keyed on wall-clock/SDL time rather than the
  frame counter; out-of-order `eng_rng` callers (editor, render effects).

## Lockstep protocol

- UDP. Per frame: `{frame, inputs[4], digest-of-an-earlier-frame}`, acked;
  a small ring buffer retransmits lost packets.
- Input delay d: frame N runs when both sides hold both players' inputs
  for N. d = 2-3 on a LAN, 4-6 over the internet. Neither side ever runs
  ahead (rollback is a separate later step).
- Mid-match buy-in/join and the front end (select, continue) must go
  through the same input path or be disabled in net mode.
- Audio needs nothing: each side's emulated Z80 consumes the same command
  stream.

## Hashes

1. **Handshake hash** (once, at connect): executable, every pak the match
   reads (base, wrestlers/NN, gfx, badges, arenas, weapons), the resolved
   profile (`mods/order.txt`, the EFFECTIVE rule tables after layering),
   match setup (picks, stage, seats) and the RNG seed. Hash each part
   separately and report the first mismatch by name ("badges.pak differs").
   Leave out pure visuals/audio (skin tiles, WAV overrides, rope art,
   camera zoom, video smoothing).
2. **Per-frame digest** (the regress digest): `eng_obj` array, referee,
   weapons, RNG, match words, rumble state — not video/sprite RAM. Hash at
   the same point of the frame on both sides (after the object pass,
   before render); hash fields not padding. Sent a few frames behind the
   inputs so a late packet never stalls on it. First mismatch = "desync at
   frame N": dump both states for diffing.

## Pairing server (user 2026-08-30)

Direct peer-to-peer needs one side reachable (port forward / same LAN).
Add a **very basic rendezvous server**: it waits for two hosts to connect,
pairs them, and gets out of the way.

- Tiny standalone C program (`tools/wfrendezvous.c`, no engine deps),
  runnable on a small VPS like any other service.
- Protocol: a client connects (UDP, keep-alives), sends a room code
  (typed in the wfeditor Tools page, or blank = "next free"); the server
  holds it until a second client presents the same code, then sends each
  the other's public address:port (UDP hole punching). Both then talk
  direct lockstep; if the punch fails (symmetric NAT) the server can
  optionally RELAY the input packets - they are tiny, so relaying costs
  nothing.
- Also the place to compare the handshake hashes BEFORE pairing, so a
  mismatched build/pak is reported at the lobby, not at the bell.
- No accounts, no persistence, no match state on the server - it never
  simulates anything; the game stays peer-to-peer lockstep.

## Later: rollback

Only if internet latency is felt. Needs save/restore of `eng_state` +
video RAM (plain memory) and re-simulating up to ~8 frames; audio stays
un-rolled-back. Mostly plumbing plus an audit that nothing outside
`eng_state` + video RAM carries match state.

## Rough sizes (planning only)

Replay ~200 lines; lockstep + editor page ~400-600 lines; rollback ~500
lines + the audit.
