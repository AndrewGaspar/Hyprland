#include "PluginPath.hpp"

#include <system_error>

#ifndef HYPXR_PLUGIN_DIR
#define HYPXR_PLUGIN_DIR "/usr/lib/hypxrland/plugins"
#endif

// Where the hypxrland package installs its own plugin builds (hypxr-pkgs). hyprland-xr.lua loads
// hyprshare from here, so a build that keeps its plugins elsewhere (a fishfood build, see
// scripts/fishfood-plugins.sh) must serve its own copy for that path as well. In the packaged
// build HYPXR_PLUGIN_DIR is this directory, so such a request resolves to itself.
static constexpr const char* PACKAGED_PLUGIN_DIR = "/usr/lib/hypxrland/plugins";

// <dir>/<package dir>/<name>.so is a plugin some package installed
static bool isPackagedPluginRoot(const std::filesystem::path& dir) {
    return dir == "/usr/lib" || dir == "/usr/lib64" || dir == PACKAGED_PLUGIN_DIR;
}

std::filesystem::path Plugins::hypxrPluginDir() {
    return HYPXR_PLUGIN_DIR;
}

std::string Plugins::resolveLoadPath(const std::string& requested, const std::filesystem::path& pluginDir) {
    if (pluginDir.empty())
        return requested;

    const auto REQUESTED = std::filesystem::path{requested}.lexically_normal();
    if (!REQUESTED.is_absolute())
        return requested;

    // only packaged plugins: a plugin under $HOME or a dev tree is loaded as asked
    const auto PACKAGE_DIR = REQUESTED.parent_path();
    if (!isPackagedPluginRoot(PACKAGE_DIR.parent_path()))
        return requested;

    const auto CANDIDATE = (pluginDir / PACKAGE_DIR.filename() / REQUESTED.filename()).lexically_normal();
    if (CANDIDATE == REQUESTED)
        return requested;

    std::error_code ec;
    if (!std::filesystem::is_regular_file(CANDIDATE, ec))
        return requested;

    return CANDIDATE.string();
}
