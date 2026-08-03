#include <openxr/XRLayout2D.hpp>
#include <state/MonitorPositionController.hpp>
#include <config/shared/monitor/MonitorRule.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

using namespace OpenXR;

// tests/xr/layout2d_arrange.cpp — 2D-plane sync against the REAL arrange() (report 12 WP-S2).
//
// tests/xr/layout2d.cpp proves the projection emits a valid block. This file proves the half that
// bit us: what CMonitorPositionController::arrange() does with that block once it is handed to it as
// explicit offsets, alongside the physical monitors the user already has.
//
// The hazard, in one sentence: arrange() places every EXPLICIT-position monitor first and then
// appends the AUTO ones flush to the right of that block, so the moment the XR monitors carry an
// explicit offset, an auto physical monitor lands to the right of THEM — and if the attach seam were
// then measured from that monitor's new position, the next sync would shift the block again, and
// again, a block-width per event, forever. xrLayout2DAttachOrigin only ever sees explicitly
// positioned monitors precisely so the seam is a fixed point. `SeamIsAFixedPoint` runs the whole
// loop (project -> attach -> arrange -> re-measure -> repeat) and pins that.

namespace {
    // The same fake arrangeable tests/state/MonitorPositionController.cpp uses — arrange() only ever
    // touches this interface, so driving it here exercises the real placement code.
    class CFakeMonitor : public Monitor::IMonitorArrangeable {
      public:
        CFakeMonitor(MONITORID id, const std::string& name, const Vector2D& size) : m_id(id), m_name(name), m_size(size), m_transformedSize(size) {}

        MONITORID                   id() const override { return m_id; }
        std::string_view            name() const override { return m_name; }
        std::string_view            description() const override { return m_desc; }
        std::string_view            shortDescription() const override { return m_desc; }
        bool                        matchesStaticSelector(std::string_view sel) const override { return sel == m_name; }
        Vector2D                    position() const override { return m_position; }
        Vector2D                    size() const override { return m_size; }
        Vector2D                    pixelSize() const override { return m_size; }
        Vector2D                    transformedSize() const override { return m_transformedSize; }
        float                       scale() const override { return 1.F; }
        Hyprutils::Math::eTransform transform() const override { return Hyprutils::Math::HYPRUTILS_TRANSFORM_NORMAL; }
        CBox                        logicalBox() const override { return {m_position, m_size}; }
        CBox                        logicalBoxMinusReserved() const override { return logicalBox(); }
        Vector2D                    middle() const override { return m_position + m_size / 2.F; }
        void                        moveTo(const Vector2D& pos) override { m_position = pos; }
        std::optional<Vector2D>     explicitPosition() const override { return m_explicitPosition; }
        Config::eAutoDirs           autoDirection() const override { return Config::DIR_AUTO_NONE; }
        Vector2D                    xwaylandPosition() const override { return m_xwaylandPosition; }
        float                       xwaylandScale() const override { return 1.F; }
        void                        setXWaylandPosition(const Vector2D& p) override { m_xwaylandPosition = p; }
        void                        setXWaylandScale(float) override {}

        MONITORID               m_id = 0;
        std::string             m_name;
        std::string             m_desc;
        Vector2D                m_position;
        Vector2D                m_size;
        Vector2D                m_transformedSize;
        std::optional<Vector2D> m_explicitPosition;
        Vector2D                m_xwaylandPosition;
        bool                    m_isXR = false;
    };

    using PFake = SP<CFakeMonitor>;

    PFake mkMon(MONITORID id, const std::string& name, const Vector2D& size, bool isXR = false) {
        auto m   = makeShared<CFakeMonitor>(id, name, size);
        m->m_isXR = isXR;
        return m;
    }

    void runArrange(const std::vector<PFake>& mons) {
        std::vector<SP<Monitor::IMonitorArrangeable>> a;
        a.reserve(mons.size());
        for (const auto& m : mons)
            a.push_back(dynamicPointerCast<Monitor::IMonitorArrangeable>(m));
        State::CMonitorPositionController{}.arrange(a, false);
    }

    SXRLayout2DInput xrIn(const std::string& name, float x, float y, float z, int w, int h) {
        SXRLayout2DInput i;
        i.name     = name;
        i.pose.pos = Vec3{x, y, z};
        i.w        = w;
        i.h        = h;
        return i;
    }

    SXRLayout2DRef ref() {
        SXRLayout2DRef r;
        r.eye   = Vec3{0.f, 1.5f, 0.f};
        r.valid = true;
        return r;
    }

    // Exactly what COpenXRManager::syncLayout2D does: project, collect the anchors (explicitly
    // positioned monitors NOT being placed by this pass), attach, write the offsets. Returns the
    // block origin so a test can watch it for drift.
    Vector2D syncPass(const std::vector<PFake>& all, const std::vector<SXRLayout2DInput>& xrMons, const SXRLayout2DConfig& cfg, eXRLayout2DAttach attach,
                      std::vector<SXRLayout2DPrev>& prev) {
        const auto res = xrProjectLayout2D(xrMons, ref(), cfg, prev);
        prev           = xrLayout2DPrevOf(res);

        std::vector<SXRLayout2DAnchorBox> anchors;
        for (const auto& m : all) {
            if (m->m_isXR)
                continue;
            if (!m->explicitPosition().has_value())
                continue;
            anchors.push_back(SXRLayout2DAnchorBox{(int)m->m_position.x, (int)m->m_position.y, (int)m->m_size.x, (int)m->m_size.y});
        }

        int ox = 0, oy = 0;
        xrLayout2DAttachOrigin(anchors, res.width, res.height, attach, ox, oy);

        for (size_t i = 0; i < xrMons.size(); ++i) {
            const auto& slot = res.slots[i];
            for (const auto& m : all)
                if (m->m_name == slot.name)
                    m->m_explicitPosition = Vector2D{(double)(ox + slot.x), (double)(oy + slot.y)};
        }
        return Vector2D{(double)ox, (double)oy};
    }

    const PFake& byName(const std::vector<PFake>& mons, const std::string& n) {
        static PFake null;
        for (const auto& m : mons)
            if (m->m_name == n)
                return m;
        return null;
    }
}

// THE regression test for the seam: run project -> attach -> arrange -> re-measure five times over.
// The block origin, every monitor position, and the physical monitor's own position must all be
// identical from the second pass onwards. A seam measured from an auto monitor marches right by a
// block-width per pass and fails this on iteration 2.
TEST(Layout2DArrange, SeamIsAFixedPoint) {
    SXRLayout2DConfig cfg;

    // A physical monitor with an AUTO position — the case that drifts if the seam is measured wrong.
    const auto phys = mkMon(0, "DP-1", {1920, 1080});
    const auto a    = mkMon(1, "XR-a", {1920, 1080}, true);
    const auto b    = mkMon(2, "XR-b", {1920, 1080}, true);
    const std::vector<PFake> all{phys, a, b};

    const std::vector<SXRLayout2DInput> xrMons{
        xrIn("XR-a", -1.2f, 1.5f, -1.5f, 1920, 1080),
        xrIn("XR-b", 1.2f, 1.5f, -1.5f, 1920, 1080),
    };

    std::vector<SXRLayout2DPrev> prev;
    Vector2D                     firstOrigin;
    Vector2D                     firstPhys, firstA, firstB;

    for (int pass = 0; pass < 5; ++pass) {
        const Vector2D origin = syncPass(all, xrMons, cfg, XR_L2D_ATTACH_RIGHT, prev);
        runArrange(all);

        if (pass == 0) {
            firstOrigin = origin;
            firstPhys   = phys->m_position;
            firstA      = a->m_position;
            firstB      = b->m_position;
            continue;
        }
        EXPECT_EQ(origin, firstOrigin) << "pass " << pass << ": the attach seam moved";
        EXPECT_EQ(phys->m_position, firstPhys) << "pass " << pass << ": the physical monitor drifted";
        EXPECT_EQ(a->m_position, firstA) << "pass " << pass;
        EXPECT_EQ(b->m_position, firstB) << "pass " << pass;
    }

    // And the layout it settled on is a usable one: XR-a left of XR-b, flush, and the auto physical
    // monitor appended to the right of the block with no gap.
    EXPECT_EQ(a->m_position.x + a->m_size.x, b->m_position.x);
    EXPECT_EQ(b->m_position.x + b->m_size.x, phys->m_position.x);
}

// An EXPLICITLY positioned physical monitor is the other half: it must keep its exact configured
// position, and the XR block must land flush to its right (attach = right, the documented default).
TEST(Layout2DArrange, ExplicitPhysicalMonitorKeepsItsPositionAndSeamsToTheBlock) {
    SXRLayout2DConfig cfg;

    const auto phys = mkMon(0, "DP-1", {2560, 1440});
    phys->m_explicitPosition = Vector2D{0, 0};
    const auto a = mkMon(1, "XR-a", {1920, 1080}, true);
    const auto b = mkMon(2, "XR-b", {1920, 1080}, true);
    const std::vector<PFake> all{phys, a, b};

    const std::vector<SXRLayout2DInput> xrMons{
        xrIn("XR-a", -1.2f, 1.5f, -1.5f, 1920, 1080),
        xrIn("XR-b", 1.2f, 1.5f, -1.5f, 1920, 1080),
    };

    std::vector<SXRLayout2DPrev> prev;
    for (int pass = 0; pass < 3; ++pass) {
        syncPass(all, xrMons, cfg, XR_L2D_ATTACH_RIGHT, prev);
        runArrange(all);
        EXPECT_EQ(phys->m_position, (Vector2D{0, 0})) << "pass " << pass << ": the user's explicit monitor= position must be untouched";
    }

    EXPECT_EQ(a->m_position.x, 2560) << "the XR block seams to the right edge of the physical block";
    EXPECT_EQ(a->m_position.x + a->m_size.x, b->m_position.x);
    // Vertically centered on the physical block: (1440 - 1080) / 2.
    EXPECT_EQ(a->m_position.y, 180);
}

// A PINNED XR monitor (an explicit user monitor= offset) is not placed by the projection, keeps its
// position exactly, and the auto-placed block is attached clear of it — no overlap.
TEST(Layout2DArrange, PinnedXRMonitorIsRespectedAndNotOverlapped) {
    SXRLayout2DConfig cfg;

    const auto pin = mkMon(0, "XR-pin", {1280, 720}); // NOT flagged m_isXR: it is an anchor, not ours
    pin->m_explicitPosition = Vector2D{0, 0};
    const auto a = mkMon(1, "XR-a", {1920, 1080}, true);
    const auto b = mkMon(2, "XR-b", {1920, 1080}, true);
    const std::vector<PFake> all{pin, a, b};

    const std::vector<SXRLayout2DInput> xrMons{
        xrIn("XR-a", -1.2f, 1.5f, -1.5f, 1920, 1080),
        xrIn("XR-b", 1.2f, 1.5f, -1.5f, 1920, 1080),
    };

    std::vector<SXRLayout2DPrev> prev;
    syncPass(all, xrMons, cfg, XR_L2D_ATTACH_RIGHT, prev);
    runArrange(all);

    EXPECT_EQ(pin->m_position, (Vector2D{0, 0}));
    for (const auto& m : {a, b}) {
        const bool overlapsX = m->m_position.x < 1280 && m->m_position.x + m->m_size.x > 0;
        const bool overlapsY = m->m_position.y < 720 && m->m_position.y + m->m_size.y > 0;
        EXPECT_FALSE(overlapsX && overlapsY) << m->m_name << " overlaps the pinned monitor";
    }
}

// A headset-only session (no other monitor at all): the XR block IS the layout, at the origin.
TEST(Layout2DArrange, HeadsetOnlySessionPutsTheBlockAtTheOrigin) {
    SXRLayout2DConfig cfg;

    const auto a = mkMon(0, "XR-a", {1920, 1080}, true);
    const auto b = mkMon(1, "XR-b", {1920, 1080}, true);
    const std::vector<PFake> all{a, b};

    const std::vector<SXRLayout2DInput> xrMons{
        xrIn("XR-a", -1.2f, 1.5f, -1.5f, 1920, 1080),
        xrIn("XR-b", 1.2f, 1.5f, -1.5f, 1920, 1080),
    };

    std::vector<SXRLayout2DPrev> prev;
    syncPass(all, xrMons, cfg, XR_L2D_ATTACH_RIGHT, prev); // no anchors -> same result as `around`
    runArrange(all);

    EXPECT_EQ(a->m_position, (Vector2D{0, 0}));
    EXPECT_EQ(b->m_position, (Vector2D{1920, 0}));
}

// `around` ignores the anchors outright, which is what makes it the escape hatch when a user does
// not want the XR block chained to the physical block.
TEST(Layout2DArrange, AttachAroundIgnoresAnchors) {
    std::vector<SXRLayout2DAnchorBox> anchors{{0, 0, 2560, 1440}, {2560, 0, 1920, 1080}};
    int                               x = -1, y = -1;

    xrLayout2DAttachOrigin(anchors, 3840, 1080, XR_L2D_ATTACH_AROUND, x, y);
    EXPECT_EQ(x, 0);
    EXPECT_EQ(y, 0);

    xrLayout2DAttachOrigin(anchors, 3840, 1080, XR_L2D_ATTACH_RIGHT, x, y);
    EXPECT_EQ(x, 4480) << "flush right of the anchored bounding box";
    EXPECT_EQ(y, 180) << "vertically centered on it";

    xrLayout2DAttachOrigin({}, 3840, 1080, XR_L2D_ATTACH_RIGHT, x, y);
    EXPECT_EQ(x, 0) << "nothing anchored -> the block is the layout";
    EXPECT_EQ(y, 0);
}
