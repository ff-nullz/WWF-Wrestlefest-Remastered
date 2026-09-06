#ifndef WF_STREAM_DECODE_H
#define WF_STREAM_DECODE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WF_THINKER_MAX_SPR 160   /* the LOD taunt panels' base pose (row 0x2C
                                    pose 0, D802 count 0x55) compiles 87 hw
                                    records + overlays; 64 truncated it */

typedef struct {
    int16_t x;
    int16_t y;
    uint16_t tile;
    uint8_t flipx;
    uint8_t flipy;
    uint8_t chain;
    uint8_t pal; /* 0x1C1610 index; with identity table this is the bank */
    /* What the ROM actually stores in spriteram, which is not always what
     * the fields above hold: 0xD374/0xD376 write the tile bytes unmasked
     * while `tile` has the chain mask applied, and 0xD36A writes the attr
     * byte after its or/eor. A byte-exact spriteram write needs the raw
     * values, not the interpreted ones. */
    uint8_t raw_tile_lo;
    uint8_t raw_tile_hi;
    uint8_t attr;
    /* High byte of the three words at +2, +4 and +6. The ROM stores whole
     * words but only ever computes their low byte, so the high byte is
     * whatever the register happened to hold: y (or 0xFF-y) in the
     * byte-stream loops, and the stream pointer itself in the two pointer
     * loops, where a literal leaks its own address into spriteram.
     * draw_sprites() never reads these, but a byte-exact write must. */
    uint8_t w2_hi;
    uint8_t w4_hi;
    uint8_t w6_hi;
} WfThinkerSpr;

const char *wf_thinker_error(void);

/* ---- live-machine integration -------------------------------------------
 * The port compiles this file directly and drives it from the running
 * machine. Everything below is additive: the offline entry points above keep
 * their exact previous behaviour so tools/thinker is unaffected. */

/* Fill the decoder's private memory image from an arbitrary reader instead of
 * ROM chip files. The port passes m68k_read_memory_8, so any stream overlay
 * the game has already applied is picked up for free. */
int wf_thinker_init_live(void);   /* engine: ROM reads via the data layer (tbl.h) */

/* Decode against a caller-supplied object origin and animation words rather
 * than the offline dummy object. ox/oy are the object's +0x14/+0x16 screen
 * words, so emitted x/y land in live spriteram coordinates. */
/* The wrestler the owner is linked to (its object +0x02), for 0xDA12's
 * partner slice. Pass -1 when there is no live link; the decoder then
 * compiles only the owner's body. Sticky until changed. */
void wf_thinker_set_partner_row(int row);

/* Telemetry for the last decode's 0xDA12 interaction, for attributing a
 * misplaced carried body. multi is bit0 = owner track had a slice count,
 * bit1 = partner track did. */
void wf_thinker_last_da12(int *seen, int *row, int *ox, int *oy, int *multi);

int wf_thinker_decode_obj(uint32_t stream_base, uint32_t stream_off, int flip,
                          int16_t ox, int16_t oy, uint16_t row, uint16_t pose,
                          WfThinkerSpr *out, int cap);

/* Nonzero when the last decode hit a work/emit ceiling. A truncated decode is
 * an incomplete sprite list and must never be compared or drawn as if whole. */
int wf_thinker_last_truncated(void);

#define WF_THINKER_EV_N 19
#define WF_THINKER_TAG_D2AE    0u
#define WF_THINKER_TAG_D802    1u
#define WF_THINKER_TAG_D71A    2u
#define WF_THINKER_TAG_D43A    3u
#define WF_THINKER_TAG_OVERLAY 0x80u
/* Bit 6 distinguishes the two overlay call sites, which are NOT the same ROM
 * routine: header bit 9 reaches 0xD43A (00D2FA / 00D852) while the header
 * bit 8 trailer reaches 0xD540 (00D3DC / 00D9D6). */
#define WF_THINKER_TAG_TRAILER 0x40u

#ifdef __cplusplus
}
#endif

#endif
