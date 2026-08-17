#pragma once

// Deliberately compiles unconditionally (no OpenXR headers, no HAVE_OPENXR guard) so the
// config/parser layer can live outside the gate and hyprland_gtests can always exercise it
// (docs/openxr/07-roadmap.md conventions; parser tests in tests/xr/parser.cpp).

#include <cstdint>
#include <string>
#include <string_view>
#include <optional>
#include <expected>
#include <vector>

#include "../helpers/math/Math.hpp"
#include "XRAnchor.hpp" // OpenXR::SXRAnchorState + eXRAnchorMode/eXRHand (unconditional pure math)

namespace OpenXR {
    // Environment blend mode (doc 01). A HAVE_OPENXR-free mirror of XrEnvironmentBlendMode so the
    // selection logic (pickBlendMode) is a pure, unconditionally-compiled function that
    // hyprland_gtests can exercise without a runtime. The guarded session code converts to/from
    // the real XrEnvironmentBlendMode enum (xrBlendModeToXr / xrBlendModeFromXr in XRSession.hpp).
    enum eXRBlendMode : uint8_t {
        XR_BLEND_OPAQUE = 0, // composite over black — the classic VR "floating in a void" look
        XR_BLEND_ALPHA,      // composite over the runtime's passthrough underlay via layer alpha
        XR_BLEND_ADDITIVE,   // additive (optical see-through / additive displays)
    };

    // "opaque" | "alpha" | "additive" — the config/IPC string form (doc 05).
    std::string blendModeToString(eXRBlendMode mode);

    // Does this blend mode composite our layers over anything but a black void? Luma-keyed
    // transparency (openxr:black_alpha) only REVEALS something under alpha (passthrough) or additive;
    // under OPAQUE the runtime paints black behind us, so keying would just make monitors look dim and
    // dirty. The manager gates the feature on this and warns once (report 09 §3.1).
    inline bool blendModeShowsThrough(eXRBlendMode mode) {
        return mode == XR_BLEND_ALPHA || mode == XR_BLEND_ADDITIVE;
    }

    // Result of pickBlendMode: the chosen mode plus whether the user's explicit request could not
    // be honored (so the caller can emit the unsupported->fallback WARN — doc 01).
    struct SXRBlendModePick {
        eXRBlendMode mode                 = XR_BLEND_OPAQUE;
        bool         requestedUnsupported = false;
    };

    // Pure blend-mode selection (doc 01). `supported` is the runtime's advertised list in
    // preference order (xrEnumerateEnvironmentBlendModes returns preferred-first). `config` is the
    // openxr:blend_mode value: "auto" (or anything unrecognized) => the runtime's first-listed
    // (preferred) mode; an explicit "opaque"/"alpha"/"additive" is honored iff supported, else it
    // falls back to the preferred mode with requestedUnsupported=true. An empty supported list
    // (spec-illegal, but defended) yields XR_BLEND_OPAQUE.
    SXRBlendModePick pickBlendMode(const std::vector<eXRBlendMode>& supported, const std::string& config);

    // openxr:monitors_follow_session mode (research/18 + report-18 addendum). Governs WHEN
    // XR-created monitors behave like a plugged external display.
    enum eXRMonitorFollowMode : uint8_t {
        XR_FOLLOW_OFF = 0,  // never unplug — always-present (the pre-feature behavior). Legacy 0/false/no.
        XR_FOLLOW_SESSION,  // plug while an OpenXR session EXISTS (start()..stop()). Legacy 1/true/yes.
        XR_FOLLOW_VISIBLE,  // plug only while the session is VISIBLE/FOCUSED — a doffed/standby headset
                            // (WiVRn keeps a session alive on the shelf) reads as unplugged. Default.
    };

    // Parse the openxr:monitors_follow_session config string to the mode. Accepts the new
    // "off"|"session"|"visible" spellings AND the legacy boolean spellings for config compat:
    // 0/false/no/off -> OFF, 1/true/yes/session -> SESSION, visible/focused -> VISIBLE. Anything
    // unrecognized (including empty) -> VISIBLE (the default). Case/whitespace-insensitive. Pure.
    eXRMonitorFollowMode parseMonitorFollowMode(const std::string& v);

    // openxr:recenter — what the HEADSET's own recenter button means for the monitor group
    // (doc 03 §8.4). The ladder in §8.1 exists to hold monitors still in the room across a
    // reference-space change, which is right for an involuntary one (a re-don, a guardian
    // re-derive) and wrong for the deliberate press of a user who wants the room brought to them.
    // This is that choice, and it is a choice: `follow` cannot distinguish a deliberate recenter
    // from an involuntary one, because the runtime does not tell us which it was.
    enum eXRRecenterPolicy : uint8_t {
        XR_RECENTER_HOLD = 0, // the shipped behavior: re-express anchors, monitors stay in the room
        XR_RECENTER_FOLLOW,   // a reference-space change ALSO re-seats the group to the current head
    };

    // "hold" (or anything unrecognized, including empty) -> HOLD; "follow"/"reseat"/"me" -> FOLLOW.
    // Case/whitespace-insensitive. Pure.
    eXRRecenterPolicy parseRecenterPolicy(const std::string& v);
    // The canonical spelling, for `hyprctl openxr status`.
    const char*       recenterPolicyName(eXRRecenterPolicy p);

    // Cross-GPU linear-buffer policy (research/17 addendum). When the XR EGL context lives on a
    // DIFFERENT physical GPU than the one the compositor allocates the headless output's buffers on
    // (hybrid: desktop on the AMD iGPU, WiVRn/runtime on the NVIDIA dGPU via openxr:gpu), the XR
    // context cannot import the AMD-vendor-tiled buffers — NVIDIA's EGL rejects the foreign tiling
    // modifier with EGL_BAD_ATTRIBUTE and the quad goes black. The multi-GPU-standard fix is to make
    // that output's swapchain allocate DRM_FORMAT_MOD_LINEAR buffers (importable by any GPU). This
    // mirrors openxr:force_linear = auto | on | off.
    enum eForceLinearMode : uint8_t {
        XR_LINEAR_AUTO = 0, // force linear iff a cross-GPU split is positively detected (default)
        XR_LINEAR_ON,       // always force linear on XR-bound outputs
        XR_LINEAR_OFF,      // never force (leave native tiling; for debugging / same-GPU-only setups)
    };
    // "auto" (or anything unrecognized) => XR_LINEAR_AUTO; "on"/"true"/"1" => ON; "off"/"false"/"0" => OFF.
    eForceLinearMode parseForceLinearMode(const std::string& s);

    // Pure decision (tests/xr/force_linear.cpp): should the XR-bound headless output allocate LINEAR
    // buffers? `gpusKnown` says whether the caller could positively resolve BOTH the XR EGL device and
    // the buffer allocator's device; `sameGpu` is their physical-device equality (the caller computes
    // it with DRM::sameGpu → drmDevicesEqual, which is node-type agnostic — a render node and a card
    // node of ONE GPU compare equal). AUTO forces linear only when both devices are known AND they are
    // genuinely different GPUs — an unknown pair stays native (same-GPU is the common case and linear
    // costs compositing throughput). ON/OFF ignore the devices. Taking a resolved `sameGpu` boolean
    // (rather than raw major/minor) is deliberate: comparing an XR *render* node against an allocator
    // *card* node by their device numbers always mis-reports "different GPU" on a single-GPU box and
    // was the NVIDIA all-black root cause. No OpenXR/aquamarine types so hyprland_gtests can exercise
    // it without a runtime.
    bool shouldForceLinear(eForceLinearMode mode, bool gpusKnown, bool sameGpu);

    // ---- Kernel-taint tripwire (doc 01, "Sick-driver refusal") -------------------------------
    // The forensic case (hard reboot #6): an NVIDIA driver use-after-free cascaded through the
    // kernel, which printed "Fixing recursive fault but reboot is needed!" 29 minutes before the
    // machine died. The compositor was a bystander — but XR bring-up would have walked straight
    // into that already-corrupt GPU driver seconds later, because it CANNOT avoid it: libglvnd's
    // very first eglGetProcAddress loads every installed vendor library, and the count-only
    // eglQueryDevicesEXT then opens /dev/nvidiactl + /dev/nvidia0 before a single device handle
    // exists to filter on (measured; see doc 01). No pin, no ordering and no lazy query can dodge
    // a wedged vendor driver once we start enumerating. The only move that actually protects the
    // user is to not start at all — which is what this tripwire does.
    //
    // The signal is /proc/sys/kernel/tainted bit 7, TAINT_DIE ('D'): "kernel has oopsed before".
    // It is set for the whole boot and is never cleared, which is exactly the semantics wanted —
    // once any kernel oops has happened, no amount of waiting makes the driver trustworthy again.
    // Deliberately NARROW: the everyday taint bits (proprietary/out-of-tree/unsigned modules —
    // 12288 on a stock NVIDIA box) say nothing about driver health and must never block XR.
    inline constexpr uint32_t XR_TAINT_DIE_BIT  = 7;
    inline constexpr uint64_t XR_TAINT_DIE_MASK = 1ull << XR_TAINT_DIE_BIT; // 128

    // Where the kernel publishes it. A constant so tests can name it without hardcoding a path.
    inline constexpr const char* XR_TAINT_PROC_PATH = "/proc/sys/kernel/tainted";

    struct SKernelTaintVerdict {
        bool        oopsed  = false; // TAINT_DIE is set — the kernel has taken an oops this boot
        bool        blocked = false; // refuse bring-up (oopsed AND the escape hatch is off)
        std::string reason;          // the message for the log AND the `blocked:` status line
        bool        operator==(const SKernelTaintVerdict&) const = default;
    };

    // Parse the contents of /proc/sys/kernel/tainted: a single unsigned decimal, usually with a
    // trailing newline. std::nullopt when it does not parse (or the file was unreadable, which the
    // caller signals by passing an empty string). Pure, so the truth table is unit-testable
    // (tests/xr/kernel_taint.cpp) without a tainted kernel, matching resolveRuntimeJsonEnv's split.
    std::optional<uint64_t> parseKernelTaint(std::string_view contents);

    // Read + parse in one go. Lives here rather than in the (HAVE_OPENXR-guarded) manager purely so
    // the tests can exercise the SAME read the manager performs — against the real /proc file and
    // against temp files — instead of a copy of it that is free to drift. A read that silently
    // returned nullopt would disable the tripwire forever without a single symptom, which is
    // exactly the kind of bug that has to be covered rather than eyeballed. Never throws; any I/O
    // failure is nullopt (fail open, see evaluateKernelTaint).
    std::optional<uint64_t> readKernelTaint(const std::string& path = XR_TAINT_PROC_PATH);

    // Decide whether XR bring-up may proceed. `taint` is parseKernelTaint's result; `ignore` is
    // openxr:ignore_kernel_taint.
    //   nullopt (unreadable/unparsable) => FAIL OPEN. A missing /proc entry (container, exotic
    //     kernel) must never cost the user their XR session — the tripwire is a safety net over a
    //     rare event, not a precondition we can prove.
    //   TAINT_DIE clear                 => proceed silently.
    //   TAINT_DIE set, ignore=false     => BLOCKED, with the reason string below.
    //   TAINT_DIE set, ignore=true      => proceed, oopsed=true and a reason saying it was
    //     overridden, so the caller can still WARN rather than going quiet.
    SKernelTaintVerdict evaluateKernelTaint(std::optional<uint64_t> taint, bool ignore);

    // openxr:runtime_json plumbing (WP-XR1). The compositor may override which OpenXR runtime the
    // session handshakes against by pointing the loader at a specific manifest — the loader ONLY reads
    // the XR_RUNTIME_JSON environment variable for this (there is no programmatic override in the
    // loader API), so we must setenv/unsetenv it on the main thread before the handshake helper spawns.
    // setenv in a threaded process is hazardous (glibc can realloc `environ`, use-after-free vs a
    // concurrent getenv), so we mutate it as rarely as possible: this pure function decides the minimal
    // action given the process's ORIGINAL XR_RUNTIME_JSON (captured once at first start), the value
    // currently in the environment, and the requested config value. Toggling the config back to empty
    // restores the original login environment, so the flat<->XR toggle is fully reversible via config.
    // No env access here — the caller reads getenv and applies the action — so it is unit-testable
    // (tests/xr/runtime_json.cpp) without touching the real process environment.
    enum eRuntimeJsonAction : uint8_t {
        XR_RTJSON_NOOP = 0, // environment already matches the desired value — do not touch environ
        XR_RTJSON_SET,      // setenv("XR_RUNTIME_JSON", action.value)
        XR_RTJSON_UNSET,    // unsetenv("XR_RUNTIME_JSON") — restore "no override" (original had none)
    };
    struct SRuntimeJsonEnvAction {
        eRuntimeJsonAction kind = XR_RTJSON_NOOP;
        std::string        value;           // meaningful only for XR_RTJSON_SET
        bool               operator==(const SRuntimeJsonEnvAction&) const = default;
    };
    // configValue = openxr:runtime_json (empty => "no override, use whatever the process launched
    // with"). originalPresent/originalValue = the XR_RUNTIME_JSON the process had at first start.
    // currentPresent/currentValue = what is in the environment right now. Returns the minimal action.
    SRuntimeJsonEnvAction resolveRuntimeJsonEnv(const std::string& configValue, bool originalPresent, const std::string& originalValue, bool currentPresent,
                                                const std::string& currentValue);

    // Plugged-state policy (research/18 + report-18/19 addenda — XR monitors behave like unplugged
    // external monitors while the headset is not being worn). Pure and unconditional so
    // hyprland_gtests can exercise it (tests/xr/plugged.cpp). Instantaneous predicate — the anti-flap
    // grace and the first-plug blip guard are the caller's concern (see xrDeferFirstPlug).
    //   OFF     => always plugged.
    //   SESSION => sessionUp (a session exists; WiVRn-on-the-shelf counts).
    //   VISIBLE => gate on the CONJUNCTION of BOTH available real signals (report-20 issue D):
    //     * sessionVisible must be true. WiVRn drops VISIBLE->SYNCHRONIZED reliably on doff, so
    //       visibility is the reliable doff signal even when presence sticks 'present' in standby.
    //     * AND, when presenceSupported (XR_EXT_user_presence advertised AND supported): userPresent,
    //       but only once presenceKnown (>=1 presence event seen) — before the first event we read as
    //       ABSENT so the session-create sprint never plugs.
    //     * otherwise (no presence ext): visibility alone is the signal.
    //   Requiring BOTH means a doff (visibility drop) unplugs even if presence is stuck, and a
    //   presence-absent unplugs even if a stale VISIBLE bit lingers — the fix for the never-unplugs
    //   standby bug. The first-plug settle (xrDeferFirstPlug) still guards the create-time blip.
    bool wantXRMonitorsPlugged(eXRMonitorFollowMode mode, bool sessionUp, bool sessionVisible, bool presenceSupported, bool presenceKnown, bool userPresent);

    // ---- default scale for XR-created monitors (task #129) ----

    // Config::CMonitorRule::m_scale is a REQUEST, not a value: anything <= 0.1 means "auto" and sends
    // CMonitor::applyMonitorRule to getDefaultScale(). Anything above it is a scale the user actually
    // asked for. Same threshold as Monitor.cpp's `autoScale`, kept here so the policy below and the
    // tests agree with the consumer.
    constexpr float XR_RULE_SCALE_AUTO_MAX = 0.1F;
    inline bool     xrRuleScaleIsExplicit(float ruleScale) {
        return ruleScale > XR_RULE_SCALE_AUTO_MAX;
    }

    // The scale the user's config PINNED on one output: the newest monitor rule that names it, or the
    // auto sentinel if none does. `rules` is the rule manager's list in declaration order (anything
    // with .m_name / .m_scale); `matches` is the output's own selector match.
    //
    // Why not just read CMonitorRuleManager::get()'s merged scale: get() falls back to the first
    // NAMELESS rule when nothing matches by selector — the `monitor = , preferred, auto, 1` catch-all
    // almost every config carries — and that fallback is not an opinion about XR-2, it is exactly what
    // "no rule for XR-2" looks like. Taking its scale would let one catch-all silently disable the XR
    // default for everyone who has one. So the nameless rule is skipped here explicitly, rather than
    // left to the selector matcher's incidental behavior.
    template <typename Rules, typename Matcher>
    float xrPinnedRuleScale(const Rules& rules, Matcher&& matches) {
        float pinned = -1.F;
        for (const auto& r : rules) {
            if (r.m_name.empty())
                continue;
            if (r.m_scaleOwnedByXR)
                continue;
            if (matches(r.m_name))
                pinned = r.m_scale; // keep walking: later rules outrank earlier ones, as in get()
        }
        return pinned;
    }

    // Which scale the compositor should own in an XR monitor's persistent rule, or nullopt to
    // decline ownership. A caller that previously stored an XR-owned scale must then return that
    // field to auto; a user-owned value remains untouched.
    //
    // Why this exists: a headless output has no EDID, so getDefaultScale()'s PPI heuristic reads a
    // 1920x1080 quad as a tiny high-density panel and picks 2.0 — legible on a desk, unusably cramped
    // through a headset. Voice/keybind-created monitors (`hyprctl openxr create` -> XR-2, XR-3, …)
    // have no `monitor =` line to correct it, which is why configs grew XR-2..XR-8 workaround blocks
    // with a cliff at XR-9. The compositor owns the default instead.
    //
    // Precedence, highest first — the same shape as the pixel-mode ladder (doc 05 §3.1):
    //   1. an explicit scale in a matching `monitor =` rule (ruleScaleExplicit) — untouched;
    //   2. this default, for XR-CREATED outputs only;
    //   3. Hyprland's PPI guess, when the default is opted out of (configuredDefault <= 0).
    // Never applied to a pre-existing output an `xrmonitor` line merely ADOPTED (createdByXR ==
    // false): that is one of the user's real monitors with a real EDID, and its scale is not ours.
    // Never applied to a stereo output either: the pack needs one buffer pixel per physical pixel per
    // eye, so getDefaultScale() deliberately pins those to 1.0 (research/24 §3.8) and writing an
    // explicit scale here would both defeat that and trip applyMonitorRule's "stereo with scale !=
    // 1.0" warning. XR monitors are never stereo today; the guard keeps it that way by construction.
    //
    // And never applied unless it divides `pixelSize` (the mode this output will run) into whole
    // logical pixels. A scale that doesn't divide is not merely imprecise: applyMonitorRule treats a
    // non-auto scale as the USER's, so it snaps to the nearest clean divisor, logs an ERR and raises
    // a red "Invalid scale passed to monitor" notification — blaming the user for a number the
    // compositor picked. 1.25 is clean on every 16:9 mode (1920x1080 -> 1536x864, 2560x1440 ->
    // 2048x1152) but not on 4:3 ones (1024x768 -> 819.2x614.4), so this is reachable in practice.
    // When it doesn't divide we simply decline and the PPI guess stands, which is where such a
    // monitor was already. pixelSize {} (mode not knowable yet) declines for the same reason: this
    // only ever claims a scale it can prove.
    std::optional<float> xrDefaultMonitorScale(bool createdByXR, bool ruleScaleExplicit, bool stereoOutput, const Vector2D& pixelSize, bool skipScaleChecks,
                                               float configuredDefault);

    // ---- idle-inhibit policy (research/20 phase 2) ----

    // openxr:inhibit_idle mode. Governs WHEN a live XR session raises the compositor's
    // ext-idle-notify inhibit bit (hypridle & co stop counting down).
    enum eXRIdleInhibitMode : uint8_t {
        XR_INHIBIT_OFF = 0,  // never inhibit from XR. Legacy 0/false/no.
        XR_INHIBIT_FOCUSED,  // inhibit while the session is FOCUSED — the pre-research/20 behavior,
                             // and what an explicit legacy `inhibit_idle = true` still means.
        XR_INHIBIT_PRESENT,  // inhibit while the headset is actually WORN (default). See
                             // wantXRIdleInhibit for the exact signal fold + the no-presence fallback.
    };

    // Parse the openxr:inhibit_idle config string. Accepts the new "off"|"focused"|"present"
    // spellings AND the legacy boolean spellings for config compat:
    //   0/false/no/off/none                  -> OFF
    //   1/true/yes/focused/focus             -> FOCUSED  (an existing `inhibit_idle = true` keeps its
    //                                                     exact old meaning — no silent widening)
    //   present/worn/presence/user_presence  -> PRESENT
    // Anything unrecognized (including empty) -> PRESENT (the default). Case/whitespace-insensitive. Pure.
    eXRIdleInhibitMode parseIdleInhibitMode(const std::string& v);

    // "off" | "focused" | "present" — the config/IPC string form (surfaced in `hyprctl openxr status`).
    std::string        idleInhibitModeToString(eXRIdleInhibitMode mode);

    // Idle-inhibit predicate (research/20 §5 option D, phase 2). Pure and unconditional so
    // hyprland_gtests can exercise it (tests/xr/idle_inhibit.cpp) — the monado null/remote driver the
    // hyprtester suite runs against cannot script don/doff, so this truth table IS the presence coverage.
    //   OFF     => false.
    //   FOCUSED => sessionFocused (VISIBLE alone deliberately does not inhibit — the old semantics).
    //   PRESENT => a live session AND the headset is worn:
    //     * no session                      -> false.
    //     * runtime WITHOUT XR_EXT_user_presence (null runtime, XREAL over direct Monado, remote
    //       drivers) -> fall back to FOCUSED semantics exactly. There is no wear signal to gate on and
    //       "session exists" would pin the inhibit on forever.
    //     * runtime WITH presence -> require the CONJUNCTION of BOTH real signals, exactly like the
    //       plug gate (wantXRMonitorsPlugged): sessionVisible AND presenceKnown AND userPresent.
    //       Presence alone is NOT sufficient: WiVRn's user_presence STICKS 'present' while the headset
    //       sits doffed in standby (the empirically-established fact behind report-20 issue D), so a
    //       presence-only gate would hold the desktop awake forever on a doffed headset — reinstating,
    //       on the Wayland side, precisely the bug phase 1 fixes on the logind side. Requiring
    //       visibility too means a doff releases the inhibit even when presence is stuck; requiring
    //       presence too means the session-create visibility sprint (WiVRn reaches VISIBLE within ~40ms
    //       even doffed) does not raise it. Before the first presence event we read as ABSENT.
    //   Note PRESENT is strictly WIDER than FOCUSED in the worn case: it also covers worn-but-not-focused
    //   (runtime dashboard in front, overlay mode with another app focused) — the hole research/20 §5.D
    //   set out to close.
    bool wantXRIdleInhibit(eXRIdleInhibitMode mode, bool sessionUp, bool sessionVisible, bool sessionFocused, bool presenceSupported, bool presenceKnown, bool userPresent);

    // First-plug settle guard (report-20 issue D). Whether a would-be plug must be DEFERRED because it
    // is the first plug of the session and visibility has not yet been sustained past the session-start
    // blip window. Applies to the visibility side REGARDLESS of presence support: at session creation a
    // presence-capable runtime can report 'present' within a millisecond (indistinguishable from a
    // spurious blip), so requiring visibility to be sustained is the safety margin before the FIRST
    // plug. Any subsequent plug (everPlugged) never defers — later edges use the anti-flap grace.
    // Pure/gtestable; the caller supplies how long visibility has been continuously true.
    bool xrDeferFirstPlug(bool everPlugged, int64_t visibleSustainedMs, int64_t blipMs);

    // Re-probe backoff schedule (report-17 WP-L3 / report-20 issue B1). Milliseconds to wait before
    // the Nth consecutive re-probe of an absent runtime while dormant in UNAVAILABLE. `attempt` is
    // 0-based; doubling from baseMs, clamped to capMs. Pure/gtestable so the manager's dormant timer
    // (which reads config + owns the CEventLoopTimer) can rely on it. HEADSET-wait (runtime up, headset
    // undonned) uses a fixed gentle cadence instead and does not consult this.
    int64_t xrReprobeBackoffMs(int attempt, int64_t baseMs, int64_t capMs);

    // ---- event-driven re-probe: inotify watch-path derivation (don-the-headset dead-air fix) ----
    // Live evidence (boot 2026-07-14, instance c41d16e2*_1784014116): WiVRn's main server creates
    // and LISTENS on $XDG_RUNTIME_DIR/wivrn/comp_ipc at service startup (create_listen_socket in
    // wivrn server/main.cpp) — the socket exists long before the headset is donned, and it answers
    // IPC in a degraded "no headset" mode that advertises no EGL/GLES extensions (so probes fail at
    // the required-extension check while connect() succeeds). When the headset connects, the main
    // server FORKS the real compositor server, which INHERITS the listen socket — donning produces
    // ZERO filesystem events at the socket path. What the forked server DOES touch is the pid file:
    // monado u_process.c pidfile_open/pidfile_write on XRT_IPC_SERVICE_PID_FILENAME —
    // $XDG_RUNTIME_DIR/wivrn.pid for WiVRn (server/CMakeLists.txt), monado.pid for stock monado —
    // created on the first don of a boot (IN_CREATE) and truncated+rewritten on later dons
    // (pidfile_close leaves the file behind; IN_MODIFY). So the trigger set is sockets AND pid
    // files, and the manager's mask must include IN_MODIFY|IN_CLOSE_WRITE, not just IN_CREATE.
    //
    // A single directory to inotify-watch, plus the basenames whose events there mean something.
    // triggerNames: a create/move-in/modify of one of these = the runtime (or its real server) is
    //   materializing -> probe immediately.
    // subdirNames: creating one of these is a nested socket directory appearing -> start watching it
    //   too (its own socket lands inside a moment later; see xrReprobeWatchDirs for the pairing).
    struct SXRReprobeWatch {
        std::string              dir;          // absolute directory to watch
        std::vector<std::string> triggerNames; // basenames whose create/move-in/modify here triggers a probe
        std::vector<std::string> subdirNames;  // basenames whose creation here means "also watch dir/<name>"
    };

    // Derive the watch set from the value of $XDG_RUNTIME_DIR (pass the raw env string; may be empty).
    // Returns {} when runtimeDir is empty — without XDG_RUNTIME_DIR the socket location is unknown and
    // the caller falls back to the timer alone. Otherwise returns, in order:
    //   [0] $XDG_RUNTIME_DIR       triggers={"monado_comp_ipc","monado.pid","wivrn.pid"}, subdirs={"wivrn"}
    //   [1] $XDG_RUNTIME_DIR/wivrn triggers={"comp_ipc"}
    // Sources (all resolve via monado u_file_get_runtime_dir == $XDG_RUNTIME_DIR):
    //   - monado CMakeLists.txt: XRT_IPC_MSG_SOCK_FILENAME "monado_comp_ipc", XRT_IPC_SERVICE_PID_FILENAME "monado.pid"
    //   - WiVRn  server/CMakeLists.txt: XRT_IPC_MSG_SOCK_FILENAME "wivrn/comp_ipc", XRT_IPC_SERVICE_PID_FILENAME "wivrn.pid"
    // The [1] entry is emitted unconditionally (pure); the manager only inotify-adds it once the
    // directory exists, and adds it dynamically when the "wivrn" subdir creation event fires on [0].
    std::vector<SXRReprobeWatch> xrReprobeWatchDirs(const std::string& runtimeDir);

    // Pure predicates over a watch spec + an event basename, so the trigger decision is gtestable:
    bool xrReprobeTriggerMatch(const SXRReprobeWatch& w, const std::string& name); // name is a probe trigger
    bool xrReprobeSubdirMatch(const SXRReprobeWatch& w, const std::string& name);  // name is a nested dir -> watch it too

    // Debounce between a trigger event and the probe: the socket/server can exist a beat before it is
    // accept()ing usefully, so a probe fired the same instant may still fail. On a matching event the
    // manager resets the backoff to attempt 0 (so if the debounced probe DOES fail it falls back to
    // the FAST end of the schedule, not the grown delay) and arms this one-shot.
    inline constexpr int XR_REPROBE_WATCH_DEBOUNCE_MS = 150;

    // Backoff-policy fix (live-evidence bug 1): the next probe delay, replacing the raw backoff call.
    //   headsetWait     — HEADSET-class wait (xrGetSystem FORM_FACTOR_UNAVAILABLE, or the runtime was
    //                     reachable but lacked the required extensions — WiVRn's pre-don degraded
    //                     mode): fixed base cadence, never grows. This is the leg that turned the
    //                     live boot's dead-air into 30s: the degraded-mode failure was classified as
    //                     "no runtime" and the backoff grew to the cap.
    //   activityRecent  — relevant filesystem activity was seen in the watched dirs within
    //                     XR_REPROBE_ACTIVITY_WINDOW_MS: the runtime is materializing; poll hard
    //                     (base cadence) instead of honoring a grown backoff.
    //   otherwise       — the plain xrReprobeBackoffMs schedule.
    int64_t xrReprobeDelayMs(bool headsetWait, bool activityRecent, int attempt, int64_t baseMs, int64_t capMs);

    // How long after the last relevant watch event the backoff stays capped at the base interval.
    inline constexpr int64_t XR_REPROBE_ACTIVITY_WINDOW_MS = 60000;

    // The known runtime IPC socket paths under $XDG_RUNTIME_DIR ({} when runtimeDir is empty):
    //   $RT/monado_comp_ipc  and  $RT/wivrn/comp_ipc
    // Used by the manager to refine the degraded-runtime wait classification: WiVRn's CLIENT lib
    // answers xrEnumerateInstanceExtensionProperties with a degraded list even when NO server exists
    // (verified in an isolated $XDG_RUNTIME_DIR), so "enumerate answered" alone cannot distinguish
    // "service up, headset undonned" (poll at base forever) from "service not running" (back off).
    // A LISTENING server always has its socket on disk — stat'ing these is the missing bit.
    std::vector<std::string> xrRuntimeSocketPaths(const std::string& runtimeDir);

    // ---- session-loss hardening (freeze audit, 2026-07-14) ----
    // A live wivrn restart under a two-client session froze the desktop. The frame loop must never make
    // a BLOCKING xr call that cannot time out: if it wedges, the main thread's join() in stop() wedges
    // with it and the compositor stops painting the desktop. These bound every such wait.

    // Ceiling for xrWaitSwapchainImage. On a healthy runtime the image is ready ~immediately, so a wait
    // this long means the runtime is wedged/dying — we treat it as loss (drop the session, reprobe)
    // rather than block. NEVER XR_INFINITE_DURATION. Value is nanoseconds (XrDuration). 2s is far above
    // any normal jitter (won't false-drop on a Wi-Fi hiccup) yet bounds the worst-case frame-thread
    // stall — and therefore the worst-case main-thread join — to ~2s instead of forever.
    inline constexpr long long XR_SWAPCHAIN_WAIT_TIMEOUT_NS = 2'000'000'000LL;

    // Consecutive xrWaitFrame/xrBeginFrame hard-failures the frame loop tolerates before latching loss.
    // pollEvents at the top of each iteration normally classifies a dead runtime within one iteration;
    // this is the backstop for a failure code that isn't INSTANCE_LOST/SESSION_LOST, so the loop can
    // never busy-spin on a dead runtime forever.
    inline constexpr int XR_MAX_FRAME_FAIL_STREAK = 8;

    // Diagnostic "slow handshake" threshold (ms) for the reconnect handshake (xrCreateInstance +
    // xrGetSystem). Task #89 phase 2 (blocker B): the handshake now runs FULLY off the main thread and
    // marshals its result back through the handshake eventfd — the main thread never parks on it, so the
    // 30s-cadence reprobe against a cold WiVRn socket (which spawns+fails a monado child over several
    // seconds) no longer stalls the desktop. This constant is therefore no longer an abandon deadline;
    // it is only the threshold past which a completed handshake logs a WARN so the journal records that a
    // runtime was slow to answer. Kept generous so a healthy cold start never trips the warning.
    inline constexpr int XR_HANDSHAKE_TIMEOUT_MS = 5000;

    // Bounded wait (ms) for the direct-mode session BRING-UP (xrCreateSession + createSpaces + initBlitGL
    // + chooseSwapchainFormat + input->init). Task #89 phase 2 (blocker A): these run on a helper thread
    // while the main thread parks in a bounded wait (the runBoundedHandshake idiom). Bring-up happens only
    // when a headset is actually present (a deliberate `xr --direct`), so this park is normally sub-second;
    // the ceiling only bites on a genuine cross-process deadlock with the runtime on a sick leased
    // connector (e80e03be #3 class). On timeout the main thread ABANDONS the helper (which self-cleans on
    // late return) and falls back to UNAVAILABLE + reprobe — the desktop stays responsive instead of
    // freezing. Generous (8-10s) so a healthy bring-up never abandons.
    inline constexpr int XR_BRINGUP_TIMEOUT_MS = 10000;
}

namespace OpenXR {
    // WP5 unification: the parser now produces the canonical doc-03 SXRAnchorState directly
    // (absorbing WP4's placeholder SXRAnchorSpec). The parser stays pure/unconditional; it just
    // includes the equally-pure XRAnchor.hpp for the shared enums/state type.

    // Anchor mode -> the string used by doc 05 §4.3 (status/layout):
    // local|head|body|device:left|device:right.
    std::string anchorModeToString(eXRAnchorMode mode, eXRHand device);
    std::string anchorModeToString(const SXRAnchorState& state);

    // Inverse of parseXRMonitorLine (doc 05 §2.2 grammar / doc 03 §7 pose->text serialization
    // rules): produces one paste-ready `xrmonitor = ...` config line. Pure and unconditional so
    // it is shared by COpenXRManager::layoutDump() (the live `hyprctl openxr layout` path, which
    // resolves `pose` per doc 03 §7 — the anchor's live solved world pose for LOCAL, the
    // persistent stored offset for head/body/device) and by the round-trip unit test
    // (tests/xr/parser.cpp) that this line, reparsed through parseXRMonitorLine, reproduces an
    // equivalent SXRMonitorParams. `anchor.anchorPose` is ignored — `pose` is what gets printed.
    std::string serializeXRMonitorLine(const std::string& name, Vector2D resolution, std::optional<float> refreshHz, const SXRAnchorState& anchor, const SXRPose& pose,
                                        float sizeMeters);
}

// Parameters describing one XR monitor. Every create path (config keyword, dispatcher,
// hyprctl) funnels these into COpenXRManager::createXRMonitor (doc 02). Absent optionals fall
// back to headless/openxr:* defaults.
struct SXRMonitorParams {
    std::string             m_name;        // e.g. "XR-1"; must be unique
    std::optional<Vector2D> m_resolution;  // WxH pixel mode; absent => headless default (1920x1080)
    std::optional<float>    m_refreshRate; // @Hz part; absent => headless default (60)
    std::optional<float>    m_sizeMeters;  // quad width in meters; absent => *openxr:default_size

    // Parsed anchor as the canonical doc-03 state (WP5). anchorPose.rot is built from the
    // parsed yaw/pitch; widthMeters is left at its default here (seeded from m_sizeMeters /
    // openxr:default_size when the layer is created).
    OpenXR::SXRAnchorState m_anchor;
    // True iff an anchor-spec was explicitly given (config keyword always does; the create verb
    // may omit it, in which case the caller places the monitor along the current gaze, doc 05 §3.1).
    bool m_anchorProvided = false;
};

namespace OpenXR {
    // Pure parser for the xrmonitor config keyword value (doc 05 §2.2/§2.3):
    //   <name>, <mode>, <anchor-spec>[, <kv>]...
    // Returns the parsed params or a human-readable error. Compiled unconditionally.
    std::expected<SXRMonitorParams, std::string> parseXRMonitorLine(const std::string& args);

    // Pure parser for the `xrmonitor create` dispatcher / `hyprctl openxr create` verb (doc 05
    // §3.1): space-separated `<name> [WxH[@Hz]] [anchor-spec]`, with defaults applied by the
    // caller (mode defaults to 1920x1080@60, anchor defaults to anchor:local when absent).
    std::expected<SXRMonitorParams, std::string> parseXRMonitorCreateArgs(const std::string& args);
}
