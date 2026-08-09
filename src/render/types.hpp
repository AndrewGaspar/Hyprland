#pragma once

#include "Framebuffer.hpp"
#include "../desktop/DesktopTypes.hpp"
#include "../helpers/cm/ColorManagement.hpp"
#include "../protocols/core/Compositor.hpp"
#include <hyprgraphics/color/Color.hpp>
#include <hyprutils/math/Box.hpp>
#include <hyprutils/math/Mat3x3.hpp>
#include <hyprutils/math/Region.hpp>
#include <hyprutils/math/Vector2D.hpp>

namespace Render {
    const std::vector<const char*> ASSET_PATHS = {
#ifdef DATAROOTDIR
        DATAROOTDIR,
#endif
        "/usr/share",
        "/usr/local/share",
    };

    enum eDamageTrackingModes : int8_t {
        DAMAGE_TRACKING_INVALID = -1,
        DAMAGE_TRACKING_NONE    = 0,
        DAMAGE_TRACKING_MONITOR,
        DAMAGE_TRACKING_FULL,
    };

    enum eRenderPassMode : uint8_t {
        RENDER_PASS_ALL = 0,
        RENDER_PASS_MAIN,
        RENDER_PASS_POPUP
    };

    enum eRenderMode : uint8_t {
        RENDER_MODE_NORMAL              = 0,
        RENDER_MODE_FULL_FAKE           = 1,
        RENDER_MODE_TO_BUFFER           = 2,
        RENDER_MODE_TO_BUFFER_READ_ONLY = 3,
    };

    struct SRenderWorkspaceUntilData {
        PHLLS     ls;
        PHLWINDOW w;
    };

    enum eRenderProjectionType : uint8_t {
        RPT_MONITOR,
        RPT_MIRROR,
        RPT_FB,
        RPT_EXPORT,
    };

    struct SRenderModifData {
        enum eRenderModifType : uint8_t {
            RMOD_TYPE_SCALE,        /* scale by a float */
            RMOD_TYPE_SCALECENTER,  /* scale by a float from the center */
            RMOD_TYPE_TRANSLATE,    /* translate by a Vector2D */
            RMOD_TYPE_ROTATE,       /* rotate by a float in rad from top left */
            RMOD_TYPE_ROTATECENTER, /* rotate by a float in rad from center */
        };

        std::vector<std::pair<eRenderModifType, std::any>> modifs;

        void                                               applyToBox(Hyprutils::Math::CBox& box);
        void                                               applyToRegion(Hyprutils::Math::CRegion& rg);
        float                                              combinedScale();

        bool                                               enabled = true;
    };

    struct SRenderData {
        // can be private
        Hyprutils::Math::Mat3x3 targetProjection;

        // ----------------------

        // used by public
        Hyprutils::Math::Vector2D fbSize = {-1, -1};
        PHLMONITORREF             pMonitor;

        eRenderProjectionType     projectionType = RPT_MONITOR;

        SP<IFramebuffer>          currentFB = nullptr; // current rendering to
        SP<IFramebuffer>          mainFB    = nullptr; // main to render to
        SP<IFramebuffer>          outFB     = nullptr; // out to render to (if offloaded, etc)

        CRegion                   damage;
        CRegion                   finalDamage; // damage used for final off -> main

        SRenderModifData          renderModif;
        float                     mouseZoomFactor    = 1.f;
        bool                      mouseZoomUseMouse  = true; // true by default
        bool                      useNearestNeighbor = false;
        bool                      blockScreenShader  = false;

        Vector2D                  primarySurfaceUVTopLeft     = Vector2D(-1, -1);
        Vector2D                  primarySurfaceUVBottomRight = Vector2D(-1, -1);

        // TODO remove and pass directly
        CBox                   clipBox = {}; // scaled coordinates
        PHLWINDOWREF           currentWindow;
        WP<CWLSurfaceResource> surface;

        bool                   transformDamage = true;
        bool                   noSimplify      = false;

        // --- the stereo per-eye producer (research/24 §5.3 WP S1 + §6.1 WP D2) ---
        //
        // ONE eye index for BOTH per-eye effects. Which pane of a stereo output is being
        // composited RIGHT NOW; -1 means "not a per-eye pass", which is every frame on every
        // ordinary monitor, every frame on a stereo monitor with nothing raised and no stereo
        // content on it (§6.4.1's fast path), and every off-cycle render (screencopy, a fake
        // frame, a mirror source).
        //
        // Depth reads it gated on >= 0 — a mono frame must not shift anything, so the mono path
        // costs an integer compare and is otherwise untouched. The stereo-CONTENT crop instead
        // reads it as eye `max(stereoPane, 0)`: a capture of a stereo output has exactly one pane
        // to hand out and §3.6 says that pane is the left eye, which is also what the pane loop
        // draws first.
        int stereoPane = -1;
        // ...and the crop's own witness: set when a stereo-declared surface actually made it onto
        // a pane this frame. Not the producer's trigger any more (the pane count is decided before
        // the frame is built, exactly as depth's is) — it is what `hyprctl monitors` reports as
        // `stereoContent`, i.e. "is my stereo window engaged, or is the rule sitting behind the
        // fullscreen gate?".
        bool stereoContentDrawn = false;
        // The finished composite of each pane BEFORE the one still being built, for the pack in
        // CHyprOpenGLImpl::end(). Empty in the fast path, where end() duplicates the single
        // composite into every pane exactly as WP F1 shipped it.
        std::vector<SP<IFramebuffer>> stereoPaneFBs;
        // ...and the union of those panes' FINAL damage. finalDamage is one slot per pass and each
        // pane's pass overwrites it, so the pack — which blits every pane through a single damage
        // region — would otherwise scissor earlier panes to the LAST pane's blur-expanded region.
        CRegion stereoPaneDamage;
    };

    struct STFRange {
        float min = 0;
        float max = 80;
    };

    struct SCMSettings {
        NColorManagement::eTransferFunction  sourceTF = NColorManagement::CM_TRANSFER_FUNCTION_GAMMA22;
        NColorManagement::eTransferFunction  targetTF = NColorManagement::CM_TRANSFER_FUNCTION_GAMMA22;
        STFRange                             srcTFRange;
        STFRange                             dstTFRange;
        float                                srcRefLuminance = 80;
        float                                dstRefLuminance = 80;
        std::array<std::array<double, 3>, 3> convertMatrix;

        bool                                 needsTonemap    = false;
        int                                  tonemapMode     = 1; // 1 - default, 2 - clamp, 3 - limited
        float                                maxLuminance    = 80;
        float                                dstMaxLuminance = 80;
        std::array<std::array<double, 3>, 3> dstPrimaries2XYZ;
        bool                                 needsSDRmod             = false;
        float                                sdrSaturation           = 1.0;
        float                                sdrBrightnessMultiplier = 1.0;
    };
}
