# DIP switches — stock inventory and engine wiring

Sources: MAME `technos/ddragon3.cpp` (`INPUT_PORTS_START( wwfwfest )`,
authoritative for switch names/values/factory defaults) and the ROM itself
(`reference/maincpu.asm`) for how the bits reach work RAM and who reads them.

## Plumbing (ROM 0x1E1E)

The board scatters the two 8-switch banks over the four input ports
(MAME custom members `dsw_3f_r` / `dsw_c0_r`):

| port | bits | carries |
|---|---|---|
| `$140020` (P1) | 12-13 | SW2:7-8 |
| `$140022` (P2) | 8-13 | SW2:1-6 |
| `$140024` (P3) | 8-13 | SW1:1-6 |
| `$140026` (P4) | 8-9 | SW1:7-8 |

ROM **0x1E1E** (called from the 0x654 reset path at 0x6D6) gathers them and
`not.w`s the result into **`$1C0066`** — which round-trips to plainly
`$1C0066 = ~(SW2 << 8 | SW1)`: byte `$1C0066` = ~SW2, byte `$1C0067` = ~SW1,
**a set bit = switch OFF**. Every consumer tests these inverted bits.

Engine: `src/dips.c` — synthetic rules table **`dips`** (raw switch values,
defaults = factory; editable in wfeditor's Rules panel, exported to
`data/tables/rules/dips.json`); `eng_dip_word()` serves the `$1C0066` word so
wired consumers keep the stock masks. Harness poke: `WF_DIPS=0xNNNN`
(raw `SW2<<8|SW1`, before inversion).

## SW1

| switch | name | values (raw) | factory | work-RAM | consumers |
|---|---|---|---|---|---|
| SW1:1-2 | Coin A | 3=1C/1C, 2=1C/2C, 1=2C/1C, 0=3C/1C | 3 | `$1C0067` bits 0-1 | **0x408** indexes coinage table 0x42C `{coins/credit, credits/coin}` → `$1C004B/$1C004D` (wired: `credit.c coinage_init`). Also re-read at 0x5E8 (two-unit continue) and 0xC458-area prompts. MAME lists no Coin B — the second chute uses the same rate. |
| SW1:3 | Buy In Price | 1=1 coin, 0=as start price | 1 | `$1C0067` bit 2 | **0x592** via 0x55A D0=5 (0x588): empty-seat join price (0x1938, 0xC4EC prompt logic). Wired (V429): `credit.c eng_take_join` (`take_priced(2)`), consumed by the mid-game join `hud.c eng_join_tick` (0x6E8C tag / 0x1936 rumble). |
| SW1:4 | Regain Power Price | 1=1 coin, 0=as start price | 1 | `$1C0067` bit 3 | **0x592** via 0x55A D0=2: the mid-match START refill (0x8C32). Wired: `credit.c eng_take_buyin` → `take_priced(3)` (bit set = one credit 0x5CE, clear = part-coin first 0x59A). |
| SW1:5 | Continue Price | 1=1 coin, 0=as start price | 0 | `$1C0067` bit 4 | **0x592** via 0x55A D0=1 (0x16F2): the continue screen. Wired: `campaign.c` → `eng_take_continue` (`take_priced(4)`). The pair-continue D0=3 / D0=4 two-unit path (0x5DE/0x638, also 0xA24/0xA68) is TODO EXACT. |
| SW1:6 | Demo Sounds | 1=on, 0=off | 1 | `$1C0067` bit 5 | **0x205E**: sound poster 0x2052 drops the command while `$1C007C == 0` (attract, before any START). Wired: `main.c` gates the front-end music post. |
| SW1:7 | Flip Screen | 1=off, 0=on | 1 | `$1C0067` bit 6 | **0x203E** (routine 0x2006): sets `$10000B` bit 0 (video flip). Wired: `main.c` blits the frame rotated 180° when on. |
| SW1:8 | FBI Logo | 1=on, 0=off | 1 | `$1C0067` bit 7 | **0x724**: skips the "Winners Don't Use Drugs" splash to 0x790. Wired: `attract.c` holds the black gap (the 0x790 cards are TODO EXACT). |

## SW2

| switch | name | values (raw) | factory | work-RAM | consumers |
|---|---|---|---|---|---|
| SW2:1-2 | Difficulty | 3=Normal, 2=Easy, 1=Hard, 0=Hardest | 3 | `$1C0066` bits 0-1 (word `& 0x300`) | **0x10806**: index into `stage_handicap_dip` (0x1085C: `+0 / -15 / +15 / +30`) added to the CPU stage energy. Wired: `campaign.c eng_camp_hp`. Inverted index: 0=Normal 1=Easy 2=Hard 3=Hardest. |
| SW2:3-4 | Players | 3=4 players, 2=3, 1=2 | 3 | `$1C0066` bits 2-3 (word `& 0xC00` / `0x800`) | Cabinet layout: `0x800` set = 2-player board — START scan 0x97E, HUD row paths 0xC75E/0xC81E/0xC8A2/0xC92A/0xC9F8, plate pick 0x13C4, 0x5804/0x5F40/0x60A8/0x6E44/0x8BA8. Engine deliberately plays a 2-player board seated 4-wide (`hud.c` header); since V429 the dip CLAMPS the joinable ports (`hud.c eng_join_maxports`: 4/3/2 ports may buy in mid-game, dead seats lose their BUY-IN plates) — the stock cabinet-path forks themselves stay TODO EXACT. |
| SW2:5 | (unused) | 1=off | 1 | `$1C0066` bit 4 | No reads found. |
| SW2:6-7 | Clear Stage Power Up | 3=24, 2=32, 1=12, 0=none | 3 | `$1C0066` bits 5-6 (word `& 0x6000`) | **0x90EA** (routine 0x90D6): after a cleared stage, each surviving man gets `word(0x912A + ((~dip & 0x6000) >> 12))` = `{0x18, 0x20, 0x0C, 0x00}` energy, clamped to max. Engine: 0x90D6 (leftover-energy scoring/refill) is TODO EXACT (`referee.c`), so the row is documented but unconsumed. |
| SW2:8 | Championship Game | 1=5th, 0=4th | 1 | `$1C0066` bit 7 (word `& 0x8000`) | **0x1A86**: when "4th", the stage counter `$1C0163` gets an extra bump at 3 and 8 (0x1AA4) — a defence is skipped and the title matches come one stage sooner. Wired: `campaign.c eng_camp_stage`. |

Variant: `wwfwfesta` differs only in the SW1:8 factory default (FBI logo off).

## Not modelled

- The **service coin** (`$140020` bit 2, debounce `$1C0056`, routine 0x4BC)
  and MAME's SERVICE1 bit have no input mapped.
- Test-mode DIP display (0x1C34/0x1D1A raw port dumps) — no service menu.
