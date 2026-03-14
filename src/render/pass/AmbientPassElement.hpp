#pragma once
#include "PassElement.hpp"

class ITexture;

class CAmbientPassElement : public IPassElement {
  public:
    struct SAmbientData {
        SP<ITexture> tex;        // window surface texture
        CBox         windowBox;  // window size in transformed coords
        CBox         monitorBox; // monitor size in transformed coords
        float        a = 1.F;   // overall alpha
    };

    CAmbientPassElement(const SAmbientData& data);
    virtual ~CAmbientPassElement() = default;

    virtual bool                needsLiveBlur();
    virtual bool                needsPrecomputeBlur();
    virtual std::optional<CBox> boundingBox();
    virtual CRegion             opaqueRegion();

    virtual const char*         passName() {
        return "CAmbientPassElement";
    }

    virtual ePassElementType type() {
        return EK_AMBIENT;
    };

    SAmbientData m_data;
};
