#include "XRViewpoint.hpp"

#include <cmath>

using namespace OpenXR;

static bool finiteVec(const Vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

static bool finiteQuat(const Quat& q) {
    return std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) && std::isfinite(q.w);
}

SXRViewpointGeometry OpenXR::surfaceRelativeViewpoint(const SXRPose& contentWorldPose, float widthMeters, float heightMeters, const std::array<Vec3, 2>& viewWorldPositions,
                                                      size_t viewCount) {
    SXRViewpointGeometry out;

    if (viewCount < 1 || viewCount > out.viewPositions.size())
        return out;
    if (!std::isfinite(widthMeters) || !std::isfinite(heightMeters) || widthMeters <= 0.F || heightMeters <= 0.F)
        return out;
    if (!finiteVec(contentWorldPose.pos) || !finiteQuat(contentWorldPose.rot))
        return out;

    const float rotNormSq = contentWorldPose.rot.x * contentWorldPose.rot.x + contentWorldPose.rot.y * contentWorldPose.rot.y + contentWorldPose.rot.z * contentWorldPose.rot.z +
        contentWorldPose.rot.w * contentWorldPose.rot.w;
    if (!std::isfinite(rotNormSq) || rotNormSq <= 1e-12F)
        return out;

    out.viewCount             = viewCount;
    out.widthMeters           = widthMeters;
    out.heightMeters          = heightMeters;
    const Quat worldToContent = qInverse(qNormalize(contentWorldPose.rot));
    out.valid                 = true;

    for (size_t i = 0; i < viewCount; ++i) {
        if (!finiteVec(viewWorldPositions[i])) {
            out.valid = false;
            continue;
        }

        out.viewPositions[i] = qRotate(worldToContent, viewWorldPositions[i] - contentWorldPose.pos);
        if (!finiteVec(out.viewPositions[i]) || out.viewPositions[i].z <= XR_VIEWPOINT_Z_EPSILON)
            out.valid = false;
    }

    return out;
}
