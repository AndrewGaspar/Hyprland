#include "XRViewpointLedger.hpp"

#include <algorithm>
#include <limits>

using namespace OpenXR;

CXRViewpointIssuedSampleLedger::CXRViewpointIssuedSampleLedger(size_t capacity) : m_capacity(capacity) {
    ;
}

bool CXRViewpointIssuedSampleLedger::issue(uint64_t sample) {
    if (sample == 0 || m_capacity == 0 || std::ranges::find(m_samples, sample) != m_samples.end())
        return false;

    if (m_samples.size() == m_capacity)
        m_samples.pop_front();
    m_samples.push_back(sample);
    return true;
}

bool CXRViewpointIssuedSampleLedger::consume(uint64_t sample) {
    const auto IT = std::ranges::find(m_samples, sample);
    if (IT == m_samples.end())
        return false;

    m_samples.erase(IT);
    return true;
}

void CXRViewpointIssuedSampleLedger::clear() {
    m_samples.clear();
}

size_t CXRViewpointIssuedSampleLedger::size() const {
    return m_samples.size();
}

bool OpenXR::nextViewpointEpoch(uint64_t current, uint64_t& next) {
    next = 0;
    if (current == std::numeric_limits<uint64_t>::max())
        return false;

    next = current + 1;
    return next != 0;
}
