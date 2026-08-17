#pragma once

// Deliberately compiles unconditionally (no OpenXR headers, no HAVE_OPENXR guard) — it is pure
// std::thread plumbing with no XR/EGL/Vulkan types in its interface, so hyprland_gtests can
// exercise it directly (tests/xr/bounded_call.cpp).
//
// THE THROWAWAY-THREAD PATTERN, in one place.
//
// XR bring-up has to make several calls that are, in practice, unbounded: they enter a third-party
// GPU driver or the OpenXR runtime, and a sick one can block forever rather than return an error.
// Observed instances:
//   * vkCreateInstance inside the Vulkan GPU probe — deadlocks against the runtime's own in-process
//     Vulkan usage (hung indefinitely vs Monado's null compositor).
//   * libglvnd EGL device enumeration — eglGetProcAddress loads EVERY installed vendor library and
//     the count-only eglQueryDevicesEXT then opens /dev/nvidiactl + /dev/nvidia0 (measured, doc 01).
//     Against a wedged NVIDIA driver that is a main-thread hang, i.e. a frozen desktop.
//   * xrGetSystemEGLDeviceMND — answered in-process, but the runtime services it by calling back
//     through our eglGetProcAddress, so it inherits the same exposure.
//
// None of those may run on the compositor main thread, because a hang there is not "XR failed to
// start", it is "the user's whole desktop is gone and only a power cycle gets it back" (2026-07-15).
//
// ACCEPTED COST, stated plainly: on timeout the worker thread is ABANDONED, not killed — there is no
// safe way to cancel a thread stuck inside a driver. It keeps its stack and whatever driver locks it
// holds for the life of the process, i.e. it leaks. That is a deliberate trade: one leaked thread in
// a session that is already refusing to bring XR up, versus a frozen desktop. The same trade the
// Vulkan probe has always made.
//
// The `abandon` flag handed to the callable is how a late unblock stays safe: the worker must poll
// it before touching any resource the caller may have torn down (an XrInstance, in practice) and
// bail if it is set. Its result is discarded once abandoned.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace OpenXR {
    // How long bring-up waits on any single driver/runtime probe before abandoning it. One value for
    // all of them: this is "how long a healthy driver could conceivably take", not a per-call tuning
    // knob, and 3s is already an eternity for what are supposed to be local queries.
    inline constexpr int XR_PROBE_TIMEOUT_MS = 3000;

    // Run `fn` on a detached throwaway thread and wait at most `timeoutMs` for it.
    //   returns the value  — the probe completed within the budget.
    //   returns nullopt    — TIMED OUT. The thread was abandoned (see above) and `abandon` is set,
    //                        so a late unblock discards its result instead of writing it back.
    // `fn` is invoked as fn(const std::atomic<bool>& abandon) and must return T.
    //
    // The healthy case — every probe here answers in well under a millisecond — returns as soon as
    // the worker signals, not on a poll tick. That matters because bring-up now makes three of
    // these calls back to back, and a poll-granularity floor would be pure added latency on the
    // path a user waits through every time they put the headset on.
    template <typename Fn>
    auto runBoundedProbe(Fn&& fn, int timeoutMs = XR_PROBE_TIMEOUT_MS) -> std::optional<decltype(fn(std::declval<const std::atomic<bool>&>()))> {
        using T = decltype(fn(std::declval<const std::atomic<bool>&>()));

        // All shared state lives behind shared_ptrs precisely BECAUSE the worker may outlive this
        // frame: an abandoned thread keeps its own references alive, so nothing here dangles once
        // we return — including the mutex and condition_variable it signals on the way out. This is
        // the one subtlety that has to be right, and it is why none of this is a stack local.
        struct SShared {
            std::mutex              mtx;
            std::condition_variable cv;
            bool                    done = false; // guarded by mtx
            std::optional<T>        result;       // guarded by mtx
            std::atomic<bool>       abandon{false};
        };
        auto sh = std::make_shared<SShared>();

        std::thread([sh, fn = std::forward<Fn>(fn)]() mutable {
            auto r = fn(static_cast<const std::atomic<bool>&>(sh->abandon));
            {
                std::lock_guard lk(sh->mtx);
                // Re-check under the lock: the caller sets `abandon` and stops caring, so a worker
                // that unblocks late must drop its answer rather than publish into a result nobody
                // will read. Taking the lock also means the caller can never observe a half-written
                // result.
                if (!sh->abandon.load(std::memory_order_acquire))
                    sh->result = std::move(r);
                sh->done = true;
            }
            sh->cv.notify_one();
        }).detach();

        std::unique_lock lk(sh->mtx);
        if (sh->cv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&] { return sh->done; }))
            return std::move(sh->result);

        // Timed out. Latch `abandon` while still holding the lock so the worker cannot be midway
        // through deciding whether to publish.
        sh->abandon.store(true, std::memory_order_release);
        return std::nullopt;
    }
}
