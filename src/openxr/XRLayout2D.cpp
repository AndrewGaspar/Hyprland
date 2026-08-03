#include "XRLayout2D.hpp"

#include <algorithm>
#include <cmath>

using namespace OpenXR;

namespace {
    constexpr float DEG = 57.29577951308232F; // 180 / pi

    int             iround(float v) {
        return (int)std::lround(v);
    }
}

const char* OpenXR::xrLayout2DVerticalName(eXRLayout2DVertical v) {
    return v == XR_L2D_VERT_WORLD_HEIGHT ? "world_height" : "elevation";
}

const char* OpenXR::xrLayout2DAttachName(eXRLayout2DAttach a) {
    return a == XR_L2D_ATTACH_AROUND ? "around" : "right";
}

std::optional<eXRLayout2DVertical> OpenXR::xrParseLayout2DVertical(const std::string& s) {
    if (s == "elevation")
        return XR_L2D_VERT_ELEVATION;
    if (s == "world_height")
        return XR_L2D_VERT_WORLD_HEIGHT;
    return std::nullopt;
}

std::optional<eXRLayout2DAttach> OpenXR::xrParseLayout2DAttach(const std::string& s) {
    if (s == "right")
        return XR_L2D_ATTACH_RIGHT;
    if (s == "around")
        return XR_L2D_ATTACH_AROUND;
    return std::nullopt;
}

float OpenXR::xrWrapDeg180(float deg) {
    if (!std::isfinite(deg))
        return 0.F;
    // fmod keeps the sign of the dividend, so this lands in (-360, 360) first and then folds.
    float d = std::fmod(deg, 360.F);
    if (d > 180.F)
        d -= 360.F;
    else if (d <= -180.F)
        d += 360.F;
    // A dividend of exactly -180 comes out of the fold as +180, which is the closed end — good.
    return d;
}

void OpenXR::xrLayout2DAngles(const SXRLayout2DInput& mon, const SXRLayout2DRef& ref, const SXRLayout2DConfig& cfg, float& azDegOut, float& elDegOut, float& vPxOut) {
    // A follow-frame (head/body) monitor's persistent offset is ALREADY expressed about the frame
    // origin — which is the eye for HEAD and the eye's XZ at body height for BODY — so it needs no
    // reference subtraction at all: its angle relative to you is constant by construction (§3b).
    // A world monitor is measured from the LATCHED reference eye and forward (§3a).
    const Vec3 d   = mon.followFrame ? mon.pose.pos : (mon.pose.pos - ref.eye);
    const float horiz = std::sqrt(d.x * d.x + d.z * d.z);

    float       az;
    if (horiz < 1e-4F)
        az = 0.F; // exactly at the eye, or exactly overhead/underfoot: azimuth is undefined
    else
        az = std::atan2(d.x, -d.z) * DEG; // right-positive: +X is to your right when facing -Z

    if (!mon.followFrame && ref.valid)
        az -= ref.yaw * DEG;
    azDegOut = xrWrapDeg180(az);

    const float len = std::sqrt(horiz * horiz + d.y * d.y);
    elDegOut        = len < 1e-4F ? 0.F : std::atan2(d.y, horiz) * DEG;

    // Layout y is DOWN-positive, so a monitor above eye level gets a negative vertical coordinate.
    vPxOut = cfg.vertical == XR_L2D_VERT_WORLD_HEIGHT ? -d.y * cfg.pxPerMeter : -elDegOut * cfg.pxPerDegree;
    if (!std::isfinite(vPxOut))
        vPxOut = 0.F;
}

std::vector<SXRLayout2DPrev> OpenXR::xrLayout2DPrevOf(const SXRLayout2DResult& r) {
    std::vector<SXRLayout2DPrev> out;
    out.reserve(r.slots.size());
    for (const auto& s : r.slots)
        out.push_back(SXRLayout2DPrev{s.name, s.azDeg, s.vPx});
    return out;
}

void OpenXR::xrLayout2DAttachOrigin(const std::vector<SXRLayout2DAnchorBox>& anchors, int blockW, int blockH, eXRLayout2DAttach attach, int& outX, int& outY) {
    outX = 0;
    outY = 0;
    if (attach == XR_L2D_ATTACH_AROUND || anchors.empty())
        return;

    int  maxR = 0, minT = 0, maxB = 0;
    bool first = true;
    for (const auto& a : anchors) {
        if (first) {
            maxR  = a.x + a.w;
            minT  = a.y;
            maxB  = a.y + a.h;
            first = false;
        } else {
            maxR = std::max(maxR, a.x + a.w);
            minT = std::min(minT, a.y);
            maxB = std::max(maxB, a.y + a.h);
        }
    }

    outX = maxR;                                    // flush right of the anchored block
    outY = minT + ((maxB - minT) - blockH) / 2;     // vertically centered on it
    (void)blockW;
}

SXRLayout2DResult OpenXR::xrProjectLayout2D(const std::vector<SXRLayout2DInput>& mons, const SXRLayout2DRef& ref, const SXRLayout2DConfig& cfg, const std::vector<SXRLayout2DPrev>& prev) {
    SXRLayout2DResult out;
    if (mons.empty())
        return out;

    const float PXDEG      = cfg.pxPerDegree > 0.F ? cfg.pxPerDegree : 1.F;
    const float ROW_MERGE  = std::max(0.F, cfg.rowMergeDeg) * PXDEG;
    const float HYST_DEG   = std::max(0.F, cfg.reorderHysteresisDeg);
    const float HYST_PX    = HYST_DEG * PXDEG;
    const int   MIN_OVERLAP = std::max(0, cfg.minOverlapPx);

    // ---- 1. unwrap + hysteresis (§2.2, §4) ----
    struct SEntry {
        size_t      idx  = 0;
        std::string name;
        float       az = 0.F, el = 0.F, v = 0.F;
        int         w = 1, h = 1;
    };
    std::vector<SEntry> ent;
    ent.reserve(mons.size());
    for (size_t i = 0; i < mons.size(); ++i) {
        SEntry e;
        e.idx  = i;
        e.name = mons[i].name;
        e.w    = std::max(1, mons[i].w);
        e.h    = std::max(1, mons[i].h);
        xrLayout2DAngles(mons[i], ref, cfg, e.az, e.el, e.v);

        // Hold the previously ADOPTED angles while the monitor has moved less than the margin. The
        // remembered value is the one that was actually used, so a monitor drifting by less than the
        // threshold per sync still adopts its true angle once the accumulated delta crosses it —
        // the hold cannot silently accumulate error (§4).
        for (const auto& p : prev) {
            if (p.name != e.name)
                continue;
            if (HYST_DEG > 0.F && std::fabs(xrWrapDeg180(e.az - p.azDeg)) < HYST_DEG)
                e.az = p.azDeg;
            if (HYST_PX > 0.F && std::fabs(e.v - p.vPx) < HYST_PX)
                e.v = p.vPx;
            break;
        }
        ent.push_back(e);
    }

    // ---- 2. rows / tiers (§2.4 step 1) ----
    // Sort top-to-bottom by the vertical coordinate; ties by azimuth then name so the result is
    // independent of the caller's monitor ordering.
    std::vector<size_t> order(ent.size());
    for (size_t i = 0; i < order.size(); ++i)
        order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        if (ent[a].v != ent[b].v)
            return ent[a].v < ent[b].v;
        if (ent[a].az != ent[b].az)
            return ent[a].az < ent[b].az;
        return ent[a].name < ent[b].name;
    });

    // Greedy tiering against the row's SEED (not a running mean): a seed cannot drift, so a long
    // ladder of monitors 1 px apart cannot chain into one arbitrarily tall row.
    std::vector<std::vector<size_t>> rows;
    for (size_t oi = 0; oi < order.size(); ++oi) {
        const size_t i = order[oi];
        if (rows.empty() || ent[i].v - ent[rows.back().front()].v > ROW_MERGE)
            rows.push_back({});
        rows.back().push_back(i);
    }

    // ---- 3. columns within a row (§2.4 step 2) ----
    for (auto& row : rows)
        std::sort(row.begin(), row.end(), [&](size_t a, size_t b) {
            if (ent[a].az != ent[b].az)
                return ent[a].az < ent[b].az;
            return ent[a].name < ent[b].name;
        });

    // ---- 4. lay each row out left->right, edge-touching, with a clamped vertical stagger ----
    // The stagger is what carries "up and to the right" into the 2D plane: a monitor floating higher
    // than its left neighbour gets a raised y, clamped so the two still share at least
    // minOverlapPx of vertical range — which is exactly what Hyprland's directional lookup needs
    // (STICKS on the x edges AND INTERSECTLEN > 0 on y), and what makes the cursor cross at the
    // height your eyes expect instead of sliding along a seam.
    struct SPlaced {
        size_t idx = 0;
        int    x = 0, y = 0;
    };
    std::vector<std::vector<SPlaced>> placed(rows.size());
    std::vector<int>                  rowWidth(rows.size(), 0);
    std::vector<int>                  rowHeight(rows.size(), 0);
    std::vector<float>                rowMeanAz(rows.size(), 0.F);

    for (size_t r = 0; r < rows.size(); ++r) {
        const auto& row     = rows[r];
        float       rowMinV = ent[row.front()].v;
        for (size_t i : row)
            rowMinV = std::min(rowMinV, ent[i].v);

        int   x       = 0;
        bool  hasPrev = false;
        int   prevY = 0, prevH = 0;
        float azSum = 0.F;

        for (size_t i : row) {
            const int h = ent[i].h;
            int       y = iround(ent[i].v - rowMinV);

            if (hasPrev) {
                const int need = std::min({MIN_OVERLAP, h, prevH});
                const int lo   = prevY + need - h;    // highest y that still overlaps by `need`
                const int hi   = prevY + prevH - need; // lowest  y that still overlaps by `need`
                y              = lo > hi ? prevY : std::clamp(y, lo, hi);
            }

            placed[r].push_back(SPlaced{i, x, y});
            x += ent[i].w;
            prevY   = y;
            prevH   = h;
            hasPrev = true;
            azSum += ent[i].az;
        }

        // Normalize the row so its topmost edge is 0, and record its extent.
        int minY = placed[r].front().y, maxB = placed[r].front().y + ent[placed[r].front().idx].h;
        for (const auto& p : placed[r]) {
            minY = std::min(minY, p.y);
            maxB = std::max(maxB, p.y + ent[p.idx].h);
        }
        for (auto& p : placed[r])
            p.y -= minY;

        rowWidth[r]  = x;
        rowHeight[r] = maxB - minY;
        rowMeanAz[r] = azSum / (float)row.size();
    }

    // ---- 5. stack rows top->bottom, aligned horizontally by angle (§2.4 step 3) ----
    std::vector<int> rowX(rows.size(), 0);
    std::vector<int> rowY(rows.size(), 0);
    int              yCursor = 0;
    for (size_t r = 0; r < rows.size(); ++r) {
        rowY[r] = yCursor;
        yCursor += rowHeight[r];

        // Put the row where its monitors' mean azimuth says it belongs...
        rowX[r] = iround(rowMeanAz[r] * PXDEG - (float)rowWidth[r] / 2.F);

        // ...then guarantee the shared horizontal seam actually has contact, so `movefocus u/d`
        // between the tiers resolves (INTERSECTLEN > 0) instead of silently doing nothing.
        if (r > 0) {
            const int need = std::min({MIN_OVERLAP, rowWidth[r], rowWidth[r - 1]});
            const int lo   = rowX[r - 1] + need - rowWidth[r];
            const int hi   = rowX[r - 1] + rowWidth[r - 1] - need;
            rowX[r]        = lo > hi ? rowX[r - 1] : std::clamp(rowX[r], lo, hi);
        }
    }

    // ---- 6. emit, normalized to a (0, 0) origin ----
    out.slots.resize(ent.size());
    int minX = 0, minY = 0;
    bool first = true;
    for (size_t r = 0; r < rows.size(); ++r)
        for (size_t c = 0; c < placed[r].size(); ++c) {
            const auto& p = placed[r][c];
            const int   X = rowX[r] + p.x;
            const int   Y = rowY[r] + p.y;
            if (first) {
                minX  = X;
                minY  = Y;
                first = false;
            } else {
                minX = std::min(minX, X);
                minY = std::min(minY, Y);
            }
        }

    int maxR = 0, maxB = 0;
    first = true;
    for (size_t r = 0; r < rows.size(); ++r)
        for (size_t c = 0; c < placed[r].size(); ++c) {
            const auto& p = placed[r][c];
            const auto& e = ent[p.idx];

            SXRLayout2DSlot s;
            s.name  = e.name;
            s.col   = (int)c;
            s.row   = (int)r;
            s.x     = rowX[r] + p.x - minX;
            s.y     = rowY[r] + p.y - minY;
            s.w     = e.w;
            s.h     = e.h;
            s.azDeg = e.az;
            s.elDeg = e.el;
            s.vPx   = e.v;

            out.slots[e.idx] = s; // indexed by the caller's original order

            if (first) {
                maxR  = s.x + s.w;
                maxB  = s.y + s.h;
                first = false;
            } else {
                maxR = std::max(maxR, s.x + s.w);
                maxB = std::max(maxB, s.y + s.h);
            }
        }

    out.width  = maxR;
    out.height = maxB;
    out.rows   = (int)rows.size();
    return out;
}
