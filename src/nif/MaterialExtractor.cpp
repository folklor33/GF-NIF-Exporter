#include "nif/MaterialExtractor.hpp"

#include "obj/NiAVObject.h"
#include "obj/NiAlphaProperty.h"
#include "obj/NiMaterialProperty.h"
#include "obj/NiProperty.h"
#include "obj/NiSourceTexture.h"
#include "obj/NiTexturingProperty.h"
#include "obj/NiVertexColorProperty.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace gfnif {
namespace {

/*! Base texture slot index in NiTexturingProperty. The slot order is fixed by
 *  the NIF format; 0 is the diffuse/base map, which is all Phase 2 exports. */
constexpr int kBaseTextureSlot = 0;

void CopyColor3(const Niflib::Color3& src, float (&dst)[3]) {
    dst[0] = src.r;
    dst[1] = src.g;
    dst[2] = src.b;
}

bool NearlyEqual(float a, float b) { return std::fabs(a - b) < 1e-6f; }

bool SameColor(const float (&a)[3], const float (&b)[3]) {
    return NearlyEqual(a[0], b[0]) && NearlyEqual(a[1], b[1]) && NearlyEqual(a[2], b[2]);
}

/*! Two materials are interchangeable when every exported field matches. */
bool SameMaterial(const MaterialData& a, const MaterialData& b) {
    return a.diffuseTexturePath == b.diffuseTexturePath &&
           a.sourceTextureName == b.sourceTextureName && a.hasVertexColor == b.hasVertexColor &&
           a.textureFound == b.textureFound && SameColor(a.ambient, b.ambient) &&
           SameColor(a.diffuse, b.diffuse) && SameColor(a.specular, b.specular) &&
           SameColor(a.emissive, b.emissive) && NearlyEqual(a.glossiness, b.glossiness) &&
           NearlyEqual(a.alpha, b.alpha) && a.hasAlphaProperty == b.hasAlphaProperty &&
           a.alphaBlendEnabled == b.alphaBlendEnabled && a.srcBlendMode == b.srcBlendMode &&
           a.dstBlendMode == b.dstBlendMode && a.alphaTestEnabled == b.alphaTestEnabled &&
           a.alphaTestFunc == b.alphaTestFunc && a.alphaTestThreshold == b.alphaTestThreshold;
}

} // namespace

MaterialExtractor::MaterialExtractor(const TextureResolver& resolver,
                                     std::vector<std::string>* warnings)
    : resolver_(resolver), warnings_(warnings) {}

int MaterialExtractor::ExtractFor(Niflib::NiAVObject* geometry,
                                  std::vector<MaterialData>& materials) {
    if (geometry == nullptr) {
        return -1;
    }

    MaterialData mat;
    bool any = false;

    // NiAVObject::GetProperties() returns the properties attached to this block
    // only. GF models attach them per geometry rather than inheriting from a
    // parent NiNode, so no upward walk is needed here.
    for (const Niflib::Ref<Niflib::NiProperty>& prop : geometry->GetProperties()) {
        if (prop == NULL) {
            continue;
        }

        if (auto* matProp = dynamic_cast<Niflib::NiMaterialProperty*>(
                static_cast<Niflib::NiProperty*>(prop))) {
            CopyColor3(matProp->GetAmbientColor(), mat.ambient);
            CopyColor3(matProp->GetDiffuseColor(), mat.diffuse);
            CopyColor3(matProp->GetSpecularColor(), mat.specular);
            CopyColor3(matProp->GetEmissiveColor(), mat.emissive);
            mat.glossiness = matProp->GetGlossiness();
            // niflib exposes this as "transparency"; in the NIF it is an alpha
            // value where 1.0 means fully opaque. Clamped: see MaterialData::alpha.
            mat.alpha = std::clamp(matProp->GetTransparency(), 0.0f, 1.0f);
            any = true;
        } else if (auto* alphaProp = dynamic_cast<Niflib::NiAlphaProperty*>(
                       static_cast<Niflib::NiProperty*>(prop))) {
            mat.hasAlphaProperty = true;
            mat.alphaBlendEnabled = alphaProp->GetBlendState();
            mat.srcBlendMode = static_cast<uint8_t>(alphaProp->GetSourceBlendFunc());
            mat.dstBlendMode = static_cast<uint8_t>(alphaProp->GetDestBlendFunc());
            mat.alphaTestEnabled = alphaProp->GetTestState();
            mat.alphaTestFunc = static_cast<uint8_t>(alphaProp->GetTestFunc());
            mat.alphaTestThreshold = alphaProp->GetTestThreshold();
            any = true;
        } else if (auto* texProp = dynamic_cast<Niflib::NiTexturingProperty*>(
                       static_cast<Niflib::NiProperty*>(prop))) {
            if (texProp->GetTextureCount() > kBaseTextureSlot) {
                Niflib::TexDesc& base = texProp->GetTexture(kBaseTextureSlot);
                if (base.source != NULL) {
                    mat.sourceTextureName = base.source->GetTextureFileName();
                    if (!mat.sourceTextureName.empty()) {
                        const TextureResolver::Result res =
                            resolver_.Resolve(mat.sourceTextureName);
                        mat.textureFound = res.found;
                        mat.diffuseTexturePath = res.path;
                        if (!res.found && warnings_ != nullptr) {
                            warnings_->push_back("texture not found: " + mat.sourceTextureName +
                                                 " (looked for " + res.searchedFor + ")");
                        }
                    }
                }
            }
            any = true;
        } else if (dynamic_cast<Niflib::NiVertexColorProperty*>(
                       static_cast<Niflib::NiProperty*>(prop)) != nullptr) {
            mat.hasVertexColor = true;
            any = true;
        }
    }

    if (!any) {
        return -1;
    }

    for (size_t i = 0; i < materials.size(); ++i) {
        if (SameMaterial(materials[i], mat)) {
            return static_cast<int>(i);
        }
    }
    materials.push_back(mat);
    return static_cast<int>(materials.size() - 1);
}

} // namespace gfnif
