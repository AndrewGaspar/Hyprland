#include "XRSurfaces.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <sstream>

#include <hyprutils/memory/Casts.hpp>

using Hyprutils::Memory::sc;

namespace OpenXR {
    static constexpr float PI_F = 3.14159265F;

    const char*            xrPlaneLabelName(eXRPlaneLabel l) {
        switch (l) {
            case XR_PLANE_LABEL_FLOOR: return "floor";
            case XR_PLANE_LABEL_WALL: return "wall";
            case XR_PLANE_LABEL_CEILING: return "ceiling";
            case XR_PLANE_LABEL_TABLE: return "table";
            case XR_PLANE_LABEL_PLATFORM: return "platform";
            default: return "unknown";
        }
    }

    const char* xrPlaneOrientName(eXRPlaneOrient o) {
        switch (o) {
            case XR_PLANE_UP: return "up";
            case XR_PLANE_DOWN: return "down";
            case XR_PLANE_VERTICAL: return "vertical";
            default: return "arbitrary";
        }
    }

    const char* xrPlaneSourceName(eXRPlaneSource s) {
        return s == XR_PLANE_SRC_DETECTION ? "detection" : "synthetic";
    }

    const char* xrSnapKindName(eXRSnapKind k) {
        switch (k) {
            case XR_SNAP_STAND: return "stand";
            case XR_SNAP_FLAT: return "flat";
            case XR_SNAP_WALL: return "wall";
            default: return "none";
        }
    }

    eXRPlaneLabel xrParsePlaneLabel(const std::string& s, bool& ok) {
        ok = true;
        if (s == "table")
            return XR_PLANE_LABEL_TABLE;
        if (s == "platform")
            return XR_PLANE_LABEL_PLATFORM;
        if (s == "floor")
            return XR_PLANE_LABEL_FLOOR;
        if (s == "ceiling")
            return XR_PLANE_LABEL_CEILING;
        if (s == "wall")
            return XR_PLANE_LABEL_WALL;
        if (s == "plane" || s == "unknown")
            return XR_PLANE_LABEL_UNKNOWN;
        ok = false;
        return XR_PLANE_LABEL_UNKNOWN;
    }

    // ---- geometry ----

    Vec3 xrPlaneNormal(const SXRPlane& p) {
        return qRotate(p.pose.rot, Vec3{0.F, 0.F, 1.F});
    }

    eXRPlaneOrient xrClassifyPlane(const Vec3& normal) {
        const Vec3 n = normal.normalized();
        if (n.y >= std::cos(XR_PLANE_CLASS_TOL_RAD))
            return XR_PLANE_UP;
        if (n.y <= -std::cos(XR_PLANE_CLASS_TOL_RAD))
            return XR_PLANE_DOWN;
        if (std::fabs(n.y) <= std::sin(XR_PLANE_CLASS_TOL_RAD))
            return XR_PLANE_VERTICAL;
        return XR_PLANE_ARBITRARY;
    }

    Vec3 xrPlaneLocal(const SXRPlane& p, const Vec3& world) {
        return qRotate(qInverse(p.pose.rot), world - p.pose.pos);
    }

    Vec3 xrPlaneWorld(const SXRPlane& p, const SXRVec2& local, float height) {
        return p.pose.pos + qRotate(p.pose.rot, Vec3{local.x, local.y, height});
    }

    bool xrPointInPolygon(const SXRVec2& pt, const SXRVec2* poly, size_t n) {
        if (!poly || n < 3)
            return false;
        bool inside = false;
        for (size_t i = 0, j = n - 1; i < n; j = i++) {
            const SXRVec2& a = poly[i];
            const SXRVec2& b = poly[j];
            if ((a.y > pt.y) == (b.y > pt.y))
                continue;
            const float xCross = a.x + (pt.y - a.y) * (b.x - a.x) / (b.y - a.y);
            if (pt.x < xCross)
                inside = !inside;
        }
        return inside;
    }

    bool xrPlaneContains(const SXRPlane& p, const SXRVec2& local) {
        if (p.nPoly >= 3)
            return xrPointInPolygon(local, p.poly.data(), p.nPoly);
        return std::fabs(local.x) <= p.extentX * 0.5F && std::fabs(local.y) <= p.extentY * 0.5F;
    }

    SXRPlaneRayHit xrRayPlane(const SXRPlane& p, const Vec3& origin, const Vec3& dir) {
        SXRPlaneRayHit out;
        const Vec3     n     = xrPlaneNormal(p);
        const float    denom = n.dot(dir);
        if (std::fabs(denom) < 1e-6F)
            return out;
        const float t = n.dot(p.pose.pos - origin) / denom;
        if (t <= 0.F || !xrFinite(t))
            return out;
        out.hit          = true;
        out.t            = t;
        out.point        = origin + dir * t;
        const Vec3 local = xrPlaneLocal(p, out.point);
        out.local        = SXRVec2{local.x, local.y};
        out.inside       = xrPlaneContains(p, out.local);
        return out;
    }

    // ---- plane-relative storage ----

    SXRPose xrPoseToPlane(const SXRPlane& p, const SXRPose& world) {
        return poseCompose(poseInverse(p.pose), world);
    }

    SXRPose xrPoseFromPlane(const SXRPlane& p, const SXRPose& local) {
        return poseCompose(p.pose, local);
    }

    void xrPlanesReexpress(SXRPlaneSnapshot& s, const SXRPose& m) {
        const SXRPose invM = poseInverse(m);
        for (size_t i = 0; i < s.n && i < s.planes.size(); ++i)
            s.planes[i].pose = poseCompose(invM, s.planes[i].pose);
    }

    const SXRPlane* xrFindPlane(const SXRPlaneSnapshot& s, uint64_t id) {
        for (size_t i = 0; i < s.n && i < s.planes.size(); ++i) {
            if (s.planes[i].id == id)
                return &s.planes[i];
        }
        return nullptr;
    }

    // ---- the snap decision ----

    // The window's up axis and the four corners of its VISIBLE rectangle (content + chrome), world.
    static void quadCorners(const SXRSurfaceQuery& q, std::array<Vec3, 4>& out) {
        const Vec3 right = qRotate(q.content.rot, Vec3{1.F, 0.F, 0.F});
        const Vec3 up    = qRotate(q.content.rot, Vec3{0.F, 1.F, 0.F});
        // The top edge is not tracked separately (the top margin is a few centimetres); mirroring
        // the bottom extent is enough for "which corner is lowest".
        const Vec3 bottom = q.content.pos - up * q.bottomExt;
        const Vec3 top    = q.content.pos + up * q.bottomExt;
        out[0]            = bottom - right * q.halfWidth;
        out[1]            = bottom + right * q.halfWidth;
        out[2]            = top - right * q.halfWidth;
        out[3]            = top + right * q.halfWidth;
    }

    static float yawToward(const Vec3& from, const SXRSurfaceQuery& q) {
        // The yaw (qFromYaw convention: +Z rotated by `a` is (sin a, 0, cos a)) that turns a window's
        // +Z normal horizontally toward the head; the carried window's own facing when the head is
        // unknown or straight above the contact.
        if (q.headValid) {
            const Vec3  d = q.head - from;
            const float l = std::sqrt(d.x * d.x + d.z * d.z);
            if (l > 1e-3F)
                return std::atan2(d.x, d.z);
        }
        const Vec3  n = qRotate(q.content.rot, Vec3{0.F, 0.F, 1.F});
        const float l = std::sqrt(n.x * n.x + n.z * n.z);
        if (l > 1e-3F)
            return std::atan2(n.x, n.z);
        // A window carried perfectly flat: its up axis points away from where its viewer was.
        const Vec3 u = qRotate(q.content.rot, Vec3{0.F, 1.F, 0.F});
        return std::atan2(-u.x, -u.z);
    }

    SXRPose xrSnapPose(eXRSnapKind kind, const SXRPlane& p, const Vec3& contact, const SXRSurfaceQuery& q, const SXRSurfaceTuning& t) {
        if (kind == XR_SNAP_WALL) {
            Vec3 n = xrPlaneNormal(p);
            if (q.headValid && n.dot(q.head - contact) < 0.F)
                n = -n; // face into the room the user is in
            const float yaw = std::atan2(n.x, n.z);
            return SXRPose{contact + n * t.lift, qFromYaw(yaw)};
        }

        // STAND / FLAT on an upward surface. Gravity frames the window (a table a degree or two off
        // level must not lean a standing window sideways); the plane only supplies the height.
        const float tilt = kind == XR_SNAP_FLAT ? PI_F * 0.5F : std::clamp(t.standTiltRad, 0.F, PI_F * 0.5F);
        const Quat  rot  = qMul(qFromYaw(yawToward(contact, q)), qFromPitch(-tilt));
        const Vec3  up   = qRotate(rot, Vec3{0.F, 1.F, 0.F});
        return SXRPose{contact + up * q.bottomExt + Vec3{0.F, t.lift, 0.F}, rot};
    }

    SXRSurfaceCandidate xrSurfaceEvaluate(const SXRPlane& p, const SXRSurfaceQuery& q, const SXRSurfaceTuning& t, float enterDist) {
        SXRSurfaceCandidate c;
        c.planeId = p.id;

        if (p.orient == XR_PLANE_UP) {
            // The lowest visible corner is how close the window is; where its bottom edge projects is
            // where it lands.
            std::array<Vec3, 4> corners;
            quadCorners(q, corners);
            float lowest = std::numeric_limits<float>::max();
            for (const auto& k : corners)
                lowest = std::min(lowest, xrPlaneLocal(p, k).z);
            if (lowest > enterDist || lowest < -t.maxPenetrate)
                return c;

            const Vec3 up     = qRotate(q.content.rot, Vec3{0.F, 1.F, 0.F});
            const Vec3 bottom = q.content.pos - up * q.bottomExt;
            const Vec3 local  = xrPlaneLocal(p, bottom);
            if (!xrPlaneContains(p, SXRVec2{local.x, local.y}))
                return c;

            const Vec3 nWin = qRotate(q.content.rot, Vec3{0.F, 0.F, 1.F});
            c.ok            = true;
            c.kind          = q.flat || std::fabs(nWin.dot(xrPlaneNormal(p))) >= t.flatTiltCos ? XR_SNAP_FLAT : XR_SNAP_STAND;
            c.dist          = std::fabs(lowest);
            c.contact       = xrPlaneWorld(p, SXRVec2{local.x, local.y});
        } else if (p.orient == XR_PLANE_VERTICAL && t.walls) {
            // A wall: how far the window's center is from it, on the room side.
            Vec3 local = xrPlaneLocal(p, q.content.pos);
            if (q.headValid && xrPlaneLocal(p, q.head).z < 0.F)
                local.z = -local.z; // the normal points away from the user: measure from their side
            if (local.z > enterDist || local.z < -t.maxPenetrate)
                return c;
            if (!xrPlaneContains(p, SXRVec2{local.x, local.y}))
                return c;
            c.ok      = true;
            c.kind    = XR_SNAP_WALL;
            c.dist    = std::fabs(local.z);
            c.contact = xrPlaneWorld(p, SXRVec2{local.x, local.y});
        } else
            return c;

        if (q.haveRay) {
            const auto hit = xrRayPlane(p, q.rayOrigin, q.rayDir);
            c.rayHit       = hit.hit && hit.inside;
        }
        c.pose = xrSnapPose(c.kind, p, c.contact, q, t);
        if (!xrPoseFinite(c.pose))
            c.ok = false;
        return c;
    }

    SXRSurfaceCandidate xrSurfacePick(const SXRPlane* planes, size_t n, const SXRSurfaceQuery& q, const SXRSurfaceTuning& t, uint64_t stickyId) {
        SXRSurfaceCandidate best;
        // rank: horizontal before wall, ray-hit before not, then nearest
        auto better = [](const SXRSurfaceCandidate& a, const SXRSurfaceCandidate& b) {
            const bool aWall = a.kind == XR_SNAP_WALL, bWall = b.kind == XR_SNAP_WALL;
            if (aWall != bWall)
                return !aWall;
            if (a.rayHit != b.rayHit)
                return a.rayHit;
            return a.dist < b.dist;
        };
        for (size_t i = 0; planes && i < n; ++i) {
            const bool sticky = stickyId != 0 && planes[i].id == stickyId;
            auto       c      = xrSurfaceEvaluate(planes[i], q, t, sticky ? std::max(t.exitDist, t.snapDist) : t.snapDist);
            if (!c.ok)
                continue;
            c.index = i;
            // the previewed plane keeps the preview while it still qualifies — no flicker between
            // two surfaces that both qualify (a table and the floor under its edge)
            if (sticky)
                return c;
            if (!best.ok || better(c, best))
                best = c;
        }
        return best;
    }

    SXRSurfaceCandidate xrSurfaceRelease(const SXRSurfaceArm& arm, const SXRPose& releaseContent, const Vec3& head, bool headValid) {
        if (!arm.havePlane)
            return {};
        SXRSurfaceQuery q = arm.query;
        q.content         = releaseContent;
        q.head            = head;
        q.headValid       = headValid;
        const float enter = arm.preview ? std::max(arm.tuning.exitDist, arm.tuning.snapDist) : arm.tuning.snapDist;
        auto        c     = xrSurfaceEvaluate(arm.plane, q, arm.tuning, enter);
        c.planeId         = arm.plane.id;
        return c;
    }

    eXRAttachStep xrAttachStep(const SXRSurfaceAttach& a, const SXRPlane* plane, SXRPose& out) {
        if (!a.valid || !plane)
            return XR_ATTACH_HOLD;
        out           = xrPoseFromPlane(*plane, a.local);
        const float d = (out.pos - a.placed.pos).length();
        const float r = qAngleBetween(out.rot, a.placed.rot);
        if (!xrPoseFinite(out) || d > XR_ATTACH_FOLLOW_MAX_M || r > XR_ATTACH_FOLLOW_MAX_RAD)
            return XR_ATTACH_DETACH;
        if (d < XR_ATTACH_EPS_M && r < 1e-3F)
            return XR_ATTACH_HOLD;
        return XR_ATTACH_FOLLOW;
    }

    // ---- sources ----

    static std::string trim(const std::string& s) {
        const auto b = s.find_first_not_of(" \t\r\n");
        if (b == std::string::npos)
            return "";
        const auto e = s.find_last_not_of(" \t\r\n");
        return s.substr(b, e - b + 1);
    }

    static bool parseFloats(const std::string& s, float* out, size_t n) {
        std::stringstream ss(s);
        std::string       tok;
        size_t            i = 0;
        while (std::getline(ss, tok, ',')) {
            if (i >= n)
                return false;
            try {
                size_t used = 0;
                out[i]      = std::stof(trim(tok), &used);
                if (used != trim(tok).size() || !xrFinite(out[i]))
                    return false;
            } catch (...) { return false; }
            ++i;
        }
        return i == n;
    }

    std::expected<std::vector<SXRPlane>, std::string> xrParseSyntheticPlanes(const std::string& spec) {
        std::vector<SXRPlane> out;
        std::stringstream     planes(spec);
        std::string           entry;
        while (std::getline(planes, entry, ';')) {
            entry = trim(entry);
            if (entry.empty())
                continue;
            if (out.size() >= XR_PLANE_MAX)
                return std::unexpected(std::format("more than {} planes", XR_PLANE_MAX));

            std::stringstream words(entry);
            std::string       word;
            words >> word;
            bool       labelOk = false;
            const auto label   = xrParsePlaneLabel(word, labelOk);
            if (!labelOk)
                return std::unexpected(std::format("'{}': unknown surface '{}' (table|platform|floor|ceiling|wall|plane)", entry, word));

            float pos[3]  = {0.F, 0.F, 0.F};
            float size[2] = {0.F, 0.F};
            float yawDeg  = 0.F;
            bool  havePos = false, haveSize = false;
            while (words >> word) {
                const auto colon = word.find(':');
                if (colon == std::string::npos)
                    return std::unexpected(std::format("'{}': expected key:value, got '{}'", entry, word));
                const std::string key = word.substr(0, colon), val = word.substr(colon + 1);
                if (key == "pos") {
                    if (!parseFloats(val, pos, 3))
                        return std::unexpected(std::format("'{}': pos wants x,y,z metres", entry));
                    havePos = true;
                } else if (key == "size") {
                    if (!parseFloats(val, size, 2) || size[0] <= 0.F || size[1] <= 0.F)
                        return std::unexpected(std::format("'{}': size wants two positive metres w,d", entry));
                    haveSize = true;
                } else if (key == "yaw") {
                    if (!parseFloats(val, &yawDeg, 1))
                        return std::unexpected(std::format("'{}': yaw wants degrees", entry));
                } else
                    return std::unexpected(std::format("'{}': unknown key '{}' (pos|size|yaw)", entry, key));
            }
            if (!havePos || !haveSize)
                return std::unexpected(std::format("'{}': needs pos: and size:", entry));

            SXRPlane p;
            p.id         = XR_PLANE_SYNTHETIC_ID_BIT | (out.size() + 1);
            p.label      = label;
            p.source     = XR_PLANE_SRC_SYNTHETIC;
            p.extentX    = size[0];
            p.extentY    = size[1];
            p.pose.pos   = Vec3{pos[0], pos[1], pos[2]};
            const Quat Y = qFromYaw(yawDeg * PI_F / 180.F);
            if (label == XR_PLANE_LABEL_WALL)
                p.pose.rot = Y; // local +Z (the normal) faces +Z at yaw 0; local +Y is up
            else if (label == XR_PLANE_LABEL_CEILING)
                p.pose.rot = qMul(Y, qFromPitch(PI_F * 0.5F)); // local +Z -> world down
            else
                p.pose.rot = qMul(Y, qFromPitch(-PI_F * 0.5F)); // local +Z -> world up, local +Y -> world -Z (depth)
            p.orient  = xrClassifyPlane(xrPlaneNormal(p));
            p.nPoly   = 4;
            p.poly    = {};
            p.poly[0] = SXRVec2{-size[0] * 0.5F, -size[1] * 0.5F};
            p.poly[1] = SXRVec2{size[0] * 0.5F, -size[1] * 0.5F};
            p.poly[2] = SXRVec2{size[0] * 0.5F, size[1] * 0.5F};
            p.poly[3] = SXRVec2{-size[0] * 0.5F, size[1] * 0.5F};
            out.push_back(p);
        }
        return out;
    }

    SXRPlane xrCanonicalPlane(const SXRPose& rawPose, eXRPlaneOrient declared, const SXRVec2* rawPoly, size_t nRaw, float extentX, float extentY, bool& swapped) {
        SXRPlane p;
        p.pose    = rawPose;
        p.extentX = extentX;
        p.extentY = extentY;
        swapped   = false;

        // The +Z reading is right unless the declared class contradicts it AND a +Y normal would fit.
        const Vec3 nz = qRotate(rawPose.rot, Vec3{0.F, 0.F, 1.F});
        const Vec3 ny = qRotate(rawPose.rot, Vec3{0.F, 1.F, 0.F});
        if (declared != XR_PLANE_ARBITRARY && xrClassifyPlane(nz) != declared && xrClassifyPlane(ny) == declared) {
            // canonical = raw ∘ Rx(-90°): canonical +Z is raw +Y, canonical +Y is raw -Z, so a raw
            // contour vertex (u, v) meaning raw local (u, 0, v) is canonical (u, -v).
            p.pose.rot = qNormalize(qMul(rawPose.rot, qFromPitch(-PI_F * 0.5F)));
            swapped    = true;
        }
        const size_t n = std::min(nRaw, XR_PLANE_MAX_VERTS);
        for (size_t i = 0; rawPoly && i < n; ++i)
            p.poly[i] = swapped ? SXRVec2{rawPoly[i].x, -rawPoly[i].y} : rawPoly[i];
        p.nPoly  = n >= 3 ? sc<uint8_t>(n) : 0;
        p.orient = xrClassifyPlane(xrPlaneNormal(p));
        return p;
    }
}
