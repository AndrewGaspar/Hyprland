#include "XRSrgb.hpp"

#include <GLES3/gl3.h>
#include <hyprutils/memory/Casts.hpp>

#include <cstring>

// GL_EXT_sRGB_write_control (not in the core GLES3 headers).
static constexpr GLenum FRAMEBUFFER_SRGB_EXT = 0x8DB9;

bool                    OpenXR::disableSrgbWriteEncoding() {
    const auto* exts = Hyprutils::Memory::rc<const char*>(glGetString(GL_EXTENSIONS));
    if (!exts || !std::strstr(exts, "GL_EXT_sRGB_write_control"))
        return false;

    while (glGetError() != GL_NO_ERROR) {
        ;
    }

    glDisable(FRAMEBUFFER_SRGB_EXT);
    return glGetError() == GL_NO_ERROR;
}
