#pragma once

#include <filesystem>
#include <string>

namespace Plugins {
    // Where HypXRland builds of system plugins live, as <dir>/<package dir>/<name>.so.
    std::filesystem::path hypxrPluginDir();

    // The file to dlopen for a requested plugin path. A plugin only loads into the exact
    // compositor build it was compiled against, and configs written for stock Hyprland hardcode
    // stock plugin paths (Omarchy loads /usr/lib/omarchy-hyprland-titlebars/titlebars.so). When
    // the request is /usr/lib{,64}/<pkg>/<name>.so and <pluginDir>/<pkg>/<name>.so exists, that
    // build is returned instead. Anything else is returned unchanged.
    std::string resolveLoadPath(const std::string& requested, const std::filesystem::path& pluginDir = hypxrPluginDir());
}
