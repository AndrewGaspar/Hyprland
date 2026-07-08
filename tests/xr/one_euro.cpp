#include <openxr/XRMath.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

using namespace OpenXR;

// tests/xr/one_euro.cpp — WP-G6 1€ filter (Casiez, Roussel & Vogel, CHI'12) pure-math coverage.
// The load-bearing test is ReferenceSeriesMatch: OpenXR::oneEuroStep is asserted output-for-output
// against a direct double-precision port of the reference implementation
// (github.com/casiez/OneEuroFilter, 1eurofilter.cc) driven by the identical timestamped series and
// the canonical parameters. Runs with no OpenXR runtime present.

namespace {
    constexpr double PI_D = 3.14159265358979323846;

    // ---- faithful port of github.com/casiez/OneEuroFilter (1eurofilter.cc), double precision ----
    // Transcribed verbatim except for dropped exception validation. This is the ORACLE the SUT
    // (oneEuroStep) must match; the constants below (1.0 min cutoff, 0.007 beta, 1.0 dcutoff) are
    // Casiez's published starting points and our config defaults.
    class RefLowPass {
      public:
        RefLowPass(double alpha, double initval = 0.0) : y(initval), s(initval), a(alpha), initialized(false) {}
        double filter(double value) {
            double result;
            if (initialized)
                result = a * value + (1.0 - a) * s;
            else {
                result      = value;
                initialized = true;
            }
            y = value;
            s = result;
            return result;
        }
        double filterWithAlpha(double value, double alpha) {
            a = alpha;
            return filter(value);
        }
        bool   hasLastRawValue() const {
            return initialized;
        }
        double lastRawValue() const {
            return y;
        }

      private:
        double y, s, a;
        bool   initialized;
    };

    class RefOneEuro {
      public:
        RefOneEuro(double freq, double mincutoff, double beta, double dcutoff) :
            m_freq(freq), m_mincutoff(mincutoff), m_beta(beta), m_dcutoff(dcutoff), m_x(alpha(mincutoff)), m_dx(alpha(dcutoff)), m_lasttime(UNDEF) {}

        // timestamp in SECONDS (matches how oneEuroStep is fed dt).
        double filter(double value, double timestamp) {
            if (m_lasttime != UNDEF && timestamp != UNDEF)
                m_freq = 1.0 / (timestamp - m_lasttime);
            m_lasttime      = timestamp;
            const double dvalue  = m_x.hasLastRawValue() ? (value - m_x.lastRawValue()) * m_freq : 0.0;
            const double edvalue = m_dx.filterWithAlpha(dvalue, alpha(m_dcutoff));
            const double cutoff  = m_mincutoff + m_beta * std::fabs(edvalue);
            return m_x.filterWithAlpha(value, alpha(cutoff));
        }

      private:
        static constexpr double UNDEF = -1.0;
        double                  alpha(double cutoff) {
            const double te  = 1.0 / m_freq;
            const double tau = 1.0 / (2.0 * PI_D * cutoff);
            return 1.0 / (1.0 + tau / te);
        }
        double     m_freq, m_mincutoff, m_beta, m_dcutoff;
        RefLowPass m_x, m_dx;
        double     m_lasttime;
    };

    // A deterministic noisy ramp: a slow drift plus a bounded pseudo-random jitter (no <random> so
    // the series is identical on every platform/run).
    std::vector<double> noisySeries(size_t n) {
        std::vector<double> v(n);
        uint32_t            state = 0x1234567u;
        for (size_t i = 0; i < n; ++i) {
            state             = state * 1664525u + 1013904223u; // LCG
            const double jit  = ((double)(state >> 8 & 0xffff) / 65535.0 - 0.5) * 0.02; // ±1 cm
            v[i]              = 0.5 + 0.3 * std::sin((double)i * 0.05) + jit;
        }
        return v;
    }
}

// ---- the load-bearing oracle match ----------------------------------------------------------

TEST(OneEuro, ReferenceSeriesMatch) {
    constexpr float  MINCUT = 1.0f, BETA = 0.007f, DCUT = 1.0f;
    constexpr double DT     = 1.0 / 90.0; // 90 Hz headset

    RefOneEuro ref(90.0, MINCUT, BETA, DCUT);
    SXROneEuro sut;

    const auto series = noisySeries(400);
    double     t      = 0.0;
    for (size_t i = 0; i < series.size(); ++i) {
        const double want = ref.filter(series[i], t);
        const float  got  = oneEuroStep(sut, (float)series[i], i == 0 ? 0.f : (float)DT, MINCUT, BETA, DCUT);
        // First step: both pass the raw value through. Thereafter float-vs-double drift stays tiny.
        EXPECT_NEAR(got, (float)want, 2e-4f) << "mismatch at step " << i;
        t += DT;
    }
}

// ---- hand-derived numeric anchors (beta = 0 => cutoff is constant at mincutoff) ---------------
// dt = 0.1 s, mincutoff = 1 Hz: alpha = 1/(1 + (1/(2*pi*1))/0.1) = 0.3858695. Feeding 0 then 1.0:
//   step0 (passthrough) = 0
//   step1 = alpha*1 + (1-alpha)*0                    = 0.3858695
//   step2 = alpha*1 + (1-alpha)*0.3858695            = 0.6228438

TEST(OneEuro, HandDerivedAnchors) {
    SXROneEuro s;
    EXPECT_FLOAT_EQ(oneEuroStep(s, 0.0f, 0.0f, 1.0f, 0.0f), 0.0f); // passthrough
    EXPECT_NEAR(oneEuroStep(s, 1.0f, 0.1f, 1.0f, 0.0f), 0.3858695f, 1e-5f);
    EXPECT_NEAR(oneEuroStep(s, 1.0f, 0.1f, 1.0f, 0.0f), 0.6228438f, 1e-5f);
}

// ---- first sample passes through exactly -----------------------------------------------------

TEST(OneEuro, FirstSamplePassthrough) {
    SXROneEuro s;
    EXPECT_FLOAT_EQ(oneEuroStep(s, 3.5f, 1.0f / 90.0f, 1.0f, 0.007f), 3.5f);
    EXPECT_TRUE(s.init);
    EXPECT_FLOAT_EQ(s.xHat, 3.5f);
    EXPECT_FLOAT_EQ(s.dxHat, 0.0f);
}

// ---- step response converges monotonically toward a held constant ----------------------------

TEST(OneEuro, MonotoneConvergenceToConstant) {
    SXROneEuro   s;
    const float  dt = 1.0f / 90.0f;
    oneEuroStep(s, 0.0f, 0.f, 1.0f, 0.007f); // seed at 0
    float prev = 0.f;
    for (int i = 0; i < 500; ++i) {
        const float cur = oneEuroStep(s, 1.0f, dt, 1.0f, 0.007f); // hold target = 1
        EXPECT_GE(cur, prev - 1e-6f); // never decreases (monotone climb)
        EXPECT_LE(cur, 1.0f + 1e-5f); // never overshoots the target
        prev = cur;
    }
    EXPECT_NEAR(prev, 1.0f, 1e-2f); // settles at the target
}

// ---- the filter actually reduces jitter around a held value ----------------------------------

TEST(OneEuro, ReducesJitter) {
    SXROneEuro  s;
    const float dt = 1.0f / 90.0f;
    oneEuroStep(s, 0.0f, 0.f, 1.0f, 0.007f);
    double rawVar = 0.0, filtVar = 0.0;
    int    count = 0;
    for (int i = 0; i < 400; ++i) {
        const float noise = (i % 2 == 0 ? 0.01f : -0.01f); // ±1 cm square jitter around 0
        const float got   = oneEuroStep(s, noise, dt, 1.0f, 0.007f);
        if (i > 50) { // let it settle
            rawVar += (double)noise * noise;
            filtVar += (double)got * got;
            ++count;
        }
    }
    ASSERT_GT(count, 0);
    EXPECT_LT(filtVar / count, rawVar / count * 0.25); // at least 4x variance reduction
}

// ---- dt robustness: non-positive dt holds; varied dt never produces NaN ----------------------

TEST(OneEuro, DtRobustness) {
    SXROneEuro s;
    oneEuroStep(s, 2.0f, 0.f, 1.0f, 0.007f);
    const float held = oneEuroStep(s, 5.0f, 0.0f, 1.0f, 0.007f); // dt == 0 -> hold last filtered
    EXPECT_FLOAT_EQ(held, 2.0f);
    const float heldNeg = oneEuroStep(s, 9.0f, -0.01f, 1.0f, 0.007f); // dt < 0 -> hold
    EXPECT_FLOAT_EQ(heldNeg, 2.0f);
    // A wildly varying but positive dt stays finite.
    for (int i = 0; i < 100; ++i) {
        const float dt  = (i % 3 == 0) ? 0.5f : (i % 3 == 1) ? 0.001f : 0.011f;
        const float got = oneEuroStep(s, (float)std::sin(i * 0.3), dt, 1.0f, 0.007f);
        EXPECT_TRUE(std::isfinite(got));
    }
}

// ---- reset re-arms passthrough ----------------------------------------------------------------

TEST(OneEuro, ResetClears) {
    SXROneEuro  s;
    const float dt = 1.0f / 90.0f;
    for (int i = 0; i < 20; ++i)
        oneEuroStep(s, 1.0f, i == 0 ? 0.f : dt, 1.0f, 0.007f);
    EXPECT_TRUE(s.init);
    s.reset();
    EXPECT_FALSE(s.init);
    EXPECT_FLOAT_EQ(oneEuroStep(s, 7.0f, dt, 1.0f, 0.007f), 7.0f); // first sample after reset = passthrough
}

// ---- pose filter: axis independence, quaternion stays unit, hemisphere flip is handled --------

TEST(OneEuro, PoseFilterMatchesPerAxis) {
    const float    dt = 1.0f / 90.0f;
    SXROneEuroPose fp;
    SXROneEuro     ax, ay, az;

    for (int i = 0; i < 100; ++i) {
        const float x  = 0.2f * std::sin(i * 0.07f);
        const float y  = 1.0f + 0.1f * std::cos(i * 0.05f);
        const float z  = -1.5f + 0.05f * std::sin(i * 0.09f);
        SXRPose     in{Vec3{x, y, z}, Quat{0, 0, 0, 1}};
        const float ddt = i == 0 ? 0.f : dt;
        const SXRPose out = oneEuroStepPose(fp, in, ddt, 1.0f, 0.007f);
        // Position axes must match standalone scalar filters exactly.
        EXPECT_FLOAT_EQ(out.pos.x, oneEuroStep(ax, x, ddt, 1.0f, 0.007f));
        EXPECT_FLOAT_EQ(out.pos.y, oneEuroStep(ay, y, ddt, 1.0f, 0.007f));
        EXPECT_FLOAT_EQ(out.pos.z, oneEuroStep(az, z, ddt, 1.0f, 0.007f));
        // Orientation stays a unit quaternion.
        const float n = std::sqrt(out.rot.x * out.rot.x + out.rot.y * out.rot.y + out.rot.z * out.rot.z + out.rot.w * out.rot.w);
        EXPECT_NEAR(n, 1.0f, 1e-4f);
    }
}

TEST(OneEuro, PoseFilterQuaternionHemisphere) {
    const float    dt = 1.0f / 90.0f;
    SXROneEuroPose fp;

    // Feed a steady rotation, then the ANTIPODAL representation (-q) of the same rotation. Without
    // hemisphere alignment the component-wise low-pass would average q and -q toward the origin and
    // qNormalize would yield a garbage/near-arbitrary quat. With alignment the filtered rotation
    // stays close to the intended one.
    const Quat q  = qFromYaw(0.6f);              // some rotation
    const Quat qn = Quat{-q.x, -q.y, -q.z, -q.w}; // same rotation, negated

    SXRPose out;
    for (int i = 0; i < 60; ++i)
        out = oneEuroStepPose(fp, SXRPose{Vec3{}, q}, i == 0 ? 0.f : dt, 1.0f, 0.007f);
    // Now flip to the antipode for many frames.
    for (int i = 0; i < 60; ++i)
        out = oneEuroStepPose(fp, SXRPose{Vec3{}, qn}, dt, 1.0f, 0.007f);

    // The filtered quaternion represents (near) the same rotation as q: |dot| ~ 1.
    const float d = std::fabs(out.rot.x * q.x + out.rot.y * q.y + out.rot.z * q.z + out.rot.w * q.w);
    EXPECT_NEAR(d, 1.0f, 1e-2f);
}
