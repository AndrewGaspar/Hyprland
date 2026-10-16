#pragma once

// XRPlaneSource — research/32 §1/§5: the plane-source abstraction. A source hands the surfaces code
// planes in the canonical form of XRSurfaces.hpp; WHERE they come from is its business. Implemented
// by the synthetic config-declared provider (C0, below — pure, gtested) and by the
// XR_EXT_plane_detection consumer (C1, XRPlaneDetection.hpp). C3 (XR_EXT_spatial_plane_tracking)
// slots in as a third implementation: its discovery snapshot maps onto poll() exactly as the
// begin/state/get cycle of EXT_plane_detection does.
//
// Compiled UNCONDITIONALLY: the interface carries no OpenXR types. MAIN THREAD ONLY, every method —
// a source may read config strings and make runtime calls; the frame thread only ever sees the POD
// snapshot COpenXRManager publishes from what poll() produced.

#include "XRSurfaces.hpp"
#include "../helpers/memory/Memory.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace OpenXR {
    // What a poll is told about the world it is locating against.
    struct SXRPlanePollContext {
        int64_t xrTime     = 0;   // a recent predicted display time (XrTime ns); 0 = no frame yet
        float   floorShift = 0.F; // add to y: the LOCAL fallback's openxr:floor_offset (0 under LOCAL_FLOOR)
        Vec3    head;             // the newest head pose the frame thread published, solver frame
        bool    headValid = false;
    };

    enum eXRPlanePoll : uint8_t {
        XR_PLANE_POLL_UNCHANGED = 0, // nothing new: keep publishing what this source gave last time
        XR_PLANE_POLL_UPDATED,       // `out` holds this source's complete, freshly located plane set
        XR_PLANE_POLL_UNAVAILABLE,   // the source cannot produce planes (for now); its planes are dropped
    };

    class IXRPlaneSource {
      public:
        virtual ~IXRPlaneSource() = default;

        // Stable, short, for status/logs: "synthetic", "plane_detection".
        virtual const char* name() const = 0;
        // One poll step. Never blocks for long: a source with asynchronous work (a runtime detection)
        // advances one step per call and answers UNCHANGED while it waits.
        virtual eXRPlanePoll poll(const SXRPlanePollContext& ctx, std::vector<SXRPlane>& out) = 0;
        // One human-readable line: what the source is doing and, when it produces nothing, why.
        virtual std::string status() const = 0;
        // The reference space changed under the source (a recenter, a discontinuity): whatever it
        // published last is in a space that is gone, so the next poll must publish afresh even if
        // nothing else changed. A source that re-locates on every poll anyway may ignore it.
        virtual void invalidate();
        // Release runtime objects before the session goes (main thread, frame thread joined).
        // runtimeLost: the runtime IPC is dead — skip the doomed destroy calls.
        virtual void shutdown(bool runtimeLost);
    };

    // C0: planes declared by openxr:surfaces_synthetic, in LOCAL_FLOOR. "Relative to LOCAL_FLOOR" is
    // meant literally — they are re-published against the CURRENT origin after every reference-space
    // change (invalidate), so a recenter moves them with the origin. A fixture for testing and for
    // runtimes with no plane detection, not a model of a room.
    class CXRSyntheticPlaneSource : public IXRPlaneSource {
      public:
        // Re-parse the spec (main thread, on config reload). An invalid spec keeps NO planes and
        // reports the parse error in status(); returns false then.
        bool         setSpec(const std::string& spec);
        size_t       planeCount() const;

        const char*  name() const override;
        eXRPlanePoll poll(const SXRPlanePollContext& ctx, std::vector<SXRPlane>& out) override;
        std::string  status() const override;
        void         invalidate() override;

      private:
        std::string           m_spec;
        std::string           m_error;
        std::vector<SXRPlane> m_planes;
        uint32_t              m_generation = 0;
        bool                  m_dirty      = true;
        float                 m_lastShift  = 0.F;
    };

    // The set of sources the surfaces feature merges: the synthetic provider (always present, empty
    // unless declared) and at most one RUNTIME source — XR_EXT_plane_detection today, the
    // XR_EXT_spatial_plane_tracking backend (C3) tomorrow; whichever the session could bring up.
    // MAIN THREAD ONLY. Pure: the runtime source is injected, so the merge is gtested with a fake.
    class CXRSurfaceSources {
      public:
        // openxr:surfaces_synthetic, re-parsed on reload. False (and no synthetic planes) on a bad spec.
        bool setSyntheticSpec(const std::string& spec);
        // The session's runtime source (session start), and its release (session end: shutdown + drop).
        void setRuntimeSource(UP<IXRPlaneSource>&& src);
        void detachRuntime(bool runtimeLost);
        // Re-publish everything on the next poll (the reference space changed under us).
        void invalidate();
        // Poll every source once. True when the merged set changed: `out` then holds it, with a fresh
        // generation. Synthetic planes come first, the runtime's fill what is left of XR_PLANE_MAX.
        bool poll(const SXRPlanePollContext& ctx, SXRPlaneSnapshot& out);
        // The last merged set (main-thread copy, for status), and each source's status line.
        const SXRPlaneSnapshot& merged() const;
        std::string             syntheticStatus() const;
        std::string             runtimeStatus() const;
        // Drop every plane (surfaces switched off): the next poll that has any publishes afresh.
        void clear();

      private:
        CXRSyntheticPlaneSource m_synthetic;
        UP<IXRPlaneSource>      m_runtime;
        std::vector<SXRPlane>   m_synthPlanes, m_runtimePlanes;
        SXRPlaneSnapshot        m_merged;
        uint32_t                m_generation = 0;
        bool                    m_dirty      = true;
    };
}
