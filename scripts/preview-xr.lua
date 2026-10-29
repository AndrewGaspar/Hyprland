-- HypXRland desktop preview config — used by scripts/preview-xr.sh
-- Runs a nested dev Hyprland with the XR extension enabled against a
-- windowed Monado (remote driver), so the XR space is visible on the desktop.

hl.monitor({ output = "", mode = "preferred", position = "auto", scale = "1" })

-- No machine-specific settings here. On dual-GPU boxes Monado picks one render node and
-- Hyprland must match it (openxr.gpu) or swapchain creation crashes inside Monado — put the
-- pin in hyprtester/xr-test-local.lua (gitignored, picked up automatically) or export
-- XR_GPU=/dev/dri/renderDXXX before running preview-xr.sh.
hl.config({
    openxr = {
        enabled = true,
    },
    misc = {
        disable_hyprland_logo = false,
    },
})

-- A world-locked monitor straight ahead at eye height...
hl.xr_monitor("XR-main, 1920x1080@60, anchor:local pos:0,1.5,-1.5 yaw:0, size:1.6")

-- ...and a head-leashed side panel that follows your view with a spring.
hl.xr_monitor("XR-side, 1280x720@60, anchor:head offset:0.9,0,-1.2, size:0.9")

-- Terminal on the world-locked XR monitor so there is something to look at.
hl.workspace_rule({ workspace = "1", monitor = "XR-main", default = true })
hl.on("hyprland.start", function()
    hl.exec_cmd("kitty || foot || alacritty")
end)

-- Handy binds inside the nested session (SUPER = the nested compositor's mod).
hl.bind("SUPER + N", hl.dsp.xrmonitor("create XR-extra 1280x720 anchor:body offset:0,-0.3,-1.0"))
hl.bind("SUPER + M", hl.dsp.xrmonitor("destroy XR-extra"))
hl.bind("SUPER + RIGHT", hl.dsp.xrmonitor("rotate 15"))
hl.bind("SUPER + LEFT", hl.dsp.xrmonitor("rotate -15"))
hl.bind("SUPER + UP", hl.dsp.xrmonitor("distance 0.25"))
hl.bind("SUPER + DOWN", hl.dsp.xrmonitor("distance -0.25"))
hl.bind("SUPER + C", hl.dsp.xrmonitor("center"))
hl.bind("SUPER + Q", hl.dsp.exit())
