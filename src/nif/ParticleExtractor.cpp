#include "nif/ParticleExtractor.hpp"

#include "niflib.h"
#include "nif_math.h"
#include "obj/NiAVObject.h"
#include "obj/NiColorData.h"
#include "obj/NiGeometryData.h"
#include "obj/NiNode.h"
#include "obj/NiObject.h"
#include "obj/NiParticleSystem.h"
#include "obj/NiPSysBoxEmitter.h"
#include "obj/NiPSysColorModifier.h"
#include "obj/NiPSysData.h"
#include "obj/NiPSysEmitter.h"
#include "obj/NiPSysGravityModifier.h"
#include "obj/NiPSysGrowFadeModifier.h"
#include "obj/NiPSysMeshEmitter.h"
#include "obj/NiPSysModifier.h"
#include "obj/NiPSysRotationModifier.h"
#include "obj/NiPSysVolumeEmitter.h"
#include "obj/NiTriBasedGeom.h"

#include <cstdlib>
#include <map>
#include <set>
#include <sstream>
#include <string>

namespace gfnif {
namespace {

using Niflib::NiObject;
using Niflib::NiObjectRef;

// --- name resolution, mirroring AnimationExtractor.cpp's ResolveTargetIndex,
// but also reporting which of the two arrays (bone vs. plain node) matched,
// since ParticleSystemData::attachNodeIsBone needs to know. ---

int FindBoneByName(const SkeletonData* skeleton, const std::string& name) {
    if (skeleton == nullptr) return -1;
    for (size_t i = 0; i < skeleton->bones.size(); ++i) {
        if (skeleton->bones[i].name == name) return static_cast<int>(i);
    }
    return -1;
}

int FindNodeByName(const std::vector<SceneNode>* nodes, const std::string& name) {
    if (nodes == nullptr) return -1;
    for (size_t i = 0; i < nodes->size(); ++i) {
        if ((*nodes)[i].name == name) return static_cast<int>(i);
    }
    return -1;
}

/*! Resolves `name` against whichever of `skeleton`/`nodes` the file
 *  populated. Returns the index and sets `outIsBone`; -1/false when neither
 *  has a match. */
int ResolveIndex(const SkeletonData* skeleton, const std::vector<SceneNode>* nodes,
                 const std::string& name, bool& outIsBone) {
    const int boneIndex = FindBoneByName(skeleton, name);
    if (boneIndex >= 0) {
        outIsBone = true;
        return boneIndex;
    }
    outIsBone = false;
    return FindNodeByName(nodes, name);
}

/*! Resolves any NiAVObject (not just by name -- an object may be unnamed, or
 *  its name may collide with a sibling) against the same two arrays, by
 *  walking up its own ancestor chain until a name matches. Used for
 *  ParticleSystemData::attachNodeIndex and the Ptr-valued object references
 *  (Emitter Object, Gravity Object) that niflib exposes as a raw NiObject*
 *  rather than a name -- see the file header comment on why these have no
 *  usable getter beyond GetPtrs(). */
int ResolveObjectIndex(Niflib::NiAVObject* obj, const SkeletonData* skeleton,
                       const std::vector<SceneNode>* nodes, bool& outIsBone) {
    Niflib::NiAVObject* cur = obj;
    while (cur != nullptr) {
        const std::string name = cur->GetName();
        if (!name.empty()) {
            const int idx = ResolveIndex(skeleton, nodes, name, outIsBone);
            if (idx >= 0) return idx;
        }
        cur = cur->GetParent();
    }
    outIsBone = false;
    return -1;
}

/*! niflib generates NO public getters for the scalar fields of the particle
 *  emitter/modifier classes (NiPSysEmitter, NiPSysBoxEmitter,
 *  NiPSysMeshEmitter, every NiPSysXModifier) -- confirmed by reading their
 *  headers: only GetType() exists (see PHASE5_FINDINGS Etape 1). What niflib
 *  DOES generate for every one of them is asString(), which prints every
 *  field as "  Label:  value\n" -- the exact same text NifSkope's own detail
 *  panel shows, since niflib is what NifSkope itself is built on.
 *
 *  Rather than patch the niflib submodule (rejected: Phase 1 decided to keep
 *  it pristine) or rely on fragile memory-layout tricks, this phase parses
 *  that generated text. AsStringFields() runs once per object and returns a
 *  label->value map old enough call sites can just look up the field they
 *  want by its exact printed label. This is more reliable than it sounds:
 *  the label strings are generated code (BEGIN/END CUSTOM CODE markers in the
 *  .cpp show nothing here is hand-maintained), so they only change if niflib
 *  itself changes, at which point ExtractField's fallback (see below) keeps
 *  reporting a clean warning rather than silently reading garbage. */
std::map<std::string, std::string> AsStringFields(const NiObject* obj) {
    std::map<std::string, std::string> fields;
    std::istringstream in(const_cast<NiObject*>(obj)->asString(false));
    std::string line;
    while (std::getline(in, line)) {
        // Lines look like "  Label Text:  value" -- possibly with more
        // leading spaces for a nested/array field, which this parser
        // deliberately ignores (array entries use "Label[i]:", never queried
        // here). Split on the FIRST "  :  " style separator: labels are
        // "Two Words:  ", so find ":  " (colon, two spaces) as the anchor.
        size_t sep = line.find(":  ");
        if (sep == std::string::npos) continue;
        std::string label = line.substr(0, sep);
        // Trim leading spaces from the label.
        size_t labelStart = label.find_first_not_of(' ');
        if (labelStart == std::string::npos) continue;
        label = label.substr(labelStart);
        std::string value = line.substr(sep + 3);
        // First occurrence wins: a subclass's asString() calls the base
        // class's asString() first (confirmed by reading the generated
        // .cpp), so a derived class's own field of the same label (none
        // observed in the particle hierarchy, but defensive) would otherwise
        // silently overwrite the base one a caller might have wanted.
        fields.emplace(label, value);
    }
    return fields;
}

/*! Looks up `label` in `fields` and parses it as a float. Returns `def` and
 *  appends a warning naming the missing field if not found or unparsable --
 *  this is the seam that would surface a niflib version change breaking the
 *  asString() format (see AsStringFields), rather than silently exporting a
 *  wrong value. */
float ExtractFloat(const std::map<std::string, std::string>& fields, const char* label, float def,
                   const std::string& context, std::vector<std::string>* warnings) {
    auto it = fields.find(label);
    if (it == fields.end()) {
        if (warnings != nullptr) {
            warnings->push_back(context + ": field '" + label + "' not found in asString() output");
        }
        return def;
    }
    char* end = nullptr;
    const float v = std::strtof(it->second.c_str(), &end);
    if (end == it->second.c_str()) {
        if (warnings != nullptr) {
            warnings->push_back(context + ": field '" + label + "' = '" + it->second +
                                "' did not parse as a number");
        }
        return def;
    }
    return v;
}

int ExtractEnum(const std::map<std::string, std::string>& fields, const char* label,
                const std::map<std::string, int>& enumNames, int def, const std::string& context,
                std::vector<std::string>* warnings) {
    auto it = fields.find(label);
    if (it == fields.end()) return def;
    auto enumIt = enumNames.find(it->second);
    if (enumIt == enumNames.end()) {
        if (warnings != nullptr) {
            warnings->push_back(context + ": field '" + label + "' has unrecognised enum text '" +
                                it->second + "'");
        }
        return def;
    }
    return enumIt->second;
}

bool ExtractBool(const std::map<std::string, std::string>& fields, const char* label, bool def) {
    auto it = fields.find(label);
    if (it == fields.end()) return def;
    return it->second != "0";
}

/*! `Color4` prints as "{R:     1 G:     1 B:     1 A:     1}" -- distinct
 *  enough from the plain "label:  value" lines that a dedicated small parser
 *  is clearer than trying to force it through ExtractFloat. */
void ExtractColor4(const std::map<std::string, std::string>& fields, const char* label,
                   float (&out)[4]) {
    auto it = fields.find(label);
    if (it == fields.end()) return;
    const std::string& s = it->second;
    float r, g, b, a;
    if (std::sscanf(s.c_str(), "{R: %f G: %f B: %f A: %f}", &r, &g, &b, &a) == 4) {
        out[0] = r;
        out[1] = g;
        out[2] = b;
        out[3] = a;
    }
}

void ExtractVec3(const std::map<std::string, std::string>& fields, const char* label,
                 float (&out)[3]) {
    auto it = fields.find(label);
    if (it == fields.end()) return;
    float x, y, z;
    if (std::sscanf(it->second.c_str(), "( %f, %f, %f)", &x, &y, &z) == 3) {
        out[0] = x;
        out[1] = y;
        out[2] = z;
    }
}

const std::map<std::string, int>& ForceTypeNames() {
    static const std::map<std::string, int> m = {
        {"FORCE_PLANAR", 0}, {"FORCE_SPHERICAL", 1}, {"FORCE_UNKNOWN", 2}};
    return m;
}

const std::map<std::string, int>& VelocityTypeNames() {
    static const std::map<std::string, int> m = {
        {"VELOCITY_USE_NORMALS", 0}, {"VELOCITY_USE_RANDOM", 1}, {"VELOCITY_USE_DIRECTION", 2}};
    return m;
}

const std::map<std::string, int>& EmitFromNames() {
    static const std::map<std::string, int> m = {{"EMIT_FROM_VERTICES", 0},
                                                  {"EMIT_FROM_FACE_CENTER", 1},
                                                  {"EMIT_FROM_EDGE_CENTER", 2},
                                                  {"EMIT_FROM_FACE_SURFACE", 3},
                                                  {"EMIT_FROM_EDGE_SURFACE", 4}};
    return m;
}

void ToColumnMajor(const Niflib::Matrix44& m, float (&out)[16]) {
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            out[i * 4 + j] = m[i][j];
        }
    }
}

/*! Every NiPSysModifier's base fields (Name/Order/Target/Active) precede the
 *  subclass's own in asString() -- these base fields are never queried here
 *  (Target is resolved via GetPtrs() instead, see ExtractGravity), this
 *  helper just extracts the base NiPSysEmitter fields shared by both
 *  supported emitter types. */
void ExtractEmitterBaseFields(const std::map<std::string, std::string>& fields,
                              const std::string& context, std::vector<std::string>* warnings,
                              ParticleEmitterData& out) {
    out.speed = ExtractFloat(fields, "Speed", 0.0f, context, warnings);
    out.speedVariation = ExtractFloat(fields, "Speed Variation", 0.0f, context, warnings);
    out.declination = ExtractFloat(fields, "Declination", 0.0f, context, warnings);
    out.declinationVariation = ExtractFloat(fields, "Declination Variation", 0.0f, context, warnings);
    out.planarAngle = ExtractFloat(fields, "Planar Angle", 0.0f, context, warnings);
    out.planarAngleVariation = ExtractFloat(fields, "Planar Angle Variation", 0.0f, context, warnings);
    ExtractColor4(fields, "Initial Color", out.initialColor);
    out.initialRadius = ExtractFloat(fields, "Initial Radius", 0.0f, context, warnings);
    out.radiusVariation = ExtractFloat(fields, "Radius Variation", 0.0f, context, warnings);
    out.lifeSpan = ExtractFloat(fields, "Life Span", 0.0f, context, warnings);
    out.lifeSpanVariation = ExtractFloat(fields, "Life Span Variation", 0.0f, context, warnings);
}

/*! Resolves a Ptr<NiObject>-valued field (Emitter Object / Gravity Object)
 *  via GetPtrs() rather than text: these print as a raw pointer address in
 *  asString(), which is useless once parsed, but niflib's generated GetPtrs()
 *  already includes them (confirmed by reading NiPSysVolumeEmitter.cpp /
 *  NiPSysGravityModifier.cpp) -- the same mechanism Phase 3/4 already use for
 *  skin/skeleton back-references. Returns the FIRST NiAVObject pointer among
 *  `obj`'s own Ptrs (both classes expose exactly one), or nullptr. */
Niflib::NiAVObject* FindPtrObject(NiObject* obj) {
    for (NiObject* p : obj->GetPtrs()) {
        if (auto* av = dynamic_cast<Niflib::NiAVObject*>(p)) {
            return av;
        }
    }
    return nullptr;
}

} // namespace

ParticleExtractor::ParticleExtractor(std::vector<std::string>* warnings) : warnings_(warnings) {}

void ParticleExtractor::Extract(Niflib::NiAVObject* root, const SkeletonData* skeleton,
                                const std::vector<SceneNode>* nodes, MaterialExtractor& materials,
                                SceneData& scene, ParticleStats& stats) {
    if (root == nullptr) return;

    // Collect every NiParticleSystem reachable from `root`, the same
    // Ref/child walk MeshExtractor's own WalkNode uses for geometry --
    // NiParticleSystem is itself a NiAVObject (NiGeometry leaf), so it is
    // found the same way a NiTriShape is, via NiNode::GetChildren().
    std::vector<Niflib::NiParticleSystem*> systems;
    std::set<NiObject*> visited;
    std::vector<Niflib::NiAVObject*> stack{root};
    while (!stack.empty()) {
        Niflib::NiAVObject* obj = stack.back();
        stack.pop_back();
        if (obj == nullptr || !visited.insert(obj).second) continue;
        if (auto* ps = dynamic_cast<Niflib::NiParticleSystem*>(obj)) {
            systems.push_back(ps);
        }
        if (auto* node = dynamic_cast<Niflib::NiNode*>(obj)) {
            for (const auto& child : node->GetChildren()) {
                stack.push_back(static_cast<Niflib::NiAVObject*>(child));
            }
        }
    }
    if (systems.empty()) return;

    for (Niflib::NiParticleSystem* ps : systems) {
        ParticleSystemData data;
        data.name = ps->GetName();
        const std::string context = "particle system '" + data.name + "'";
        ToColumnMajor(ps->GetLocalTransform(), data.localMatrix);

        // --- attach node: where this system sits in the scene graph ---
        data.attachNodeIndex =
            ResolveObjectIndex(ps->GetParent(), skeleton, nodes, data.attachNodeIsBone);
        if (data.attachNodeIndex >= 0) {
            ++stats.systemsAttachNodeResolved;
        } else {
            ++stats.systemsAttachNodeOrphaned;
            if (warnings_ != nullptr) {
                warnings_->push_back(context + ": attach node not resolved in skeleton/node list");
            }
        }

        // --- max particle capacity: a real typed getter, since NiPSysData
        // shares NiGeometryData with ordinary meshes (see SceneModel.hpp) ---
        if (auto* psysData = dynamic_cast<Niflib::NiPSysData*>(
                static_cast<Niflib::NiObject*>(ps->GetData()))) {
            data.maxParticles = psysData->GetVertexCount();
        }

        // --- material/texture: identical mechanism to a mesh's own ---
        data.materialIndex = materials.ExtractFor(ps, scene.materials);
        if (data.materialIndex >= 0) {
            ++stats.systemsMaterialResolved;
            if (scene.materials[data.materialIndex].textureFound) {
                ++stats.systemsTextureFound;
            }
        }

        // --- modifiers, including the single emitter among them ---
        bool emitterFound = false;
        for (const NiObjectRef& r : ps->GetRefs()) {
            if (r == NULL) continue;
            auto* mod = dynamic_cast<Niflib::NiPSysModifier*>(static_cast<NiObject*>(r));
            if (mod == nullptr) continue;

            if (auto* emitter = dynamic_cast<Niflib::NiPSysEmitter*>(mod)) {
                emitterFound = true;
                const std::map<std::string, std::string> fields = AsStringFields(emitter);
                ParticleEmitterData& e = data.emitter;

                if (auto* box = dynamic_cast<Niflib::NiPSysBoxEmitter*>(emitter)) {
                    e.type = "NiPSysBoxEmitter";
                    ExtractEmitterBaseFields(fields, context, warnings_, e);
                    e.boxWidth = ExtractFloat(fields, "Width", 0.0f, context, warnings_);
                    e.boxHeight = ExtractFloat(fields, "Height", 0.0f, context, warnings_);
                    e.boxDepth = ExtractFloat(fields, "Depth", 0.0f, context, warnings_);
                    ++stats.emittersBox;
                } else if (auto* mesh = dynamic_cast<Niflib::NiPSysMeshEmitter*>(emitter)) {
                    e.type = "NiPSysMeshEmitter";
                    ExtractEmitterBaseFields(fields, context, warnings_, e);
                    e.meshInitialVelocityType = ExtractEnum(fields, "Initial Velocity Type",
                                                            VelocityTypeNames(), 0, context, warnings_);
                    e.meshEmissionType =
                        ExtractEnum(fields, "Emission Type", EmitFromNames(), 0, context, warnings_);
                    ExtractVec3(fields, "Emission Axis", e.meshEmissionAxis);
                    for (const NiObjectRef& mr : mesh->GetRefs()) {
                        if (mr == NULL) continue;
                        if (auto* geo = dynamic_cast<Niflib::NiTriBasedGeom*>(static_cast<NiObject*>(mr))) {
                            e.meshEmitterMeshNames.push_back(geo->GetName());
                        }
                    }
                    ++stats.emittersMesh;
                } else {
                    e.type = emitter->GetType().GetTypeName();
                    ++stats.emittersUnsupported;
                    if (warnings_ != nullptr) {
                        warnings_->push_back(context + ": unsupported emitter type '" + e.type +
                                             "', parameters not extracted");
                    }
                    continue;
                }

                // Emitter Object (NiPSysVolumeEmitter, both Box and Mesh
                // inherit it): the true spatial origin of the emitter's
                // shape, routinely a sibling "<name>-Emitter" NiNode distinct
                // from the particle system's own parent (PHASE5_FINDINGS).
                Niflib::NiAVObject* emitterObj = FindPtrObject(emitter);
                if (emitterObj != nullptr) {
                    bool isBone = false;
                    e.emitterObjectNodeIndex = ResolveObjectIndex(emitterObj, skeleton, nodes, isBone);
                    if (e.emitterObjectNodeIndex >= 0) ++stats.emitterObjectResolved;
                }
                if (e.emitterObjectNodeIndex < 0) {
                    e.emitterObjectNodeIndex = data.attachNodeIndex;
                }
                continue;
            }

            ParticleModifierData m;
            m.type = mod->GetType().GetTypeName();

            if (auto* gravity = dynamic_cast<Niflib::NiPSysGravityModifier*>(mod)) {
                const std::map<std::string, std::string> fields = AsStringFields(gravity);
                ExtractVec3(fields, "Gravity Axis", m.gravityAxis);
                m.gravityDecay = ExtractFloat(fields, "Decay", 0.0f, context, warnings_);
                m.gravityStrength = ExtractFloat(fields, "Strength", 0.0f, context, warnings_);
                m.gravityForceType =
                    ExtractEnum(fields, "Force Type", ForceTypeNames(), 0, context, warnings_);
                m.gravityTurbulence = ExtractFloat(fields, "Turbulence", 0.0f, context, warnings_);
                m.gravityTurbulenceScale =
                    ExtractFloat(fields, "Turbulence Scale", 1.0f, context, warnings_);
                Niflib::NiAVObject* gravObj = FindPtrObject(gravity);
                if (gravObj != nullptr) {
                    bool isBone = false;
                    m.gravityObjectNodeIndex = ResolveObjectIndex(gravObj, skeleton, nodes, isBone);
                    if (m.gravityObjectNodeIndex >= 0) ++stats.gravityObjectResolved;
                }
                ++stats.modifiersGravity;
            } else if (auto* rotation = dynamic_cast<Niflib::NiPSysRotationModifier*>(mod)) {
                const std::map<std::string, std::string> fields = AsStringFields(rotation);
                m.rotationInitialSpeed = ExtractFloat(fields, "Initial Rotation Speed", 0.0f, context, warnings_);
                m.rotationInitialSpeedVariation =
                    ExtractFloat(fields, "Initial Rotation Speed Variation", 0.0f, context, warnings_);
                m.rotationInitialAngle = ExtractFloat(fields, "Initial Rotation Angle", 0.0f, context, warnings_);
                m.rotationInitialAngleVariation =
                    ExtractFloat(fields, "Initial Rotation Angle Variation", 0.0f, context, warnings_);
                m.rotationRandomSpeedSign = ExtractBool(fields, "Random Rot Speed Sign", false);
                m.rotationRandomInitialAxis = ExtractBool(fields, "Random Initial Axis", true);
                ExtractVec3(fields, "Initial Axis", m.rotationInitialAxis);
                ++stats.modifiersRotation;
            } else if (auto* growFade = dynamic_cast<Niflib::NiPSysGrowFadeModifier*>(mod)) {
                const std::map<std::string, std::string> fields = AsStringFields(growFade);
                m.growTime = ExtractFloat(fields, "Grow Time", 0.0f, context, warnings_);
                m.fadeTime = ExtractFloat(fields, "Fade Time", 0.0f, context, warnings_);
                ++stats.modifiersGrowFade;
            } else if (auto* color = dynamic_cast<Niflib::NiPSysColorModifier*>(mod)) {
                Niflib::NiColorData* colorData = nullptr;
                for (const NiObjectRef& cr : color->GetRefs()) {
                    if (cr == NULL) continue;
                    colorData = dynamic_cast<Niflib::NiColorData*>(static_cast<NiObject*>(cr));
                    if (colorData != nullptr) break;
                }
                if (colorData != nullptr) {
                    for (const auto& key : colorData->GetKeys()) {
                        ParticleColorKey ck;
                        ck.time = key.time;
                        ck.value[0] = key.data.r;
                        ck.value[1] = key.data.g;
                        ck.value[2] = key.data.b;
                        ck.value[3] = key.data.a;
                        m.colorKeys.push_back(ck);
                    }
                } else {
                    ++stats.modifiersColorKeysMissing;
                }
                ++stats.modifiersColor;
            } else {
                // Boilerplate types kept as a type-only entry (AgeDeath,
                // BoundUpdate, Position, Spawn -- see SceneModel.hpp), plus
                // any genuinely unrecognised modifier type: both are kept in
                // the corpus's authored order for a consumer that wants the
                // full stack, but only the four dynamic_cast branches above
                // carry real fields. A type this exporter has never seen
                // before (not in Phase 1/5's inventory) still gets a warning
                // so a future corpus update is visible rather than silent.
                static const std::set<std::string> kKnownBoilerplate = {
                    "NiPSysAgeDeathModifier", "NiPSysBoundUpdateModifier", "NiPSysPositionModifier",
                    "NiPSysSpawnModifier"};
                if (kKnownBoilerplate.find(m.type) == kKnownBoilerplate.end()) {
                    ++stats.modifiersUnsupported;
                    if (warnings_ != nullptr) {
                        warnings_->push_back(context + ": unrecognised modifier type '" + m.type +
                                             "', kept as type-only entry");
                    }
                }
            }
            data.modifiers.push_back(std::move(m));
        }

        if (!emitterFound) {
            if (warnings_ != nullptr) {
                warnings_->push_back(context + ": no emitter found among its modifiers; skipped");
            }
            continue;
        }

        ++stats.systemsTotal;
        scene.particleSystems.push_back(std::move(data));
    }
}

} // namespace gfnif
