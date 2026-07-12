#include <openxr/XRDmabufImport.hpp>

#include <gtest/gtest.h>

#include <drm_fourcc.h> // DRM_FORMAT_MOD_INVALID
#include <optional>
#include <vector>

using namespace OpenXR;

// buildDmabufImportAttribs (XRDmabufImport.cpp) is the pure, HAVE_OPENXR-free attrib-list builder the
// XR blit hands to eglCreateImageKHR. WP-L2: the cross-GPU black screen was the modifier attribs
// being absent — NVIDIA rejects modifier-less imports. These tests pin the with/without-modifier,
// INVALID-modifier, and multi-plane behavior without a runtime.

namespace {
    // Return the value that follows `token` in the attrib list, or nullopt if the token is absent.
    // Attribs are (token, value) pairs terminated by EGL_NONE.
    std::optional<EGLint> attribValue(const std::vector<EGLint>& a, EGLint token) {
        for (size_t i = 0; i + 1 < a.size() && a[i] != EGL_NONE; i += 2) {
            if (a[i] == token)
                return a[i + 1];
        }
        return std::nullopt;
    }
    bool hasToken(const std::vector<EGLint>& a, EGLint token) {
        return attribValue(a, token).has_value();
    }
    // The list must be EGL_NONE-terminated and have an odd length (N pairs + terminator).
    void expectWellFormed(const std::vector<EGLint>& a) {
        ASSERT_FALSE(a.empty());
        EXPECT_EQ(a.back(), EGL_NONE);
        EXPECT_EQ(a.size() % 2u, 1u);
    }
}

// ---- core header attribs are always present ----

TEST(XRDmabufAttribs, EmitsSizeFormatAndPlane0) {
    auto a = buildDmabufImportAttribs(1920, 1080, DRM_FORMAT_XRGB8888, {{7, 0, 7680}}, DRM_FORMAT_MOD_LINEAR, true);
    expectWellFormed(a);
    EXPECT_EQ(attribValue(a, EGL_WIDTH), 1920);
    EXPECT_EQ(attribValue(a, EGL_HEIGHT), 1080);
    EXPECT_EQ(attribValue(a, EGL_LINUX_DRM_FOURCC_EXT), (EGLint)DRM_FORMAT_XRGB8888);
    EXPECT_EQ(attribValue(a, EGL_DMA_BUF_PLANE0_FD_EXT), 7);
    EXPECT_EQ(attribValue(a, EGL_DMA_BUF_PLANE0_OFFSET_EXT), 0);
    EXPECT_EQ(attribValue(a, EGL_DMA_BUF_PLANE0_PITCH_EXT), 7680);
}

// ---- WP-L2 core: modifiers passed iff supported AND explicit ----

TEST(XRDmabufAttribs, PassesModifiersWhenSupported) {
    // LINEAR is modifier 0x0 — the exact case that failed on NVIDIA (research/17 §4.2).
    auto a = buildDmabufImportAttribs(1920, 1080, DRM_FORMAT_XRGB8888, {{7, 0, 7680}}, DRM_FORMAT_MOD_LINEAR, true);
    ASSERT_TRUE(hasToken(a, EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT));
    ASSERT_TRUE(hasToken(a, EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT));
    EXPECT_EQ(attribValue(a, EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT), (EGLint)(uint32_t)(DRM_FORMAT_MOD_LINEAR & 0xFFFFFFFF));
    EXPECT_EQ(attribValue(a, EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT), (EGLint)(uint32_t)(DRM_FORMAT_MOD_LINEAR >> 32));
}

TEST(XRDmabufAttribs, SplitsHighAndLowModifierWords) {
    // A vendor modifier with both words set (e.g. an AMD tiling modifier shape).
    const uint64_t mod = 0x0123456789ABCDEFull;
    auto           a   = buildDmabufImportAttribs(64, 64, DRM_FORMAT_ARGB8888, {{3, 16, 256}}, mod, true);
    EXPECT_EQ((uint32_t)*attribValue(a, EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT), 0x89ABCDEFu);
    EXPECT_EQ((uint32_t)*attribValue(a, EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT), 0x01234567u);
}

TEST(XRDmabufAttribs, OmitsModifiersWhenExtUnavailable) {
    // No EGL_EXT_image_dma_buf_import_modifiers -> legacy list, do not regress those drivers.
    auto a = buildDmabufImportAttribs(1920, 1080, DRM_FORMAT_XRGB8888, {{7, 0, 7680}}, DRM_FORMAT_MOD_LINEAR, false);
    EXPECT_FALSE(hasToken(a, EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT));
    EXPECT_FALSE(hasToken(a, EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT));
    expectWellFormed(a);
}

TEST(XRDmabufAttribs, OmitsInvalidModifierEvenWhenSupported) {
    // DRM_FORMAT_MOD_INVALID means "implicit" — passing it as an explicit attrib is itself a
    // BAD_ATTRIBUTE on strict drivers, so it must be omitted (matches OpenGL.cpp:646).
    auto a = buildDmabufImportAttribs(1920, 1080, DRM_FORMAT_XRGB8888, {{7, 0, 7680}}, DRM_FORMAT_MOD_INVALID, true);
    EXPECT_FALSE(hasToken(a, EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT));
    EXPECT_FALSE(hasToken(a, EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT));
    expectWellFormed(a);
}

// ---- multi-plane ----

TEST(XRDmabufAttribs, MultiPlaneEmitsPerPlaneAttribsAndModifiers) {
    auto a = buildDmabufImportAttribs(1920, 1080, DRM_FORMAT_NV12, {{10, 0, 1920}, {11, 4096, 1920}}, DRM_FORMAT_MOD_LINEAR, true);
    // Plane 0
    EXPECT_EQ(attribValue(a, EGL_DMA_BUF_PLANE0_FD_EXT), 10);
    EXPECT_EQ(attribValue(a, EGL_DMA_BUF_PLANE0_OFFSET_EXT), 0);
    EXPECT_EQ(attribValue(a, EGL_DMA_BUF_PLANE0_PITCH_EXT), 1920);
    EXPECT_TRUE(hasToken(a, EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT));
    // Plane 1
    EXPECT_EQ(attribValue(a, EGL_DMA_BUF_PLANE1_FD_EXT), 11);
    EXPECT_EQ(attribValue(a, EGL_DMA_BUF_PLANE1_OFFSET_EXT), 4096);
    EXPECT_EQ(attribValue(a, EGL_DMA_BUF_PLANE1_PITCH_EXT), 1920);
    EXPECT_TRUE(hasToken(a, EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT));
    EXPECT_TRUE(hasToken(a, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT));
    // No plane 2
    EXPECT_FALSE(hasToken(a, EGL_DMA_BUF_PLANE2_FD_EXT));
    expectWellFormed(a);
}

TEST(XRDmabufAttribs, ClampsPlaneCountToFour) {
    // Five plane descriptors -> at most 4 planes emitted (EGL only defines PLANE0..3).
    auto a = buildDmabufImportAttribs(8, 8, DRM_FORMAT_XRGB8888, {{1, 0, 32}, {2, 0, 32}, {3, 0, 32}, {4, 0, 32}, {5, 0, 32}}, DRM_FORMAT_MOD_LINEAR, true);
    EXPECT_TRUE(hasToken(a, EGL_DMA_BUF_PLANE3_FD_EXT));
    EXPECT_EQ(attribValue(a, EGL_DMA_BUF_PLANE3_FD_EXT), 4);
    expectWellFormed(a);
}

// ---- content-path names ----

TEST(XRDmabufAttribs, ContentPathNames) {
    EXPECT_STREQ(xrContentPathName(XR_CONTENT_NONE), "none");
    EXPECT_STREQ(xrContentPathName(XR_CONTENT_DMABUF), "dmabuf");
    EXPECT_STREQ(xrContentPathName(XR_CONTENT_CPU), "cpu");
    EXPECT_STREQ(xrContentPathName(XR_CONTENT_BLACK), "black");
    EXPECT_STREQ(xrContentPathName(200), "none"); // unknown -> none
}
