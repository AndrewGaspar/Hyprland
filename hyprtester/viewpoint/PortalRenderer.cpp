#include "PortalRenderer.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <functional>
#include <limits>
#include <mutex>
#include <numeric>
#include <thread>
#include <vector>

#include <hyprutils/memory/Casts.hpp>

using namespace Hyprutils::Memory;

struct SHit {
    double   distance = std::numeric_limits<double>::infinity();
    uint32_t color    = 0;
    // Distance attenuation coefficient for the winning hit. Shading is deferred to
    // the end of trace() so occluded candidates never pay for it; a negative value
    // means the surface is emissive and keeps its base color.
    double attenuation = -1.0;
};

namespace {

    // Persistent workers for the two pixel loops. Rows are claimed from one shared
    // cursor, every row is written by exactly one worker, and nothing accumulates
    // across rows — so the rendered bytes are independent of the schedule and of the
    // worker count. The pool only ever grows and is joined at process exit.
    class CRowPool {
      public:
        static CRowPool& instance() {
            static CRowPool POOL;
            return POOL;
        }

        CRowPool(const CRowPool&)            = delete;
        CRowPool(CRowPool&&)                 = delete;
        CRowPool& operator=(const CRowPool&) = delete;
        CRowPool& operator=(CRowPool&&)      = delete;

        ~CRowPool() {
            {
                const std::lock_guard LOCK(m_mutex);
                m_quit = true;
                ++m_generation;
            }
            m_wake.notify_all();
            for (auto& thread : m_threads) {
                if (thread.joinable())
                    thread.join();
            }
        }

        // `workers` counts the caller, which participates in the same row cursor.
        void run(uint32_t rows, uint32_t workers, const std::function<void(uint32_t)>& row) {
            std::unique_lock lock(m_mutex);
            // The generation is captured before the bump below, so a thread spawned
            // here still observes this job instead of sleeping through it.
            while (m_threads.size() + 1 < workers)
                m_threads.emplace_back([this, index = m_threads.size(), seen = m_generation] { work(index, seen); });

            m_row  = &row;
            m_rows = rows;
            m_cursor.store(0, std::memory_order_relaxed);
            m_participants = sc<uint32_t>(std::min(sc<size_t>(workers - 1U), m_threads.size()));
            m_outstanding  = m_participants;
            ++m_generation;
            lock.unlock();
            m_wake.notify_all();

            drain();

            lock.lock();
            m_done.wait(lock, [this] { return m_outstanding == 0; });
            m_row = nullptr;
        }

      private:
        CRowPool() = default;

        void drain() {
            for (uint32_t row = m_cursor.fetch_add(1, std::memory_order_relaxed); row < m_rows; row = m_cursor.fetch_add(1, std::memory_order_relaxed))
                (*m_row)(row);
        }

        void work(size_t index, uint64_t seen) {
            std::unique_lock lock(m_mutex);
            while (true) {
                m_wake.wait(lock, [this, seen] { return m_generation != seen; });
                seen = m_generation;
                if (m_quit)
                    return;
                if (index >= m_participants)
                    continue;

                lock.unlock();
                drain();
                lock.lock();
                if (--m_outstanding == 0)
                    m_done.notify_one();
            }
        }

        std::mutex                           m_mutex;
        std::condition_variable              m_wake;
        std::condition_variable              m_done;
        std::vector<std::thread>             m_threads;
        const std::function<void(uint32_t)>* m_row = nullptr;
        std::atomic<uint32_t>                m_cursor{0};
        uint32_t                             m_rows         = 0;
        uint32_t                             m_participants = 0;
        uint32_t                             m_outstanding  = 0;
        uint64_t                             m_generation   = 0;
        bool                                 m_quit         = false;
    };

    void renderRows(uint32_t rows, uint32_t threads, const std::function<void(uint32_t)>& row) {
        if (rows == 0)
            return;

        const uint32_t WORKERS = std::min(std::max(threads, 1U), rows);
        if (WORKERS < 2) {
            for (uint32_t y = 0; y < rows; ++y)
                row(y);
            return;
        }

        CRowPool::instance().run(rows, WORKERS, row);
    }

}

static bool finiteVec(const ViewpointDemo::SVec3& vec) {
    return std::isfinite(vec.x) && std::isfinite(vec.y) && std::isfinite(vec.z);
}

static bool imageValid(const ViewpointDemo::SImage& image) {
    if (image.width < 2 || (image.width & 1U) != 0 || image.height == 0 || image.stridePixels < image.width)
        return false;

    const size_t ROWS = image.height - 1;
    if (ROWS > (std::numeric_limits<size_t>::max() - image.width) / image.stridePixels)
        return false;

    return image.pixels.size() >= ROWS * image.stridePixels + image.width;
}

static uint32_t rgb(uint8_t red, uint8_t green, uint8_t blue) {
    return 0xFF000000U | (sc<uint32_t>(red) << 16U) | (sc<uint32_t>(green) << 8U) | sc<uint32_t>(blue);
}

static uint32_t shade(uint32_t color, double factor) {
    factor              = std::clamp(factor, 0.0, 1.0);
    const uint8_t RED   = sc<uint8_t>(std::round(sc<double>((color >> 16U) & 0xFFU) * factor));
    const uint8_t GREEN = sc<uint8_t>(std::round(sc<double>((color >> 8U) & 0xFFU) * factor));
    const uint8_t BLUE  = sc<uint8_t>(std::round(sc<double>(color & 0xFFU) * factor));
    return rgb(RED, GREEN, BLUE);
}

static ViewpointDemo::SVec3 rayPoint(const ViewpointDemo::SRay& ray, double distance) {
    return {
        .x = ray.origin.x + ray.direction.x * distance,
        .y = ray.origin.y + ray.direction.y * distance,
        .z = ray.origin.z + ray.direction.z * distance,
    };
}

static bool inRange(double value, double low, double high) {
    return value >= low && value <= high;
}

static bool gridLine(double coordinate, double spacing, double width) {
    const double CELL = coordinate / spacing;
    return std::abs(CELL - std::round(CELL)) < width;
}

static void considerRoomPlane(const ViewpointDemo::SRay& ray, double numerator, double denominator, uint32_t plane, SHit& hit) {
    constexpr double MIN_DISTANCE = 1.000001;
    if (std::abs(denominator) < 1e-12)
        return;

    const double DISTANCE = numerator / denominator;
    if (DISTANCE <= MIN_DISTANCE || DISTANCE >= hit.distance)
        return;

    // The bounds test precedes the grid-line color so a miss never pays for it.
    const auto POINT = rayPoint(ray, DISTANCE);
    uint32_t   color = 0;

    switch (plane) {
        case 0: // back wall, z = -5
            if (!inRange(POINT.x, -2.8, 2.8) || !inRange(POINT.y, -1.7, 1.7))
                return;
            color = gridLine(POINT.x, 0.5, 0.035) || gridLine(POINT.y, 0.5, 0.035) ? rgb(91, 130, 151) : rgb(35, 55, 67);
            break;
        case 1: // floor / ceiling
            if (!inRange(POINT.x, -2.8, 2.8) || !inRange(POINT.z, -5.0, 0.0))
                return;
            color = gridLine(POINT.x, 0.5, 0.035) || gridLine(POINT.z, 0.5, 0.035) ? rgb(98, 92, 82) : rgb(42, 39, 37);
            break;
        case 2: // side walls
            if (!inRange(POINT.y, -1.7, 1.7) || !inRange(POINT.z, -5.0, 0.0))
                return;
            color = gridLine(POINT.y, 0.5, 0.035) || gridLine(POINT.z, 0.5, 0.035) ? rgb(84, 104, 111) : rgb(34, 45, 49);
            break;
        default: return;
    }

    hit.distance    = DISTANCE;
    hit.color       = color;
    hit.attenuation = 0.035;
}

// One slab of the box test. Kept as an explicit per-axis call so the arithmetic is
// the same sequence the indexed loop used to run, without the array staging.
static bool clipBoxAxis(double origin, double direction, double low, double high, double& entry, double& exit) {
    if (std::abs(direction) < 1e-12)
        return inRange(origin, low, high);

    double nearDistance = (low - origin) / direction;
    double farDistance  = (high - origin) / direction;
    if (nearDistance > farDistance)
        std::swap(nearDistance, farDistance);
    entry = std::max(entry, nearDistance);
    exit  = std::min(exit, farDistance);
    return entry <= exit;
}

static bool intersectBox(const ViewpointDemo::SRay& ray, const ViewpointDemo::SVec3& low, const ViewpointDemo::SVec3& high, double& outDistance) {
    constexpr double MIN_DISTANCE = 1.000001;
    double           entry        = MIN_DISTANCE;
    double           exit         = std::numeric_limits<double>::infinity();

    if (!clipBoxAxis(ray.origin.x, ray.direction.x, low.x, high.x, entry, exit) || !clipBoxAxis(ray.origin.y, ray.direction.y, low.y, high.y, entry, exit) ||
        !clipBoxAxis(ray.origin.z, ray.direction.z, low.z, high.z, entry, exit))
        return false;

    outDistance = entry;
    return std::isfinite(entry);
}

static void considerBox(const ViewpointDemo::SRay& ray, const ViewpointDemo::SVec3& low, const ViewpointDemo::SVec3& high, uint32_t color, SHit& hit) {
    double distance = 0.0;
    if (!intersectBox(ray, low, high, distance) || distance >= hit.distance)
        return;

    hit.distance    = distance;
    hit.color       = color;
    hit.attenuation = 0.045;
}

static void considerAimMarker(const ViewpointDemo::SRay& ray, SHit& hit) {
    // This point is authoritative world state: its location never depends on the
    // viewer. Head translation changes only where its projection lands.
    const auto       CENTER = ViewpointDemo::authoritativeAimImpact();
    constexpr double RADIUS = 0.12;

    const auto       OFFSET = ViewpointDemo::SVec3{.x = ray.origin.x - CENTER.x, .y = ray.origin.y - CENTER.y, .z = ray.origin.z - CENTER.z};
    const double     A      = ray.direction.x * ray.direction.x + ray.direction.y * ray.direction.y + ray.direction.z * ray.direction.z;
    const double     B      = 2.0 * (OFFSET.x * ray.direction.x + OFFSET.y * ray.direction.y + OFFSET.z * ray.direction.z);
    const double     C      = OFFSET.x * OFFSET.x + OFFSET.y * OFFSET.y + OFFSET.z * OFFSET.z - RADIUS * RADIUS;
    const double     DISC   = B * B - 4.0 * A * C;
    if (DISC < 0.0 || A <= 0.0)
        return;

    const double DISTANCE = (-B - std::sqrt(DISC)) / (2.0 * A);
    if (DISTANCE <= 1.000001 || DISTANCE >= hit.distance)
        return;

    hit.distance    = DISTANCE;
    hit.color       = rgb(255, 76, 57);
    hit.attenuation = -1.0;
}

static uint32_t trace(const ViewpointDemo::SRay& ray) {
    SHit hit;

    considerRoomPlane(ray, -5.0 - ray.origin.z, ray.direction.z, 0, hit);
    considerRoomPlane(ray, -1.7 - ray.origin.y, ray.direction.y, 1, hit);
    considerRoomPlane(ray, 1.7 - ray.origin.y, ray.direction.y, 1, hit);
    considerRoomPlane(ray, -2.8 - ray.origin.x, ray.direction.x, 2, hit);
    considerRoomPlane(ray, 2.8 - ray.origin.x, ray.direction.x, 2, hit);

    considerBox(ray, {.x = -1.15, .y = -1.45, .z = -1.25}, {.x = -0.55, .y = -0.65, .z = -0.65}, rgb(213, 135, 64), hit);
    considerBox(ray, {.x = 0.38, .y = -1.35, .z = -2.75}, {.x = 1.15, .y = -0.35, .z = -1.95}, rgb(56, 164, 139), hit);
    considerBox(ray, {.x = -0.48, .y = -0.55, .z = -4.35}, {.x = 0.35, .y = 0.45, .z = -3.8}, rgb(110, 99, 196), hit);
    considerAimMarker(ray, hit);

    if (!std::isfinite(hit.distance))
        return rgb(10, 14, 18);
    // Identical arithmetic to shading at hit time, evaluated once for the survivor.
    return hit.attenuation < 0.0 ? hit.color : shade(hit.color, 1.0 / (1.0 + hit.distance * hit.attenuation));
}

static uint32_t fallbackPixel(uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    const double NX      = (sc<double>(x) + 0.5) / sc<double>(width);
    const double NY      = (sc<double>(y) + 0.5) / sc<double>(height);
    const bool   BORDER  = x < 3 || y < 3 || x + 3 >= width || y + 3 >= height;
    const bool   CROSS   = std::abs(NX - 0.5) < 0.004 || std::abs(NY - 0.5) < 0.006;
    const bool   QUARTER = std::abs(NX - 0.25) < 0.002 || std::abs(NX - 0.75) < 0.002 || std::abs(NY - 0.25) < 0.003 || std::abs(NY - 0.75) < 0.003;
    if (BORDER)
        return rgb(48, 144, 168);
    if (CROSS)
        return rgb(195, 218, 220);
    if (QUARTER)
        return rgb(48, 87, 100);

    const uint32_t CELL = (x * 8U / width + y * 8U / height) & 1U;
    return CELL ? rgb(18, 28, 34) : rgb(13, 21, 27);
}

bool ViewpointDemo::offAxisFrustum(const SVec3& eye, const SPortalSize& portal, double nearPlane, SFrustum& out) {
    out = {};
    if (!finiteVec(eye) || !std::isfinite(portal.widthMeters) || !std::isfinite(portal.heightMeters) || !std::isfinite(nearPlane) || portal.widthMeters <= 0.0 ||
        portal.heightMeters <= 0.0 || eye.z <= 1e-6 || nearPlane <= 0.0)
        return false;

    out.left   = nearPlane * (-portal.widthMeters * 0.5 - eye.x) / eye.z;
    out.right  = nearPlane * (portal.widthMeters * 0.5 - eye.x) / eye.z;
    out.bottom = nearPlane * (-portal.heightMeters * 0.5 - eye.y) / eye.z;
    out.top    = nearPlane * (portal.heightMeters * 0.5 - eye.y) / eye.z;
    out.near   = nearPlane;
    return std::isfinite(out.left) && std::isfinite(out.right) && std::isfinite(out.bottom) && std::isfinite(out.top);
}

ViewpointDemo::SVec3 ViewpointDemo::authoritativeAimImpact() {
    return {.x = 0.0, .y = 0.0, .z = -3.65};
}

bool ViewpointDemo::fitSBSRenderSize(uint32_t packedDestinationWidth, uint32_t packedDestinationHeight, uint32_t maximumEyeWidth, uint32_t maximumHeight, SRenderSize& out) {
    out = {};
    if (packedDestinationWidth == 0 || (packedDestinationWidth & 1U) != 0 || packedDestinationHeight == 0 || maximumEyeWidth == 0 || maximumHeight == 0)
        return false;

    const uint32_t EYE_DESTINATION_WIDTH = packedDestinationWidth / 2U;
    const uint32_t DIVISOR               = std::gcd(EYE_DESTINATION_WIDTH, packedDestinationHeight);
    const uint32_t ASPECT_WIDTH          = EYE_DESTINATION_WIDTH / DIVISOR;
    const uint32_t ASPECT_HEIGHT         = packedDestinationHeight / DIVISOR;
    const uint32_t SCALE                 = std::min(maximumEyeWidth / ASPECT_WIDTH, maximumHeight / ASPECT_HEIGHT);
    if (SCALE == 0)
        return false;

    out = {.width = ASPECT_WIDTH * SCALE, .height = ASPECT_HEIGHT * SCALE};
    return true;
}

bool ViewpointDemo::feedbackShouldBeEnabled(const SFeedbackState& state) {
    return state.capabilitiesSupported && state.mappingSupported && !state.stickyDisabled;
}

bool ViewpointDemo::portalRay(const SVec3& eye, const SPortalSize& portal, uint32_t pixelX, uint32_t pixelY, uint32_t paneWidth, uint32_t paneHeight, SRay& out) {
    out = {};
    if (!finiteVec(eye) || !std::isfinite(portal.widthMeters) || !std::isfinite(portal.heightMeters) || portal.widthMeters <= 0.0 || portal.heightMeters <= 0.0 || eye.z <= 1e-6 ||
        paneWidth == 0 || paneHeight == 0 || pixelX >= paneWidth || pixelY >= paneHeight)
        return false;

    const double SURFACE_X = ((sc<double>(pixelX) + 0.5) / sc<double>(paneWidth) - 0.5) * portal.widthMeters;
    const double SURFACE_Y = (0.5 - (sc<double>(pixelY) + 0.5) / sc<double>(paneHeight)) * portal.heightMeters;
    out.origin             = eye;
    out.direction          = {.x = SURFACE_X - eye.x, .y = SURFACE_Y - eye.y, .z = -eye.z};
    return true;
}

uint32_t ViewpointDemo::defaultRenderThreads() {
    const uint32_t AVAILABLE = std::thread::hardware_concurrency();
    return AVAILABLE == 0 ? 1U : std::min(AVAILABLE, MAX_AUTO_RENDER_THREADS);
}

bool ViewpointDemo::renderPortalSBS(const SImage& image, const SPortalSize& portal, const SStereoViews& views, uint32_t threads) {
    if (!imageValid(image) || !std::isfinite(portal.widthMeters) || !std::isfinite(portal.heightMeters) || portal.widthMeters <= 0.0 || portal.heightMeters <= 0.0 ||
        !finiteVec(views.left) || !finiteVec(views.right) || views.left.z <= 1e-6 || views.right.z <= 1e-6)
        return false;

    const uint32_t PANE_WIDTH = image.width / 2U;
    const uint32_t RIGHT_X    = image.width - PANE_WIDTH;
    std::fill(image.pixels.begin(), image.pixels.end(), rgb(5, 8, 11));

    // Pixel-center portal-plane coordinates, evaluated with exactly the expressions
    // portalRay() uses. imageValid() plus the guard above already establish every
    // portalRay() precondition for the whole pane, so it cannot fail per pixel; the
    // tables just move its two divisions out of the hot loop bit-for-bit unchanged.
    std::vector<double> surfaceX(PANE_WIDTH);
    for (uint32_t x = 0; x < PANE_WIDTH; ++x)
        surfaceX[x] = ((sc<double>(x) + 0.5) / sc<double>(PANE_WIDTH) - 0.5) * portal.widthMeters;

    std::vector<double> surfaceY(image.height);
    for (uint32_t y = 0; y < image.height; ++y)
        surfaceY[y] = (0.5 - (sc<double>(y) + 0.5) / sc<double>(image.height)) * portal.heightMeters;

    const std::array<SVec3, 2>    EYES     = {views.left, views.right};
    const std::array<uint32_t, 2> BASE     = {0U, RIGHT_X};
    const uint32_t                CENTER_X = PANE_WIDTH / 2U;
    const uint32_t                CENTER_Y = image.height / 2U;

    // One row of the packed frame, both panes. Rows never interact.
    renderRows(image.height, threads, [&](uint32_t y) {
        // A portal-locked center reticle makes the invariant visible: the cyan mark
        // stays fixed while the red authoritative world impact moves under viewer
        // translation. trace() is pure, so reticle pixels can skip it outright.
        const bool NARROW_Y = y + 1U >= CENTER_Y && y <= CENTER_Y + 1U;
        const bool CROSS_Y  = y + 8U >= CENTER_Y && y <= CENTER_Y + 8U;

        for (size_t eyeIndex = 0; eyeIndex < EYES.size(); ++eyeIndex) {
            const SVec3& EYE = EYES[eyeIndex];
            uint32_t*    row = image.pixels.data() + sc<size_t>(y) * image.stridePixels + BASE[eyeIndex];

            SRay         ray;
            ray.origin      = EYE;
            ray.direction.y = surfaceY[y] - EYE.y;
            ray.direction.z = -EYE.z;

            for (uint32_t x = 0; x < PANE_WIDTH; ++x) {
                if ((NARROW_Y && x + 8U >= CENTER_X && x <= CENTER_X + 8U) || (CROSS_Y && x + 1U >= CENTER_X && x <= CENTER_X + 1U)) {
                    row[x] = rgb(71, 225, 231);
                    continue;
                }

                ray.direction.x = surfaceX[x] - EYE.x;
                row[x]          = trace(ray);
            }
        }
    });

    return true;
}

bool ViewpointDemo::renderFallbackSBS(const SImage& image, uint32_t threads) {
    if (!imageValid(image))
        return false;

    const uint32_t PANE_WIDTH = image.width / 2U;
    const uint32_t RIGHT_X    = image.width - PANE_WIDTH;
    std::fill(image.pixels.begin(), image.pixels.end(), rgb(5, 8, 11));

    renderRows(image.height, threads, [&](uint32_t y) {
        uint32_t* row = image.pixels.data() + sc<size_t>(y) * image.stridePixels;
        for (uint32_t x = 0; x < PANE_WIDTH; ++x) {
            const uint32_t COLOR = fallbackPixel(x, y, PANE_WIDTH, image.height);
            row[x]               = COLOR;
            row[RIGHT_X + x]     = COLOR;
        }
    });

    return true;
}

uint64_t ViewpointDemo::pixelHash(const SImage& image) {
    if (!imageValid(image))
        return 0;

    uint64_t hash = 1469598103934665603ULL;
    for (uint32_t y = 0; y < image.height; ++y) {
        for (uint32_t x = 0; x < image.width; ++x) {
            hash ^= image.pixels[sc<size_t>(y) * image.stridePixels + x];
            hash *= 1099511628211ULL;
        }
    }
    return hash;
}
