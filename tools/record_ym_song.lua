-- Record one YM2151 song by posting a sound-latch command after boot.
-- (wfengine copy of wrestlefest-decomp/tools/record_ym_song.lua, extended
--  for long captures; MAME-side rendering scripts are the accepted
--  exception to the C-only tooling rule — the renderer is MAME's own
--  YM2151 + OKI cores, reached through the stock Z80 NMI path.)
--
--   WF_MUSIC_CMD=0x04 mame wwfwfest -rompath roms -video none -nothrottle \
--     -skip_gameinfo -autoboot_script tools/record_ym_song.lua \
--     -seconds_to_run 190 -wavwrite data/music/cmd_04.wav
--
-- Writes 0x00 (stop) then the requested command through the real 68000
-- latch at 0x14000C so the Z80 NMI path is the stock one.
--
-- WF_MUSIC_STOP_FRAME (default 90) / WF_MUSIC_PLAY_FRAME (default 100):
-- engine-side audio.c skips MUSIC_SKIP_FRAME=100 frames of preamble, so the
-- play frame must stay 100 for any wav that ships in data/music/.
--
-- Long captures: once our command is posted, the still-running attract mode
-- would eventually post its own sound commands (its demo match fires punches
-- and crowd about a minute in) and contaminate the render. A passive write
-- tap on 0x14000C rewrites every later 68000 post to 0x3190 — command 0x90
-- is above the Z80 consumer's 0x81 limit (sound ROM 0x007a) and is ignored,
-- so the song keeps playing untouched. Replacements are counted and printed
-- at exit so a contaminated-capture bug cannot pass silently.

local machine = manager.machine
local program = machine.devices[":maincpu"].spaces["program"]
local screen = machine.screens[":screen"]

local cmd = tonumber(os.getenv("WF_MUSIC_CMD") or "4")
local stop_at = tonumber(os.getenv("WF_MUSIC_STOP_FRAME") or "90")
local play_at = tonumber(os.getenv("WF_MUSIC_PLAY_FRAME") or "100")
local posted = false
local stopped = false
local muted = 0

-- Anchor the tap globally: MAME collects a tap whose handler becomes
-- unreachable (measured in tools/oracle.lua in the decomp repo).
MUTE_TAP = MUTE_TAP or nil

emu.register_frame_done(function()
    local f = screen:frame_number()
    if (not stopped) and f >= stop_at then
        program:write_u16(0x14000C, 0x3100)
        stopped = true
        print(string.format("record_ym_song: frame %d stop 0x00", f))
    end
    if (not posted) and f >= play_at then
        program:write_u16(0x14000C, 0x3100 + (cmd & 0xFF))
        posted = true
        print(string.format("record_ym_song: frame %d play 0x%02X", f, cmd))
        MUTE_TAP = program:install_write_tap(0x14000C, 0x14000D, "mute_late",
            function(offset, data, mask)
                muted = muted + 1
                return 0x3190
            end)
    end
end)

if emu.add_machine_stop_notifier then emu.add_machine_stop_notifier(function()
    print(string.format("record_ym_song: muted %d late sound posts", muted))
end) end
