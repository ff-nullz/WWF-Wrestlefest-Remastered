

1. Fix glitches in-game
   
   EARTHQUAKE
   - earthquakes finisher doesnt work
   - earthquakes bearhug submission glitches
   
   CRUSH
   - his shoulder breaker (special move) from grapple causes opponent do disappear (soft lock)
   
   SLAUGHTER
   - splash pin from top rope spaens another slaugher sprite (similar to bossman)
   
   JAKE
   - standing clothesline attack - check animation (it looks off)
   
   Warrior
   - Splash finisher - when splash causes pin, two sprites display overlaid, 
     the splash pin sprite, and the standard pin sprite
     - when opponent running at you, power slam by warrior is badly glitched/soft locks
   
   BOSS MAN
   - during suplex and pile driver, when opponent falls after move, the fall animation appears to fall low
   - splash off the top rope, causes a pin, but also spawns another playable bossman
   - boss man slam (executed when opponent is running at you) glitches. similar to warriors power slam
   - running splash finisher causes pin, but during pin, two sprites visible (similar to warriors splash pin)
   
   
   DIBIASE
   - FIGURE 4 leg attac submission glitches
   - standing submission (million dollar dream) glitches/doesnt work
     
   PERFECT
   - mr perfects dropkick is only vertical (it should also travel horizontally)
   - perfect plex glitches badly - doesnt work, or trigger pin
   
   HOGAN
   - hogans legdrop still has sprites missing around his foot/leg at the last frame
   - suplex, towards the end of the animation, two sprites of opponent show
   - atomic drop near the ropes causes hogans sprite to postiion too far outside the ring (needs correction)
   
   RING OUT
   - when ring-out, sometimes when you engage with the opponent tag on the far right, you and opponent teleport to the left
   - rope layering while ring-out arena is displayed - and wrestler is inside ring (wrestler should be behind ropes when in the ring)
   - when ring-out, and we go back into the ring, the tag partners outside need to go back onto the apron (sometimes they get stuck off the apron)
   - CPU - when ring-out, CPU should also control the 2 other tag players (brawling)
   - stairs and box weapon outside of ring needs to be put in place
     -> DONE 2026-08-23 (weapon.c + anim/core/ai/hit; docs/engine-specs/weapons.md):
        both spawn on the ringside floor when the ring-out scene shows, human
        pickup with A/B beside them (move 0x70), carry poses + slower walk,
        swing/throw (0x1E/0x1F, hit record 0xF result 6, 0x14 damage), CPU
        fetches and swings them too; dropped when hit or when climbing back in

  REFEREE
   - ref still sometimes stands infront of both wrestlers (gets in the way)
   
   GENERAL
   
   - in the stock game, on occasion, when a wrestler is fatigued, they cant complete a throw, and the opponent gets a reverse throw - implement
   
   - when someone is irish whipped, they should not be able to attack for a cerain amount of time/frames
  
   - when two cpus grapple, nobody wins (grapples shouldnt go on forever)
   
   - during a pin, when opponents run in, they should be ONLY able to stop a pin via a stomp
   - if you re-pin while all wrestlers are still in the ring, sometimes they leave (wierd behaviour)
 
   - after playing a few times (5?) sprites stopped appearing, and only shadows appeared (black sprites)
   
   - sometime during announce screen, announcer womans pallete is corrupt
  
   - leg attacks are submissions - and submissions are like pins, so tag partners should be able to run in and kick/punch the submission to abort
   
   - fatigue indicator missing- panting while getting up (which should trigger choke holds and bearhug submissions)
   
   - running - you can hold direction towards ropes and it redirects run towared ropes (endless rope rebound)
   - running at ropes towards opponents tag partner, then pressing fire at ropes, causes the tag partners animation to teleport inside the ring for a moment (looks like i can attack him) - but its just a phantom sprite
   
   - Sometimes the tagged opponent hangs out in the ring (doesnt leave at timeout)
   - You can pin the tagged opponent (this isnt allowed in stock, only active tagged opponent can be pinned)


- PIN rules - currently, cpu oftens just kicks out. ability to kick out needs to decrease based on energy levels

- submissions - same as pin, cpu can always kick out. mechanics need to match stock, with match-ending if energy is 0 in a sub (give up)

- TAG while in headlock - double teaming doesnt work (yet) - to be implemented

- 1P (2P, 3P, 4P) badges above wrestlers heads (when appropriate) - mimic stock

- ~~HUD visuals, such as opt-ins for coins, text above energy meters~~ done V389 (0xC710 prompts, 0x8BA2 START buy-in); mini wrestler-select menu when in rumble mode — open

- ~~CPU players should have HUD/Energy meters hidden~~ done V389 (stock: gauge/portrait only for seated humans, 0x7506/0x7720)

- ~~button-mash animation overlay (get up) while in-game is missing.~~ done V387 (0x899C)


---------- NEXT STEPS

         
- TEST ALL PLAYERS AND ALL MOVES AND ALL MECHANICS 

- GET 'TOURNAMENT' in tag team working (road towards championshp/LOD) with intermediate screens

- RUMBLE mode

- ENDING CREDIT PAGES

- ATTRACT/SPLASH/DEMO pages



- ROMS - extract assets, and everything else, catalog properly in tables of some kind, in view of discarding roms
         (discuss formats, probably json, but could also do sqlite)

----------------------- THIS IS THE 'I HAVE A COMPLETE STOCK PORT' LINE ----------------------

MODS 

- FIRST MOD, GET LOD WORKING, FIX FINISHING MOVE, TITLES, PORTRAIT NEAR GAUGE
- START TO ADD WWF SUPERSTARS PLAYERS (ANDRE THE GIANT)


- CPU in tag team never goes for a tag (added 2026-08-23)
- Picking up a downed opponent: stock does not allow it every time and not while the victim has lots of energy (he rolls away as you try) — transcribe the stock rule (CPU side: 0x1DB28 roll table 0x23F78/0x23FC4 by difficulty×band; human side TBD; user: it is inconsistent — sometimes works at full energy — i.e. an RNG roll) (added 2026-08-23)

---------- PLAYTEST 2 (user, 2026-08-23 evening, after rumble + HUD + data layer)

GENERAL
- [x] tag team campaign — full ladder + continue + ceremony + interludes (V399..V405)

RUMBLE
- [x] some men get stuck downed and never get up — stale pin intent (V395)
- [x] CPU facing jitters — NEAR approach now waits (0x3D) at a held/engaged target; 0x20BDA exact target filter (V395)
- [x] CPU re-target: exact 0x20BDA candidate filter + 0x1CB06 rumble idle re-target (V395)
- [x] CPU punches towards grappling men — waits beside instead (0x1CCBC) (V395)
- [x] CPU just stands — stale mutual links (ground move struck mid-move), stale engaged flag, dx==0 deadlock (V395/V398)
- [x] double cover — 0x216E8 helper + 0x13400 pile faller (V396)
- [x] eliminated men: +0x32 b4 no longer cleared at rise — shame pose 0x1D3 + walk-out 0x7A, unhittable (V395)
- [x] grappling stops — the ROM's deferred divorce (bset #7,+0x26 / 0x2930) modelled; tag recall walk not tie-up eligible; held-by-nobody release (V398); 60k-frame CPU runs clean
- [~] behind grab 0x1C/0x1D: slide fixed (unrouted moves clear the mover); full subsystem in progress (agent)
- [?] stomp vs cover in rumble — works in the `rpin` probe drive; need the user's exact case
- [x] HUD: CPU rows are empty seats (big white plates), partner row gauge+portrait+1P (V397, MAME 0021/3101)
- [x] HUD tag partner gauge/portrait (V397)
- [?] referee: the engine's tag idle walk already parks him at the top (y 0x118) — SM5 0x205AC exact; rumble idles at the 0x1F9B0 point; need the user's case

---------- PLAYTEST 3 (user, 2026-08-23 night)
- [x] audio: match-start "congratulations" — fixed V407 (match-END announce 0x2A mis-fired from eng_intro_end; 0x312F is the bell)
- [x] audio: in-match YM music — stage BGMs cmd 04/06/0A/0C/0F re-rendered full-length + trigger per stage (V407)
- [x] audio: select music cmd 03 (37.3s) + char-select/aisle cmd 0E (58s) re-rendered with loop seams (V407)
- [ ] extra human players P2/P3/P4 buy in with credits by pressing their START (tag: P2 = P1's partner; P3/P4 = the right-side pair; rumble: four individual men)
- [x] keyboard map JSON + wfeditor Input tab (V406)
- [x] stock DIP switches — docs/dip-switches.md + 'dips' rules table, key consumers wired (V406)
- [?] tag: I can grapple my own partner — audited (see PLAYTEST 4 note), need the exact scenario
- [ ] tag: partners sometimes run in and just stand there (inconsistent with stock)
- [ ] "1P/2P/3P/4P" indicator over the human's head at match start / on tag / "change over" marker over the partner when control switches to him during a pin run-in (stock)
- [ ] multiplayer tag details (user): the partner player on the apron walks up/down, has 2 attacks on a nearby opponent: a punch (differs from the standard one — check stock) and a GRAB (holds the opponent for the active player to hit; expires or opponent knocked down)
- [x] rumble: climb-in animation not aligned with the ropes — the entrant's pre-walk
  stopped inside walk_to's 8px arrival tolerance ((0x280,0xF9)); stock 0x1D74A clamps
  the walkway walk to exactly (0x279, 0x100) before move 0x69 starts, and the climb
  poses 0x20-0x26 bake the whole 3-rope section (tiles 0x2544-46, bank 14, 7 cols ×
  y +25/+41/+57) that only lines up at that spot. Snapped on arrival (V401).
- [ ] rumble palette check (engine observation, V401 session): during the climb-in probe
  a wrestler in the pin pile rendered with green body / pink highlights (colors of sprite
  bank 1), and stray bank-1 pixels showed near the climber's hip. Possibly the stock
  0x2AEA/$1C1610 per-match palette-slot allocator (engine keeps identity banks) or rumble
  duplicate recolors — verify against MAME before chasing.

---------- PLAYTEST 4 (user, 2026-08-23 late night)
- [x] tag matches 2/3: glitched/corrupted ring-out physics, match-3 ring-out soft lock —
  root cause: the engine played EVERY stage in scene 0 while the ring-out enter/return
  tables (0xFA00/0xFA0A) are per-stage arena pairs. Stock 0xC98 sets the match scene from
  0xD12[stage] (00 05 01 05 00 05 00 01 05 00): stage 1 = scene 5 (Wrestling Challenge
  arena, ring-out view 6), stage 2 = scene 1 = THE CAGE. Fixed V402: scene per stage at
  init + the 0x2831C cage law (nobody leaves; airborne men bounce off the wall, zone 6).
- [?] Hogan walkout animation missing + soft-lock — believed to be the same scene
  corruption (Hogan thrown out in the broken ringside scene); the aisle walk-in with
  Hogan verified working headless (charselect drive → aisle → match). NEEDS USER RETEST.
- [x] missing arenas (cage etc.) — the three arenas now rotate per the stock ladder
  table; the cage composes (steel mesh FG) with its own law. V402.
- [?] continue → opponent's tag partner stayed in the ring for a while — engine: partners
  are frozen through the ring intro, then walk out ~2s after the bell (recall → 0x4F
  climb-out, verified headless). Stock 0xA6B0 hides the announcer plates at stage != 0 but
  the step timing looks the same — needs a stock side-by-side before changing. RETEST.
- [x] ladder drive harness: stamped never re-armed after a no-front re-init + campaign
  never armed in bare runs (stage pinned 0) — fixed V402; full 0..9 ladder + ceremony
  runs headless (WF_CPU2=1 WF_LADDER=win --drive ladder).
- [x] rumble: referee post-count walk-back (0x1FF52), diagonal-run guard + stock run stops, CPU horizontal approach (0x1CDF4/0x1D01A) — MERGED V404
- [x] CPU difficulty: pin kick-outs by hp class 0x1E452 + tie-up exchange counter 0xF766
  + rumble bias row 0xF8D0 — MERGED V403
- [x] audio: the match-start "congratulations" was the match-END announce 0x2A mis-fired
  from eng_intro_end (0x312F is the RING BELL — Z80 0x20DF + OKI 31j10 phrase 15; intro-end
  burst now 3100/3120/312F/31NN, MAME-identical); in-match stage BGMs cmd 04/06/0A/0C/0F and
  game/char-select cmd 03/0E re-rendered full-length with loop seams — MERGED V407
- [x] LOD interludes: belt/matches-left (0xBDA6), LOD talk before stages 4/9 (0xAE20), title-win card (0xB608) — MERGED V405
- [x] keyboard map JSON (data/keymap.json + mods overlay + wfeditor Input panel) and stock DIP switches ('dips' rules table, docs/dip-switches.md; coinage/prices/difficulty/championship-4th/FBI/flip/demo-sounds wired) — MERGED V406
- [?] tag "can grapple own partner": not reproducible headless; audited vs stock — the
  tie-up scan only ever pairs a man with +0x7A (never the teammate, both engine and ROM),
  the hit prefilter 0x24126 has NO teammate veto in stock either (partner strikes are
  legal, just silent — 0x24900 skips the crowd sound), and 30k-frame tag runs show
  opp==teammate never occurs. Added the scan's missing 0xF5A2 same-side-of-ropes test.
  Need the user's exact scenario (probably during a pin run-in) to chase further.
- [x] rumble ending: King of the Royal Rumble (0x20254) — ref $8009 win pose, winner walks
  to (0x2C0,0x160), announcer move 0x8E walks in, name + phrase 0x2B, winner per-wrestler
  pose move 0x79, then the championship ceremony -> attract (never another match). MERGED V408.
- [x] wfeditor UI overhaul: fullscreen + F11, dark theme, tooltips everywhere, decoded
  Match panel (state/flag names, roster names), Rules form with explanations. MERGED V408.
- [x] pin only breakable by stomp — the scripted pins (reversal cover 0xC048, splash pin,
  0x152DC family) never carried hit record 0x1D, so punches could not land on the pinner;
  the plain cover already worked (verified: WF_JAB rpin probe → react 0 → 0x24C0C BREAK).
  All pinning men now carry 0x801D (V410). Note (user): a stomp on the pinner = kick-out
  equivalent — that IS the 0x24C0C break (pinner thrown off, count dies). V410.
- [x] grapple reversal teleport (Hogan reversed Earthquake, pair jumped across the ring) —
  the diverting throws (press slam 0x19, Jake slam 0x33, suplex 0x26 family) PARK the
  hidden victim up to 0x100 away ("clear of play"); a failed 0x1129E roll resumed the pair
  at the park spot. handler_fumble init re-seats the victim at the thrower. V410.
- [x] rumble: Hogan's walk-out missing + soft lock (1P game) — when the LAST human was
  eliminated, the engine stamped result 0x8001 on every active man the moment the
  eliminated bit set, freezing the walk-out mid-exit and leaving nothing to end the match.
  Stock (0x20244) only latches $1C16C5 b0 + stops the clock; the arena keeps running while
  the per-player continue queue counts down, then b1 -> $16C5==3 -> 0x1AFC game over.
  Engine now: b0 + continue window (queue/buy-back 0x201D8 TODO EXACT) -> results -> game
  over -> attract. Verified 1P E2E headless (WF_SEATED=1). V411.
- [x] rumble walk-in: choosing Hogan showed no walk-in — the 0x7C28/0x7C48 Hogan/id-9
  order swaps are TAG-only (stock 0x7C1C branches past them in the rumble); the engine
  applied them, handing w[0] to slot 1 and deactivating Hogan's walker (rumble walks
  k==0 only). Verified: full gameselect→rumble→charselect→aisle drive shows Hogan
  walking with his plate. V412. (The V411 all-humans-out fix stands on its own.)
- [->] ringside weapons: stairs + box pickup/swing/drop (+0x74 b7 / +0x76 objects) —
  agent, in progress.
- [x] double team: the enemy partner entering after the move lands = already in V409
  (0x21424 arms the victim's apron partner with the 0x214BA countdown + autopilot at
  the holder — the same run-in machinery as the pin break).
- [x] third-match ring-out corrupt ("camera pan") — the 0x298B4 camera clamp table has
  SEVEN rows; row 6 (arena-B ringside) = {140,200,3C0,200} with y LOCKED. The engine
  registered 6, so scene 6 clamped with the match limits and the camera panned onto
  unauthored map rows. V413.
- [x] continue-portrait palette (all wrestlers) + post-continue black aisle/select box —
  the front scenes never ran their 0x2AEA installs; armed the on-demand bank map in the
  continue screen (portrait pal 0x1E + TV-static overlay pal 0x16), the aisle (0x7C16
  #$34 + per-man banks) and the charselect. V414.
- [ ] ringside facing rule (user, stock-checked): a ring-out man FACES the nearest ENEMY
  who is also outside (never the tagged-in man inside); not free directional facing.
  Exception: run + direction runs that way (same as in-ring behaviour). To transcribe
  in the ringside brawl targeting.
- [?] Sgt Slaughter's grapple head-punch (0x32 three-punches) "reversed mid-move" — could
  not reproduce (runs to completion vs mashing CPU both directions); need the exact
  scenario (who reversed, what it looked like).
- [x] Slaughter throw reversed mid-move — his cat-D move 0x3B (the TORTURE RACK, 0x17D12)
  was UNROUTED: the raw cells played while the victim sat in 0x7B with the auto-reverse
  clock running. Fully transcribed (lift/0x8D bounce/carry/rack with submission cues,
  0x62 mash-out -> 0x5A, KO -> 0x6E/0x6F, run-in arm). V417.
- [x] rumble CPU pairs grappling forever — engine cap: the 4th lockup-timer expiry knees
  unconditionally (0x1F05E kept missing on both sides). V416.
- [?] Slaughter whip -> returning Hogan hit with a knee -> froze MID-AIR (recovered on the
  next hit) — the cat-3 catch path (rec 04 res 3 -> react 0x0E fly-over) verified clean
  E2E headless; cat-4/react-0x10 and react-0x14 paths routed too. User: it was a PUNCH
  (B1) and Hogan was the CPU. Scripted whip+punch probes resolve cleanly — still not
  reproduced; suspicion: the whipped-run return state (f33 b4) hitting a case-4 angle
  branch. Watch for it in play; a save-state-style trace would pin it.
- [->] repeated pin run-ins confuse the engine (user spec: every pin re-arms both
  partners + resets the in-ring grace) — agent, in progress.
- [x] ringside weapons: STEPS + BOX — full stock system (objects $1C0F1C/$1C1028 machine
  0xFDEE, pickup 0xF0BA/move 0x70, carry stance rec 0xE + slow walk, swing cat 0x11 ->
  0x1E/0x1F rec 0xF dmg 0x14 + throw, tumble/rest, drops on climb-in/struck, CPU arm
  0x1C6B6 + 0x23ED0 roll, no tie-up/rescue while armed). MERGED V418.
- [x] campaign aisle on EVERY match -> first match only (0xAC0 -> 0xB1C vs 0xBD2);
  interludes/continue rematches go straight to the init. V419.
- [x] repeated pin run-ins "everything gets confused" — three real bugs: (1) the referee's
  SM2 escort walked to target.x ± 0x30 AWAY from centre (stock 0x1FC92: ∓0x50 TOWARD it) —
  near a rope the dest sat outside, the ref stuck in SM2 forever and no later pin counted;
  + the SM2 head now re-runs the pin hunt each frame (0x1F9F8). (2) a second pin re-arming
  an INSIDE partner never restored the run-in marker, routing him to the walk-out machine
  mid-grace. (3) eng_tag_swap set the pad bit unconditionally — CPU tags minted phantom
  pad-readers (0x188DE/0x188E6 gate). Per the user's spec: every pin re-arms both partners
  wherever they are and the in-ring grace RESETS (verified stock: 0x133E2/0x215FA/0x1D582).
  WF_PIN=again[:frame] = the two-pin repro drive. MERGED V420.
- [x] 1P/2P/3P/4P chips + the CHANGE UP flasher — the $1C14CE overlay machine (0x8710)
  transcribed: row 0x1E poses (0-2 = the animated red CHANGE UP, 3-6 = green/yellow/pink/
  blue 1P-4P), 0x80-frame life, floats at z+0x68, pal 0x2AEA #$35. Triggers were already
  in hand_pad/pad-restore; go-live arms the P chip on every pad man. Engine shows them in
  1P too (stock gates on the 2P-vs flag $1C007C — TODO EXACT with multiplayer). V421.
- [->] attract cycle + leaderboard — agent, in progress.
- [x] rope-stop move 0x3F (both buttons folklore; really an energy-band d100) — 0x18370
  lean handler + the 0x1EDC4 whipped-run roll vs 0x1EE38[stage][band*2] weights (20/15/10%
  early stages, richer later). Deviation from stock kept on purpose: the engine rolls for
  every whipped man, not just under the 2P-vs flag $1C007C. V422.
- [x] double-team polish: (1) the teammate's corner-climb glitch flash — the receive now
  seeds the corner spot (side-picked x/y, z=0x180) before state 8 row 4, no more one-frame
  teleport; (2) after the move both attackers are inside, so the ENEMY partner now enters
  and brawls — 0x214BA forced-run-in mark (f32|=0x04) rides eng_ai_rescue_tick and clears
  on fire. V422.
- [x] Boss Man splash pin didn't trigger the run-ins — the scripted pins latched the cue
  but never armed the partners; splash (0x13EDE) and the reversal cover (0xC048) now call
  eng_tag_arm_pin like the normal cover. V422.
- [x] SOFT LOCK: "ref kept counting to 3 over and over" — the completed fall (cell 7)
  never cleared the pinner's f35 b0 cue; after the win pose the idle hunt re-found the
  live cue and each recount re-stamped results 0x4000 over the finalized 0x8000, so the
  match could never end. Cue + pinning now die with the fall. V422.
- [x] grapple + in-ring timer expiry -> "opponent spawned another sprite and walked out"
  — the usher recall yanked a mid-grapple man into the walk-out under the grapple
  composite; the apron machine now waits out state 5/0x0C/0xFF/partner-linked men
  (stock 0x202BA only ever points out free-standing men). V422.
- [x] post-run-in FIXATION soft lock ("couldnt grapple the opponent in the ring, fixated
  on the wrestler outside") — the per-frame target selector 0x2095A/0x20A3A -> 0x20CCC
  (humans) / 0x20B44 (CPU) was never transcribed for tag/singles (the rumble half was);
  the tie-up scan pairs strictly through +0x7A (0xF586), so an opp left pointing at the
  recalled apron man locked the legal pair out of grappling forever. Transcribed into
  tieup.c: legal man re-points at the LEGAL enemy when the enemy partner is back out.
  Probe: WF_PIN=again — P1+P3 lock up again after the ushers point the partners out.
  + user spec: run-in CPU goals — stomp if teammate pinned (already in the rescue think
  0x1D3F0), brawl the opposing intruder, fall back to the STANDING enemy when the pick
  is downed (armed rescuers re-pair when their target is gone/out/downed). V423.
- [x] tag cascade (timeout during a grapple on the recalled man): (1) the recall walk-out
  now waits for a FREE-STANDING man — state 4/2/3 slipped the V422 guard and walked out
  of a lying/flying pose ("walked off screen above the ropes"); (2) a stuck OUTSIDE bit
  (f33 b2) on a man entering the ring flipped him onto the ringside motion law (z floor
  0x100 — "fell below the ring") and blocked the b2-equality tie-up gate (0xF5A2 —
  "couldn't grapple" even face to face): the 0x4D/0x4E enders and the swap now clear it;
  the do-nothing fixation itself was the V423 selector. V424.
- [x] P chip hides the moment its man engages (grapple/move/held) — user request,
  engine addition over the stock 0x80-frame life. V424.
- [x] two CPU wrestlers grappling forever — an ENGAGED autopilot man whose run-in grace
  marker died mid-grapple was handed back to the apron machine (which rightly waits out
  engaged men): nobody drove his side of the lockup/hold. Engaged b6 men stay AI-routed.
  V424.
- [x] tag win celebration (user spec): at the bell every decided man is freed (engaged
  pairs unlinked, a lying WINNER springs up, a winning apron partner climbs in via 0x4E),
  the fall now stamps BOTH teams (winners 0x4000 / losers 0x4001), and the apron machine
  stands down once the match is decided — so both winners take the stock 0x8B walk to the
  mid-ring spots (0x254/0x29C) and high-five (0x1AD10, slap 0x30); losers take 0x8C.
  Probe: pin drive shows "win: o0 -> 254" + "win: o1 -> 29C". V425.
- [x] CAGE whip: a whipped runner hitting the cage (zone 5, +0x44 live) FALLS with
  damage 8 (react 0x17) instead of rebounding — 0x11CFA; the ringside barrier crash
  (zone 3, dmg 0x0A, sound 0x28 — 0x11CE0) came with it, and the default turn now
  matches stock for every other facing-side clip (0x11D14). V425.
- [x] wfeditor v2 (user request): change management — every table/rule value that
  differs from data/stock.json renders AMBER (grid cells + rule rows), tooltips show the
  stock value, rule rows get a "stock N" column with one-click revert, table lists mark
  drifted tables "*", the status bar counts tables off stock; a "Stock" button reverts a
  whole table to pristine. data/stock.json = ONE pristine-values JSON written by the new
  C tool `wfengine --export-stock` (ROM backend, self-verified; Tools panel button
  regenerates it). Style pass: gold active tabs/rows, rounded widgets. V428.
- [x] MULTIPLAYER BUY-IN block (agent, merged V430): 4-seat joins (0x18C4 -> 0x6E5E, price
  SW1:3, legal-man buy hands the pad to the APRON partner 0x6E9A, P chip, no regain
  double-charge 0x6EE2); Players DIP ($1C0066 & 0x800 = the 2P-cabinet fork 0x70CC/0x6F0E
  — NOT rumble); rumble live joins (0x18E4 queue -> 0x9132 pick, ids 4/5 banned) + the
  continue window (0x2021C 0x3001 words, 0xBCFE decay + "PLAYER-n IS DISQUALIFIED" blink;
  stock has NO 1P buy-back — $1C16C5 gate 0x18DA -> 0x1AFC attract); apron-partner
  controls (rail walk 0xF2F6, cat 0x13 presses: punch 0x39 rec 0x19 / behind grab 0x1C
  +0x32 b0 -> 0x52 hold); the apron WORRY POSE (0x110E0 -> pose 0x1D3 for 0x20 frames on
  a teammate's slam) — baseline moved for it (digest 838997b7d1c1e8f3, explained in
  ba093b4); P3 = IJKL+Y/U+3/7, P4 = numpad+KP0/Enter+4/8; WF_P3/WF_P4 script envs.
- [x] LAUNCH PROFILES (user request, phase 1 of the mod plan): a profile = a named
  launchable version of the game — profiles/<name>.json { name, description, mods:[...] }
  with its own pak cache under build/profiles/<name>/ (wrestler/gfx paks reuse stock when
  the profile's mods don't touch them). `wfengine --profile X` (auto-packs when stale),
  `--profiles`, `--pack-profile X`. STOCK LOCK: no profile = zero mods, and the editor's
  base layer is READ-ONLY — every Save requires a mod layer, so data/ game data can never
  be corrupted. mods/order.txt is retired (the profile IS the order; leftover file is
  ignored with a note); tables/keymap/gfx/wrestler mod resolution all route through
  src/profile.c. Editor "Profiles" panel: stock row (locked, Launch), profile rows with
  Launch/create, per-profile mod checklist with ordering, Save/Pack; save-layer picker;
  profile name in the status bar + game window title. Shipped example: profiles/sandbox
  (endless run-in brawls, slow count). V431.
- [x] close-whip counter didn't trigger ("tries to throw just a standard punch") — the
  anti-run category gate tested state==2; stock 0xE2EA tests the RUNNING FLAG (+0x33 b4),
  which rides through the rope turn/skid — exactly the close-whip window; + the 0xE2E0
  +0x34 b5 self-gate. V432.
- [x] rumble "sometimes i cannot pin a downed opponent" — a degraded press (entry 0xFF /
  out of every proximity window) cleared the presser's link (0xDFC8) but left the LYING
  man's back-link pointing at him, and the pin category gate (opp->partner < 0, 0xE0B8)
  then blocked every later cover. Degrade paths now drop the back-link (engine guard,
  TODO EXACT the stock unlink site). Baseline moved (digest ae126c76ce21e055): the 1200f
  selftest itself hit the stale-link path — the old digest baked the blocked-pin state.
- [x] rumble DOUBLE PIN (user-confirmed stock): B2 at a live pile JOINS the cover (the
  same second-cover 0x48 the CPU helper fires, 0x209D6 target hand-off) instead of
  breaking it; B1 keeps the stomp/break. Every extra cover pushes the CPU victim's
  kick-out one half-count later (TODO EXACT the ROM's pile term). WF_RPIN=join probe.
  V433.
- [x] MOD PLAN PHASE 2 batch 1 — `game_rules` synthetic table (all NEUTRAL in stock, so
  the stock digest is untouched): unlimited_energy (1 humans / 2 all; per-frame refill),
  unlimited_time (hud 0x26370 gate), difficulty_offset (row shift on the 0xF878 tie-up
  bias), speed_pct_human/cpu (walk/run 0x116AE/0x11D4A scale), cpu_kickout_delay
  (0x1E45C shift). Editor Rules panel documents each. Example profile: trainer
  (unlimited energy everyone + no clock; verified hp pinned at max vs stock 91). V434.
- [x] "opponents sprite just touches me -> i fall" — the ROM's struck-victim epilogue
  0x24E24-0x24E40 was never transcribed: the victim's ATTACK RECORD (+0x4C) survived the
  hit, so a man knocked down mid-swing flew/lay with his strike box live and floored
  anyone he touched. Now cleared with hold_t / the running flag / last_pair, per the ROM.
  Baseline moved (6625629732f85acb — every selftest hit's aftermath changes). V435.
- [x] run-ins "sometimes just stand there" — the brawl tail only struck ONCE when the
  LEGAL man wandered close and never approached anyone. Now: priority 1 stomp the pile
  (unchanged, 0x1D3F0), priority 2 close in on the enemy INTRUDER (selector target,
  downed pick falls back to the standing legal man) and strike/grab on a 0x30-frame
  re-roll instead of a one-shot latch. Probe: WF_PIN=again shows the P2+P4 intruder
  lockup. V435.
- [x] win high-five faced the same way — the pose faced the opponent link; the pair now
  face EACH OTHER (legal spot 0x254 faces right, partner 0x29C faces left). V435.
- [x] MOD PHASE 2 batch 2: dedicated RUN key (keymap entry, default unbound, = B1+B2 —
  bind it in the editor Input panel, engine-native config so it works in stock too);
  ref_knockdown + ref_down_frames (strikes floor the ref — SM10, kneeling shake, counts
  and hunts stop, the pin cue survives; recovery timer); weapon_dq (a landed 0x1E/0x1F
  swing DQs the swinger's team — results + win pose + celebration). All neutral in
  stock. V435.
- [x] merged the attract-fix branch (all 7 items: card palettes via the 0x8CF8 bank
  installs, blit newline keeps the record column 0x251DE — the garbled demo text AND the
  trademark centring, demo results gated off 0x10F8 ("holding their heads"), TIME plate
  after the wipe-in, title-card text set, LOD taunt screens 0xAE20 modes 1/2, prompt
  spam gate 0xC718). V437.
- [x] MOD: cpu_energy_meters — CPU HUD rows draw gauge + portrait (LOD share Hawk's
  portrait per the user); buy-in presses still work under it. V438.
- [x] MOD: grapple_gauge — a discreet 3-cell HUD-style bar at bottom centre showing the
  tie-up hold clock (0x12526 seed) draining; BLINKS once the 0x12550 throw window opens.
  Fixed spot (floating collided with the GET-UP overlay eraser). V438.
- [x] wfeditor: --editor is EDITOR-ONLY now (launch games from the Profiles panel;
  --editor --play keeps the side-by-side window); Input panel gained the P3/P4 pair
  toggle (the buy-in seats were unmappable from the UI). V438.
- [x] wfeditor v3 (user request): header EDITING dropdown — pick which profile's tables
  the editor shows/edits, stock included (READ-ONLY, for flipping back and forth to
  compare values; table registry hot-reloads from the profile's pak); Quick save button
  (apply live + write the current table's JSON to the profile's save layer + repack its
  paks in one click); the save layer follows the dropdown (a profile with no mods gets
  one named after itself); Profiles tab laid out as a proper table (PROFILE/DESCRIPTION/
  Edit/Launch columns, stock row with Compare). V439.
- [x] MOD PHASE 2 batch 3: exit_ring (hold into a touched rope ~0.5s -> the 0x1B4DC
  over-the-rope hop, no damage; tag only, never cage); backup run-ins (backup_avg_secs/
  _max/_stay: a random not-in-match wrestler takes the rumble entry 0x7430/0x69, brawls
  as an intruder vs a random man, leaves via the 0x7A ringside walk-off; verified w10
  in -> brawl -> leave -> w8 in). Example profile: chaos. V440.
- [x] rumble "some characters decided not to get up / grapple forever" — V435's struck
  epilogue ran on VETOED (shrugged) hits too, zeroing the victim's hold/get-up clock
  (+0x46) and record mid-hold; stock skips the 0x24DEC/0x24E02 tail on carry-set
  (0x240F0 bcs). strike() now reports effect and the epilogue is landed-gated. V440.
- [x] rumble facing jitter -> 3px dead band in eng_face_opponent (0x10C46 has none —
  user-requested debounce); get-up mash overlay slowed ~30% (0x89CA 2 -> 3 frames);
  window title em-dash -> plain '-' (mojibake in the WM). Baseline moved for the dead
  band (18ffaf29d9dc208b). V440.
- [x] MOD camera zoom (phase-2): camera_zoom_pct — the compose window grows past 320x240
  (up to 512x384; the FG/BG planes are 512x512 and sprite coords 9-bit, so a wider window
  just shows MORE of the same arena). Renderer owns the pitch (all frame buffers sized
  512x384), sprite screen-clip and camera target/clamps are view-aware (locked axes
  recentre), FG0 anchors to the view edges (rows>=16 bottom, cols>=20 right). Match view
  only — every front-end page stays stock 320x240. Example profile: widescreen (75%).
  Fixed en route: wf_video_write_ppm allocated 320x240 and drew the zoomed view into it
  (heap corruption on --shot). V441.
- [x] MOD deterministic grapple (phase-2 COMPLETE): grapple_pick — the stock facelock
  throw feels random because the 0xDD.. selection alternates the matrix BANK every press
  and follows the victim's energy band; with the mod on the HELD DIRECTION picks the row
  (neutral/UP/DOWN = the band-0/1/2 throws, bank 0) and B1/B2/both still pick the column
  — same input = same throw, always a stock-reachable move for that wrestler (no freeze
  risk). Probe: stock cat=B/entry=24 vs precision cat=A/entry=18 on identical input.
  Example profile: precision. V442.
- [x] PHASE 3 — MODE DESCRIPTORS (mode_rules, all stock-neutral): team_size (1 =
  SINGLES seating, slots 1/3 empty, verified via the pin drive), pin_enabled (hunt
  gate), ref_enabled (no referee), countout_enabled (20-count frozen), weapons_mode
  (0 none / 2 HARDCORE — the 0x19A66 climb-in drop skipped, carry into the ring),
  time_limit_min (BCD; 0 = no clock), rumble bell (2..8 — slots 2/3 carry CPUs above 7,
  buy-ins take them over) / live cap / spawn interval, runin_enabled. Profiles:
  one-on-one, battle-royale (8 at the bell, fast entrances, verified 8 seated),
  hardcore. V443.
- [x] camera mods reworked per the user: camera_fixed — a LOCKED view (no panning),
  sized PER SCENE from the camera-limits row (a y-locked ring-out strip letterboxes to
  240 tall instead of painting void — "the arena is corrupted"), centred on the pan
  range; fixedview profile. camera_zoom_pct (panning zoom) stays for those who want it.
  V443.
- [x] merged the attract agent's Hawk-taunt fix: the D1FC stream decoder clamped strips
  at 32 — the taunt base pose is ONE 85-strip record (stock spriteram dump: 87 live
  records), so cell 0 drew only the left third ("half of hawk disappeared").
  Clamps raised to the count byte's range, WF_THINKER_MAX_SPR 64 -> 160; MAME-oracle
  verified, zero dropouts over the whole talk screen. V443.
- [x] MULTI-MAN TEAMS (phase 3 tail): mode team_size 1..3 + team_size_b (0 = same) —
  SURVIVOR 3v3 (slots 0/1/4 vs 2/3/5, teammate links CIRCULAR so the stock tag swap
  rotates through the trio untouched) and HANDICAP (1 vs 2/3). The fall now stamps
  results by SIDE (the teammate-link stamp missed third men). Profiles: survivor,
  handicap. Verified: survivor pin lifecycle + run-ins + celebration. TODO: a third
  winner shares the 0x29C high-five spot (cosmetic); third-man picks use (leader+6)%12.
  V444.
- [->] fixedview "still out" on the user's live game — NOT reproducible headless (both
  arenas clean at 512x384, live-match + stage 1 probes). Diagnostic added: with
  camera_fixed on the WINDOW TITLE carries [scene cam view], so the next screenshot
  self-documents the state. V444.
- [x] "time runs over demo" — the match clock's FG0 cells (rows 28-31) survived a demo
  segment's end onto the next card page ("TAG MATCH" title card with stale TIME digits
  over it). Wiped at the seg-over transition (stock's page path re-clears FG0 via
  0x1F9E). V445.
- [note] the campaign LOD interludes the user asked for long ago (matches-left title
  card + belt + the pre-title talk at stages 4/9) turn out to be transcribed and armed
  (campaign.c 0xBD2 -> interlude.c queue) — needs a front-end campaign run to verify
  visually.
- [x] wfeditor: SHEET BROWSER in the Wrestlers panel — all 65536 sprite tiles, 512 a
  page, drawn with the selected wrestler's pens, hover = tile id. (The pose browser
  finds a pose's tiles; this locates/inspects raw art — groundwork for clone editing.)
  V446.
- [x] RUMBLE BUY-IN PICK GRID (0x9132, the last multiplayer TODO EXACT): a joining seat
  picks from the portrait strip at FG0 $C1B30 — not-in-play ids (4/5 banned, 0x926C) or,
  arena full (6+ live, 0x9178), the LIVE CPU men to TAKE OVER (0x9282/0x972C). Stick
  L/R moves the seat's claim-checked cursor (0x9578), any button commits, the 0x140-frame
  timer (0x934E) commits for you; each port's nP label rides under its hovered portrait
  (stock's 0x96C6 cursor art TODO EXACT). Old instant-spawn path now harness-only
  (WF_JOINPICK). Probes: takeover commit ("port 3 TAKES OVER w3") + join grid. V447.
- [x] hardcore "clotheslined my own tag partner on the apron into the abyss" — the
  0x2414C victim gate was never transcribed: an OUTSIDE/apron victim is only hittable
  while the ringside view shows ($1C0161 b1). In-ring swings can no longer reach apron
  men. (Friendly fire itself is stock.) V447.
- [x] wfeditor Profiles columns reordered: profile | Edit | Launch | description. V447.
- [x] pick grid made FULLY stock per the user: portraits packed at stride 0xC (0x9236),
  the per-seat 3x2 cursor MARKER (0x970C tiles) drawn two rows above the hovered
  portrait at $C1930 (0x95AE/0x961C — replaces the engine's nP-label approximation),
  and cursor movement is LEVEL-based with the stock 5-frame repeat gate
  (0x9500/0x9556), not edge-based. V448.
- [x] merged the CLONE INFRASTRUCTURE branch (phase 4): wrestler ids 12..15 = clones —
  ROM tables resolve through the BASE id (eng_ws_base, ~50 sites), package data (name/
  stats/palette/sheet) from the clone's own pak with per-file base fallback; sprite
  emit rides virtual rows 0x60+ (rows 12/14/15 are ref/rope/weapons) and a clone
  BORROWS the highest free stock palette bank; packer probes mods for wrestlers/12..15
  (12.pak = 294 bytes for a re-palette). Example: profiles/clones — "HOLLYWOOD",
  a blue Hogan. Regress digest UNMOVED. V449.
- [x] SURVIVOR elimination (mode `elimination`): a fall ELIMINATES the pinned man (walk-
  off 0x7A, next teammate promoted via the stock swap, links re-circled) until a side is
  empty — then the normal result/celebration; cell-6 result stamp gated meanwhile.
  survivor profile now 3v3+elimination. V449.
- [x] survivor human tag broken ("i couldnt tag out my partner but cpu could") — with
  circular 3-man links an apron man FOLLOWED his circular-next (another apron man) and
  never posted; eng_team_legal() now keys the apron/rescue machinery on the side's
  IN-RING man. V449.
- [x] "i got 2 warriors" — the auto third man now picks the first id NOBODY uses; and
  the new MIRROR system (user request): any duplicate seat becomes a runtime ALT clone
  with auto-transformed pens (R/B nibble swap) — verified: stock Hogan vs a blue ALT
  Hogan; mode `mirror_picks` lets the SELECT SCREEN take duplicate picks (profile:
  mirror). V450.
- [x] mirror "glitches bad during walkout" — the AISLE used the raw wrestler id as the
  stream/plate/palette index; an ALT/clone id (12+) indexed garbage. Aisle art now
  routes through eng_ws_base (in-game was already clean via the sprite virtual rows).
  V451.
- [x] CHANGE-UP partner "climbs back out immediately" — at the pad restore (0x2130A)
  the inside partner went back on autopilot with no run-in marker, so the apron machine
  walked him straight out; he now keeps the in-ring GRACE (ai_sub/ai_runin_t) and brawls
  until the recall, per the user's V420 spec. V452.
- [x] CPU rescuer "punches the air, doesn't kick me off" — the V447 outside-victim hit
  gate exempts PINNING men (a stale outside bit on the pinner shielded his cover from
  the break). V452.
- [x] wfeditor: PROFILE CATEGORIES (user request) — profiles/<name>.json gains
  "category"; the Profiles table groups under gold headers (modes/camera/practice/
  roster/controls/fun/misc); all shipped profiles categorized. V452.
- [x] MODE cage_escape_win: hold into the cage wall ~2/3s -> the 0x4F over-the-top climb
  (user: "use the rope climb animation"), drop outside, the escaper's team WINS
  (results + celebration). Verified E2E headless. MOD extended_moves: strikes knock a
  climbing/perched man off the buckle (states 8/9/0xA carry no stock hit record; eng_topple
  launch arc, once per swing, enemies only). Profile: cagematch (both). Category labels
  removed from the Profiles panel per the user (silent clustering stays). V454.
- [x] "warrior running over and over" soft lock (survivor + ref KO) — a stale +0x44 word
  (left from the pin) survived into a SELF-run: the 0xF42E reversal is disabled while
  +0x44 != 0 and the rope turn decrements it once per bounce (a flag-like value = ~32k
  bounces). Both selector run entries now zero grap44. V456.
- [x] ref-KO polish (user): hit thud + crowd on the KO, the downed ref is STILL (single
  kneel pose, no shake), recovery default 30s (0x708); "oh no" announcer phrase TODO
  (phrase map). V456.
- [x] wfeditor: Quick launch button beside Quick save (launches the dropdown's profile /
  stock). V456.
- [x] grapple gauge REWORKED per the user: a TUG-OF-WAR — six cells split at centre, the
  HOLDER's screen side fills with his remaining hold clock (full side = in charge; the
  bar crosses to the victim as the auto-reverse nears), blinks when the throw window
  opens; HUMAN grapples only (no CPU-vs-CPU display). V458.
- [x] survivor third winner gets his own high-five spot (0x2C8). V458.
- [x] 1v1 TOURNAMENT card (mode team_size 1 + front campaign): a new interlude page
  before each match — "TOURNAMENT ROAD / ROUND N", the DEFEATED list (small ranking
  name runs 0xA5BE), NEXT opponent, "N WINS TO THE TITLE"; START/button or ~5s moves
  on. Engine page, no ROM analog. V458.
- [x] 3-man teams: after tagging the THIRD man in, the CPU froze and couldn't be
  grappled — the 0x20CCC/0x20B44 target selectors normalised "the enemy legal man" by
  ONE teammate hop (lands on another apron man in a trio) and never side-checked the
  scan (a second own teammate could read as the enemy). New side-aware enemy_pair():
  legal man + most-relevant other (in-ring intruder first, else apron); identical to
  the ROM hop for 2-man teams (digest unmoved). V459.
- [x] clone identity polish: the aisle shows a NAMED clone's own name (ASCII face via
  the new eng_fg0_text) instead of the base's big plate; the ring-intro announcer
  SKIPS the name call for named clones (no sampled voice for custom names — better
  silent than "Hulk Hogan" for HOLLYWOOD). V460.
- [x] wfeditor PROFILE SETTINGS PAGE (user decision, option 2): the profile popup now
  shows the two game-rules tables (MATCH MODE + GENERAL MODS) MERGED for that profile —
  labeled rows, help tooltips, amber + "stock N" where non-stock; edits Save into the
  profile's OWN mod (mods/<name>/, auto-created + appended) and repack. The raw mod-
  layer checklist moved behind "advanced: mod layers". You think purely in profiles now;
  layers remain the power-user substrate. WF_EDITOR_POPUP=<name> screenshot harness.
  V461.
- [x] merged the SELECT-EXTENDED branch: mode select_extended — SELECT PLAYERS becomes a
  2x6 grid, the LEGION OF DOOM pickable at positions 10/11 (old-project portrait art,
  verified pixel-identical against the engine's own composed cells for five stock
  siblings), B2 on a cell cycles a registered CLONE of that wrestler (name tag over the
  cell); picks flow through 0x5DAC unchanged; legends profile. Stock page byte-identical
  with the row off. V466-era merge.
- [x] wfeditor polish batch (user): Quick save greys out on stock; mods carry
  descriptions (mods/<m>/mod.json) shown in the advanced layer list; the settings popup
  opens from a "mod edit" BUTTON beside the profile name (not the name click); the
  advanced layers section is a bounded scrolling group (it was corrupting the popup
  layout); create-profile row on top; Edit column retired.
- [x] LOD aisle name — the TAG walkout was fine (both plates 51/52 = "THE LEGION OF
  DOOM", verified rendering); the RUMBLE plate rows for ids 4/5 are 0 in the ROM (stock
  never rumbles LOD). New: eng_fg0_bigtext = the GENUINE aisle name face (blit mode-1
  tiles, ASCII-driven) + eng_blit_text (decode a plate back to its string); the rumble
  aisle falls back to the tag plate's text in that face, and CLONE names now render in
  the big face too (tag + rumble). V47x.
- [x] wfeditor batch: layer editing = its own "layer edit" button/popup; Profiles is the
  FIRST tab and the start tab; Reload inline beside Create; pause/resume = a small
  play/pause symbol right of the tabs, greyed when no in-process game; WF_PICKS +
  WF_BLIT harness envs.
