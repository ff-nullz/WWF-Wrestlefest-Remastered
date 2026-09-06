/* Camera — transcription of ROM 0x26936 (centroid-error follow servo)
 * and 0x2983C (scene limits clamp, table 0x298B4).
 * Spec: scratchpad mv/world-screen.md §3 (2026-08-22).
 *
 * Group mode: error = average over camera-relevant objects of
 * (x - camX - 0xA0, y - camY + 0xF0), slew-clamped to +/-4 px/frame,
 * then unsigned-clamped to the per-scene limits. Solo/scripted modes
 * arrive with the modes that use them.
 */
#include "engine.h"
#include "tbl.h"

static const tbl_def camera_tables[] = {
    { "camera_scene_limits", "base/scene", 0x298B4, 7 * 8, TK_U16, 4,
      "0x2983C camera clamp: per scene ($1C007E*8) {xmin, ymin, xmax, ymax} -> $1C17E6/$1C17EE; 7 rows — scene 6 (arena-B ringside) = {140,200,3C0,200}, y LOCKED like scene 2 (row 7 is code)" },
};
TBL_REGISTER(camera_tables)

static int16_t clamp4(int32_t v)
{
    return (int16_t)(v > 4 ? 4 : (v < -4 ? -4 : v));
}

void eng_camera_update(eng_state *st)
{
    int32_t ex = 0, ey = 0;
    int cnt = 0;

    for (int i = 0; i < ENG_MAX_OBJS; i++) {           /* iterator 0x250E */
        const eng_obj *o = &st->obj[i];
        if (!o->active || (o->st_flags & SF_QUEUED)) continue;   /* queued rumble entrant: not in the arena */
        if (o->cam_mode == 2) return;  /* +0x4B = 2: camera frozen (the
                                          pickup 0x13180 sets it; 0x26936) */
        if ((st->g161 & 2u) && (o->role & RF_OUTSIDE) && (o->role & RF_LEGAL)) {
            /* 0x26936 SOLO (D7 = 0x100): in the ringside scene the
             * outside legal man alone steers the view — err_x = x - camX
             * - 0xA0, err_y = y + z - 0x100 - camY + 0xF0, +/-4 px/frame
             * (ringout-scene.md §1d; oracle fuzz2 f2529-2544: cam x
             * 0x190 -> 0x150 = faller 0x1F0 - 0xA0). */
            ex = (o->x >> 16) - st->cam_x - 0xA0 - (wf_view_w() - 320) / 2;
            ey = (o->y >> 16) + (o->z >> 16) - 0x100 - st->cam_y + 0xF0 - (wf_view_h() - 240) / 2;
            st->cam_x += clamp4(ex);
            st->cam_y += clamp4(ey);
            cnt = 0;
            goto clamp;
        }
        if (o->cam_mode == 3 || !(o->role & RF_LEGAL))   /* legal men only (+0x33
                                                  b0): oracle tag-left.wfo fit */
            continue;
        cnt++;
        ex += (int16_t)((o->x >> 16) - st->cam_x - 0xA0 - (wf_view_w() - 320) / 2);
        ey += (int16_t)((o->y >> 16) - st->cam_y + 0xF0 - (wf_view_h() - 240) / 2);  /* no z in group */
    }
    if (cnt) {
        /* Stock: step = clamp4(error / cnt) (0x269FC). Playtest smoothing
         * (temporary, not ROM): short corrections move 1 px/frame so the
         * small centroid snaps of holds/pickups don't jerk the view. */
        int32_t sx = clamp4(ex / cnt), sy = clamp4(ey / cnt);
        if (ex > -0x20 && ex < 0x20 && sx) sx = sx > 0 ? 1 : -1;
        if (ey > -0x20 && ey < 0x20 && sy) sy = sy > 0 ? 1 : -1;
        st->cam_x += sx;
        st->cam_y += sy;
    }

clamp:
    if (eng_mod_rule(MODR_CAM_FIXED)
        && (st->scene == 0 || st->scene == 1 || st->scene == 5 || st->scene == 7)) {   /* mod: LOCKED view — the
                                          camera pins to the arena centre and
                                          never moves ("fixed view... no panning
                                          or zooming", user 2026-08-24) */
        uint32_t len2 = 0;
        if (tbl_bytes(TBL(camera_scene_limits), &len2) && len2 >= 8) {
            unsigned row = (st->scene >= 0 && (uint32_t)st->scene < len2 / 8u) ? (unsigned)st->scene : 0u;
            int e0 = (int)tbl16(TBL(camera_scene_limits), row * 8u),
                e1 = (int)tbl16(TBL(camera_scene_limits), row * 8u + 2u),
                e2 = (int)tbl16(TBL(camera_scene_limits), row * 8u + 4u),
                e3 = (int)tbl16(TBL(camera_scene_limits), row * 8u + 6u);
            st->cam_x = (e0 + e2 + 320 - wf_view_w()) / 2;
            st->cam_y = (e1 + e3 + 240 - wf_view_h()) / 2;
        }
        return;
    }
    {                                                  /* 0x2983C clamp */
        uint32_t len = 0;
        if (tbl_bytes(TBL(camera_scene_limits), &len) && len >= 8) {   /* 0x298B4 */
            unsigned row = (st->scene >= 0 && (uint32_t)st->scene < len / 8u) ? (unsigned)st->scene : 0u;
            int e0 = (int)tbl16(TBL(camera_scene_limits), row * 8u),
                e1 = (int)tbl16(TBL(camera_scene_limits), row * 8u + 2u),
                e2 = (int)tbl16(TBL(camera_scene_limits), row * 8u + 4u),
                e3 = (int)tbl16(TBL(camera_scene_limits), row * 8u + 6u);
            {   /* MOD zoom: a wider window eats clamp range on the far
                 * side; a LOCKED axis (min == max) recentres instead. */
                int dx = wf_view_w() - 320, dy = wf_view_h() - 240;
                if (dx) { if (e2 - dx > e0) e2 -= dx; else { e0 -= dx / 2; e2 = e0; } }
                if (dy) { if (e3 - dy > e1) e3 -= dy; else { e1 -= dy / 2; e3 = e1; } }
            }
            if (st->cam_x < e0) st->cam_x = e0;          /* xmin */
            else if (st->cam_x >= e2) st->cam_x = e2;    /* xmax */
            if (st->cam_y < e1) st->cam_y = e1;          /* ymin */
            else if (st->cam_y >= e3) st->cam_y = e3;    /* ymax */
        }
    }
}
