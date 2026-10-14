#include <openxr/XRSrgb.hpp>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

#include <array>
#include <cstdint>

#include <gtest/gtest.h>

// OpenXR::disableSrgbWriteEncoding (XRSrgb.cpp) is the fix for washed-out XR monitors: the
// GL_SRGB8_ALPHA8 swapchains must receive the desktop's already-sRGB-encoded bytes UNCHANGED, but
// GLES encodes linear->sRGB on every write to an sRGB attachment by default. These tests run the
// exact GL operations CXRGraphics uses (shader draw, glClear, glBlitFramebuffer in and out of the
// swapchain) on a real surfaceless GLES3 context and read the stored bytes back. They self-skip
// when no EGL/GLES3 context (or the extension) is available.

static constexpr int     SIZE = 4;
static constexpr uint8_t MID  = 128; // sRGB-encoded mid-grey; double-encoded it becomes ~188

class XRSrgbWrites : public ::testing::Test {
  protected:
    void SetUp() override {
        m_display = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
        if (m_display == EGL_NO_DISPLAY || !eglInitialize(m_display, nullptr, nullptr))
            GTEST_SKIP() << "no surfaceless EGL display";
        m_initialized = true;
        eglBindAPI(EGL_OPENGL_ES_API);
        const EGLint ATTRS[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        m_context            = eglCreateContext(m_display, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ATTRS);
        if (m_context == EGL_NO_CONTEXT || !eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, m_context))
            GTEST_SKIP() << "no GLES3 context";

        const char* vs = "#version 300 es\n"
                         "void main() { vec2 p = vec2(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0); gl_Position = vec4(p, 0.0, 1.0); }";
        const char* fs = "#version 300 es\nprecision highp float; uniform sampler2D uTex; out vec4 o;\n"
                         "void main() { o = vec4(texture(uTex, vec2(0.5)).rgb, 1.0); }";
        GLuint      v  = glCreateShader(GL_VERTEX_SHADER);
        glShaderSource(v, 1, &vs, nullptr);
        glCompileShader(v);
        GLuint f = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(f, 1, &fs, nullptr);
        glCompileShader(f);
        m_prog = glCreateProgram();
        glAttachShader(m_prog, v);
        glAttachShader(m_prog, f);
        glLinkProgram(m_prog);
        glDeleteShader(v);
        glDeleteShader(f);
        GLint linked = 0;
        glGetProgramiv(m_prog, GL_LINK_STATUS, &linked);
        if (!linked)
            GTEST_SKIP() << "test shader failed to link";
        glGenVertexArrays(1, &m_vao);
    }

    void TearDown() override {
        if (!m_initialized)
            return;
        if (m_context != EGL_NO_CONTEXT) {
            eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            eglDestroyContext(m_display, m_context);
        }
        eglTerminate(m_display);
    }

    // A SIZE x SIZE texture of internalFormat, optionally filled with MID.
    static GLuint makeTex(GLenum internalFormat, bool fill) {
        std::array<uint8_t, SIZE * SIZE * 4> px = {};
        px.fill(MID);
        GLuint t = 0;
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, SIZE, SIZE, 0, GL_RGBA, GL_UNSIGNED_BYTE, fill ? px.data() : nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        return t;
    }

    static GLuint bindDrawFbo(GLuint tex) {
        GLuint fbo = 0;
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        return fbo;
    }

    // The STORED red byte (glReadPixels never converts sRGB in GLES).
    static int stored(GLuint tex) {
        GLuint                 fbo = bindDrawFbo(tex);
        std::array<uint8_t, 4> p   = {};
        glReadPixels(1, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p.data());
        glDeleteFramebuffers(1, &fbo);
        return p[0];
    }

    // CXRGraphics' content pass: a raw (UNORM / external-OES) desktop texture drawn into the swapchain.
    void draw(GLuint src, GLuint dst) {
        GLuint fbo = bindDrawFbo(dst);
        glViewport(0, 0, SIZE, SIZE);
        glUseProgram(m_prog);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, src);
        glUniform1i(glGetUniformLocation(m_prog, "uTex"), 0);
        glBindVertexArray(m_vao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glDeleteFramebuffers(1, &fbo);
    }

    static void blit(GLuint src, GLuint dst) {
        GLuint fbos[2] = {};
        glGenFramebuffers(2, fbos);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fbos[0]);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, src, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbos[1]);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst, 0);
        glBlitFramebuffer(0, 0, SIZE, SIZE, 0, 0, SIZE, SIZE, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glDeleteFramebuffers(2, fbos);
    }

    EGLDisplay m_display     = EGL_NO_DISPLAY;
    EGLContext m_context     = EGL_NO_CONTEXT;
    bool       m_initialized = false;
    GLuint     m_prog        = 0;
    GLuint     m_vao         = 0;
};

// The bug, pinned: without the fix GLES re-encodes the already-encoded desktop on write.
TEST_F(XRSrgbWrites, DefaultGlesDoubleEncodes) {
    const GLuint SRC       = makeTex(GL_RGBA8, true);
    const GLuint SWAPCHAIN = makeTex(GL_SRGB8_ALPHA8, false);
    draw(SRC, SWAPCHAIN);
    EXPECT_NEAR(stored(SWAPCHAIN), 188, 2);
}

TEST_F(XRSrgbWrites, RawWritesAfterDisable) {
    if (!OpenXR::disableSrgbWriteEncoding())
        GTEST_SKIP() << "GL_EXT_sRGB_write_control unavailable";

    const GLuint SRC       = makeTex(GL_RGBA8, true);
    const GLuint SWAPCHAIN = makeTex(GL_SRGB8_ALPHA8, false);

    // Content pass (dmabuf / CPU-keyed shader).
    draw(SRC, SWAPCHAIN);
    EXPECT_EQ(stored(SWAPCHAIN), MID);

    // Clears (margins, black fallback, chrome fills).
    GLuint fbo = bindDrawFbo(SWAPCHAIN);
    glClearColor(MID / 255.F, 0.F, 0.F, 1.F);
    glClear(GL_COLOR_BUFFER_BIT);
    glDeleteFramebuffers(1, &fbo);
    EXPECT_EQ(stored(SWAPCHAIN), MID);

    // CPU-staging blit into the swapchain.
    const GLuint OTHER = makeTex(GL_SRGB8_ALPHA8, false);
    blit(SRC, OTHER);
    EXPECT_EQ(stored(OTHER), MID);

    // Chrome snapshot: swapchain -> UNORM snapshot -> swapchain must be lossless.
    const GLuint SNAPSHOT = makeTex(GL_RGBA8, false);
    blit(OTHER, SNAPSHOT);
    EXPECT_EQ(stored(SNAPSHOT), MID);
    const GLuint RESTORED = makeTex(GL_SRGB8_ALPHA8, false);
    blit(SNAPSHOT, RESTORED);
    EXPECT_EQ(stored(RESTORED), MID);
}
