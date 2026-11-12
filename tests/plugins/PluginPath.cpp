// Plugins::resolveLoadPath() lets HypXRland load its own build of a plugin that a stock-Hyprland
// config asks for by its system path (Omarchy hardcodes /usr/lib/omarchy-hyprland-titlebars/
// titlebars.so). The stock build would only fail its version check.

#include <plugins/PluginPath.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <unistd.h>

namespace fs = std::filesystem;

class PluginPathTest : public testing::Test {
  protected:
    void SetUp() override {
        m_dir = fs::temp_directory_path() / ("hypxr-plugin-path-" + std::to_string(getpid()));
        fs::create_directories(m_dir / "titlebars-pkg");
        std::ofstream{m_dir / "titlebars-pkg" / "titlebars.so"} << "x";
    }

    void TearDown() override {
        fs::remove_all(m_dir);
    }

    fs::path m_dir;
};

TEST_F(PluginPathTest, systemPathUsesTheHypXRlandBuild) {
    const auto EXPECTED = (m_dir / "titlebars-pkg" / "titlebars.so").string();
    EXPECT_EQ(Plugins::resolveLoadPath("/usr/lib/titlebars-pkg/titlebars.so", m_dir), EXPECTED);
    EXPECT_EQ(Plugins::resolveLoadPath("/usr/lib64/titlebars-pkg/titlebars.so", m_dir), EXPECTED);
    EXPECT_EQ(Plugins::resolveLoadPath("/usr/lib//titlebars-pkg/./titlebars.so", m_dir), EXPECTED);
}

TEST_F(PluginPathTest, noHypXRlandBuildLoadsAsAsked) {
    EXPECT_EQ(Plugins::resolveLoadPath("/usr/lib/titlebars-pkg/other.so", m_dir), "/usr/lib/titlebars-pkg/other.so");
    EXPECT_EQ(Plugins::resolveLoadPath("/usr/lib/other-pkg/titlebars.so", m_dir), "/usr/lib/other-pkg/titlebars.so");
}

TEST_F(PluginPathTest, nonSystemPathsAreNeverRedirected) {
    // a dev build or a hyprpm plugin must load exactly as asked, even with a matching name
    EXPECT_EQ(Plugins::resolveLoadPath("/home/u/titlebars-pkg/titlebars.so", m_dir), "/home/u/titlebars-pkg/titlebars.so");
    EXPECT_EQ(Plugins::resolveLoadPath("/usr/local/lib/titlebars-pkg/titlebars.so", m_dir), "/usr/local/lib/titlebars-pkg/titlebars.so");
    EXPECT_EQ(Plugins::resolveLoadPath("/usr/lib/titlebars.so", m_dir), "/usr/lib/titlebars.so");
    EXPECT_EQ(Plugins::resolveLoadPath("titlebars-pkg/titlebars.so", m_dir), "titlebars-pkg/titlebars.so");
    EXPECT_EQ(Plugins::resolveLoadPath("titlebars.so", m_dir), "titlebars.so");
}

TEST_F(PluginPathTest, aHypXRlandBuildPathLoadsAsAsked) {
    const auto OWN = (m_dir / "titlebars-pkg" / "titlebars.so").string();
    EXPECT_EQ(Plugins::resolveLoadPath(OWN, m_dir), OWN);
}

TEST_F(PluginPathTest, aDirectoryIsNotAPlugin) {
    fs::create_directories(m_dir / "dir-pkg" / "dir.so");
    EXPECT_EQ(Plugins::resolveLoadPath("/usr/lib/dir-pkg/dir.so", m_dir), "/usr/lib/dir-pkg/dir.so");
}

TEST_F(PluginPathTest, emptyPluginDirDisablesRedirects) {
    EXPECT_EQ(Plugins::resolveLoadPath("/usr/lib/titlebars-pkg/titlebars.so", ""), "/usr/lib/titlebars-pkg/titlebars.so");
}
