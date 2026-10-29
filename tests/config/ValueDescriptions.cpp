#include <config/values/ConfigValues.hpp>

#include <gtest/gtest.h>

#include <string_view>

using namespace Config::Values;

// `hyprctl descriptions` is Values::getAsJson(), which splices every option's name and description
// verbatim into JSON string literals — nothing is escaped. One double quote, backslash or control
// character in any of them makes the whole array invalid JSON for every consumer (jq, settings UIs,
// hyprtester's hyprctlJsonErrors). The 0.57 rebase shipped exactly that: openxr:surfaces_synthetic
// quoted its example in "…". Write examples in backticks.
TEST(Config, valueNamesAndDescriptionsAreJsonSafe) {
    for (const auto& value : CONFIG_VALUES) {
        const std::string_view NAME        = value->name() ? value->name() : "";
        const std::string_view DESCRIPTION = value->description() ? value->description() : "";

        for (const auto& [WHAT, TEXT] : {std::pair{"name", NAME}, std::pair{"description", DESCRIPTION}}) {
            for (const char c : TEXT) {
                const bool UNSAFE = c == '"' || c == '\\' || static_cast<unsigned char>(c) < 0x20;
                EXPECT_FALSE(UNSAFE) << NAME << ": JSON-unsafe character 0x" << std::hex << static_cast<int>(static_cast<unsigned char>(c)) << " in its " << WHAT;
                if (UNSAFE)
                    break;
            }
        }
    }
}
