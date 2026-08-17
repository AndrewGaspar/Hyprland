#include <openxr/XRMonitorConfig.hpp>
#include <openxr/XRBoundedCall.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>

using namespace OpenXR;

// The kernel-taint tripwire (doc 01, "Sick-driver refusal"). XR bring-up cannot avoid initializing
// every installed GPU vendor driver — libglvnd loads them all on the first eglGetProcAddress and
// contacts their kernel drivers on the count-only eglQueryDevicesEXT — so the only real defense
// against a driver the kernel has already oopsed inside is to refuse to start. These tests pin the
// truth table so COpenXRManager::start() can apply it blindly.
//
// The file read itself stays in the manager (the resolveRuntimeJsonEnv split), so everything here
// runs without a tainted kernel, without /proc, and without HAVE_OPENXR.

// ---- parseKernelTaint -----------------------------------------------------------------------

TEST(XRKernelTaint, ParsesPlainDecimal) {
    EXPECT_EQ(parseKernelTaint("0"), std::optional<uint64_t>{0});
    EXPECT_EQ(parseKernelTaint("128"), std::optional<uint64_t>{128});
    EXPECT_EQ(parseKernelTaint("12288"), std::optional<uint64_t>{12288});
}

TEST(XRKernelTaint, ParsesTheRealProcFormat) {
    // What the kernel actually writes: the value plus a trailing newline.
    EXPECT_EQ(parseKernelTaint("12288\n"), std::optional<uint64_t>{12288});
    EXPECT_EQ(parseKernelTaint("  4096 \n"), std::optional<uint64_t>{4096});
    EXPECT_EQ(parseKernelTaint("\t512\r\n"), std::optional<uint64_t>{512});
}

TEST(XRKernelTaint, ParsesLargeValues) {
    // Every taint bit set — must not overflow or wrap into a false negative.
    EXPECT_EQ(parseKernelTaint("18446744073709551615"), std::optional<uint64_t>{UINT64_MAX});
}

TEST(XRKernelTaint, RejectsUnparsableContents) {
    // Empty (also how the manager signals "could not open the file"), junk, partial junk, and
    // negatives. Anything we do not fully understand is nullopt => fail open.
    EXPECT_FALSE(parseKernelTaint("").has_value());
    EXPECT_FALSE(parseKernelTaint("   \n").has_value());
    EXPECT_FALSE(parseKernelTaint("abc").has_value());
    EXPECT_FALSE(parseKernelTaint("128abc").has_value());
    EXPECT_FALSE(parseKernelTaint("0x80").has_value());
    EXPECT_FALSE(parseKernelTaint("-1").has_value());
    EXPECT_FALSE(parseKernelTaint("12 34").has_value());
}

// ---- readKernelTaint ------------------------------------------------------------------------
// The manager calls exactly this, so these cover the real read path rather than a copy of it. A
// read that silently returned nullopt would disable the tripwire forever with no symptom at all.

TEST(XRKernelTaint, ReadsTheRealProcFile) {
    // The one assertion that catches "our read does not match the kernel's actual format". Skipped
    // rather than failed where /proc is not mounted, so this stays portable.
    if (!std::filesystem::exists(XR_TAINT_PROC_PATH))
        GTEST_SKIP() << XR_TAINT_PROC_PATH << " not present";

    const auto taint = readKernelTaint();
    ASSERT_TRUE(taint.has_value()) << "could not read/parse the real " << XR_TAINT_PROC_PATH << " — the tripwire would silently never fire";

    // Nothing is asserted about the VALUE: a healthy dev box reads 0 or 12288, and a box that has
    // oopsed legitimately reads bit 7. Both are valid; only "we could parse it" is under test.
    SUCCEED() << "kernel taint reads " << *taint;
}

TEST(XRKernelTaint, ReadsAFileWithTheProcFormat) {
    const auto path = std::filesystem::temp_directory_path() / "hypxr_taint_test_value";
    {
        std::ofstream f(path);
        f << "12416\n";
    }
    EXPECT_EQ(readKernelTaint(path.string()), std::optional<uint64_t>{12416});
    std::filesystem::remove(path);
}

TEST(XRKernelTaint, MissingFileReadsAsNullopt) {
    // Fail open: a kernel without the entry must not cost the user their XR session.
    EXPECT_FALSE(readKernelTaint("/nonexistent/hypxr/definitely/not/here").has_value());
}

TEST(XRKernelTaint, GarbageFileReadsAsNullopt) {
    const auto path = std::filesystem::temp_directory_path() / "hypxr_taint_test_garbage";
    {
        std::ofstream f(path);
        f << "not a number\n";
    }
    EXPECT_FALSE(readKernelTaint(path.string()).has_value());
    std::filesystem::remove(path);
}

TEST(XRKernelTaint, EmptyFileReadsAsNullopt) {
    const auto path = std::filesystem::temp_directory_path() / "hypxr_taint_test_empty";
    { std::ofstream f(path); }
    EXPECT_FALSE(readKernelTaint(path.string()).has_value());
    std::filesystem::remove(path);
}

TEST(XRKernelTaint, ReadAndEvaluateComposeAsTheManagerComposesThem) {
    const auto path = std::filesystem::temp_directory_path() / "hypxr_taint_test_oops";
    {
        std::ofstream f(path);
        f << "12416\n"; // 12288 (module bits) | 128 (TAINT_DIE)
    }
    EXPECT_TRUE(evaluateKernelTaint(readKernelTaint(path.string()), false).blocked);
    EXPECT_FALSE(evaluateKernelTaint(readKernelTaint(path.string()), true).blocked); // escape hatch
    std::filesystem::remove(path);
}

// ---- evaluateKernelTaint --------------------------------------------------------------------

TEST(XRKernelTaint, UntaintedKernelProceeds) {
    const auto v = evaluateKernelTaint(std::optional<uint64_t>{0}, false);
    EXPECT_FALSE(v.blocked);
    EXPECT_FALSE(v.oopsed);
    EXPECT_TRUE(v.reason.empty());
}

TEST(XRKernelTaint, OrdinaryTaintBitsDoNotBlock) {
    // THE case that must not regress: a stock NVIDIA box reads 12288 (bit 12 out-of-tree module +
    // bit 13 unsigned module) on every boot. Those say nothing about driver health. Blocking on
    // them would make the tripwire fire permanently on the exact machines XR is used on.
    for (const uint64_t taint : {(uint64_t)1, (uint64_t)2, (uint64_t)64, (uint64_t)4096, (uint64_t)8192, (uint64_t)12288, (uint64_t)0x40000}) {
        const auto v = evaluateKernelTaint(taint, false);
        EXPECT_FALSE(v.blocked) << "taint " << taint << " must not block";
        EXPECT_FALSE(v.oopsed) << "taint " << taint << " must not read as an oops";
    }
}

TEST(XRKernelTaint, TaintDieBlocks) {
    const auto v = evaluateKernelTaint(std::optional<uint64_t>{XR_TAINT_DIE_MASK}, false);
    EXPECT_TRUE(v.blocked);
    EXPECT_TRUE(v.oopsed);
    EXPECT_FALSE(v.reason.empty());
}

TEST(XRKernelTaint, TaintDieMaskIsBitSeven) {
    // Pinned against a typo: TAINT_DIE is bit 7 == 128. Getting this wrong either never fires or
    // fires on every NVIDIA box, and both failures are silent.
    EXPECT_EQ(XR_TAINT_DIE_BIT, 7u);
    EXPECT_EQ(XR_TAINT_DIE_MASK, 128u);
}

TEST(XRKernelTaint, TaintDieBlocksAlongsideOtherBits) {
    // The realistic post-oops reading on the forensic machine: the everyday module bits PLUS
    // TAINT_DIE. The other bits must not mask the one that matters.
    const auto v = evaluateKernelTaint(std::optional<uint64_t>{12288 + 128}, false);
    EXPECT_TRUE(v.blocked);
    EXPECT_TRUE(v.oopsed);
}

TEST(XRKernelTaint, EscapeHatchProceedsButStillReports) {
    // openxr:ignore_kernel_taint = 1: proceed, but do NOT go quiet — the caller still WARNs, so a
    // developer who set the hatch months ago is reminded why their session is unusual.
    const auto v = evaluateKernelTaint(std::optional<uint64_t>{XR_TAINT_DIE_MASK}, true);
    EXPECT_FALSE(v.blocked);
    EXPECT_TRUE(v.oopsed);
    EXPECT_FALSE(v.reason.empty());
    EXPECT_NE(v.reason.find("ignore_kernel_taint"), std::string::npos);
}

TEST(XRKernelTaint, EscapeHatchIsANoOpOnAHealthyKernel) {
    const auto v = evaluateKernelTaint(std::optional<uint64_t>{12288}, true);
    EXPECT_FALSE(v.blocked);
    EXPECT_FALSE(v.oopsed);
    EXPECT_TRUE(v.reason.empty());
}

TEST(XRKernelTaint, UnreadableTaintFailsOpen) {
    // A missing/unparsable /proc/sys/kernel/tainted (container, exotic kernel) must never cost the
    // user their XR session. The tripwire is a safety net over a rare event, not a precondition.
    for (const bool ignore : {false, true}) {
        const auto v = evaluateKernelTaint(std::nullopt, ignore);
        EXPECT_FALSE(v.blocked);
        EXPECT_FALSE(v.oopsed);
        EXPECT_TRUE(v.reason.empty());
    }
}

TEST(XRKernelTaint, BlockedReasonIsSelfContained) {
    // This exact string is both the ERR log line and the `blocked:` line in `hyprctl openxr
    // status`, where it appears with no surrounding context — so it has to say what happened, why
    // it matters here, and what to do about it, on its own.
    const auto v = evaluateKernelTaint(std::optional<uint64_t>{128}, false);
    ASSERT_TRUE(v.blocked);
    EXPECT_NE(v.reason.find("oops"), std::string::npos);          // what happened
    EXPECT_NE(v.reason.find("GPU driver"), std::string::npos);    // why it matters here
    EXPECT_NE(v.reason.find("reboot"), std::string::npos);        // what to do
    EXPECT_NE(v.reason.find("128"), std::string::npos);           // the raw value, for a bug report
}

TEST(XRKernelTaint, EndToEndFromProcContents) {
    // The two halves composed the way the manager composes them.
    const auto healthy = evaluateKernelTaint(parseKernelTaint("12288\n"), false);
    EXPECT_FALSE(healthy.blocked);

    const auto oopsed = evaluateKernelTaint(parseKernelTaint("12416\n"), false); // 12288 | 128
    EXPECT_TRUE(oopsed.blocked);

    const auto unreadable = evaluateKernelTaint(parseKernelTaint(""), false);
    EXPECT_FALSE(unreadable.blocked);
}

// ---- runBoundedProbe ------------------------------------------------------------------------
// The throwaway-thread pattern the EGL enumeration, the EGL device query and the Vulkan probe all
// share. Its contract is what keeps a wedged GPU driver from freezing the desktop.

TEST(XRBoundedProbe, ReturnsAFastResult) {
    const auto r = runBoundedProbe([](const std::atomic<bool>&) { return 42; }, 3000);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, 42);
}

TEST(XRBoundedProbe, TimesOutOnAStuckCall) {
    // The whole point: a probe that never returns must not hold the caller. The worker below
    // watches `abandon` so this test does not leak a thread for the rest of the run.
    const auto start = std::chrono::steady_clock::now();
    const auto r     = runBoundedProbe(
        [](const std::atomic<bool>& abandon) {
            while (!abandon.load(std::memory_order_acquire))
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            return 7;
        },
        100);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_FALSE(r.has_value());
    // Bounded by the budget (plus the poll granularity and scheduling slop), NOT by the callee.
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 2000);
}

TEST(XRBoundedProbe, SignalsAbandonOnTimeout) {
    // A late-unblocking worker must be able to see that the caller moved on, so it can bail before
    // touching anything the caller has since torn down (an XrInstance, in practice).
    auto       observed = std::make_shared<std::atomic<bool>>(false);
    const auto r        = runBoundedProbe(
        [observed](const std::atomic<bool>& abandon) {
            for (int i = 0; i < 400 && !abandon.load(std::memory_order_acquire); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            observed->store(abandon.load(std::memory_order_acquire), std::memory_order_release);
            return 1;
        },
        50);
    EXPECT_FALSE(r.has_value());

    for (int i = 0; i < 200 && !observed->load(std::memory_order_acquire); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    EXPECT_TRUE(observed->load(std::memory_order_acquire)) << "the abandoned worker never saw the abandon flag";
}

TEST(XRBoundedProbe, CarriesNonTrivialResultsBack) {
    // The EGL scan returns a struct owning heap strings across the thread boundary.
    struct SScan {
        bool                     ok = false;
        std::vector<std::string> paths;
    };
    const auto r = runBoundedProbe(
        [](const std::atomic<bool>&) {
            return SScan{true, {"/dev/dri/renderD128", "/dev/dri/renderD129"}};
        },
        3000);
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->ok);
    ASSERT_EQ(r->paths.size(), 2u);
    EXPECT_EQ(r->paths[0], "/dev/dri/renderD128");
}
