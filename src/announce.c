/* Ring announcer — transcription of the 0xA0E8 / 0xA0FE request driver.
 * Gameplay posts a request pair ($1C15D2 = wrestler id or 0x0F for none,
 * $1C15D3 = phrase id); the driver first says the wrestler's name (table
 * 0xA1B6: {cmd, dur} per id), then steps through the phrase's {cmd, dur}
 * sequence (0xA216 -> record {count, (cmd, dur)...}) issuing one sound
 * command per step and waiting `dur` frames. SIMPLIFIED: the 0xA404
 * legal-man check that can veto the name is skipped (TODO EXACT); a new
 * request restarts the sequence at step 0 (the ROM leaves $1C15D6 stale). */
#include "wf.h"
#include "engine.h"
#include "tbl.h"

static unsigned rom16c(uint32_t a)
{ return ((unsigned)tbl_ra8(a) << 8) | tbl_ra8(a + 1); }

/* Tables this file owns (docs/adr-001-data-formats.md). */
static const tbl_def announce_tables[] = {
    { "ws_announcer_name_clip", "wrestler", 0xA1B6, 24 * 4, TK_U16, 2,
      "0xA130 announcer: {sound cmd (|0x3100), frames} per name id; rows 0..11 = wrestler ids, 12..23 further name ids ($1C15D2, 0x0F = none) — TODO EXACT who uses 12..23" },
    { "announce_phrase_ptrs",   "base/announce", 0xA216, 46 * 4, TK_U32, 1,
      "0xA160 announcer: long -> phrase record per phrase id 0..45 ($1C15D3)" },
    { "announce_phrase_records","base/announce", 0xA2CE, 0xA404 - 0xA2CE, TK_U16, 0,
      "0xA166 phrase records {count, (sound cmd, frames)*}; code 0xA404 follows" },
};
TBL_REGISTER(announce_tables)

void eng_announce(eng_state *st, unsigned name, unsigned phrase)
{
    if (name != 0x0Fu) {   /* clone slots announce as their BASE (rows 12..23
         * of the name-clip table are OTHER name ids, not clones); only a
         * REGISTERED clone is mapped so those rows stay reachable. 0x0F is
         * the "no name" sentinel ($1C15D2) — clone slot 15 keeps it and so
         * has no name call. */
        int b = eng_ws_clone_base((int)name);
        if (b >= 0 && !eng_ws_sound((int)name, ENG_SND_NAME_CALL, NULL, NULL))
            name = (unsigned)b;   /* a clone with his OWN name call keeps his id (ann_step resolves it) */
    }
    st->ann_req_name = (uint8_t)name;                  /* $1C15D2 */
    st->ann_req_phrase = (uint8_t)phrase;              /* $1C15D3 */
    if (getenv("WF_SNDLOG")) fprintf(stderr, "announce: request name %u phrase 0x%02X\n", name, phrase);
}

static void ann_step(eng_state *st)                    /* 0xA116 */
{
    if (st->ann_name != 0x0F) {                        /* say the name */
        unsigned cmd = 0; const char *wav = NULL;
        int k = eng_ws_sound((int)st->ann_name, ENG_SND_NAME_CALL, &cmd, &wav);   /* wrestler sound map */
        if (getenv("WF_SNDLOG")) fprintf(stderr, "announce: name %u -> map %d\n", st->ann_name, k);
        if (k == 2) {                                  /* library WAV: its own length */
            double secs = eng_sound_wav(wav);
            st->ann_t = (uint16_t)(secs > 0 ? secs * 57.4448 + 0.5 : 48);
        } else if (k == 1) {                           /* another stock clip: its row's frames if it has one */
            unsigned fr = 48;
            for (uint32_t r = 0; r < 24; r++)
                if ((tbl16(TBL(ws_announcer_name_clip), r * 4u) & 0xFFu) == (cmd & 0xFFu)) { fr = tbl16(TBL(ws_announcer_name_clip), r * 4u + 2); break; }
            eng_sound(cmd & 0xFFu);
            st->ann_t = (uint16_t)fr;
        } else {
            uint32_t e = (uint32_t)st->ann_name * 4u;
            eng_sound(tbl16(TBL(ws_announcer_name_clip), e) & 0xFFu);
            st->ann_t = (uint16_t)tbl16(TBL(ws_announcer_name_clip), e + 2);   /* $1C15D8 */
        }
        st->ann_name = 0x0F;
        return;
    }
    {
        uint32_t rec = tbl32(TBL(announce_phrase_ptrs), (uint32_t)st->ann_phrase * 4u);
        unsigned n = rom16c(rec);
        if (st->ann_step >= n) {                       /* 0xA19A: done */
            st->ann_active = 0; st->ann_step = 0; st->ann_t = 0;
            return;
        }
        eng_sound(rom16c(rec + 2 + st->ann_step * 4u) & 0xFFu);
        st->ann_t = (uint16_t)rom16c(rec + 4 + st->ann_step * 4u);
        st->ann_step++;
    }
}

void eng_announce_tick(eng_state *st)
{
    if (st->ann_req_phrase) {                          /* 0xA0E8: new request */
        st->ann_name = st->ann_req_name;
        st->ann_phrase = st->ann_req_phrase;
        st->ann_req_phrase = 0;
        st->ann_step = 0; st->ann_active = 1;
        ann_step(st);
        return;
    }
    if (!st->ann_active) return;                       /* 0xA0FE tick */
    if (st->ann_t && --st->ann_t) return;
    ann_step(st);
}
