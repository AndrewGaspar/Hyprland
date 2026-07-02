#pragma once

#ifdef WITH_XR_TESTS

#include "../../shared.hpp"
#include "../../xr/xr_helpers.hpp"

#include <memory>
#include <vector>

inline std::vector<std::shared_ptr<CTestCase>> xrTestCases;

#ifndef INCLUDED_FROM_MAIN
// What this group of tests is called
#define TEST_GROUP_NAME "xr"
// Where our group's test cases will be stored
#define GROUP_TEST_CASE_STORAGE xrTestCases
#endif // !defined(INCLUDED_FROM_MAIN)

// Skip the current test (counts as a pass) when the XR suite can't run:
// no monado-service, or a vendored-wire ABI mismatch (docs/openxr/06-testing.md §4.2/§5.3).
#define XR_SKIP_IF_UNAVAILABLE()                                                                                                                                                    \
    do {                                                                                                                                                                           \
        std::string _xrSkipReason;                                                                                                                                                 \
        if (XR::shouldSkip(_xrSkipReason)) {                                                                                                                                        \
            XR::logSkip(name(), _xrSkipReason);                                                                                                                                     \
            return;                                                                                                                                                                \
        }                                                                                                                                                                          \
    } while (0)

#endif // WITH_XR_TESTS
