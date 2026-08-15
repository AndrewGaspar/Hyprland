#include "PortalRendererGL.hpp"

#include <array>
#include <format>
#include <string_view>
#include <vector>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <wayland-egl.h>

#include <hyprutils/memory/Casts.hpp>

using namespace Hyprutils::Memory;
using namespace ViewpointDemo;

namespace {

    // The vertex stage is a single oversized triangle addressed by gl_VertexID, so
    // there is no attribute buffer, no element buffer, and no per-frame upload.
    constexpr const char* VERTEX_SOURCE = R"GLSL(#version 300 es
void main() {
    vec2 corner = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

    // Shared prologue: the packed-SBS addressing both fragment shaders need.
    //
    // GL's origin is bottom-left and the CPU image's row 0 is the top, so the
    // shaders reconstruct the CPU row index as `height - 1 - fragY`. That makes
    // `float(pixel.y) + 0.5` equal to `float(uPane.y) - gl_FragCoord.y`, which is
    // the expression portalRay() feeds into the surface-plane coordinate. On the
    // window back end EGL presents with the mirrored convention, so this lands right
    // side up; on the offscreen back end readback() flips the rows on the way out.
    constexpr const char* FRAGMENT_PROLOGUE = R"GLSL(#version 300 es
precision highp float;
precision highp int;

// x = one-eye pane width in pixels, y = packed image height.
uniform ivec2 uPane;

out vec4 fragColor;

struct SPixel {
    int x;     // pane-local column, CPU convention
    int y;     // packed row, CPU convention (0 is the top)
    int pane;  // 0 = left eye, 1 = right eye
};

SPixel packedPixel() {
    ivec2  frag = ivec2(gl_FragCoord.xy);
    SPixel pixel;
    pixel.pane = frag.x < uPane.x ? 0 : 1;
    pixel.x    = frag.x - pixel.pane * uPane.x;
    pixel.y    = uPane.y - 1 - frag.y;
    return pixel;
}
)GLSL";

    // Faithful translation of trace() in PortalRenderer.cpp. Every constant,
    // comparison, and rejection order is the CPU reference's; the only intentional
    // differences are:
    //   - `float` where the reference uses `double` (GLSL ES has no fp64);
    //   - +inf replaced by FAR_DISTANCE, which no scene distance can reach;
    //   - the grid decision is deferred out of considerRoomPlane() to the end of the
    //     shader. The test is pure in the hit point, so deferring it is exact; it
    //     exists so the antialiased path can take a derivative in uniform control
    //     flow, where fwidth() is well defined.
    constexpr const char* PORTAL_FRAGMENT_SOURCE = R"GLSL(
uniform vec2 uPortal;      // portal width/height in meters
uniform vec3 uEyeLeft;
uniform vec3 uEyeRight;
uniform int  uAntialias;

const float FAR_DISTANCE = 1.0e30;
const float MIN_DISTANCE = 1.000001;
const float GRID_SPACING = 0.5;
const float GRID_WIDTH   = 0.035;  // half-width in cell units, exactly as gridLine() uses

struct SHit {
    float dist;
    vec3  base;       // 0..255, matching the CPU's integer channel space
    vec3  lineColor;  // 0..255
    vec2  cell;       // grid coordinates, already divided by GRID_SPACING
    float atten;      // negative means emissive: the base color survives unshaded
    bool  grid;
};

void considerRoomPlane(vec3 origin, vec3 dir, float numerator, float denominator, int plane, inout SHit hit) {
    if (abs(denominator) < 1e-12)
        return;

    float travel = numerator / denominator;
    if (travel <= MIN_DISTANCE || travel >= hit.dist)
        return;

    vec3 point = origin + dir * travel;
    vec3 base;
    vec3 lineColor;
    vec2 cell;

    if (plane == 0) {          // back wall, z = -5
        if (point.x < -2.8 || point.x > 2.8 || point.y < -1.7 || point.y > 1.7)
            return;
        cell      = point.xy / GRID_SPACING;
        lineColor = vec3(91.0, 130.0, 151.0);
        base      = vec3(35.0, 55.0, 67.0);
    } else if (plane == 1) {   // floor / ceiling
        if (point.x < -2.8 || point.x > 2.8 || point.z < -5.0 || point.z > 0.0)
            return;
        cell      = vec2(point.x, point.z) / GRID_SPACING;
        lineColor = vec3(98.0, 92.0, 82.0);
        base      = vec3(42.0, 39.0, 37.0);
    } else {                   // side walls
        if (point.y < -1.7 || point.y > 1.7 || point.z < -5.0 || point.z > 0.0)
            return;
        cell      = vec2(point.y, point.z) / GRID_SPACING;
        lineColor = vec3(84.0, 104.0, 111.0);
        base      = vec3(34.0, 45.0, 49.0);
    }

    hit.dist      = travel;
    hit.base      = base;
    hit.lineColor = lineColor;
    hit.cell      = cell;
    hit.atten     = 0.035;
    hit.grid      = true;
}

bool clipBoxAxis(float origin, float dir, float low, float high, inout float entry, inout float exitAt) {
    if (abs(dir) < 1e-12)
        return origin >= low && origin <= high;

    float nearDistance = (low - origin) / dir;
    float farDistance  = (high - origin) / dir;
    if (nearDistance > farDistance) {
        float held   = nearDistance;
        nearDistance = farDistance;
        farDistance  = held;
    }
    entry  = max(entry, nearDistance);
    exitAt = min(exitAt, farDistance);
    return entry <= exitAt;
}

void considerBox(vec3 origin, vec3 dir, vec3 low, vec3 high, vec3 color, inout SHit hit) {
    float entry  = MIN_DISTANCE;
    float exitAt = FAR_DISTANCE;
    if (!clipBoxAxis(origin.x, dir.x, low.x, high.x, entry, exitAt) || !clipBoxAxis(origin.y, dir.y, low.y, high.y, entry, exitAt) ||
        !clipBoxAxis(origin.z, dir.z, low.z, high.z, entry, exitAt))
        return;
    if (entry >= hit.dist)
        return;

    hit.dist  = entry;
    hit.base  = color;
    hit.atten = 0.045;
    hit.grid  = false;
}

void considerAimMarker(vec3 origin, vec3 dir, inout SHit hit) {
    // Authoritative world state: the impact point never depends on the viewer.
    // Head translation changes only where its projection lands.
    vec3  center = vec3(0.0, 0.0, -3.65);
    float radius = 0.12;

    vec3  offset = origin - center;
    float a      = dir.x * dir.x + dir.y * dir.y + dir.z * dir.z;
    float b      = 2.0 * (offset.x * dir.x + offset.y * dir.y + offset.z * dir.z);
    float c      = offset.x * offset.x + offset.y * offset.y + offset.z * offset.z - radius * radius;
    float disc   = b * b - 4.0 * a * c;
    if (disc < 0.0 || a <= 0.0)
        return;

    float travel = (-b - sqrt(disc)) / (2.0 * a);
    if (travel <= MIN_DISTANCE || travel >= hit.dist)
        return;

    hit.dist  = travel;
    hit.base  = vec3(255.0, 76.0, 57.0);
    hit.atten = -1.0;
    hit.grid  = false;
}

SHit trace(vec3 origin, vec3 dir) {
    SHit hit;
    hit.dist      = FAR_DISTANCE;
    hit.base      = vec3(0.0);
    hit.lineColor = vec3(0.0);
    hit.cell      = vec2(0.0);
    hit.atten     = -1.0;
    hit.grid      = false;

    considerRoomPlane(origin, dir, -5.0 - origin.z, dir.z, 0, hit);
    considerRoomPlane(origin, dir, -1.7 - origin.y, dir.y, 1, hit);
    considerRoomPlane(origin, dir,  1.7 - origin.y, dir.y, 1, hit);
    considerRoomPlane(origin, dir, -2.8 - origin.x, dir.x, 2, hit);
    considerRoomPlane(origin, dir,  2.8 - origin.x, dir.x, 2, hit);

    considerBox(origin, dir, vec3(-1.15, -1.45, -1.25), vec3(-0.55, -0.65, -0.65), vec3(213.0, 135.0,  64.0), hit);
    considerBox(origin, dir, vec3( 0.38, -1.35, -2.75), vec3( 1.15, -0.35, -1.95), vec3( 56.0, 164.0, 139.0), hit);
    considerBox(origin, dir, vec3(-0.48, -0.55, -4.35), vec3( 0.35,  0.45,  -3.8), vec3(110.0,  99.0, 196.0), hit);
    considerAimMarker(origin, dir, hit);
    return hit;
}

// Measure of the covered set — the union of [n - GRID_WIDTH, n + GRID_WIDTH] over
// the integers — up to `t`, expressed relative to cell `anchor` so the subtraction
// in gridCoverage() never cancels catastrophically at 32-bit precision.
float gridMeasure(float t, float anchor) {
    float cell = round(t);
    return (cell - anchor) * (2.0 * GRID_WIDTH) + clamp(t - cell + GRID_WIDTH, 0.0, 2.0 * GRID_WIDTH);
}

// With antialiasing off this is exactly gridLine(): a hard threshold on the distance
// to the nearest grid line, in cell units. With it on, the same set is box-filtered
// over the pixel's footprint, so a line thinner than a pixel converges on its duty
// cycle instead of dropping in and out as the head moves.
float gridCoverage(float cell, float footprint) {
    if (uAntialias == 0)
        return abs(cell - round(cell)) < GRID_WIDTH ? 1.0 : 0.0;

    float radius = max(footprint * 0.5, 1.0e-7);
    float anchor = round(cell);
    return clamp((gridMeasure(cell + radius, anchor) - gridMeasure(cell - radius, anchor)) / (2.0 * radius), 0.0, 1.0);
}

// Deferred shading, evaluated once for the surviving hit exactly as trace() does on
// the CPU. With antialiasing off, mix() collapses to one of its two endpoints
// bit-exactly, so the whole expression reduces to the reference's shade() call.
vec3 resolve(SHit hit, vec2 footprint) {
    if (hit.dist >= FAR_DISTANCE)
        return vec3(10.0, 14.0, 18.0);

    vec3 color = hit.base;
    if (hit.grid)
        color = mix(hit.base, hit.lineColor, max(gridCoverage(hit.cell.x, footprint.x), gridCoverage(hit.cell.y, footprint.y)));

    if (hit.atten < 0.0)
        return color;

    return round(color * clamp(1.0 / (1.0 + hit.dist * hit.atten), 0.0, 1.0));
}

void main() {
    SPixel pixel = packedPixel();
    vec3   eye   = pixel.pane == 0 ? uEyeLeft : uEyeRight;

    float surfaceX = ((float(pixel.x) + 0.5) / float(uPane.x) - 0.5) * uPortal.x;
    float surfaceY = (0.5 - (float(pixel.y) + 0.5) / float(uPane.y)) * uPortal.y;
    vec3  dir      = vec3(surfaceX - eye.x, surfaceY - eye.y, -eye.z);

    // trace() and the derivative below run for every fragment, reticle included, so
    // fwidth() is always reached in uniform control flow.
    //
    // Across a silhouette the quad straddles two surfaces and the derivative is not a
    // footprint at all, but the filter is self-limiting: a huge radius integrates whole
    // cells and converges on the grid's duty cycle, which is exactly what an
    // infinitely distant surface should return. The artifact is therefore one rim of
    // wall pixels shaded as if very far away, not a smear.
    SHit hit   = trace(eye, dir);
    vec3 color = resolve(hit, fwidth(hit.cell));

    // A portal-locked center reticle makes the invariant visible: the cyan mark stays
    // fixed while the red authoritative world impact moves under viewer translation.
    int  centerX = uPane.x / 2;
    int  centerY = uPane.y / 2;
    bool narrowY = pixel.y + 1 >= centerY && pixel.y <= centerY + 1;
    bool crossY  = pixel.y + 8 >= centerY && pixel.y <= centerY + 8;
    if ((narrowY && pixel.x + 8 >= centerX && pixel.x <= centerX + 8) || (crossY && pixel.x + 1 >= centerX && pixel.x <= centerX + 1))
        color = vec3(71.0, 225.0, 231.0);

    fragColor = vec4(color / 255.0, 1.0);
}
)GLSL";

    // Translation of fallbackPixel(). It is a hard-edged calibration target, so it is
    // deliberately never antialiased: the border and quarter marks exist to be
    // measured against the destination rectangle.
    constexpr const char* FALLBACK_FRAGMENT_SOURCE = R"GLSL(
vec3 fallbackPixel(int x, int y, int width, int height) {
    float nx        = (float(x) + 0.5) / float(width);
    float ny        = (float(y) + 0.5) / float(height);
    bool  border    = x < 3 || y < 3 || x + 3 >= width || y + 3 >= height;
    bool  crossMark = abs(nx - 0.5) < 0.004 || abs(ny - 0.5) < 0.006;
    bool  quarter   = abs(nx - 0.25) < 0.002 || abs(nx - 0.75) < 0.002 || abs(ny - 0.25) < 0.003 || abs(ny - 0.75) < 0.003;
    if (border)
        return vec3(48.0, 144.0, 168.0);
    if (crossMark)
        return vec3(195.0, 218.0, 220.0);
    if (quarter)
        return vec3(48.0, 87.0, 100.0);

    int cell = (x * 8 / width + y * 8 / height) & 1;
    return cell == 1 ? vec3(18.0, 28.0, 34.0) : vec3(13.0, 21.0, 27.0);
}

void main() {
    SPixel pixel = packedPixel();
    fragColor    = vec4(fallbackPixel(pixel.x, pixel.y, uPane.x, uPane.y) / 255.0, 1.0);
}
)GLSL";

    bool extensionListed(const char* list, std::string_view wanted) {
        if (!list)
            return false;

        std::string_view remaining = list;
        while (!remaining.empty()) {
            const size_t END   = remaining.find(' ');
            const auto   TOKEN = remaining.substr(0, END);
            if (TOKEN == wanted)
                return true;
            if (END == std::string_view::npos)
                return false;
            remaining = remaining.substr(END + 1);
        }
        return false;
    }

    std::string infoLog(GLuint object, bool program) {
        GLint length = 0;
        if (program)
            glGetProgramiv(object, GL_INFO_LOG_LENGTH, &length);
        else
            glGetShaderiv(object, GL_INFO_LOG_LENGTH, &length);

        std::vector<char> log(sc<size_t>(length > 0 ? length : 1), '\0');
        if (program)
            glGetProgramInfoLog(object, sc<GLsizei>(log.size()), nullptr, log.data());
        else
            glGetShaderInfoLog(object, sc<GLsizei>(log.size()), nullptr, log.data());
        return log.data();
    }

    GLuint compileStage(GLenum stage, const char* prologue, const char* body, std::string& error) {
        const GLuint SHADER = glCreateShader(stage);
        if (SHADER == 0) {
            error = "glCreateShader failed";
            return 0;
        }

        const std::array<const GLchar*, 2> STRINGS = {prologue, body};
        glShaderSource(SHADER, body ? 2 : 1, STRINGS.data(), nullptr);
        glCompileShader(SHADER);

        GLint compiled = GL_FALSE;
        glGetShaderiv(SHADER, GL_COMPILE_STATUS, &compiled);
        if (compiled == GL_TRUE)
            return SHADER;

        error = std::format("shader compilation failed: {}", infoLog(SHADER, false));
        glDeleteShader(SHADER);
        return 0;
    }

    GLuint linkProgram(const char* fragmentBody, std::string& error) {
        const GLuint VERTEX = compileStage(GL_VERTEX_SHADER, VERTEX_SOURCE, nullptr, error);
        if (VERTEX == 0)
            return 0;

        const GLuint FRAGMENT = compileStage(GL_FRAGMENT_SHADER, FRAGMENT_PROLOGUE, fragmentBody, error);
        if (FRAGMENT == 0) {
            glDeleteShader(VERTEX);
            return 0;
        }

        const GLuint PROGRAM = glCreateProgram();
        glAttachShader(PROGRAM, VERTEX);
        glAttachShader(PROGRAM, FRAGMENT);
        glLinkProgram(PROGRAM);
        glDeleteShader(VERTEX);
        glDeleteShader(FRAGMENT);

        GLint linked = GL_FALSE;
        glGetProgramiv(PROGRAM, GL_LINK_STATUS, &linked);
        if (linked == GL_TRUE)
            return PROGRAM;

        error = std::format("program link failed: {}", infoLog(PROGRAM, true));
        glDeleteProgram(PROGRAM);
        return 0;
    }

    bool sizeValid(uint32_t packedWidth, uint32_t height) {
        return packedWidth >= 2 && (packedWidth & 1U) == 0 && height > 0;
    }

}

struct CPortalRendererGL::SState {
    EGLDisplay     display = EGL_NO_DISPLAY;
    EGLContext     context = EGL_NO_CONTEXT;
    EGLSurface     surface = EGL_NO_SURFACE;
    EGLConfig      config  = nullptr;
    wl_egl_window* window  = nullptr;

    GLuint         vertexArray     = 0;
    GLuint         framebuffer     = 0;
    GLuint         renderbuffer    = 0;
    GLuint         portalProgram   = 0;
    GLuint         fallbackProgram = 0;

    uint32_t             packedWidth = 0;
    uint32_t             height      = 0;
    bool                 antialias   = true;
    std::string          description;
    std::vector<uint8_t> readbackScratch;

    bool                 makeCurrent() const {
        return eglMakeCurrent(display, surface, surface, context) == EGL_TRUE;
    }

    // Selects a config whose alpha size is exactly zero, so a live window buffer is
    // opaque XRGB — the same pixel contract the shm path commits.
    bool chooseConfig(EGLint surfaceType, std::string& error) {
        const std::array<EGLint, 13> ATTRIBUTES = {
            EGL_SURFACE_TYPE, surfaceType, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 0, EGL_NONE,
        };

        EGLint total = 0;
        if (eglChooseConfig(display, ATTRIBUTES.data(), nullptr, 0, &total) != EGL_TRUE || total <= 0) {
            error = "no EGL config offers an 8-bit RGB ES3 renderable surface";
            return false;
        }

        std::vector<EGLConfig> configs(sc<size_t>(total));
        if (eglChooseConfig(display, ATTRIBUTES.data(), configs.data(), total, &total) != EGL_TRUE || total <= 0) {
            error = "eglChooseConfig could not enumerate configs";
            return false;
        }

        for (const auto& CANDIDATE : configs) {
            EGLint alpha = -1;
            if (eglGetConfigAttrib(display, CANDIDATE, EGL_ALPHA_SIZE, &alpha) == EGL_TRUE && alpha == 0) {
                config = CANDIDATE;
                return true;
            }
        }

        config = configs.front();
        return true;
    }

    bool createContext(std::string& error) {
        if (eglBindAPI(EGL_OPENGL_ES_API) != EGL_TRUE) {
            error = "eglBindAPI(EGL_OPENGL_ES_API) failed";
            return false;
        }

        const std::array<EGLint, 3> ATTRIBUTES = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE};
        context                                = eglCreateContext(display, config, EGL_NO_CONTEXT, ATTRIBUTES.data());
        if (context == EGL_NO_CONTEXT) {
            error = std::format("eglCreateContext for OpenGL ES 3 failed (0x{:x})", sc<uint32_t>(eglGetError()));
            return false;
        }
        return true;
    }

    bool buildPrograms(std::string& error) {
        portalProgram = linkProgram(PORTAL_FRAGMENT_SOURCE, error);
        if (portalProgram == 0)
            return false;

        fallbackProgram = linkProgram(FALLBACK_FRAGMENT_SOURCE, error);
        if (fallbackProgram == 0)
            return false;

        glGenVertexArrays(1, &vertexArray);
        glBindVertexArray(vertexArray);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glDisable(GL_SCISSOR_TEST);

        const auto* RENDERER = rc<const char*>(glGetString(GL_RENDERER));
        const auto* VENDOR   = eglQueryString(display, EGL_VENDOR);
        description          = std::format("{} via {}", RENDERER ? RENDERER : "unknown renderer", VENDOR ? VENDOR : "unknown EGL vendor");
        return true;
    }

    // Everything a draw shares: target, viewport, clear, program, packing uniform.
    // The caller adds its own uniforms and issues the single glDrawArrays.
    void beginDraw(GLuint program) const {
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glViewport(0, 0, sc<GLsizei>(packedWidth), sc<GLsizei>(height));
        // The reference fills the packed image with this color before drawing. Every
        // visible pixel is overwritten either way; the clear only keeps a partial
        // frame from ever showing something else.
        glClearColor(5.0F / 255.0F, 8.0F / 255.0F, 11.0F / 255.0F, 1.0F);
        glClear(GL_COLOR_BUFFER_BIT);

        glUseProgram(program);
        glUniform2i(glGetUniformLocation(program, "uPane"), sc<GLint>(packedWidth / 2U), sc<GLint>(height));
        glBindVertexArray(vertexArray);
    }

    void teardown() {
        if (display == EGL_NO_DISPLAY)
            return;

        if (context != EGL_NO_CONTEXT && makeCurrent()) {
            if (renderbuffer != 0)
                glDeleteRenderbuffers(1, &renderbuffer);
            if (framebuffer != 0)
                glDeleteFramebuffers(1, &framebuffer);
            if (vertexArray != 0)
                glDeleteVertexArrays(1, &vertexArray);
            if (portalProgram != 0)
                glDeleteProgram(portalProgram);
            if (fallbackProgram != 0)
                glDeleteProgram(fallbackProgram);
        }

        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (surface != EGL_NO_SURFACE)
            eglDestroySurface(display, surface);
        if (context != EGL_NO_CONTEXT)
            eglDestroyContext(display, context);
        if (window)
            wl_egl_window_destroy(window);
        eglTerminate(display);
        display = EGL_NO_DISPLAY;
    }
};

CPortalRendererGL::CPortalRendererGL() : m_state(std::make_unique<SState>()) {
    ;
}

CPortalRendererGL::~CPortalRendererGL() {
    m_state->teardown();
}

std::unique_ptr<CPortalRendererGL> CPortalRendererGL::offscreen(uint32_t packedWidth, uint32_t height, std::string& error) {
    error.clear();
    if (!sizeValid(packedWidth, height)) {
        error = "offscreen size needs an even width of at least 2 and a nonzero height";
        return nullptr;
    }

    if (!extensionListed(eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS), "EGL_MESA_platform_surfaceless")) {
        error = "EGL_MESA_platform_surfaceless is unavailable";
        return nullptr;
    }

    const auto GET_PLATFORM_DISPLAY = rc<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    if (!GET_PLATFORM_DISPLAY) {
        error = "eglGetPlatformDisplayEXT is unavailable";
        return nullptr;
    }

    auto  renderer = std::make_unique<CPortalRendererGL>();
    auto& state    = *renderer->m_state;

    state.display  = GET_PLATFORM_DISPLAY(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    if (state.display == EGL_NO_DISPLAY) {
        error = "eglGetPlatformDisplayEXT(EGL_PLATFORM_SURFACELESS_MESA) failed";
        return nullptr;
    }

    EGLint major = 0;
    EGLint minor = 0;
    if (eglInitialize(state.display, &major, &minor) != EGL_TRUE) {
        error         = std::format("eglInitialize on the surfaceless platform failed (0x{:x})", sc<uint32_t>(eglGetError()));
        state.display = EGL_NO_DISPLAY;
        return nullptr;
    }

    // EGL 1.5 folds EGL_KHR_surfaceless_context into core. The FBO below is the only
    // draw target, so no EGL surface is ever created for this back end.
    if ((major < 1 || (major == 1 && minor < 5)) && !extensionListed(eglQueryString(state.display, EGL_EXTENSIONS), "EGL_KHR_surfaceless_context")) {
        error = "EGL is older than 1.5 and lacks EGL_KHR_surfaceless_context";
        return nullptr;
    }

    if (!state.chooseConfig(EGL_PBUFFER_BIT, error) || !state.createContext(error))
        return nullptr;

    if (!state.makeCurrent()) {
        error = std::format("eglMakeCurrent without a surface failed (0x{:x})", sc<uint32_t>(eglGetError()));
        return nullptr;
    }

    if (!state.buildPrograms(error))
        return nullptr;

    glGenFramebuffers(1, &state.framebuffer);
    glGenRenderbuffers(1, &state.renderbuffer);
    if (!renderer->resize(packedWidth, height)) {
        error = "could not allocate the offscreen RGBA8 render target";
        return nullptr;
    }

    return renderer;
}

std::unique_ptr<CPortalRendererGL> CPortalRendererGL::onSurface(wl_display* display, wl_surface* surface, uint32_t packedWidth, uint32_t height, std::string& error) {
    error.clear();
    if (!display || !surface) {
        error = "a Wayland display and surface are required";
        return nullptr;
    }
    if (!sizeValid(packedWidth, height)) {
        error = "window size needs an even width of at least 2 and a nonzero height";
        return nullptr;
    }

    const auto* CLIENT_EXTENSIONS = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    if (!extensionListed(CLIENT_EXTENSIONS, "EGL_KHR_platform_wayland") && !extensionListed(CLIENT_EXTENSIONS, "EGL_EXT_platform_wayland")) {
        error = "no EGL Wayland platform extension";
        return nullptr;
    }

    const auto GET_PLATFORM_DISPLAY = rc<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    if (!GET_PLATFORM_DISPLAY) {
        error = "eglGetPlatformDisplayEXT is unavailable";
        return nullptr;
    }

    auto  renderer = std::make_unique<CPortalRendererGL>();
    auto& state    = *renderer->m_state;

    // No device is named here on purpose: Mesa resolves the wl_display to whatever
    // the compositor's dmabuf feedback advertises, which is what keeps the swap chain
    // zero-copy on a multi-GPU machine.
    state.display = GET_PLATFORM_DISPLAY(EGL_PLATFORM_WAYLAND_KHR, display, nullptr);
    if (state.display == EGL_NO_DISPLAY) {
        error = "eglGetPlatformDisplayEXT(EGL_PLATFORM_WAYLAND_KHR) failed";
        return nullptr;
    }

    if (eglInitialize(state.display, nullptr, nullptr) != EGL_TRUE) {
        error         = std::format("eglInitialize on the Wayland platform failed (0x{:x})", sc<uint32_t>(eglGetError()));
        state.display = EGL_NO_DISPLAY;
        return nullptr;
    }

    if (!state.chooseConfig(EGL_WINDOW_BIT, error) || !state.createContext(error))
        return nullptr;

    state.window = wl_egl_window_create(surface, sc<int>(packedWidth), sc<int>(height));
    if (!state.window) {
        error = "wl_egl_window_create failed";
        return nullptr;
    }

    state.surface = eglCreateWindowSurface(state.display, state.config, rc<EGLNativeWindowType>(state.window), nullptr);
    if (state.surface == EGL_NO_SURFACE) {
        error = std::format("eglCreateWindowSurface failed (0x{:x})", sc<uint32_t>(eglGetError()));
        return nullptr;
    }

    if (!state.makeCurrent()) {
        error = std::format("eglMakeCurrent on the window surface failed (0x{:x})", sc<uint32_t>(eglGetError()));
        return nullptr;
    }

    // The demo draws when a viewpoint sample arrives, not on a frame clock, and it
    // draws from inside the Wayland dispatch loop. A throttled swap would park that
    // loop, so the swap is left unthrottled exactly like the shm path's commit.
    eglSwapInterval(state.display, 0);

    if (!state.buildPrograms(error))
        return nullptr;

    state.packedWidth = packedWidth;
    state.height      = height;
    return renderer;
}

void CPortalRendererGL::setAntialiasGrid(bool enabled) {
    m_state->antialias = enabled;
}

bool CPortalRendererGL::antialiasGrid() const {
    return m_state->antialias;
}

uint32_t CPortalRendererGL::packedWidth() const {
    return m_state->packedWidth;
}

uint32_t CPortalRendererGL::height() const {
    return m_state->height;
}

std::string CPortalRendererGL::description() const {
    return m_state->description;
}

bool CPortalRendererGL::resize(uint32_t packedWidth, uint32_t height) {
    auto& state = *m_state;
    if (!sizeValid(packedWidth, height))
        return false;
    if (state.packedWidth == packedWidth && state.height == height)
        return true;
    if (!state.makeCurrent())
        return false;

    if (state.window) {
        wl_egl_window_resize(state.window, sc<int>(packedWidth), sc<int>(height), 0, 0);
        state.packedWidth = packedWidth;
        state.height      = height;
        return true;
    }

    glBindRenderbuffer(GL_RENDERBUFFER, state.renderbuffer);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, sc<GLsizei>(packedWidth), sc<GLsizei>(height));
    glBindFramebuffer(GL_FRAMEBUFFER, state.framebuffer);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, state.renderbuffer);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return false;

    state.packedWidth = packedWidth;
    state.height      = height;
    state.readbackScratch.assign(sc<size_t>(packedWidth) * height * 4U, 0);
    return true;
}

bool CPortalRendererGL::drawPortal(const SPortalSize& portal, const SStereoViews& views) {
    auto& state = *m_state;
    // Shared with renderPortalSBS(), so a scene the CPU path would refuse is refused
    // identically here.
    if (!portalSceneValid(portal, views))
        return false;
    if (state.packedWidth == 0 || !state.makeCurrent())
        return false;

    const GLuint PROGRAM = state.portalProgram;
    state.beginDraw(PROGRAM);
    glUniform2f(glGetUniformLocation(PROGRAM, "uPortal"), sc<GLfloat>(portal.widthMeters), sc<GLfloat>(portal.heightMeters));
    glUniform3f(glGetUniformLocation(PROGRAM, "uEyeLeft"), sc<GLfloat>(views.left.x), sc<GLfloat>(views.left.y), sc<GLfloat>(views.left.z));
    glUniform3f(glGetUniformLocation(PROGRAM, "uEyeRight"), sc<GLfloat>(views.right.x), sc<GLfloat>(views.right.y), sc<GLfloat>(views.right.z));
    glUniform1i(glGetUniformLocation(PROGRAM, "uAntialias"), state.antialias ? 1 : 0);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    return glGetError() == GL_NO_ERROR;
}

bool CPortalRendererGL::drawFallback() {
    auto& state = *m_state;
    if (state.packedWidth == 0 || !state.makeCurrent())
        return false;

    state.beginDraw(state.fallbackProgram);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    return glGetError() == GL_NO_ERROR;
}

bool CPortalRendererGL::finish() {
    if (!m_state->makeCurrent())
        return false;
    glFinish();
    return true;
}

bool CPortalRendererGL::present() {
    auto& state = *m_state;
    if (state.surface == EGL_NO_SURFACE)
        return false;
    return eglSwapBuffers(state.display, state.surface) == EGL_TRUE;
}

bool CPortalRendererGL::readback(const SImage& image) {
    auto& state = *m_state;
    if (state.window || state.framebuffer == 0)
        return false;
    if (image.width != state.packedWidth || image.height != state.height || image.stridePixels < image.width)
        return false;
    if (image.pixels.size() < sc<size_t>(image.height - 1) * image.stridePixels + image.width)
        return false;
    if (!state.makeCurrent())
        return false;

    glBindFramebuffer(GL_FRAMEBUFFER, state.framebuffer);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, sc<GLsizei>(state.packedWidth), sc<GLsizei>(state.height), GL_RGBA, GL_UNSIGNED_BYTE, state.readbackScratch.data());
    if (glGetError() != GL_NO_ERROR)
        return false;

    // glReadPixels hands back the bottom row first; the CPU image's row 0 is the top.
    for (uint32_t y = 0; y < state.height; ++y) {
        const uint8_t* source      = state.readbackScratch.data() + sc<size_t>(state.height - 1 - y) * state.packedWidth * 4U;
        uint32_t*      destination = image.pixels.data() + sc<size_t>(y) * image.stridePixels;
        for (uint32_t x = 0; x < state.packedWidth; ++x) {
            const uint8_t* texel = source + sc<size_t>(x) * 4U;
            destination[x]       = 0xFF000000U | (sc<uint32_t>(texel[0]) << 16U) | (sc<uint32_t>(texel[1]) << 8U) | sc<uint32_t>(texel[2]);
        }
    }
    return true;
}
