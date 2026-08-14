#include "XRViewpointTransport.hpp"

#include <hyprutils/memory/Casts.hpp>

#include <cmath>
#include <limits>

using namespace OpenXR;
using namespace Hyprutils::Memory;

SXRViewpointU64 OpenXR::splitViewpointU64(uint64_t value) {
    return {
        .hi = sc<uint32_t>(value >> 32),
        .lo = sc<uint32_t>(value & 0xFFFFFFFFULL),
    };
}

uint64_t OpenXR::joinViewpointU64(const SXRViewpointU64& value) {
    return (sc<uint64_t>(value.hi) << 32) | sc<uint64_t>(value.lo);
}

bool OpenXR::encodeViewpointPositionUM(double meters, int32_t& out) {
    out = 0;
    if (!std::isfinite(meters))
        return false;

    const double rounded = std::round(meters * XR_VIEWPOINT_UM_PER_METER);
    if (!std::isfinite(rounded) || rounded < sc<double>(std::numeric_limits<int32_t>::min()) || rounded > sc<double>(std::numeric_limits<int32_t>::max()))
        return false;

    out = sc<int32_t>(rounded);
    return true;
}

double OpenXR::decodeViewpointPositionUM(int32_t micrometers) {
    return sc<double>(micrometers) / XR_VIEWPOINT_UM_PER_METER;
}

bool OpenXR::encodeViewpointDimensionUM(double meters, uint32_t& out) {
    out = 0;
    if (!std::isfinite(meters) || meters <= 0.0)
        return false;

    const double rounded = std::round(meters * XR_VIEWPOINT_UM_PER_METER);
    if (!std::isfinite(rounded) || rounded < 1.0 || rounded > sc<double>(std::numeric_limits<uint32_t>::max()))
        return false;

    out = sc<uint32_t>(rounded);
    return true;
}

double OpenXR::decodeViewpointDimensionUM(uint32_t micrometers) {
    return sc<double>(micrometers) / XR_VIEWPOINT_UM_PER_METER;
}

bool OpenXR::encodedViewpointSampleValid(const SXRViewpointEncodedSample& sample) {
    if (sample.flags != XR_VIEWPOINT_SAMPLE_VALID || sample.viewCount < 1 || sample.viewCount > sample.viewPositions.size() || sample.widthUM == 0 || sample.heightUM == 0)
        return false;

    for (uint32_t i = 0; i < sample.viewCount; ++i)
        if (sample.viewPositions[i].z <= XR_VIEWPOINT_Z_EPSILON_UM)
            return false;

    for (size_t i = sample.viewCount; i < sample.viewPositions.size(); ++i)
        if (sample.viewPositions[i] != SXRViewpointPositionUM{})
            return false;

    return true;
}

bool OpenXR::encodeViewpointSample(const SXRViewpointGeometry& geometry, uint64_t serial, uint64_t geometryId, SXRViewpointEncodedSample& out) {
    out = {};
    if (!geometry.valid || geometry.viewCount < 1 || geometry.viewCount > geometry.viewPositions.size())
        return false;

    SXRViewpointEncodedSample candidate;
    candidate.serial     = splitViewpointU64(serial);
    candidate.geometryId = splitViewpointU64(geometryId);
    candidate.viewCount  = sc<uint32_t>(geometry.viewCount);

    if (!encodeViewpointDimensionUM(geometry.widthMeters, candidate.widthUM) || !encodeViewpointDimensionUM(geometry.heightMeters, candidate.heightUM))
        return false;

    for (size_t i = 0; i < geometry.viewCount; ++i) {
        const Vec3& view = geometry.viewPositions[i];
        if (!encodeViewpointPositionUM(view.x, candidate.viewPositions[i].x) || !encodeViewpointPositionUM(view.y, candidate.viewPositions[i].y) ||
            !encodeViewpointPositionUM(view.z, candidate.viewPositions[i].z))
            return false;
    }

    candidate.flags = XR_VIEWPOINT_SAMPLE_VALID;
    if (!encodedViewpointSampleValid(candidate))
        return false;

    out = candidate;
    return true;
}

bool OpenXR::decodeViewpointSample(const SXRViewpointEncodedSample& sample, SXRViewpointGeometry& geometry, uint64_t& serial, uint64_t& geometryId) {
    geometry   = {};
    serial     = 0;
    geometryId = 0;
    if (!encodedViewpointSampleValid(sample))
        return false;

    SXRViewpointGeometry candidate;
    candidate.valid        = true;
    candidate.viewCount    = sample.viewCount;
    candidate.widthMeters  = sc<float>(decodeViewpointDimensionUM(sample.widthUM));
    candidate.heightMeters = sc<float>(decodeViewpointDimensionUM(sample.heightUM));
    for (size_t i = 0; i < candidate.viewCount; ++i) {
        candidate.viewPositions[i] = {
            sc<float>(decodeViewpointPositionUM(sample.viewPositions[i].x)),
            sc<float>(decodeViewpointPositionUM(sample.viewPositions[i].y)),
            sc<float>(decodeViewpointPositionUM(sample.viewPositions[i].z)),
        };
    }

    geometry   = candidate;
    serial     = joinViewpointU64(sample.serial);
    geometryId = joinViewpointU64(sample.geometryId);
    return true;
}

CXRViewpointSampleMailbox::CXRViewpointSampleMailbox(uint64_t initialGeneration) : m_generation(initialGeneration) {
    ;
}

SXRViewpointPublishResult CXRViewpointSampleMailbox::publish(const SXRViewpointEncodedSample& sample) {
    std::scoped_lock          lock(m_mutex);

    SXRViewpointPublishResult result{.generation = m_generation};
    if (!encodedViewpointSampleValid(sample) || m_generation == std::numeric_limits<uint64_t>::max())
        return result;

    ++m_generation;
    result.accepted   = true;
    result.shouldWake = !m_pending;
    result.generation = m_generation;

    if (m_pending && m_superseded != std::numeric_limits<uint64_t>::max())
        ++m_superseded;
    m_latest  = sample;
    m_pending = true;
    return result;
}

bool CXRViewpointSampleMailbox::consumeLatest(SXRViewpointMailboxRead& out) {
    std::scoped_lock lock(m_mutex);
    out = {};
    if (!m_pending)
        return false;

    out.sample     = m_latest;
    out.generation = m_generation;
    out.superseded = m_superseded;
    m_pending      = false;
    m_superseded   = 0;
    return true;
}

bool CXRViewpointSampleMailbox::clearPending() {
    std::scoped_lock lock(m_mutex);
    const bool       discarded = m_pending;
    m_latest                   = {};
    m_superseded               = 0;
    m_pending                  = false;
    return discarded;
}

bool CXRViewpointSampleMailbox::pending() const {
    std::scoped_lock lock(m_mutex);
    return m_pending;
}

uint64_t CXRViewpointSampleMailbox::generation() const {
    std::scoped_lock lock(m_mutex);
    return m_generation;
}
