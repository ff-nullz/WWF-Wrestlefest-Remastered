# Table migration brief (ADR-001 step 2) — for every agent / sitting

**Status 2026-08-23: DONE** — 164 tables, 0 unmapped, cut-over shipped (V392).
Kept as the recipe for adding tables; the "transition" wording below is history.

Goal: every 68k-ROM byte the engine reads becomes a **named, registered
table** so `wfengine` can run from `build/base.pak` with no ROM. Read
`docs/adr-001-data-formats.md` first, then `src/tbl.h`.

## What exists

- All former `wf.rom[...]` reads already go through `tbl_ra8/16/32` /
  `tbl_ra_ptr` (ROM-address-space accessors) or the per-file `rom16()/r8()`
  helpers that now wrap them. Under the ROM backend an address no table
  covers still reads from `wf.rom` (transition) but is reported once per
  page: `tbl: UNMAPPED ROM read at 0x..... (n bytes) from src/file.c:line`.
- `tools/cov_drives.sh out.txt` runs the whole drive set (rendering on) and
  collects every UNMAPPED / OUT-OF-RANGE line. `docs/rom-unmapped-2026-08-23.txt`
  is the baseline (702 sites); `docs/rom-coverage-2026-08-23.txt` the ROM
  byte ranges those runs touched (0xSTART 0xEND LEN) — the *minimum* extent
  of each table, never the full one.
- Pilot: `src/hud.c` `hud_tables[]` + `TBL_REGISTER` + `tbl16(TBL(name), off)`.

## Your job, per owned file

1. For every ROM constant the file reads (search `tbl_ra`, `rom16(`, `r8(`,
   `r16(`, `r32(`, `0x` addresses in the old helpers/macros), find the
   **full extent** of the table in `../wrestlefest-decomp/reference/maincpu.asm`
   (the reading loop's bounds, the terminator, the next data block / next
   routine), cross-check with the coverage file, and declare it:
   ```c
   static const tbl_def xxx_tables[] = {
       { "snake_case_name", "base/<subsystem>", ROM_ADDR, LEN_BYTES, TK_U8|S8|U16|S16|U32|S32, STRIDE_ELEMS, "what it is + the PCs that read it" },
   };
   TBL_REGISTER(xxx_tables)
   ```
   - Names: ADR rule 4 — descriptive, no hex (`move_table`, `reaction_records`,
     `banner_glyph_runs`). The ROM address lives only in the def.
   - Group: `base/hud`, `base/ai`, `base/anim`, `base/hit`, `base/front`,
     `base/scene`, `base/gfx`, `base/referee`, ... Per-wrestler rows
     (indexed by id 0..11): group `wrestler` and name it so the packer can
     slice it later (`ws_walk_speed`), still one table for now.
   - Kind/stride describe the JSON layout (rows of STRIDE elements). A
     pointer table is `TK_U32` stride 1; pointer-chased records (glyph runs,
     cell records, scripts) are one `TK_U8`/`TK_U16` blob covering the whole
     record region that the pointers land in — find its bounds.
   - Tables must be **disjoint** and **unique by name** across the whole
     tree (`grep -n '"' src/*.c | grep tbl_def`, `./wfengine --tables`);
     overlap aborts at start-up. If another file already covers an address,
     just read it — do not redeclare.
2. Prefer named access where the index is clean: `tbl16(TBL(name), off)`;
   keep `tbl_ra*` for pointer-chased reads (values stay ROM addresses).
   Either is fine for correctness — the ROM address space is still resolved.
3. Build with `./build.sh --no-bump`. Run `tools/cov_drives.sh /tmp/x.txt`
   and make sure **no UNMAPPED/OUT-OF-RANGE line names your files** (or
   `src/stubs.c` for bus reads your files cause). `make regress` must stay
   `OK` (the digest cannot move — the bytes are the same bytes).
4. Then prove the round trip: `./wfengine --export-all data/tables &&
   ./wfengine --pack data/tables build/base.pak` must report 0 failed.
5. Do **not** run `make baseline`. Do not touch files you do not own. Do
   not sed/python-edit `src/anim.c` (hand edits only). C only.

## Environment

`WF_ROMDIR=./rom` if you work in a worktree (rom/ is
git-ignored). `WF_DATA=rom` is the default while tables are missing;
`WF_DATA=pak` runs from `build/base.pak` (will differ until every table is in).
