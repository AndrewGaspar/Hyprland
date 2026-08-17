-- mpv-hypxr-stereo.lua — tell Hyprland what the file it is playing actually is.
--
-- mpv is the one player on Linux that exposes container stereo metadata as a
-- property (`video-params/stereo-in`, from Matroska's StereoMode / ISOBMFF st3d
-- via FFmpeg's AVStereo3D). This script observes it and tags mpv's own window,
-- so a *static* rule in your config can turn that into a stereo layout:
--
--     windowrule = stereo sbs  always, match:tag stereo-sbs
--     windowrule = stereo hsbs always, match:tag stereo-hsbs
--     windowrule = stereo tab  always, match:tag stereo-tab
--     windowrule = stereo htab always, match:tag stereo-htab
--
-- The route is deliberately generic Hyprland — a window tag and a windowrule,
-- no OpenXR verb, no `hyprctl openxr` — so it works on any `stereo:` output
-- with no headset and no runtime. See docs/openxr/05-configuration.md §8.6.
--
-- It is ADVISORY. Container flags are wrong often enough that the VR-video
-- community strips them when muxing, so a rule you wrote by hand on class or
-- title keeps winning: this script only ever adds a tag, and `stereo off`
-- remains your override. Drop it in ~/.config/mpv/scripts/.
--
-- IT ALSO HOLDS UP MPV'S END OF THE CONTRACT while a layout is tagged, which is
-- the half that is invisible until you are wearing the headset (§8.6, and the
-- 2026-08-17 live session that found it): the compositor halves mpv's SURFACE,
-- not the picture inside it. With `keepaspect` on, mpv fits the packed frame
-- into the window and centres it — and if the bars land left and right, the
-- crop takes half a bar plus the wrong part of the frame, the two eyes slide
-- apart sideways, and nothing fuses. So:
--
--   * keepaspect goes OFF, so the packed frame fills the surface. On a window
--     already at the file's aspect this changes nothing; on any other window it
--     is the difference between a distorted-but-fusable picture and no picture.
--   * the OSC is hidden, because it is drawn INTO the same surface and is
--     therefore pane-split too — half a seek bar per eye.
--
-- Both are restored the moment the layout goes away (a mono file, a shutdown).
-- To keep your own settings, put `fill=no` / `hide_osc=no` in
-- ~/.config/mpv/script-opts/hypxr-stereo.conf (or pass
-- --script-opts=hypxr-stereo-fill=no on the command line).

local utils   = require 'mp.utils'
local options = require 'mp.options'

local opts    = { fill = true, hide_osc = true }
options.read_options(opts, 'hypxr-stereo')

local TAGS = { 'stereo-sbs', 'stereo-hsbs', 'stereo-tab', 'stereo-htab' }
local SELF = 'pid:' .. tostring(utils.getpid())
local last = nil

-- what mpv had before we touched it, so "off" means "back to yours", not "off"
local savedKeepaspect = nil

-- mpv names the packing but never the half-vs-full bit — Matroska and st3d do
-- not carry it either. It is inferred from the display aspect, exactly as every
-- shipping VR-video player does: a full frame is 2x wide (or 2x tall).
local function layoutFor(stereo, aspect)
    if stereo == 'sbs2l' or stereo == 'sbs2r' then
        return (aspect and aspect >= 2.6) and 'stereo-sbs' or 'stereo-hsbs'
    elseif stereo == 'ab2l' or stereo == 'ab2r' then
        return (aspect and aspect <= 1.2) and 'stereo-tab' or 'stereo-htab'
    end
    return nil -- mono, unknown, interleaved/anaglyph forms we cannot present
end

local function tagwindow(arg)
    mp.command_native({ name = 'subprocess', playback_only = false, capture_stdout = true,
                        args = { 'hyprctl', 'dispatch', 'tagwindow', arg .. ' ' .. SELF } })
end

-- mpv's end of §8.6's contract: while a layout is tagged the packed frame must
-- FILL the surface, and nothing the player draws into that surface may matter.
local function presentation(on)
    if opts.fill then
        if on then
            if savedKeepaspect == nil then savedKeepaspect = mp.get_property_native('keepaspect') end
            mp.set_property_native('keepaspect', false)
        elseif savedKeepaspect ~= nil then
            mp.set_property_native('keepaspect', savedKeepaspect)
            savedKeepaspect = nil
        end
    end

    -- a no-op on a build with no OSC, and on uosc and friends, which is fine:
    -- this is a courtesy, not a guarantee.
    if opts.hide_osc then
        mp.commandv('script-message', 'osc-visibility', on and 'never' or 'auto', 'no-osd')
    end
end

local function apply(_, _)
    local want = layoutFor(mp.get_property('video-params/stereo-in'),
                           mp.get_property_number('video-params/aspect'))
    if want == last then return end
    for _, t in ipairs(TAGS) do
        if t ~= want then tagwindow('-' .. t) end
    end
    if want then tagwindow('+' .. want) end
    presentation(want ~= nil)
    last = want
end

mp.observe_property('video-params/stereo-in', 'string', apply)
mp.observe_property('video-params/aspect', 'number', apply)
mp.register_event('shutdown', function()
    for _, t in ipairs(TAGS) do tagwindow('-' .. t) end
    presentation(false)
end)
