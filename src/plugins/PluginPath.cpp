#include "PluginPath.hpp"

#include <system_error>

#ifndef HYPXR_PLUGIN_DIR
#define HYPXR_PLUGIN_DIR "/usr/lib/hypxrland/plugins"
#endif

static bool isSystemLibDir(const std::filesystem::path& dir) {
    return dir == "/usr/lib" || dir == "/usr/lib64";
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

    // only system plugin packages: a plugin under $HOME or a dev tree is loaded as asked
    const auto PACKAGE_DIR = REQUESTED.parent_path();
    if (!isSystemLibDir(PACKAGE_DIR.parent_path()))
        return requested;

    const auto CANDIDATE = (pluginDir / PACKAGE_DIR.filename() / REQUESTED.filename()).lexically_normal();
    if (CANDIDATE == REQUESTED)
        return requested;

    std::error_code ec;
    if (!std::filesystem::is_regular_file(CANDIDATE, ec))
        return requested;

    return CANDIDATE.string();
}
