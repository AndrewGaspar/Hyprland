-- Hyprland config for the OpenXR integration test suite (Lua; Hyprland 0.57+ has no classic
-- hyprlang front end). Used by `hyprtester --xr`, which launches through a generated
-- <run-dir>/xr-wrapper.lua that require()s this file first (hyprtester/src/main.cpp runXrSuite).

hl.monitor({ output = "HEADLESS-1", mode = "1920x1080@60", position = "0x0", scale = "1" })

hl.config({
    openxr = {
        enabled = true,
        -- The kernel-taint tripwire (doc 01) refuses bring-up when the kernel has oopsed this boot.
        -- A container shares the HOST kernel, so /proc/sys/kernel/tainted here is the host's state and
        -- the suite's result would otherwise depend on whether the developer's machine has oopsed since
        -- its last reboot. Pin the escape hatch so these tests measure the compositor, not the host —
        -- the tripwire's own behavior is covered by tests/xr/kernel_taint.cpp, which injects the value.
        ignore_kernel_taint = true,
        -- NOTE: no machine-specific settings in this file. On dual-GPU boxes (where Monado picks
        -- a different render node than Hyprland's default, causing a cross-GPU EGL crash inside
        -- Monado at xrCreateSwapchain) pin openxr.gpu via either:
        --   - an untracked hyprtester/xr-test-local.lua (gitignored, required automatically), or
        --   - HYPRTESTER_XR_GPU=/dev/dri/renderDXXX in the environment.
        -- The harness merges both through the generated wrapper config (main.cpp runXrSuite).
    },
    -- The compositor log in the preserved run dir is the only account of a failed (or crashed)
    -- case; log to it, like test.lua does.
    debug = {
        disable_logs = false,
    },
})

-- Declared-set fixtures for xr_config_declared (WP11, docs/openxr/06-testing.md §2.1). Unique
-- names come from runtime creation; these two are static because the config file is static.
-- xr_config_declared temporarily rewrites this file (and restores it) to exercise reload
-- reconciliation of a changed/removed declaration: it drops every line that starts with
-- `hl.xr_monitor(` — keep each declaration on ONE line, and keep these two exactly as-is so its
-- "original content" restore round-trips byte-for-byte.
hl.xr_monitor("XR-conf-a, 1280x720@60, anchor:local pos:0,1.4,-1.5 yaw:0, size:1.0")
hl.xr_monitor("XR-conf-b, 1024x768,    anchor:head offset:0.3,-0.1,-1.0, size:0.5")
