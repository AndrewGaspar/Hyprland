#include <helpers/memory/Memory.hpp>

#include <gtest/gtest.h>

#include <unordered_map>

// tests/xr/viewpoint_surface_keying.cpp — the hyprutils weak-pointer semantics
// CXRViewpointProtocol::m_viewpoints depends on.
//
// That map is keyed by WP<CWLSurfaceResource>, and a viewpoint deliberately OUTLIVES its surface:
// the protocol object stays usable until the client destroys it, so entries with EXPIRED keys sit
// in the map for as long as the client keeps them. If two expired keys compared equal (as
// std::weak_ptr's owner_before ordering would happily allow for a naive "both are null" rule), two
// viewpoints whose surfaces both died would alias one entry and one of them would vanish.
//
// They do not, because hyprutils keys both hash and equality on the CONTROL BLOCK, never on the
// data pointer or on expiry — and holds a weak reference to that block, so its address cannot be
// recycled underneath a live key. That is load-bearing and comes from a dependency we do not own,
// so it is pinned here rather than assumed.

namespace {
    struct SThing {
        int id = 0;
    };
}

TEST(XRViewpointSurfaceKeying, DistinctExpiredWeakPointersDoNotAlias) {
    auto    a = makeShared<SThing>(SThing{.id = 1});
    auto    b = makeShared<SThing>(SThing{.id = 2});
    WP<SThing> wa = a;
    WP<SThing> wb = b;

    ASSERT_FALSE(wa == wb);

    a.reset();
    b.reset();
    ASSERT_TRUE(wa.expired());
    ASSERT_TRUE(wb.expired());

    // Expiry must not collapse two different objects onto one key.
    EXPECT_FALSE(wa == wb);
    EXPECT_NE(std::hash<WP<SThing>>{}(wa), std::hash<WP<SThing>>{}(wb));
    // …and a key still equals itself, so an entry stays findable after its object dies.
    EXPECT_TRUE(wa == WP<SThing>(wa));
    EXPECT_EQ(std::hash<WP<SThing>>{}(wa), std::hash<WP<SThing>>{}(WP<SThing>(wa)));
}

TEST(XRViewpointSurfaceKeying, AMapKeepsOneEntryPerSurfaceAcrossDestruction) {
    std::unordered_map<WP<SThing>, int> viewpoints;

    auto                                a = makeShared<SThing>(SThing{.id = 1});
    auto                                b = makeShared<SThing>(SThing{.id = 2});
    viewpoints.emplace(WP<SThing>(a), 10);
    viewpoints.emplace(WP<SThing>(b), 20);
    ASSERT_EQ(viewpoints.size(), 2u);

    // Both surfaces die; both viewpoints live on, as the protocol allows.
    a.reset();
    b.reset();
    EXPECT_EQ(viewpoints.size(), 2u);

    // A NEW surface is a new key, so get_viewpoint on it cannot collide with a dead entry and
    // wrongly report already_constructed.
    auto fresh = makeShared<SThing>(SThing{.id = 3});
    EXPECT_FALSE(viewpoints.contains(WP<SThing>(fresh)));
    viewpoints.emplace(WP<SThing>(fresh), 30);
    EXPECT_EQ(viewpoints.size(), 3u);

    // Erasing by VALUE (what destroyViewpoint does) never hashes, so an expired key is no obstacle.
    std::erase_if(viewpoints, [](const auto& entry) { return entry.second == 10 || entry.second == 20; });
    EXPECT_EQ(viewpoints.size(), 1u);
    EXPECT_TRUE(viewpoints.contains(WP<SThing>(fresh)));
}
