# The standing hold's clocks — how long you can keep a headlock

Source: `../wrestlefest-decomp/reference/maincpu.asm` (read-only pass,
2026-08-23). Object stride 0x10C. Companions: face-tieup.md (states 0x0B/0x0C),
grapple-moves.md (moves 0x15/0x16/0x17 and the throw layer), escape-machine.md
(+0xAA mash), ai-behaviour.md §5 (the CPU side).

The user's report: *"walking around with the headlock has an auto-reverse time
(usually an Irish whip is the reverse, but I'm not sure)."* Both halves are
right, and the ROM has **two** separate clocks, on two different objects.

```
tie-up  ──►  state 0x0C  ──cat 9 B1──►  move 0x15  ◄──stick──►  move 0x16
             HOLDER's +0x46                 HOLDER                 HOLDER
             0xE0 -> 0 = role FLIP           (facelock)          (drag walk)
                                                │
                                         victim = move 0x7B
                                         VICTIM's +0x44
                                         0x80 / 0xBB -> 0
                                                │
                                                ▼
                                         move 0x7C  ──►  move 0x17  = IRISH WHIP
                                         (reversal)       on the ex-holder
```

---

## 1. Clock A — the tie-up hold, state 0x0C (+0x46), 0xE0 ticks

Handler `0x124FA`, record `0x124E2`. Already implemented (`handler_hold`,
face-tieup.md §4); restated here because it is the *other* timeout and the two
get confused.

```
; ---- entry frame ----
012526  move.w #$E0,($46,A0)      ; hold clock  = 0xE0 (224 frames, ~3.9 s @ 57.44 Hz)
01252C  clr.b  ($45,A0)           ; hold phase  = 0
; ---- every frame ----
01254C  subq.w #1,($46,A0)
012550  cmpi.w #$A0,($46,A0)
012558  move.b #$1,($45,A0)       ; at 0xA0 (0x40 frames in): phase 1 -> cat 9 opens
012560  tst.w  ($46,A0)
012566  move.w #$FF,($20,A0)      ; at 0: SELF becomes the held man
01256C  movea.l ($26,A0),A2
012570  move.w #$C,($20,A2)       ;        the PARTNER takes the hold
012576  clr.w  ($BC,A0)
01257A  clr.w  ($44,A2)
01257E  move.w #$39,($4,A2)  / 012584 or in his facing
01259A  move.w #$FFFF,($4,A0)     ; self hidden
```

So: **0x40 frames of "you can't do anything yet", then 0xA0 frames to act; at
0xE0 the hold flips to the other man** and his own 0xE0 starts.

*Oracle:* `tools/scenarios/tag.scn`, P1 (`$1C05B0`, human, +0x56 = 0) takes the
hold at f3571 with +0x46 = 0xDF and flips to state 0xFF at **f3794 = f3570 +
0xE0** — exact.

Cat 9 (`0xE1B0`, grapple-moves.md §2a) needs `+0x21 == 0x0C` **and
`+0x45 == 1`**, i.e. the press only counts inside that 0xA0-frame window, and
`0xE1C8`'s `clr.w +0x44` clears the phase byte on the way out.

## 2. Handing the victim over — move 0x15 entry (0x14CF4)

```
014D00  cmpi.w #$1,($44,A0)       ; +0x44 == 1 -> 0x14D1E: re-entry from the drag
014D08  cmpi.b #$C,($1F,A0)       ; previous state was the hold 0x0C ...
014D10  move.w #$20,D0 / 014D18 bsr $10BD0     ; ... step 0x20 BACKWARD
014D1E  bset #7,($1C,A0); clr +0x24; clr +0x22 ; (re-entry) resume the wrench loop
014D2C  cmpi.w #$3,($44,A0)       ; +0x44 == 3 -> the reversal family already set
014D36  move.w #$5,($20,A2)       ;              the victim up; otherwise:
014D3C  move.w #$7B,($60,A2)      ; *** VICTIM -> state 5, move 0x7B ***
```

Two things matter:

* **`0x10BD0(+0x20)` is BACKWARD.** `0x10BD0` negates D0 when `+0x2E` bit7 is
  set (facing right), so a *positive* argument moves against the facing. The
  engine's `add_pos_delta` has the identical rule, and the old code passed
  `-0x20` — which stepped the pair 0x20 px **forward** on every 0x0C -> 0x15
  transition. That is the reported "sprites move quite far to the right during
  the transition": a right-facing holder jumped +0x20 instead of -0x20, a 0x40
  error. *Oracle:* holder x = 0x263 in state 0x0C, **x = 0x243 on the first
  frame of move 0x15** (custom tag scenario, f3571 -> f3651). The engine now
  reproduces 0x29D -> 0x27D facing right, and 0x263 -> 0x283 facing left.
* Nothing else moves the pair. The `0x10B62(±0x18)` calls in the 0x0C entry are
  **probes**: `0x10B62` writes D0 to the look-ahead `+0x3E`, runs the bounds
  pass, and adds only the *pushback* `+0x38` to X — away from a rope they are
  a no-op, so they do not double up with the step.

`move.w #$7B,($60,A2)` is a **word** write: it also clears `+0x60` bit15, which
is the victim's "escape already rolled" latch (§4). Every 0x16 -> 0x15 toggle
re-runs this entry, so **stopping the drag walk re-arms the victim's one roll**.

Move 0x16 (`0x14DA2`) never touches the victim's `+0x60` or `+0x44`: the clock
runs on unbroken across the stance/walk toggling.

## 3. Clock B — the auto-reverse, move 0x7B (+0x44), 0x80 / 0xBB ticks

Record `0x1A532` = `{ handler 0x1A538, mode 0 }` — mode 0, so there are no
frames and no cells: the handler *is* the move. `A2` is the victim's `+0x26`,
i.e. **the holder**.

```
01A538  bset #7,($1C,A0)          ; test-and-set the init latch in one insn
01A53E  bne  $1A57A               ;   already inited -> tick
; ---- entry ----
01A540  move.w #$FFFF,($4,A0)     ; hidden; the holder draws the two-man art
01A546  btst #5,($33,A2)          ; HOLDER on fire (enrage meter 0xFDA2)?
01A54E  bset #7,($60,A0)          ;   -> pre-set the latch: NO escape roll at all
01A554  cmpi.w #$1,($44,A0)  / beq seed     ; +0x44 == 1 = "re-seed me" marker
01A55C  cmpi.b #$5,($1F,A0)  / beq rts      ; previous state 5 -> KEEP the clock
01A564  move.w #$80,($44,A0)      ; *** 0x80 = 128 frames (~2.2 s) ***
01A56A  btst #7,($56,A2)          ; holder is CPU?
01A572  move.w #$BB,($44,A0)      ; *** 0xBB = 187 frames (~3.3 s) ***
; ---- every frame ----
01A57A  subq.w #1,($44,A0)
01A57E  bne  $1A592
01A580  move.w #$5,($20,A0)       ; VICTIM -> state 5 ...
01A586  move.w #$7C,($60,A0)      ;   ... move 0x7C, the reversal
01A58C  move.w #$FF,($20,A2)      ; HOLDER -> frozen/held
```

The `+0x1F == 5` test is why the clock survives 0x16 -> 0x15: the re-entry
rewrites `+0x60` (so the anim re-inits) but the previous *state* was still 5,
so `0x1A55C` returns before the re-seed.

**The clock lives on the victim and is seeded from the HOLDER's controller
flag**: a CPU holder gets 0xBB, half a second longer to pick his throw than a
human holder's 0x80.

*Oracle (custom tag scenario, `press 3650 p1_b1 3` added to tag.scn):*

| frame | object | what |
|---|---|---|
| f3651 | o0 (human) | state 5, move 0x15, x 0x263 -> **0x243** |
| f3651 | o4 (CPU)   | state 5, move 0x7B, **+0x44 = 0x0080** (human holder) |
| f3779 | o4 | move **0x7C**, o0 -> state 0xFF — **3779 - 3651 = 128 = 0x80** |
| f3790 | o4 | move **0x17** (the whip), +11 frames |
| f3811 | o0 | state 2 — running from the whip, +21 frames |

The engine reproduces every one of those deltas (128 / +11 / +21).

## 4. The victim's escape — one probability roll per hold, not a mash meter

There is **no mash meter in the standing hold.** `+0xAA` / `+0xAB`
(escape-machine.md §3) is never seeded or ticked by move 0x7B, `0x10C60` and
`0x10D0C` are not called from it, and `0xEBC4`'s substitution ladder has no
0x7B arm — so hammering the buttons does *not* wind a counter down. (`0x111C8`
is unrelated: it is the KO check, submissions.md §1c.)

What the held man actually gets is one shot, inside the **counter selector
`0x0EA42`**, which `0xDE86` calls at `0xDEDA` after `0xEBC4`/`0xF0BA` and
before the whole category ladder. A new B1/B2 press (`+0xA3 & 3`) is required
to reach it at all.

```
00EA42  movea.l ($7A,A0),A1       ; opponent
00EA46  btst #7,($74,A0) / bne fail   ; not holding a weapon
00EA50  own state 0 or 1 -> the "counter his move" arms (0xEA64..)
00EA60  otherwise -> 0xEB68
...
00EB8E  cmpi.b #$5,($21,A0)  / bne fail   ; own state 5
00EB96  cmpi.b #$7B,($61,A0) / bne fail   ; own move 0x7B  <-- the held man
00EB9E  bset #7,($60,A0)     / bne fail   ; TEST-AND-SET: one attempt per hold
00EBA6  moveq #0,D1 / jsr $1129E          ; the roll
00EBB0  bcc fail
00EBB2  move.w #$7C,D0                    ; *** SUCCESS: the reversal ***
00EBBE  ori #1,CCR -> 0xDFE8: +0x20 = 5, +0x60 = D0
```

`0x1129E` entry 0 (`0x112C0`):

```
0112C2  A1 = 0x113AC                     ; default table
0112C8  A2 = ($7A,A0)                    ; the OPPONENT = the holder
0112CC  btst #7,($56,A2)  / beq 0x112D4
0112D4  A1 = 0x113CA                     ; holder is HUMAN: 3 bytes, by band
0112DC  D1 = $1C0162 (stage) * 3         ; holder is CPU: 10 rows of 3
0112E6  D1 += ($70,A0)                   ; + the VICTIM's energy band 0..2
0112EA  D1 = byte A1[D1]
0112EE  jsr $21B4 / andi #$F,D0
0112F8  cmp.w D1,D0 / bcc  -> carry clear = FAIL
                      else -> carry set  = SUCCESS
```

So **success = `rng() & 0x0F < threshold`**.

| table | index | rows |
|---|---|---|
| `0x113CA` (holder HUMAN) | victim band 0..2 | `03 02 03` |
| `0x113AC` (holder CPU) | stage*3 + band | s0 `03 02 01`, s1 `02 02 01`, s2 `02 01 01`, s3 `02 01 01`, s4..s9 `01 01 01` |

i.e. 3/16 = 18.8 % at best, 1/16 = 6.3 % from stage 4 on — and **only once**,
because `0xEB9E` latches. The mashing that feels like it helps is really the
*holder's* stance/walk toggling handing out fresh attempts (§2). A holder who
is **on fire** (`+0x33` bit5) pre-sets the latch at `0x1A54E`, so the victim
never gets even one.

## 5. The reversal — move 0x7C (0x1A5A0) and why it is an Irish whip

Record `0x1A594` = `{ 0x1A5A0, mode 1, n 1, dur 0x0008, cell 0x0221 }`.

```
; ---- entry ----
01A5A8  clr.b ($1,A0) / 01A5AC clr.b ($1,A2)   ; both movers off
01A5B0  bchg #7,($2E,A0)          ; the ex-victim turns
01A5B6  move.w #$FF,($20,A2)      ; the ex-HOLDER is now the held man
01A5BC  move.w #$FFFF,($4,A2)
; ---- the cell finishes (8 ticks) ----
01A5CC  move.w #$5,($20,A0)
01A5D2  move.w #$17,($60,A0)      ; *** MOVE 0x17 = THE IRISH WHIP ***
01A5D8  move.w #$FF,($20,A2)
01A5DE  bchg #7,($2E,A0)          ; turn back
01A5E4  0x10BD0(0x20,0,0)         ; step 0x20 BACKWARD
```

The user's "usually an Irish whip is the reverse" is exactly right: the
timeout and the escape roll both land on 0x7C, and 0x7C's only exit is
move 0x17. Whoever ran the clock out is thrown into the ropes.

### What the reversal is *not*

* **`0x212A0` is not the reversal routine.** It is the tag-team autopilot /
  rescue bookkeeping (`+0x33` b1 hand-over, `+0x56` b6, `+0xB5`/`+0xB6`,
  `$1C1697`), `$1C007C`-gated. The hold break never calls it.
* **Moves 0x83..0x87 are not the hold break.** `0x83` (`0x1A8A6`) is a 0x50-tick
  struggle inside a **catch** (`+0x92` pairing): at 0 the struggler goes to
  state 4 react 0x0E with `+0x68` = 8 and the catcher to move 0xC006. That is
  the run-catch fumble family (run-catch.md), not the facelock.
* The analogous *role-swap* family for other holds is **0x7D / 0x7E / 0x7F**
  (`0x1A604`, `0x1A688`, `0x1A718`): a 0x50-tick (or 2-cycle) loop that on
  expiry plays the jingle `0x0F1A`, puts **self** into `0xC07B` with `+0x44 = 1`
  (re-seed) and the **partner** into move 0x15 with `+0x44 = 3` (the
  "0x14D2C already-set-up" path). They swap holder and victim without a whip.
  **TODO EXACT:** the engine does not route 0x7D/0x7E/0x7F yet, and the writers
  that enter them are not transcribed.
* Move 0x6E / 0x6F are the submission-hold KO exits (submissions.md), unrelated.

## 6. Cat 9 / cats 0xA-0xD vs the timeouts

* Cat 9 (`0xE1B0`) is gated on **state 0x0C + `+0x45 == 1`**, i.e. only during
  clock A's 0xA0-frame window. Miss it and clock A flips the hold to the other
  man.
* Cats 0xA-0xD (`0xE1D4`) fire from **move 0x15 or 0x16** with no timer gate at
  all — the holder can throw at any point of clock B. `0xE1D4` opens with
  `clr +0x44, +0x46` **on the presser**, which is the holder, so it does not
  touch the victim's clock; the throw's own handler freezes the victim
  (`+0x20 = 0x00FF`) and the 0x7B tick stops.
* Nothing extends clock B. A holder who wants more than 0x80 frames has to land
  a throw, whip, or tag.

## 7. What the CPU does

* **CPU holder** — the state-0xC AI (`0x1F078`, ai-behaviour.md §5) picks from
  the cascade well inside the 0xA0 window and normally presses within ~0x20
  frames of taking the stance, so his 0xBB rarely runs out.
* **CPU victim** — the state-5 AI row `0x1DFE2` dispatches on the move id
  through `0x1E018`; the entry for **move 0x7B is `0x1E018 + 0x7B*4 = 0x1E204
  -> 0x1E254 = rts`**. A CPU victim in the standing hold does *nothing*: he has
  no roll (that is why `0x10D04` skipping CPU mash does not matter here) and no
  press, so he escapes **only** by clock B running out. The `0x1DA7E` /
  `victim_escape` roll family in ai.c belongs to the submission holds
  (0x5D/0x5E/0x5F/0x60/0x62/0x63, `held_move()`), which do not include 0x7B, so
  its 0xC0-frame engine guard is untouched by this work.

## 8. Data — `data/romdata/hold_rules.json`

Flat rows, read through `eng_table_rows` by `eng_hold_rule(idx, def)` in
anim.c; every call passes the stock ROM value as `def`, so a missing or short
file still plays stock.

| row | name | stock | ROM |
|---|---|---|---|
| 0 | `hold0C_seed` | 0xE0 (224) | `0x12526` |
| 1 | `hold0C_phase1_at` | 0xA0 (160) | `0x12550` |
| 2 | `drag_reverse_vs_human` | 0x80 (128) | `0x1A564` |
| 3 | `drag_reverse_vs_cpu` | 0xBB (187) | `0x1A572` |
| 4 | `counter_7D_ticks` | 0x50 (80) | `0x1A60C` (reference only) |
| 5..7 | `escape_th_human[band]` | 3 2 3 | `0x113CA` |
| 8..37 | `escape_th_cpu[stage][band]` | see §4 | `0x113AC` |

Rows 0 and 1 are wired in `handler_hold`; rows 2/3 and 5..37 in
`handler_heldvictim` and `walk_logic`. All 38 are live.

## 9. Repro

`tools/hold_demo.sh` (run from the repo root):

```sh
tools/hold_demo.sh stance    # 0x0C -> 0x15, then idle: reverse at +0x80, whip at +11
tools/hold_demo.sh drag      # ... with the stick held (move 0x16): same clock
tools/hold_demo.sh mash 1    # the victim taps B: roll WON  -> move 0x7C at once
tools/hold_demo.sh mash 2    # ... roll LOST -> the later taps do nothing (one shot)
tools/hold_demo.sh cpu       # WF_CPU2=1 victim: no roll at all, clock only
```

Oracle side (needs `../wrestlefest-decomp`):

```sh
# tag.scn + one extra press inside the hold window
head -11 tools/scenarios/tag.scn > /tmp/holdrev.scn
echo 'press 3650 p1_b1 3' >> /tmp/holdrev.scn
./wfport --roms ../wrestlefest/stock --68k \
  --scenario-file /tmp/holdrev.scn --frames 4300 --trace /tmp/holdrev.wfo
# objects $1C05B0 stride 0x10C; watch +0x20, +0x60, +0x44, +0x46, +0x45, +0x06
```
