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

local utils = require 'mp.utils'

local TAGS = { 'stereo-sbs', 'stereo-hsbs', 'stereo-tab', 'stereo-htab' }
local SELF = 'pid:' .. tostring(utils.getpid())
local last = nil

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

local function apply(_, _)
    local want = layoutFor(mp.get_property('video-params/stereo-in'),
                           mp.get_property_number('video-params/aspect'))
    if want == last then return end
    for _, t in ipairs(TAGS) do
        if t ~= want then tagwindow('-' .. t) end
    end
    if want then tagwindow('+' .. want) end
    last = want
end

mp.observe_property('video-params/stereo-in', 'string', apply)
mp.observe_property('video-params/aspect', 'number', apply)
mp.register_event('shutdown', function() for _, t in ipairs(TAGS) do tagwindow('-' .. t) end end)
