#include <desktop/view/ProcIdentity.hpp>
#include <desktop/rule/Rule.hpp>
#include <desktop/rule/matchEngine/RegexMatchEngine.hpp>
#include <desktop/rule/windowRule/WindowRule.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <unistd.h>

// task #143 — `match:exe` / `match:cmdline`, identity by PROCESS rather than by surface.
//
// Three layers are under test, which together are the whole feature minus the one line of glue in
// CWindowRule::matches (which needs a live CWindow and belongs to hyprtester):
//
//   1. cmdlineFromRaw — the only interesting parsing in the reader, and the reason `match:cmdline`
//      is writable at all: /proc gives a NUL-separated argv blob, a regex needs one string.
//   2. the reader and its seam — a real read of our own /proc entry, the failure modes that must
//      cache as empty rather than error, and the fake a test can install.
//   3. the grammar — both props reachable from the `match:` table both front-ends parse against,
//      wired to the regex engine, and the exact rule the user wants actually matching the exact
//      cmdline a live Proton game reports.

using namespace Desktop;
using namespace Desktop::Rule;

namespace {
    // verified live on 2026-08-09 against a running Proton game. This pair is the whole argument for
    // the feature: the exe is shared by every Wine app on the system, the cmdline names the game.
    constexpr const char* WINE_EXE     = "/home/ajg/.steam/steam/steamapps/common/Proton - Experimental/files/bin/wine-preloader";
    constexpr const char* WINE_CMDLINE = "Z:\\home\\ajg\\Games\\Ishimura\\game\\Dead Space.exe";

    std::string           withNuls(std::initializer_list<const char*> argv, bool trailingNul = true) {
        std::string out;
        for (const auto* a : argv) {
            out += a;
            out.push_back('\0');
        }
        if (!trailingNul && !out.empty())
            out.pop_back();
        return out;
    }
}

// --- 1. the fold: /proc's argv blob into a matchable string ---

TEST(ProcIdentity, cmdlineFoldsNulsToSingleSpaces) {
    EXPECT_EQ(Proc::cmdlineFromRaw(withNuls({"mpv", "--fs", "/tmp/a b.mkv"})), "mpv --fs /tmp/a b.mkv");
}

TEST(ProcIdentity, cmdlineDropsTheTrailingNul) {
    // the trailing NUL is universal in /proc and would otherwise become a trailing space, which
    // would break every anchored regex a user writes
    EXPECT_EQ(Proc::cmdlineFromRaw(withNuls({"kitty"})), "kitty");
    EXPECT_EQ(Proc::cmdlineFromRaw(withNuls({"kitty"}, /* trailingNul */ false)), "kitty");
    EXPECT_EQ(Proc::cmdlineFromRaw(std::string_view("kitty\0\0\0", 8)), "kitty");
}

TEST(ProcIdentity, cmdlineOfAKernelThreadIsEmpty) {
    // a kernel thread has a genuinely empty cmdline; it must fold to "" and never to " "
    EXPECT_EQ(Proc::cmdlineFromRaw(""), "");
    EXPECT_EQ(Proc::cmdlineFromRaw(std::string_view("\0", 1)), "");
}

TEST(ProcIdentity, cmdlineKeepsAnEmptyArgumentAsASeparator) {
    // argv may contain empty strings; folding must not silently join the neighbours
    EXPECT_EQ(Proc::cmdlineFromRaw(withNuls({"prog", "", "tail"})), "prog  tail");
}

TEST(ProcIdentity, cmdlineHoldsTheWineIdentityVerbatim) {
    // backslashes and the space in "Dead Space.exe" survive the fold untouched — they are what the
    // user's regex has to be written against
    EXPECT_EQ(Proc::cmdlineFromRaw(withNuls({WINE_CMDLINE})), WINE_CMDLINE);
}

// --- 2. the reader, and the seam ---

TEST(ProcIdentity, readsOurOwnProcEntry) {
    const auto IDENT = Proc::readIdentity(getpid());
    EXPECT_FALSE(IDENT.exe.empty());
    EXPECT_TRUE(IDENT.exe.starts_with("/")) << IDENT.exe;
    EXPECT_NE(IDENT.cmdline.find("hyprland_gtests"), std::string::npos) << IDENT.cmdline;
    EXPECT_FALSE(IDENT.cmdline.ends_with(" ")) << "[" << IDENT.cmdline << "]";
}

TEST(ProcIdentity, anUnreadableProcEntryCachesAsEmptyRatherThanErroring) {
    // getPID() returns -1 once a surface is gone, and a pid can exit between the map and the read.
    // Both have to be a quiet non-match: a rule that threw or logged on every dead window would be
    // worse than one that simply does not fire.
    for (const pid_t PID : {-1, 0}) {
        const auto IDENT = Proc::readIdentity(PID);
        EXPECT_TRUE(IDENT.exe.empty()) << PID;
        EXPECT_TRUE(IDENT.cmdline.empty()) << PID;
    }

    // a pid that cannot exist (above the kernel's ceiling) — the read fails, nothing throws
    const auto GONE = Proc::readIdentity(0x7FFFFFFF);
    EXPECT_TRUE(GONE.exe.empty());
    EXPECT_TRUE(GONE.cmdline.empty());
}

TEST(ProcIdentity, anEmptyIdentityNeverMatchesARule) {
    // the property that makes the failure mode above safe: whatever the rule says, "" does not match
    for (const auto* PATTERN : {".*", ".*wine.*", "^$.", "Dead Space"}) {
        CRegexMatchEngine engine(PATTERN);
        if (std::string_view(PATTERN) == ".*")
            continue; // `.*` legitimately matches the empty string; every real-world rule does not
        EXPECT_FALSE(engine.match("")) << PATTERN;
    }
}

TEST(ProcIdentity, theReaderSeamIsInstallableAndRestorable) {
    const auto REAL = Proc::readIdentity(getpid());

    Proc::setIdentityReaderForTesting([](pid_t pid) { return Proc::SIdentity{.exe = WINE_EXE, .cmdline = std::format("{} {}", WINE_CMDLINE, pid)}; });

    const auto FAKE = Proc::identityFor(4242);
    EXPECT_EQ(FAKE.exe, WINE_EXE);
    EXPECT_EQ(FAKE.cmdline, std::format("{} 4242", WINE_CMDLINE));

    Proc::setIdentityReaderForTesting(nullptr);
    EXPECT_EQ(Proc::identityFor(getpid()).exe, REAL.exe);
}

// --- 3. the grammar ---

TEST(ProcIdentity, bothPropsAreInTheMatchGrammar) {
    const auto PROPS = allMatchPropStrings();
    EXPECT_TRUE(std::ranges::contains(PROPS, std::string_view("exe")));
    EXPECT_TRUE(std::ranges::contains(PROPS, std::string_view("cmdline")));

    EXPECT_EQ(matchPropFromString("exe"), RULE_PROP_EXE);
    EXPECT_EQ(matchPropFromString("cmdline"), RULE_PROP_CMDLINE);
    EXPECT_EQ(matchPropFromString("cmdlines"), std::nullopt); // the table is exact, not a prefix
}

TEST(ProcIdentity, registeringEitherRaisesItsPropertyBit) {
    // the mask is what WindowRuleApplicator::propertiesChanged filters on, so a prop that does not
    // raise its bit is a rule that never re-evaluates
    CWindowRule exeRule("exe");
    exeRule.registerMatch(RULE_PROP_EXE, "^/usr/bin/mpv$");
    EXPECT_TRUE(exeRule.getPropertiesMask() & RULE_PROP_EXE);
    EXPECT_FALSE(exeRule.getPropertiesMask() & RULE_PROP_CMDLINE);

    CWindowRule cmdRule("cmdline");
    cmdRule.registerMatch(RULE_PROP_CMDLINE, ".*");
    EXPECT_TRUE(cmdRule.getPropertiesMask() & RULE_PROP_CMDLINE);
}

TEST(ProcIdentity, theWineGameRuleMatchesTheWineGameCmdline) {
    // THE acceptance case: `windowrule = stereo hsbs always, match:cmdline <this>` has to fire on a
    // live Proton Dead Space, whose class and title belong to the wrapper.
    const std::string CMDLINE = Proc::cmdlineFromRaw(withNuls({WINE_CMDLINE}));

    // the backslash-free form is the one to ship: `.` matches the literal `\` Wine reports, so the
    // rule does not depend on how many layers of escaping a config line survives
    CRegexMatchEngine shipped(".*Ishimura.game.Dead Space.exe");
    EXPECT_TRUE(shipped.match(CMDLINE));

    CRegexMatchEngine engine(".*Ishimura.game.Dead Space\\.exe.*");
    EXPECT_TRUE(engine.match(CMDLINE));

    // ... and does not fire on the neighbours it must not catch
    EXPECT_FALSE(engine.match(Proc::cmdlineFromRaw(withNuls({"Z:\\home\\ajg\\Games\\Ishimura\\game\\DeadSpaceLauncher.exe"}))));
    EXPECT_FALSE(engine.match(Proc::cmdlineFromRaw(withNuls({"steam", "-applaunch", "17470"}))));
    EXPECT_FALSE(engine.match(""));
}

TEST(ProcIdentity, matchIsFullMatchSoAnUnwrappedPatternIsATrap) {
    // `match:` regexes are FullMatch across the whole grammar (unlike xrrule's, which search), and
    // a cmdline is a long absolute path — so the bare pattern a user reaches for first matches
    // NOTHING. Pinned here because it is the single most likely way this feature looks broken.
    const std::string CMDLINE = Proc::cmdlineFromRaw(withNuls({WINE_CMDLINE}));

    CRegexMatchEngine bare("Ishimura.game.Dead Space\\.exe");
    EXPECT_FALSE(bare.match(CMDLINE)) << "if this ever passes, match: stopped being FullMatch";

    CRegexMatchEngine wrapped(".*Ishimura.game.Dead Space\\.exe.*");
    EXPECT_TRUE(wrapped.match(CMDLINE));
}

TEST(ProcIdentity, exeMatchesTheNativeCaseAndTheWinePreloaderTrap) {
    CRegexMatchEngine mpv("^/usr/bin/mpv$");
    EXPECT_TRUE(mpv.match("/usr/bin/mpv"));
    EXPECT_FALSE(mpv.match("/usr/local/bin/mpv"));

    // every Wine app on the box shares this exe, which is exactly why match:exe cannot identify a
    // Proton game and match:cmdline has to
    CRegexMatchEngine preloader(".*wine-preloader$");
    EXPECT_TRUE(preloader.match(WINE_EXE));
}
