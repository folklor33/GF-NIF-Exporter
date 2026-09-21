#include "nif/MaterialAnimationExtractor.hpp"

#include "texture/TextureResolver.hpp"

#include "niflib.h"
#include "nif_math.h"
#include "obj/NiAVObject.h"
#include "obj/NiAlphaController.h"
#include "obj/NiBSplineCompFloatInterpolator.h"
#include "obj/NiBSplineCompPoint3Interpolator.h"
#include "obj/NiBoolData.h"
#include "obj/NiBoolInterpolator.h"
#include "obj/NiControllerSequence.h"
#include "obj/NiFlipController.h"
#include "obj/NiFloatData.h"
#include "obj/NiFloatInterpolator.h"
#include "obj/NiInterpolator.h"
#include "obj/NiMaterialColorController.h"
#include "obj/NiObjectNET.h"
#include "obj/NiParticleSystem.h"
#include "obj/NiPoint3Interpolator.h"
#include "obj/NiPosData.h"
#include "obj/NiProperty.h"
#include "obj/NiSingleInterpController.h"
#include "obj/NiSourceTexture.h"
#include "obj/NiTextureTransformController.h"
#include "obj/NiTimeController.h"
#include "obj/NiTriBasedGeom.h"
#include "obj/NiVisController.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <set>
#include <sstream>

namespace gfnif {
namespace {

using Niflib::NiObject;
using Niflib::NiObjectRef;

/*! Same rate Phase 4 resamples a B-spline transform track at, reused here for
 *  NiBSplineCompFloatInterpolator so a clip never mixes two sampling rates.
 *  See PHASE4_FINDINGS for the error/size measurements it rests on. */
constexpr float kBSplineSampleRateHz = 30.0f;
constexpr int kBSplineDegree = 3;

/*! A track whose every key holds the same value animates nothing. Measured on
 *  the corpus, 70% of the non-transform .kf tracks are like this -- 294 k
 *  tracks and 589 k keys that would cost output size and produce no motion
 *  (docs/PHASE8_FINDINGS.md Etape 1). Dropped, and counted as dropped. */
bool IsConstant(const std::vector<float>& values, int components) {
    if (values.size() <= static_cast<size_t>(components)) return true;
    for (size_t i = static_cast<size_t>(components); i < values.size(); ++i) {
        if (std::fabs(values[i] - values[i % components]) > 1e-6f) return false;
    }
    return true;
}

// --------------------------------------------------------------------------
// reading an interpolator into a MaterialTrack's flat key arrays
// --------------------------------------------------------------------------

/*! Fills `track.times`/`track.values` from `interp`.
 *
 *  Returns false when there is nothing to read -- either the interpolator is
 *  absent, or it is a type niflib cannot decode. Both are normal outcomes and
 *  are counted separately by the caller; neither is an error. */
bool ReadInterpolator(Niflib::NiInterpolator* interp, MaterialTrack& track,
                      MaterialAnimationStats& stats) {
    if (interp == nullptr) return false;
    const int components = track.ComponentCount();

    if (auto* fi = dynamic_cast<Niflib::NiFloatInterpolator*>(interp)) {
        Niflib::Ref<Niflib::NiFloatData> data = fi->GetData();
        if (data == NULL) return false;
        for (const Niflib::Key<float>& k : data->GetKeys()) {
            track.times.push_back(k.time);
            // A 3-component property driven by a scalar interpolator does not
            // occur in this corpus, but replicating the value across the
            // components keeps the arrays consistent rather than ragged if it
            // ever does.
            for (int c = 0; c < components; ++c) track.values.push_back(k.data);
        }
        return !track.times.empty();
    }

    if (auto* pi = dynamic_cast<Niflib::NiPoint3Interpolator*>(interp)) {
        Niflib::Ref<Niflib::NiPosData> data = pi->GetData();
        if (data == NULL) return false;
        for (const Niflib::Key<Niflib::Vector3>& k : data->GetKeys()) {
            track.times.push_back(k.time);
            // A colour, stored by the NIF in a Vector3: x/y/z are r/g/b.
            track.values.push_back(k.data.x);
            if (components >= 2) track.values.push_back(k.data.y);
            if (components >= 3) track.values.push_back(k.data.z);
        }
        return !track.times.empty();
    }

    if (auto* bi = dynamic_cast<Niflib::NiBoolInterpolator*>(interp)) {
        Niflib::Ref<Niflib::NiBoolData> data = bi->GetData();
        if (data == NULL) return false;
        for (const Niflib::Key<unsigned char>& k : data->GetKeys()) {
            track.times.push_back(k.time);
            // Written as a float 0/1 rather than a byte so every material
            // track has one value type in the .gfbin and a consumer needs one
            // accessor path. NiVisData's keys are step-interpolated by
            // definition; a consumer must not smooth them.
            for (int c = 0; c < components; ++c) {
                track.values.push_back(k.data != 0 ? 1.0f : 0.0f);
            }
        }
        return !track.times.empty();
    }

    // Compressed B-spline float: niflib exposes SampleKeys() for this one, so
    // it is resampled exactly as Phase 4 resamples a transform B-spline.
    if (auto* bf = dynamic_cast<Niflib::NiBSplineCompFloatInterpolator*>(interp)) {
        const float start = bf->GetStartTime();
        const float stop = bf->GetStopTime();
        const float duration = stop - start;
        if (!(duration > 0.0f)) return false;
        const int npoints =
            std::max(2, static_cast<int>(std::ceil(duration * kBSplineSampleRateHz)) + 1);
        const std::vector<Niflib::Key<float>> keys = bf->SampleKeys(npoints, kBSplineDegree);
        if (keys.empty()) return false;
        for (const Niflib::Key<float>& k : keys) {
            track.times.push_back(k.time);
            for (int c = 0; c < components; ++c) track.values.push_back(k.data);
        }
        track.wasResampledFromBSpline = true;
        ++stats.bSplineFloatResampled;
        return true;
    }

    // Compressed B-spline Point3 (3600 links corpus-wide, all of them animated
    // colours): niflib models NiBSplinePoint3Interpolator as six undecoded
    // "Unknown Floats" and generates no sampler for it -- read the class, the
    // control-point offset/bias/multiplier the float variant exposes simply do
    // not exist here. Reconstructing them by reinterpreting those six floats
    // would be a guess, and this project does not export guessed values.
    // Counted so the gap is visible in the corpus summary.
    if (dynamic_cast<Niflib::NiBSplineCompPoint3Interpolator*>(interp) != nullptr) {
        ++stats.bSplinePoint3Unsupported;
        return false;
    }

    return false;
}

// --------------------------------------------------------------------------
// controller -> property
// --------------------------------------------------------------------------

MaterialTrackProperty PropertyForTexTransform(Niflib::TexTransform t) {
    switch (t) {
        case Niflib::TT_TRANSLATE_U: return MaterialTrackProperty::UvTranslateU;
        case Niflib::TT_TRANSLATE_V: return MaterialTrackProperty::UvTranslateV;
        case Niflib::TT_ROTATE: return MaterialTrackProperty::UvRotation;
        case Niflib::TT_SCALE_U: return MaterialTrackProperty::UvScaleU;
        case Niflib::TT_SCALE_V: return MaterialTrackProperty::UvScaleV;
    }
    return MaterialTrackProperty::UvTranslateU;
}

/*! niflib generates no getter for NiPoint3InterpController::targetColor, but
 *  its asString() prints it as the enum's own text -- the same mechanism
 *  ParticleExtractor.cpp already relies on for the particle scalars, and for
 *  the same reason (see the long comment on AsStringFields there). */
MaterialTrackProperty PropertyForMaterialColor(NiObject* ctrl, bool& outKnown) {
    outKnown = true;
    std::istringstream in(ctrl->asString(false));
    std::string line;
    while (std::getline(in, line)) {
        const size_t sep = line.find("Target Color:  ");
        if (sep == std::string::npos) continue;
        const std::string value = line.substr(sep + 15);
        if (value.rfind("TC_AMBIENT", 0) == 0) return MaterialTrackProperty::AmbientColor;
        if (value.rfind("TC_DIFFUSE", 0) == 0) return MaterialTrackProperty::DiffuseColor;
        if (value.rfind("TC_SPECULAR", 0) == 0) return MaterialTrackProperty::SpecularColor;
        if (value.rfind("TC_SELF_ILLUM", 0) == 0) return MaterialTrackProperty::EmissiveColor;
        break;
    }
    outKnown = false;
    return MaterialTrackProperty::EmissiveColor;
}

/*! Parses a .kf ControllerLink's `variable1`, which is the only place the
 *  sequence states what a NiTextureTransformController or a
 *  NiMaterialColorController drives.
 *
 *  Verified against the corpus before being relied on: the 218 476 links whose
 *  controllerType is NiTextureTransformController carry exactly the 18 distinct
 *  "<uvSet>-<slot>-TT_*" strings and nothing else, and the 11 691
 *  NiMaterialColorController links carry exactly SELF_ILLUM / DIFF / SPEC
 *  (docs/PHASE8_FINDINGS.md Etape 1). An unrecognised string is reported, not
 *  guessed at. */
bool PropertyFromVariable1(const std::string& controllerType, const std::string& var1,
                           MaterialTrackProperty& outProperty, int& outSlot) {
    outSlot = -1;
    if (controllerType == "NiAlphaController") {
        outProperty = MaterialTrackProperty::Alpha;
        return true;
    }
    if (controllerType == "NiVisController") {
        outProperty = MaterialTrackProperty::Visible;
        return true;
    }
    if (controllerType == "NiFlipController") {
        outProperty = MaterialTrackProperty::TextureIndex;
        // variable1 is the bare slot number here ("0" on every link measured).
        outSlot = std::atoi(var1.c_str());
        return true;
    }
    if (controllerType == "NiMaterialColorController") {
        if (var1 == "SELF_ILLUM") outProperty = MaterialTrackProperty::EmissiveColor;
        else if (var1 == "DIFF") outProperty = MaterialTrackProperty::DiffuseColor;
        else if (var1 == "SPEC") outProperty = MaterialTrackProperty::SpecularColor;
        else if (var1 == "AMB") outProperty = MaterialTrackProperty::AmbientColor;
        else return false;
        return true;
    }
    if (controllerType == "NiTextureTransformController") {
        // "<uvSet>-<textureSlot>-TT_<transform>"
        const size_t first = var1.find('-');
        if (first == std::string::npos) return false;
        const size_t second = var1.find('-', first + 1);
        if (second == std::string::npos) return false;
        outSlot = std::atoi(var1.substr(first + 1, second - first - 1).c_str());
        const std::string tt = var1.substr(second + 1);
        if (tt == "TT_TRANSLATE_U") outProperty = MaterialTrackProperty::UvTranslateU;
        else if (tt == "TT_TRANSLATE_V") outProperty = MaterialTrackProperty::UvTranslateV;
        else if (tt == "TT_ROTATE") outProperty = MaterialTrackProperty::UvRotation;
        else if (tt == "TT_SCALE_U") outProperty = MaterialTrackProperty::UvScaleU;
        else if (tt == "TT_SCALE_V") outProperty = MaterialTrackProperty::UvScaleV;
        else return false;
        return true;
    }
    return false;
}

// --------------------------------------------------------------------------
// target resolution
// --------------------------------------------------------------------------

/*! Resolves a controller's owner to the geometry/system/node it ultimately
 *  affects. A controller attached to a NiProperty is attributed to every
 *  NiAVObject carrying that property: the corpus shares one property list
 *  between several shapes routinely (17 933 UV controllers sit on 28 924
 *  geometries), and the original engine drives all of them. */
void ResolveTargetsByPointer(Niflib::NiAVObject* owner, const MaterialTrackTargetIndex& index,
                             std::vector<MaterialTrackTarget>& out) {
    MaterialTrackTarget t;
    t.name = owner->GetName();

    if (index.particleSystems != nullptr) {
        auto it = index.particleSystems->find(owner);
        if (it != index.particleSystems->end()) {
            t.array = MaterialTrackTarget::Array::ParticleSystems;
            t.index = it->second;
            out.push_back(t);
            return;
        }
    }
    if (index.geometry != nullptr) {
        auto it = index.geometry->find(owner);
        if (it != index.geometry->end() && it->second.index >= 0) {
            t.array = it->second.inEmitterMeshes ? MaterialTrackTarget::Array::EmitterMeshes
                                                 : MaterialTrackTarget::Array::Meshes;
            t.index = it->second.index;
            out.push_back(t);
            return;
        }
    }
    if (index.nodes != nullptr) {
        auto it = index.nodes->find(owner);
        if (it != index.nodes->end()) {
            t.array = MaterialTrackTarget::Array::Nodes;
            t.index = it->second;
            out.push_back(t);
            return;
        }
    }
    if (index.skeleton != nullptr) {
        for (size_t i = 0; i < index.skeleton->bones.size(); ++i) {
            if (index.skeleton->bones[i].name == t.name) {
                t.array = MaterialTrackTarget::Array::Bones;
                t.index = static_cast<int>(i);
                out.push_back(t);
                return;
            }
        }
    }
    // Not exported at all (a helper gizmo, a hidden shape the filter dropped).
    // Emitted orphaned rather than silently discarded, exactly as an
    // unresolved AnimationTrack is.
    out.push_back(t);
}

/*! The .kf path: a ControllerLink names its target and nothing else, so the
 *  name is matched against everything the file exported. A name carried by
 *  several meshes produces one track per match -- the .kf genuinely does not
 *  say which, the same ambiguity Phase 7 6.2 measured for mesh emitters, and
 *  applying it to all of them is what the original engine's name binding comes
 *  closest to. Counted as ambiguous so it is visible rather than silent. */
void ResolveTargetsByName(const std::string& name, const MaterialTrackTargetIndex& index,
                          std::vector<MaterialTrackTarget>& out, MaterialAnimationStats& stats) {
    const size_t before = out.size();

    if (index.particleSystemData != nullptr) {
        for (size_t i = 0; i < index.particleSystemData->size(); ++i) {
            if ((*index.particleSystemData)[i].name != name) continue;
            MaterialTrackTarget t;
            t.array = MaterialTrackTarget::Array::ParticleSystems;
            t.index = static_cast<int>(i);
            t.name = name;
            out.push_back(t);
        }
    }
    if (index.meshes != nullptr) {
        for (size_t i = 0; i < index.meshes->size(); ++i) {
            if ((*index.meshes)[i].name != name) continue;
            MaterialTrackTarget t;
            t.array = MaterialTrackTarget::Array::Meshes;
            t.index = static_cast<int>(i);
            t.name = name;
            out.push_back(t);
        }
    }
    if (index.emitterMeshes != nullptr) {
        for (size_t i = 0; i < index.emitterMeshes->size(); ++i) {
            if ((*index.emitterMeshes)[i].name != name) continue;
            MaterialTrackTarget t;
            t.array = MaterialTrackTarget::Array::EmitterMeshes;
            t.index = static_cast<int>(i);
            t.name = name;
            out.push_back(t);
        }
    }
    if (out.size() > before + 1) {
        ++stats.kfAmbiguousTargets;
    }
    if (out.size() > before) return;

    // No geometry by that name: it may be a plain node (NiVisController's
    // dominant case -- 2308 of 2309 sit on a NiNode).
    if (index.sceneNodes != nullptr) {
        for (size_t i = 0; i < index.sceneNodes->size(); ++i) {
            if ((*index.sceneNodes)[i].name != name) continue;
            MaterialTrackTarget t;
            t.array = MaterialTrackTarget::Array::Nodes;
            t.index = static_cast<int>(i);
            t.name = name;
            out.push_back(t);
            return;
        }
    }
    if (index.skeleton != nullptr) {
        for (size_t i = 0; i < index.skeleton->bones.size(); ++i) {
            if (index.skeleton->bones[i].name != name) continue;
            MaterialTrackTarget t;
            t.array = MaterialTrackTarget::Array::Bones;
            t.index = static_cast<int>(i);
            t.name = name;
            out.push_back(t);
            return;
        }
    }

    MaterialTrackTarget t;
    t.name = name;
    out.push_back(t);
}

} // namespace

MaterialAnimationExtractor::MaterialAnimationExtractor(const TextureResolver& textures,
                                                       std::vector<std::string>* warnings)
    : textures_(textures), warnings_(warnings) {}

void MaterialAnimationExtractor::ExtractEmbedded(const std::vector<NiObject*>& blocks,
                                                 const MaterialTrackTargetIndex& index,
                                                 SceneData& scene,
                                                 MaterialAnimationStats& stats) {
    // property block -> the NiAVObjects carrying it, so a controller attached
    // to a NiMaterialProperty can be traced to the geometry it colours.
    std::map<NiObject*, std::vector<Niflib::NiAVObject*>> propertyOwners;
    for (NiObject* obj : blocks) {
        auto* av = dynamic_cast<Niflib::NiAVObject*>(obj);
        if (av == nullptr) continue;
        for (const Niflib::Ref<Niflib::NiProperty>& p : av->GetProperties()) {
            if (p != NULL) propertyOwners[static_cast<NiObject*>(p)].push_back(av);
        }
    }

    AnimationClip clip;
    clip.name = "embedded-material";
    clip.originFile = "embedded";
    clip.loop = true;

    for (NiObject* obj : blocks) {
        auto* owner = dynamic_cast<Niflib::NiObjectNET*>(obj);
        if (owner == nullptr) continue;

        for (const Niflib::Ref<Niflib::NiTimeController>& c : owner->GetControllers()) {
            if (c == NULL) continue;
            auto* ctrl = static_cast<Niflib::NiTimeController*>(c);
            NiObject* ctrlObj = static_cast<NiObject*>(ctrl);

            MaterialTrackProperty property = MaterialTrackProperty::Alpha;
            int slot = -1;
            bool recognised = true;

            if (auto* tt = dynamic_cast<Niflib::NiTextureTransformController*>(ctrl)) {
                property = PropertyForTexTransform(tt->GetTextureTransformType());
                slot = static_cast<int>(tt->GetTargetTextureSlot());
            } else if (dynamic_cast<Niflib::NiAlphaController*>(ctrl) != nullptr) {
                property = MaterialTrackProperty::Alpha;
            } else if (dynamic_cast<Niflib::NiMaterialColorController*>(ctrl) != nullptr) {
                bool known = false;
                property = PropertyForMaterialColor(ctrlObj, known);
                recognised = known;
            } else if (dynamic_cast<Niflib::NiVisController*>(ctrl) != nullptr) {
                property = MaterialTrackProperty::Visible;
            } else if (dynamic_cast<Niflib::NiFlipController*>(ctrl) != nullptr) {
                property = MaterialTrackProperty::TextureIndex;
            } else {
                continue; // a transform or particle controller: another pass owns it
            }
            ++stats.controllersSeen;
            if (!recognised) {
                ++stats.controllersUnsupported;
                if (warnings_ != nullptr) {
                    warnings_->push_back(std::string("material controller '") +
                                         ctrlObj->GetType().GetTypeName() +
                                         "': unrecognised target colour; skipped");
                }
                continue;
            }

            MaterialTrack proto;
            proto.property = property;
            proto.controller = ctrlObj->GetType().GetTypeName();
            proto.textureSlot = slot;

            // NiVisController and NiUVController are not NiSingleInterpController
            // subclasses at every NIF version; the interpolator is read through
            // the base where it exists, and NiVisController's own NiVisData
            // below covers the rest.
            Niflib::NiInterpolator* interp = nullptr;
            if (auto* single = dynamic_cast<Niflib::NiSingleInterpController*>(ctrl)) {
                interp = static_cast<Niflib::NiInterpolator*>(single->GetInterpolator());
            }
            if (!ReadInterpolator(interp, proto, stats)) {
                ++stats.controllersInert;
                continue;
            }

            // A NiFlipController's ordered texture list is its own refs, in
            // declaration order (niflib's generated GetRefs appends `sources`
            // after the base class's interpolator -- read the generated .cpp).
            if (property == MaterialTrackProperty::TextureIndex) {
                for (const NiObjectRef& r : ctrlObj->GetRefs()) {
                    if (r == NULL) continue;
                    auto* src = dynamic_cast<Niflib::NiSourceTexture*>(static_cast<NiObject*>(r));
                    if (src == nullptr) continue;
                    FlipTexture ft;
                    ft.sourceTextureName = src->GetTextureFileName();
                    if (!ft.sourceTextureName.empty()) {
                        const TextureResolver::Result res = textures_.Resolve(ft.sourceTextureName);
                        ft.found = res.found;
                        ft.path = res.path;
                    }
                    proto.flipTextures.push_back(std::move(ft));
                }
            }

            if (IsConstant(proto.values, proto.ComponentCount())) {
                ++stats.tracksConstantDropped;
                continue;
            }

            // Which objects does this controller's owner stand for?
            std::vector<Niflib::NiAVObject*> affected;
            if (auto* av = dynamic_cast<Niflib::NiAVObject*>(owner)) {
                affected.push_back(av);
            } else {
                auto it = propertyOwners.find(static_cast<NiObject*>(owner));
                if (it != propertyOwners.end()) affected = it->second;
            }

            std::vector<MaterialTrackTarget> targets;
            for (Niflib::NiAVObject* av : affected) {
                ResolveTargetsByPointer(av, index, targets);
            }
            if (targets.empty()) {
                MaterialTrackTarget t;
                t.name = owner->GetName();
                targets.push_back(t);
            }

            for (const MaterialTrackTarget& target : targets) {
                MaterialTrack track = proto;
                track.target = target;
                if (target.index >= 0) {
                    ++stats.tracksResolved;
                } else {
                    ++stats.tracksOrphaned;
                }
                ++stats.tracksEmitted;
                stats.keysWritten += static_cast<long long>(track.times.size());
                clip.durationSeconds =
                    std::max(clip.durationSeconds, track.times.empty() ? 0.0f : track.times.back());
                clip.materialTracks.push_back(std::move(track));
            }
        }
    }

    if (!clip.materialTracks.empty()) {
        scene.animations.push_back(std::move(clip));
    }
}

void MaterialAnimationExtractor::ExtractFromSequence(Niflib::NiControllerSequence* sequence,
                                                     const MaterialTrackTargetIndex& index,
                                                     AnimationClip& clip,
                                                     MaterialAnimationStats& stats) {
    if (sequence == nullptr) return;

    for (const Niflib::ControllerLink& link : sequence->GetControllerData()) {
        const std::string controllerType = link.controllerType;
        if (controllerType.empty() || controllerType == "NiTransformController" ||
            controllerType == "NiMultiTargetTransformController") {
            continue; // AnimationExtractor's own pass owns these
        }
        // Particle-parameter controllers (NiPSysEmitterSpeedCtlr and friends)
        // animate emitter fields, not material state. Out of scope here, and
        // deliberately not counted as unsupported: they belong to a different
        // question than the one this pass answers.
        if (controllerType.rfind("NiPSys", 0) == 0 || controllerType.rfind("NiPS", 0) == 0 ||
            controllerType.rfind("BSPSys", 0) == 0) {
            continue;
        }
        ++stats.controllersSeen;

        MaterialTrackProperty property = MaterialTrackProperty::Alpha;
        int slot = -1;
        if (!PropertyFromVariable1(controllerType, link.variable1, property, slot)) {
            ++stats.controllersUnsupported;
            if (warnings_ != nullptr) {
                warnings_->push_back("sequence '" + sequence->GetName() + "': controller type '" +
                                     controllerType + "' with variable1 '" +
                                     static_cast<std::string>(link.variable1) +
                                     "' is not translated to a material property");
            }
            continue;
        }

        MaterialTrack proto;
        proto.property = property;
        proto.controller = controllerType;
        proto.textureSlot = slot;

        if (!ReadInterpolator(static_cast<Niflib::NiInterpolator*>(link.interpolator), proto,
                              stats)) {
            ++stats.controllersInert;
            continue;
        }
        if (IsConstant(proto.values, proto.ComponentCount())) {
            ++stats.tracksConstantDropped;
            continue;
        }

        // A .kf's NiFlipController carries no texture list of its own -- the
        // sources live on the controller block in the .nif, which the embedded
        // pass already read. The track is still emitted, with an empty
        // flipTextures: a consumer falls back to the embedded track's list for
        // the same target.
        std::vector<MaterialTrackTarget> targets;
        ResolveTargetsByName(link.nodeName, index, targets, stats);

        for (const MaterialTrackTarget& target : targets) {
            MaterialTrack track = proto;
            track.target = target;
            if (target.index >= 0) {
                ++stats.tracksResolved;
            } else {
                ++stats.tracksOrphaned;
            }
            ++stats.tracksEmitted;
            stats.keysWritten += static_cast<long long>(track.times.size());
            clip.materialTracks.push_back(std::move(track));
        }
    }
}

} // namespace gfnif
