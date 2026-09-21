// One-off diagnostic (Phase 8, Etape 1): inventories the NON-transform
// controllers of the corpus -- material colour, alpha, UV / texture transform,
// flipbook and visibility -- plus the two other candidate colour sources named
// in the brief (NiVertexColorProperty modes, the GLOW texture slot), and the
// open gravity-object question.
//
// Nothing here is a fix. It answers "which of these actually exist, on what,
// and how many", so Etape 2 extends the format for what the corpus has rather
// than for the candidate list.
//
// Not part of the shipped exporter.
//
// Usage:
//   measure-material-controllers corpus <input_root>   full corpus report
//   measure-material-controllers dump   <nif_or_kf>    per-file detail

#include "niflib.h"
#include "nif_math.h"
#include "obj/NiAVObject.h"
#include "obj/NiAlphaController.h"
#include "obj/NiAlphaProperty.h"
#include "obj/NiBoolData.h"
#include "obj/NiBoolInterpolator.h"
#include "obj/NiColorData.h"
#include "obj/NiControllerSequence.h"
#include "obj/NiFlipController.h"
#include "obj/NiFloatData.h"
#include "obj/NiFloatInterpolator.h"
#include "obj/NiGeometryData.h"
#include "obj/NiInterpolator.h"
#include "obj/NiMaterialColorController.h"
#include "obj/NiMaterialProperty.h"
#include "obj/NiNode.h"
#include "obj/NiObjectNET.h"
#include "obj/NiPSysColorModifier.h"
#include "obj/NiPSysData.h"
#include "obj/NiPSysGravityModifier.h"
#include "obj/NiParticleSystem.h"
#include "obj/NiPoint3Interpolator.h"
#include "obj/NiPosData.h"
#include "obj/NiProperty.h"
#include "obj/NiSingleInterpController.h"
#include "obj/NiSourceTexture.h"
#include "obj/NiTextureTransformController.h"
#include "obj/NiTexturingProperty.h"
#include "obj/NiTimeController.h"
#include "obj/NiTriBasedGeom.h"
#include "obj/NiUVController.h"
#include "obj/NiUVData.h"
#include "obj/NiVertexColorProperty.h"
#include "obj/NiVisController.h"
#include "obj/NiVisData.h"

#include "nif/HeaderNormalizer.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace Niflib;

namespace {

// --------------------------------------------------------------------------
// loading (same path as the exporter: normalize header, refuse block types
// niflib cannot construct, then parse)
// --------------------------------------------------------------------------

bool LoadBlocks(const fs::path& path, std::vector<NiObjectRef>& blocks) {
    std::string bytes;
    if (!gfnif::ReadWholeFile(path.string(), bytes)) return false;
    gfnif::HeaderNormalizationReport rep;
    gfnif::NormalizeNifHeader(bytes, rep);
    if (!gfnif::FindUnsupportedBlockType(bytes).empty()) return false;
    try {
        std::istringstream stream(bytes, std::ios::binary);
        blocks = ReadNifList(stream, nullptr);
    } catch (...) {
        return false;
    }
    return !blocks.empty();
}

std::vector<fs::path> CollectFiles(const fs::path& root, const char* ext, const char* subdir) {
    std::vector<fs::path> out;
    if (!fs::exists(root)) return out;
    for (auto it = fs::recursive_directory_iterator(root); it != fs::recursive_directory_iterator();
         ++it) {
        if (!it->is_regular_file()) continue;
        fs::path p = it->path();
        std::string e = p.extension().string();
        std::transform(e.begin(), e.end(), e.begin(), ::tolower);
        if (e != ext) continue;
        if (p.parent_path().filename() != subdir) continue;
        out.push_back(p);
    }
    std::sort(out.begin(), out.end());
    return out;
}

// --------------------------------------------------------------------------
// classification
// --------------------------------------------------------------------------

/*! Controller types this project already handles (Phase 4 transforms) or that
 *  belong to the particle pipeline Phase 5/7 already consume. Everything else
 *  is what this inventory is about. */
bool IsAlreadyCovered(const std::string& type) {
    static const std::set<std::string> covered = {
        "NiTransformController", "NiMultiTargetTransformController", "NiKeyframeController",
        "NiPSysUpdateCtlr", "NiPSysEmitterCtlr", "BSPSysMultiTargetEmitterCtlr",
        "NiPSysModifierActiveCtlr", "NiPSysResetOnLoopCtlr",
    };
    return covered.count(type) != 0;
}

/*! A NiPSys*Ctlr other than the ones above still animates a particle modifier
 *  parameter, not a material. Kept in a separate bucket so the material
 *  question is not drowned by them. */
bool IsParticleParamController(const std::string& type) {
    return type.rfind("NiPSys", 0) == 0 || type.rfind("NiPS", 0) == 0 ||
           type.rfind("BSPSys", 0) == 0;
}

std::string TypeOf(NiObject* o) { return o == nullptr ? "(null)" : o->GetType().GetTypeName(); }

// --------------------------------------------------------------------------
// interpolator / data inspection
// --------------------------------------------------------------------------

struct InterpInfo {
    std::string type = "(none)";
    int keyCount = -1;   // -1 = no data block
    float startTime = 0.0f;
    float stopTime = 0.0f;
    bool constantOnly = false; // interpolator with a value but no keys
};

InterpInfo InspectInterpolator(NiInterpolator* interp) {
    InterpInfo info;
    if (interp == nullptr) return info;
    info.type = TypeOf(interp);

    if (auto* fi = dynamic_cast<NiFloatInterpolator*>(interp)) {
        Ref<NiFloatData> d = fi->GetData();
        if (d != NULL) {
            std::vector<Key<float>> keys = d->GetKeys();
            info.keyCount = static_cast<int>(keys.size());
            if (!keys.empty()) {
                info.startTime = keys.front().time;
                info.stopTime = keys.back().time;
            }
        } else {
            info.constantOnly = true;
        }
    } else if (auto* pi = dynamic_cast<NiPoint3Interpolator*>(interp)) {
        Ref<NiPosData> d = pi->GetData();
        if (d != NULL) {
            std::vector<Key<Vector3>> keys = d->GetKeys();
            info.keyCount = static_cast<int>(keys.size());
            if (!keys.empty()) {
                info.startTime = keys.front().time;
                info.stopTime = keys.back().time;
            }
        } else {
            info.constantOnly = true;
        }
    } else if (auto* bi = dynamic_cast<NiBoolInterpolator*>(interp)) {
        Ref<NiBoolData> d = bi->GetData();
        if (d != NULL) {
            std::vector<Key<unsigned char>> keys = d->GetKeys();
            info.keyCount = static_cast<int>(keys.size());
            if (!keys.empty()) {
                info.startTime = keys.front().time;
                info.stopTime = keys.back().time;
            }
        } else {
            info.constantOnly = true;
        }
    }
    return info;
}

/*! True when every key holds the same value: the track exists but animates
 *  nothing, so exporting it costs bytes and buys no motion. Measured before
 *  deciding whether the exporter should filter these out. */
bool IsConstantInterpolator(NiInterpolator* interp) {
    if (interp == nullptr) return false;
    const float eps = 1e-6f;
    if (auto* fi = dynamic_cast<NiFloatInterpolator*>(interp)) {
        Ref<NiFloatData> d = fi->GetData();
        if (d == NULL) return false;
        std::vector<Key<float>> keys = d->GetKeys();
        for (size_t i = 1; i < keys.size(); ++i) {
            if (std::fabs(keys[i].data - keys[0].data) > eps) return false;
        }
        return true;
    }
    if (auto* pi = dynamic_cast<NiPoint3Interpolator*>(interp)) {
        Ref<NiPosData> d = pi->GetData();
        if (d == NULL) return false;
        std::vector<Key<Vector3>> keys = d->GetKeys();
        for (size_t i = 1; i < keys.size(); ++i) {
            if (std::fabs(keys[i].data.x - keys[0].data.x) > eps ||
                std::fabs(keys[i].data.y - keys[0].data.y) > eps ||
                std::fabs(keys[i].data.z - keys[0].data.z) > eps) {
                return false;
            }
        }
        return true;
    }
    if (auto* bi = dynamic_cast<NiBoolInterpolator*>(interp)) {
        Ref<NiBoolData> d = bi->GetData();
        if (d == NULL) return false;
        std::vector<Key<unsigned char>> keys = d->GetKeys();
        for (size_t i = 1; i < keys.size(); ++i) {
            if (keys[i].data != keys[0].data) return false;
        }
        return true;
    }
    return false;
}

// --------------------------------------------------------------------------
// asString() field reading -- the same mechanism ParticleExtractor.cpp uses,
// for the few fields niflib generates no getter for (Target Color).
// --------------------------------------------------------------------------

std::map<std::string, std::string> AsStringFields(NiObject* obj) {
    std::map<std::string, std::string> fields;
    std::istringstream in(obj->asString(false));
    std::string line;
    while (std::getline(in, line)) {
        size_t sep = line.find(":  ");
        if (sep == std::string::npos) continue;
        std::string label = line.substr(0, sep);
        size_t s = label.find_first_not_of(' ');
        if (s == std::string::npos) continue;
        label = label.substr(s);
        fields.emplace(label, line.substr(sep + 3));
    }
    return fields;
}

std::string Field(NiObject* obj, const char* label) {
    auto f = AsStringFields(obj);
    auto it = f.find(label);
    return it == f.end() ? std::string("(absent)") : it->second;
}

// --------------------------------------------------------------------------
// colour helpers -- the loader's own "is this colour actually a tint" rule
// (PHASE7_FINDINGS correctif D): a colour on the grey axis carries no hue.
// --------------------------------------------------------------------------

bool IsGrey(const Color3& c) {
    const float lo = std::min({c.r, c.g, c.b});
    const float hi = std::max({c.r, c.g, c.b});
    return (hi - lo) <= (1.0f / 255.0f);
}

// --------------------------------------------------------------------------
// per-file index: property -> the NiAVObjects carrying it, so a controller
// attached to a NiMaterialProperty can be traced back to the geometry (and in
// particular to a NiParticleSystem).
// --------------------------------------------------------------------------

struct FileIndex {
    std::map<NiObject*, std::vector<NiAVObject*>> propertyOwners;
    std::map<NiObject*, NiObjectNET*> controllerOwners;
};

FileIndex BuildIndex(const std::vector<NiObjectRef>& blocks) {
    FileIndex idx;
    for (const NiObjectRef& b : blocks) {
        NiObject* obj = static_cast<NiObject*>(b);
        if (auto* av = dynamic_cast<NiAVObject*>(obj)) {
            for (const Ref<NiProperty>& p : av->GetProperties()) {
                if (p != NULL) idx.propertyOwners[static_cast<NiObject*>(p)].push_back(av);
            }
        }
        if (auto* net = dynamic_cast<NiObjectNET*>(obj)) {
            for (const Ref<NiTimeController>& c : net->GetControllers()) {
                if (c != NULL) idx.controllerOwners[static_cast<NiObject*>(c)] = net;
            }
        }
    }
    return idx;
}

/*! What kind of thing this controller ultimately affects, for the "sur quels
 *  objets portent-ils" question. A controller on a property is attributed to
 *  the geometry carrying that property. */
std::string TargetKind(NiObject* ctrl, const FileIndex& idx) {
    auto ownerIt = idx.controllerOwners.find(ctrl);
    if (ownerIt == idx.controllerOwners.end()) return "(unowned)";
    NiObjectNET* owner = ownerIt->second;

    if (dynamic_cast<NiParticleSystem*>(owner) != nullptr) return "NiParticleSystem";
    if (dynamic_cast<NiTriBasedGeom*>(owner) != nullptr) return "mesh";
    if (dynamic_cast<NiNode*>(owner) != nullptr) return "NiNode";

    if (dynamic_cast<NiProperty*>(owner) != nullptr) {
        auto it = idx.propertyOwners.find(owner);
        if (it == idx.propertyOwners.end() || it->second.empty()) {
            return std::string("property(") + TypeOf(owner) + ") on nothing";
        }
        bool onParticles = false;
        bool onMesh = false;
        for (NiAVObject* av : it->second) {
            if (dynamic_cast<NiParticleSystem*>(av) != nullptr) {
                onParticles = true;
            } else {
                onMesh = true;
            }
        }
        std::string via = std::string("property(") + TypeOf(owner) + ") on ";
        if (onParticles && onMesh) return via + "both";
        return via + (onParticles ? "NiParticleSystem" : "mesh");
    }
    return std::string("other(") + TypeOf(owner) + ")";
}

// --------------------------------------------------------------------------
// accumulators
// --------------------------------------------------------------------------

struct TypeStat {
    long long instances = 0;
    long long files = 0;
    std::map<std::string, long long> targets;
    std::map<std::string, long long> interpolators;
    long long withKeys = 0;
    long long withoutData = 0;
    long long totalKeys = 0;
    float maxStop = 0.0f;
    std::vector<std::string> sampleFiles;
};

struct GreyBreakdown {
    long long systemsTotal = 0;
    long long allKeysBlack = 0;
    long long grey = 0; // all-black keys AND neither emissive nor diffuse tinted
    long long greyWithMatColorCtrl = 0;
    long long greyWithKfMatColorCtrl = 0;
    long long greyWithAlphaCtrl = 0;
    long long greyWithVertexColors = 0;
    long long greyWithColouredVertexColors = 0;
    long long greyWithVertexColorEmissiveMode = 0;
    long long greyWithGlowSlot = 0;
    long long greyWithFlipController = 0;
    long long greyExplainedByAny = 0;
    std::map<std::string, long long> greyVertexModes;
    std::vector<std::string> unexplainedSample;
    long long unexplained = 0;
};

/*! What a .kf drives, per model: node name -> the controller types some
 *  NiControllerSequence binds to it. Built before the .nif pass so the grey
 *  analysis can ask "is this system's colour animated from the .kf instead?" --
 *  the .nif holds the controller stub, the .kf holds the interpolator, so a
 *  .nif-only look would answer that question wrongly. */
using KfTargetMap = std::map<std::string, std::map<std::string, std::set<std::string>>>;

/*! "Sur quels objets portent-ils" -- the brief guesses glow planes, weapon
 *  effects. A name is weak evidence; the render state is not. This profiles
 *  the geometry behind every UV / alpha / colour controller. */
struct TargetProfile {
    long long geometries = 0;
    long long additive = 0;   // SRC_ALPHA -> ONE, the glow-plane signature
    long long blended = 0;
    long long alphaTested = 0;
    long long opaque = 0;
    long long tinyQuads = 0;  // <= 8 vertices: a plane or a simple billboard
    std::map<std::string, long long> topNames;
    std::map<std::string, long long> blendModes;
};

struct GravityBreakdown {
    long long modifiers = 0;
    long long withGravityObject = 0;
    long long objectDiffersFromAttachNode = 0;
    long long nonTrivialRotationBetween = 0;
    std::vector<std::string> sample;
};

void Bump(TypeStat& s, const std::string& target, const InterpInfo& interp, const std::string& file,
          std::set<std::string>& seenInFile) {
    ++s.instances;
    ++s.targets[target];
    ++s.interpolators[interp.type];
    if (interp.keyCount > 0) {
        ++s.withKeys;
        s.totalKeys += interp.keyCount;
        s.maxStop = std::max(s.maxStop, interp.stopTime);
    } else if (interp.keyCount < 0) {
        ++s.withoutData;
    }
    if (seenInFile.insert(file).second) {
        ++s.files;
        if (s.sampleFiles.size() < 8) s.sampleFiles.push_back(file);
    }
}

// --------------------------------------------------------------------------
// the grey-particle question
// --------------------------------------------------------------------------

void AnalyseGreySystems(const std::vector<NiObjectRef>& blocks, const FileIndex& idx,
                        const std::string& fileKey,
                        const std::map<std::string, std::set<std::string>>* kfForThisModel,
                        GreyBreakdown& g) {
    for (const NiObjectRef& b : blocks) {
        auto* ps = dynamic_cast<NiParticleSystem*>(static_cast<NiObject*>(b));
        if (ps == nullptr) continue;
        ++g.systemsTotal;

        // --- colour keys of the NiPSysColorModifier, if any ---
        bool hasColorKeys = false;
        bool allKeysBlack = true;
        for (const NiObjectRef& r : ps->GetRefs()) {
            if (r == NULL) continue;
            auto* cm = dynamic_cast<NiPSysColorModifier*>(static_cast<NiObject*>(r));
            if (cm == nullptr) continue;
            for (const NiObjectRef& cr : cm->GetRefs()) {
                if (cr == NULL) continue;
                auto* cd = dynamic_cast<NiColorData*>(static_cast<NiObject*>(cr));
                if (cd == nullptr) continue;
                for (const Key<Color4>& k : cd->GetKeys()) {
                    hasColorKeys = true;
                    if (k.data.r > 1.0f / 255.0f || k.data.g > 1.0f / 255.0f ||
                        k.data.b > 1.0f / 255.0f) {
                        allKeysBlack = false;
                    }
                }
            }
        }
        if (!hasColorKeys || !allKeysBlack) continue;
        ++g.allKeysBlack;

        // --- the system's own properties ---
        NiMaterialProperty* matProp = nullptr;
        NiVertexColorProperty* vcProp = nullptr;
        NiTexturingProperty* texProp = nullptr;
        for (const Ref<NiProperty>& p : ps->GetProperties()) {
            if (p == NULL) continue;
            NiProperty* prop = static_cast<NiProperty*>(p);
            if (auto* m = dynamic_cast<NiMaterialProperty*>(prop)) matProp = m;
            if (auto* v = dynamic_cast<NiVertexColorProperty*>(prop)) vcProp = v;
            if (auto* t = dynamic_cast<NiTexturingProperty*>(prop)) texProp = t;
        }

        const bool emissiveTinted = matProp != nullptr && !IsGrey(matProp->GetEmissiveColor());
        const bool diffuseTinted = matProp != nullptr && !IsGrey(matProp->GetDiffuseColor());
        if (emissiveTinted || diffuseTinted) continue;

        // This system is one of the "truly grey" ones.
        ++g.grey;
        bool explained = false;

        // Source 1: an animated colour on its material property (or on the
        // system itself -- checked both, since either is possible).
        auto hasControllerOn = [&](NiObjectNET* net, const char* wanted) {
            if (net == nullptr) return false;
            for (const Ref<NiTimeController>& c : net->GetControllers()) {
                if (c != NULL && TypeOf(static_cast<NiObject*>(c)) == wanted) return true;
            }
            return false;
        };
        if (hasControllerOn(matProp, "NiMaterialColorController") ||
            hasControllerOn(ps, "NiMaterialColorController")) {
            ++g.greyWithMatColorCtrl;
            explained = true;
        }
        // Same source, but driven from the model's .kf rather than embedded.
        if (kfForThisModel != nullptr) {
            auto it = kfForThisModel->find(ps->GetName());
            if (it != kfForThisModel->end() && it->second.count("NiMaterialColorController") != 0) {
                ++g.greyWithKfMatColorCtrl;
                explained = true;
            }
        }
        if (hasControllerOn(ps, "NiFlipController") || hasControllerOn(texProp, "NiFlipController")) {
            ++g.greyWithFlipController;
            explained = true;
        }
        // Not a colour source, but recorded: an animated opacity would at
        // least make the system visibly non-static.
        for (const Ref<NiProperty>& p : ps->GetProperties()) {
            if (p != NULL && hasControllerOn(static_cast<NiObjectNET*>(static_cast<NiProperty*>(p)),
                                             "NiAlphaController")) {
                ++g.greyWithAlphaCtrl;
                break;
            }
        }

        // Source 2: vertex colours, and the mode that would make them emissive.
        if (vcProp != nullptr) {
            std::ostringstream mode;
            mode << vcProp->GetVertexMode() << " / " << vcProp->GetLightingMode();
            ++g.greyVertexModes[mode.str()];

            auto* psysData = dynamic_cast<NiPSysData*>(static_cast<NiObject*>(ps->GetData()));
            std::vector<Color4> colors = psysData != nullptr ? psysData->GetColors()
                                                             : std::vector<Color4>();
            if (!colors.empty()) {
                ++g.greyWithVertexColors;
                // Present is not the same as coloured: a stored array of black
                // or white carries no hue and is not a colour source.
                bool anyHue = false;
                for (const Color4& c : colors) {
                    const float lo = std::min({c.r, c.g, c.b});
                    const float hi = std::max({c.r, c.g, c.b});
                    if (hi - lo > 1.0f / 255.0f) {
                        anyHue = true;
                        break;
                    }
                }
                if (anyHue) ++g.greyWithColouredVertexColors;
                if (anyHue && (vcProp->GetVertexMode() == VERT_MODE_SRC_EMISSIVE ||
                               vcProp->GetLightingMode() == LIGHT_MODE_EMISSIVE)) {
                    ++g.greyWithVertexColorEmissiveMode;
                    explained = true;
                }
            }
        }

        // Source 3: the GLOW texture slot, ignored since Phase 2.
        if (texProp != nullptr && texProp->GetTextureCount() > GLOW_MAP) {
            TexDesc& glow = texProp->GetTexture(GLOW_MAP);
            if (glow.source != NULL && !glow.source->GetTextureFileName().empty()) {
                ++g.greyWithGlowSlot;
                explained = true;
            }
        }

        if (explained) {
            ++g.greyExplainedByAny;
        } else {
            ++g.unexplained;
            if (g.unexplainedSample.size() < 30) {
                g.unexplainedSample.push_back(fileKey + " : " + ps->GetName());
            }
        }
    }
    (void)idx;
}

void ProfileTarget(NiObject* ctrl, const FileIndex& idx, TargetProfile& p) {
    auto ownerIt = idx.controllerOwners.find(ctrl);
    if (ownerIt == idx.controllerOwners.end()) return;
    NiObjectNET* owner = ownerIt->second;

    std::vector<NiAVObject*> geoms;
    if (auto* av = dynamic_cast<NiAVObject*>(owner)) {
        geoms.push_back(av);
    } else if (dynamic_cast<NiProperty*>(owner) != nullptr) {
        auto it = idx.propertyOwners.find(owner);
        if (it != idx.propertyOwners.end()) geoms = it->second;
    }

    for (NiAVObject* av : geoms) {
        auto* geo = dynamic_cast<NiTriBasedGeom*>(av);
        if (geo == nullptr) continue;
        ++p.geometries;
        if (p.topNames.size() < 4000) ++p.topNames[geo->GetName()];

        for (const Ref<NiProperty>& prop : geo->GetProperties()) {
            auto* ap = dynamic_cast<NiAlphaProperty*>(static_cast<NiProperty*>(prop));
            if (ap == nullptr) continue;
            if (ap->GetBlendState()) {
                ++p.blended;
                // BF_SRC_ALPHA -> BF_ONE, the additive combination Phase 7
                // 3.2 keyed on. The enum values are NOT the OpenGL ones
                // (BF_ONE is 0, BF_SRC_ALPHA is 6 -- NiAlphaProperty.h), so
                // they are named rather than written as literals.
                if (ap->GetSourceBlendFunc() == NiAlphaProperty::BF_SRC_ALPHA &&
                    ap->GetDestBlendFunc() == NiAlphaProperty::BF_ONE) {
                    ++p.additive;
                }
                std::ostringstream m;
                m << static_cast<int>(ap->GetSourceBlendFunc()) << " -> "
                  << static_cast<int>(ap->GetDestBlendFunc());
                ++p.blendModes[m.str()];
            } else {
                ++p.opaque;
            }
            if (ap->GetTestState()) ++p.alphaTested;
        }
        if (auto* data = dynamic_cast<NiGeometryData*>(static_cast<NiObject*>(geo->GetData()))) {
            if (data->GetVertexCount() <= 8) ++p.tinyQuads;
        }
    }
}

void PrintProfile(const char* title, const TargetProfile& p) {
    std::cout << "\n  " << title << "\n";
    std::cout << "    geometries behind the controllers : " << p.geometries << "\n";
    std::cout << "    alpha-blended                     : " << p.blended << "\n";
    std::cout << "      ... additive (SRC_ALPHA -> ONE) : " << p.additive << "\n";
    std::cout << "      blend modes (src -> dst)        :";
    for (const auto& m : p.blendModes) std::cout << " [" << m.first << " x" << m.second << "]";
    std::cout << "\n";
    std::cout << "    alpha-tested                      : " << p.alphaTested << "\n";
    std::cout << "    opaque                            : " << p.opaque << "\n";
    std::cout << "    <= 8 vertices (a plane / quad)    : " << p.tinyQuads << "\n";
    std::vector<std::pair<std::string, long long>> names(p.topNames.begin(), p.topNames.end());
    std::sort(names.begin(), names.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    std::cout << "    most frequent geometry names      :";
    for (size_t i = 0; i < names.size() && i < 8; ++i) {
        std::cout << " [" << names[i].first << " x" << names[i].second << "]";
    }
    std::cout << "\n";
}

// --------------------------------------------------------------------------
// the open gravity-object question
// --------------------------------------------------------------------------

NiAVObject* FirstPtrObject(NiObject* obj) {
    for (NiObject* p : obj->GetPtrs()) {
        if (auto* av = dynamic_cast<NiAVObject*>(p)) return av;
    }
    return nullptr;
}

Matrix44 WorldTransform(NiAVObject* obj) {
    Matrix44 m = Matrix44::IDENTITY;
    std::vector<NiAVObject*> chain;
    for (NiAVObject* cur = obj; cur != nullptr; cur = cur->GetParent()) chain.push_back(cur);
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        m = (*it)->GetLocalTransform() * m;
    }
    return m;
}

void AnalyseGravity(const std::vector<NiObjectRef>& blocks, const std::string& fileKey,
                    GravityBreakdown& gr) {
    for (const NiObjectRef& b : blocks) {
        auto* ps = dynamic_cast<NiParticleSystem*>(static_cast<NiObject*>(b));
        if (ps == nullptr) continue;
        for (const NiObjectRef& r : ps->GetRefs()) {
            if (r == NULL) continue;
            auto* grav = dynamic_cast<NiPSysGravityModifier*>(static_cast<NiObject*>(r));
            if (grav == nullptr) continue;
            ++gr.modifiers;

            NiAVObject* gravObj = FirstPtrObject(grav);
            if (gravObj == nullptr) continue;
            ++gr.withGravityObject;

            NiAVObject* attach = ps->GetParent();
            if (attach == nullptr || gravObj == attach) continue;
            ++gr.objectDiffersFromAttachNode;

            // Rotation between the two spaces: if it is (near) identity, the
            // gravity axis means the same thing in either, and ignoring
            // gravityObjectNodeIndex costs nothing.
            Matrix33 a = WorldTransform(attach).GetRotation();
            Matrix33 g = WorldTransform(gravObj).GetRotation();
            // A rotation matrix's inverse is its transpose; niflib's Matrix33
            // exposes neither, so it is written out.
            Matrix33 aT;
            for (int i = 0; i < 3; ++i) {
                for (int j = 0; j < 3; ++j) aT[i][j] = a[j][i];
            }
            Matrix33 rel = aT * g;
            float maxDev = 0.0f;
            for (int i = 0; i < 3; ++i) {
                for (int j = 0; j < 3; ++j) {
                    maxDev = std::max(maxDev, std::fabs(rel[i][j] - (i == j ? 1.0f : 0.0f)));
                }
            }
            if (maxDev > 1e-3f) {
                ++gr.nonTrivialRotationBetween;
                if (gr.sample.size() < 20) {
                    std::ostringstream o;
                    o << fileKey << " : " << ps->GetName() << " (max deviation " << maxDev << ")";
                    gr.sample.push_back(o.str());
                }
            }
        }
    }
}

// --------------------------------------------------------------------------
// report
// --------------------------------------------------------------------------

void PrintTypeTable(const char* title, const std::map<std::string, TypeStat>& stats) {
    std::cout << "\n=== " << title << " ===\n";
    if (stats.empty()) {
        std::cout << "  (none found in the corpus)\n";
        return;
    }
    // Sort by instance count, descending.
    std::vector<const std::pair<const std::string, TypeStat>*> rows;
    for (const auto& kv : stats) rows.push_back(&kv);
    std::sort(rows.begin(), rows.end(),
              [](auto* a, auto* b) { return a->second.instances > b->second.instances; });

    for (auto* row : rows) {
        const TypeStat& s = row->second;
        std::cout << "\n  " << row->first << "\n";
        std::cout << "    instances: " << s.instances << "   files: " << s.files
                  << "   with keys: " << s.withKeys << "   no data block: " << s.withoutData
                  << "   total keys: " << s.totalKeys << "   max stop time: " << s.maxStop << "s\n";
        std::cout << "    targets:";
        for (const auto& t : s.targets) std::cout << "  [" << t.first << " x" << t.second << "]";
        std::cout << "\n    interpolators:";
        for (const auto& i : s.interpolators) std::cout << "  [" << i.first << " x" << i.second << "]";
        std::cout << "\n    sample files:";
        for (const auto& f : s.sampleFiles) std::cout << " " << f;
        std::cout << "\n";
    }
}

int RunCorpus(const fs::path& root) {
    const std::vector<fs::path> nifs = CollectFiles(root, ".nif", "model");
    const std::vector<fs::path> kfs = CollectFiles(root, ".kf", "animation");

    std::map<std::string, TypeStat> materialish; // the inventory's subject
    std::map<std::string, TypeStat> particleParams;
    std::map<std::string, TypeStat> covered;
    std::map<std::string, std::set<std::string>> seen; // type -> files seen in

    GreyBreakdown grey;
    GravityBreakdown gravity;
    TargetProfile uvProfile;
    TargetProfile alphaProfile;
    TargetProfile colorProfile;

    // ------------------------------------------------------------------
    // .kf pass FIRST: the .nif pass needs to know what a model's sequences
    // drive, since a controller stub in the .nif gets its interpolator from
    // the .kf and looks inert on its own.
    // ------------------------------------------------------------------
    KfTargetMap kfTargets;
    long long kfScanned = 0;
    long long kfFailed = 0;
    long long kfSequences = 0;
    long long kfLinks = 0;
    std::map<std::string, long long> kfControllerTypes;
    std::map<std::string, long long> kfPropertyTypes;
    std::map<std::string, long long> kfVariable1;
    std::map<std::string, long long> kfNonTransformInterp;
    std::vector<std::string> kfNonTransformSample;
    std::map<std::string, long long> kfKeysByType;
    std::map<std::string, long long> kfTracksWithKeys;
    std::map<std::string, long long> kfConstantTracks;
    std::map<std::string, long long> kfConstantKeys;
    std::map<std::string, long long> kfEmptyTracks;
    std::map<std::string, long long> kfNoDataTracks;

    for (const fs::path& p : kfs) {
        std::vector<NiObjectRef> blocks;
        if (!LoadBlocks(p, blocks)) {
            ++kfFailed;
            continue;
        }
        ++kfScanned;
        const std::string key = fs::relative(p, root).generic_string();
        // "chair/animation/C013.kf" -> "chair/C013", the same key the model
        // file "chair/model/C013.nif" produces. The corpus pairs them 1:1 by
        // stem (checked: every animation/ stem has a model/ stem).
        const std::string modelKey =
            p.parent_path().parent_path().filename().string() + "/" + p.stem().string();
        auto& perNode = kfTargets[modelKey];

        for (const NiObjectRef& b : blocks) {
            auto* seq = dynamic_cast<NiControllerSequence*>(static_cast<NiObject*>(b));
            if (seq == nullptr) continue;
            ++kfSequences;
            for (const ControllerLink& link : seq->GetControllerData()) {
                ++kfLinks;
                const std::string ctype =
                    link.controllerType.empty() ? std::string("(empty)")
                                                : static_cast<std::string>(link.controllerType);
                ++kfControllerTypes[ctype];
                if (!link.propertyType.empty()) {
                    ++kfPropertyTypes[static_cast<std::string>(link.propertyType)];
                }
                if (!link.variable1.empty()) {
                    ++kfVariable1[static_cast<std::string>(link.variable1)];
                }
                if (ctype != "NiTransformController" && ctype != "(empty)" &&
                    ctype != "NiMultiTargetTransformController") {
                    perNode[static_cast<std::string>(link.nodeName)].insert(ctype);
                    ++kfNonTransformInterp[TypeOf(static_cast<NiObject*>(link.interpolator))];

                    // Key volume, per controller type: this decides whether
                    // exporting these tracks is affordable at all (Phase 6
                    // already tracks output size), and how much a
                    // constant-track filter would save.
                    const InterpInfo ii =
                        InspectInterpolator(static_cast<NiInterpolator*>(link.interpolator));
                    if (ii.keyCount > 0) {
                        kfKeysByType[ctype] += ii.keyCount;
                        kfTracksWithKeys[ctype] += 1;
                        if (IsConstantInterpolator(static_cast<NiInterpolator*>(link.interpolator))) {
                            kfConstantTracks[ctype] += 1;
                            kfConstantKeys[ctype] += ii.keyCount;
                        }
                    } else if (ii.keyCount == 0) {
                        kfEmptyTracks[ctype] += 1;
                    } else {
                        kfNoDataTracks[ctype] += 1;
                    }
                    if (kfNonTransformSample.size() < 20) {
                        kfNonTransformSample.push_back(key + " '" + seq->GetName() + "' -> " +
                                                       ctype + " on '" +
                                                       static_cast<std::string>(link.nodeName) + "'");
                    }
                }
            }
        }
    }

    long long filesScanned = 0;
    long long filesFailed = 0;
    long long filesWithMaterialish = 0;

    // --- extra colour-source census over ALL geometry, not just grey systems,
    // since the brief asks whether the GLOW slot is worth exporting at all.
    long long texPropsTotal = 0;
    long long texPropsWithGlow = 0;
    long long texPropsWithDark = 0;
    long long texPropsWithDetail = 0;
    long long vcPropsTotal = 0;
    std::map<std::string, long long> vcModes;

    for (const fs::path& p : nifs) {
        std::vector<NiObjectRef> blocks;
        if (!LoadBlocks(p, blocks)) {
            ++filesFailed;
            continue;
        }
        ++filesScanned;
        const std::string key = fs::relative(p, root).generic_string();
        const FileIndex idx = BuildIndex(blocks);

        bool any = false;
        for (const NiObjectRef& b : blocks) {
            NiObject* obj = static_cast<NiObject*>(b);
            auto* ctrl = dynamic_cast<NiTimeController*>(obj);
            if (ctrl == nullptr) continue;

            const std::string type = TypeOf(obj);
            InterpInfo interp;
            if (auto* single = dynamic_cast<NiSingleInterpController*>(ctrl)) {
                interp = InspectInterpolator(static_cast<NiInterpolator*>(single->GetInterpolator()));
            } else if (auto* uv = dynamic_cast<NiUVController*>(ctrl)) {
                interp.type = (uv->GetData() != NULL) ? "NiUVData (classic keys)" : "(no data)";
                interp.keyCount = (uv->GetData() != NULL) ? 0 : -1;
            }

            const std::string target = TargetKind(obj, idx);
            if (IsAlreadyCovered(type)) {
                Bump(covered[type], target, interp, key, seen["c:" + type]);
            } else if (IsParticleParamController(type)) {
                Bump(particleParams[type], target, interp, key, seen["p:" + type]);
            } else {
                Bump(materialish[type], target, interp, key, seen["m:" + type]);
                any = true;
                if (type == "NiTextureTransformController") {
                    ProfileTarget(obj, idx, uvProfile);
                } else if (type == "NiAlphaController") {
                    ProfileTarget(obj, idx, alphaProfile);
                } else if (type == "NiMaterialColorController") {
                    ProfileTarget(obj, idx, colorProfile);
                }
            }
        }
        if (any) ++filesWithMaterialish;

        // colour-source census
        for (const NiObjectRef& b : blocks) {
            NiObject* obj = static_cast<NiObject*>(b);
            if (auto* tp = dynamic_cast<NiTexturingProperty*>(obj)) {
                ++texPropsTotal;
                auto slotUsed = [&](TexType slot) {
                    if (tp->GetTextureCount() <= static_cast<int>(slot)) return false;
                    TexDesc& d = tp->GetTexture(slot);
                    return d.source != NULL && !d.source->GetTextureFileName().empty();
                };
                if (slotUsed(GLOW_MAP)) ++texPropsWithGlow;
                if (slotUsed(DARK_MAP)) ++texPropsWithDark;
                if (slotUsed(DETAIL_MAP)) ++texPropsWithDetail;
            } else if (auto* vc = dynamic_cast<NiVertexColorProperty*>(obj)) {
                ++vcPropsTotal;
                std::ostringstream m;
                m << vc->GetVertexMode() << " / " << vc->GetLightingMode();
                ++vcModes[m.str()];
            }
        }

        const std::string modelKey =
            p.parent_path().parent_path().filename().string() + "/" + p.stem().string();
        auto kfIt = kfTargets.find(modelKey);
        AnalyseGreySystems(blocks, idx, key,
                           kfIt == kfTargets.end() ? nullptr : &kfIt->second, grey);
        AnalyseGravity(blocks, key, gravity);
    }

    // ---------------------------------------------------------------- report
    std::cout << "############################################################\n";
    std::cout << "# Phase 8 Etape 1 -- non-transform controller inventory\n";
    std::cout << "############################################################\n\n";
    std::cout << "model .nif scanned : " << filesScanned << " (" << filesFailed << " unreadable)\n";
    std::cout << "  ... carrying >=1 non-transform, non-particle controller: " << filesWithMaterialish
              << "\n";

    PrintTypeTable("1. Material / UV / visibility controllers (the subject)", materialish);
    PrintTypeTable("2. Particle-parameter controllers (context, not the subject)", particleParams);
    PrintTypeTable("3. Controllers already covered by Phase 4/5/7", covered);

    std::cout << "\n=== 3b. What the material controllers actually sit on ===\n";
    PrintProfile("NiTextureTransformController", uvProfile);
    PrintProfile("NiAlphaController", alphaProfile);
    PrintProfile("NiMaterialColorController", colorProfile);

    std::cout << "\n=== 4. Other colour sources, corpus-wide ===\n";
    std::cout << "  NiTexturingProperty blocks        : " << texPropsTotal << "\n";
    std::cout << "    with a populated GLOW slot      : " << texPropsWithGlow << "\n";
    std::cout << "    with a populated DARK slot      : " << texPropsWithDark << "\n";
    std::cout << "    with a populated DETAIL slot    : " << texPropsWithDetail << "\n";
    std::cout << "  NiVertexColorProperty blocks      : " << vcPropsTotal << "\n";
    std::cout << "    modes (vertexMode / lightingMode):\n";
    for (const auto& m : vcModes) std::cout << "      " << m.first << " : " << m.second << "\n";

    std::cout << "\n=== 5. The grey particle systems, source by source ===\n";
    std::cout << "  particle systems total                        : " << grey.systemsTotal << "\n";
    std::cout << "  ... with colour keys, all of them black       : " << grey.allKeysBlack << "\n";
    std::cout << "  ... and neither emissive nor diffuse tinted   : " << grey.grey
              << "   <-- the grey set\n";
    std::cout << "\n  of that grey set:\n";
    std::cout << "    a NiMaterialColorController on its material : " << grey.greyWithMatColorCtrl
              << "\n";
    std::cout << "    ... or one bound from the model's .kf       : " << grey.greyWithKfMatColorCtrl
              << "\n";
    std::cout << "    a NiFlipController                          : " << grey.greyWithFlipController
              << "\n";
    std::cout << "    vertex colours present in NiPSysData        : " << grey.greyWithVertexColors
              << "\n";
    std::cout << "      ... actually carrying a hue               : "
              << grey.greyWithColouredVertexColors << "\n";
    std::cout << "      ... and in an emissive vertex/lighting mode: "
              << grey.greyWithVertexColorEmissiveMode << "\n";
    std::cout << "    a populated GLOW texture slot               : " << grey.greyWithGlowSlot << "\n";
    std::cout << "    (context) a NiAlphaController               : " << grey.greyWithAlphaCtrl << "\n";
    std::cout << "    explained by at least one of the above      : " << grey.greyExplainedByAny
              << "\n";
    std::cout << "    explained by none of them                   : " << grey.unexplained << "\n";
    std::cout << "  vertex colour modes seen on grey systems:\n";
    for (const auto& m : grey.greyVertexModes) std::cout << "      " << m.first << " : " << m.second << "\n";
    std::cout << "  sample of unexplained systems:\n";
    for (const auto& s : grey.unexplainedSample) std::cout << "      " << s << "\n";

    std::cout << "\n=== 6. .kf sequences: do they carry non-transform controllers? ===\n";
    std::cout << "  .kf scanned            : " << kfScanned << " (" << kfFailed << " unreadable)\n";
    std::cout << "  NiControllerSequence   : " << kfSequences << "\n";
    std::cout << "  ControllerLink entries : " << kfLinks << "\n";
    std::cout << "  controllerType field:\n";
    for (const auto& t : kfControllerTypes) std::cout << "      " << t.first << " : " << t.second << "\n";
    std::cout << "  propertyType field (non-empty means it targets a property):\n";
    for (const auto& t : kfPropertyTypes) std::cout << "      " << t.first << " : " << t.second << "\n";
    std::cout << "  variable1 field:\n";
    for (const auto& t : kfVariable1) std::cout << "      " << t.first << " : " << t.second << "\n";
    std::cout << "  key volume per non-transform controller type "
                 "(tracks with keys / keys / of which constant):\n";
    {
        long long allKeys = 0, allConstKeys = 0, allTracks = 0, allConstTracks = 0;
        for (const auto& t : kfKeysByType) {
            std::cout << "      " << t.first << " : " << kfTracksWithKeys[t.first] << " tracks, "
                      << t.second << " keys, constant: " << kfConstantTracks[t.first]
                      << " tracks / " << kfConstantKeys[t.first] << " keys"
                      << "   (no data block: " << kfNoDataTracks[t.first]
                      << ", empty: " << kfEmptyTracks[t.first] << ")\n";
            allKeys += t.second;
            allConstKeys += kfConstantKeys[t.first];
            allTracks += kfTracksWithKeys[t.first];
            allConstTracks += kfConstantTracks[t.first];
        }
        std::cout << "      TOTAL : " << allTracks << " tracks, " << allKeys
                  << " keys; dropping constant tracks would remove " << allConstTracks
                  << " tracks / " << allConstKeys << " keys\n";
    }
    std::cout << "  interpolators on non-transform links:\n";
    for (const auto& t : kfNonTransformInterp) std::cout << "      " << t.first << " : " << t.second << "\n";
    std::cout << "  sample:\n";
    for (const auto& s : kfNonTransformSample) std::cout << "      " << s << "\n";

    std::cout << "\n=== 7. Open point: gravityObjectNodeIndex ===\n";
    std::cout << "  NiPSysGravityModifier instances                  : " << gravity.modifiers << "\n";
    std::cout << "  ... with a gravity object pointer                : " << gravity.withGravityObject
              << "\n";
    std::cout << "  ... whose object differs from the attach node    : "
              << gravity.objectDiffersFromAttachNode << "\n";
    std::cout << "  ... with a non-identity rotation between the two : "
              << gravity.nonTrivialRotationBetween << "   <-- the only cases that matter\n";
    for (const auto& s : gravity.sample) std::cout << "      " << s << "\n";

    return 0;
}

// --------------------------------------------------------------------------
// per-file dump
// --------------------------------------------------------------------------

int RunDump(const fs::path& path) {
    std::vector<NiObjectRef> blocks;
    if (!LoadBlocks(path, blocks)) {
        std::cerr << "failed to load " << path << "\n";
        return 1;
    }
    const FileIndex idx = BuildIndex(blocks);

    std::cout << "=== controllers in " << path.filename().string() << " ===\n";
    for (const NiObjectRef& b : blocks) {
        NiObject* obj = static_cast<NiObject*>(b);
        auto* ctrl = dynamic_cast<NiTimeController*>(obj);
        if (ctrl == nullptr) continue;
        const std::string type = TypeOf(obj);
        std::cout << "\n  " << type << "  target=" << TargetKind(obj, idx)
                  << "  flags=" << ctrl->GetFlags() << "  [" << ctrl->GetStartTime() << " .. "
                  << ctrl->GetStopTime() << "]  freq=" << ctrl->GetFrequency() << "\n";

        if (auto* single = dynamic_cast<NiSingleInterpController*>(ctrl)) {
            InterpInfo i = InspectInterpolator(static_cast<NiInterpolator*>(single->GetInterpolator()));
            std::cout << "    interpolator=" << i.type << " keys=" << i.keyCount << " [" << i.startTime
                      << " .. " << i.stopTime << "]" << (i.constantOnly ? " (constant, no data)" : "")
                      << "\n";
        }
        if (dynamic_cast<NiMaterialColorController*>(ctrl) != nullptr) {
            std::cout << "    Target Color: " << Field(obj, "Target Color") << "\n";
        }
        if (auto* tt = dynamic_cast<NiTextureTransformController*>(ctrl)) {
            std::cout << "    slot=" << tt->GetTargetTextureSlot()
                      << " transform=" << tt->GetTextureTransformType() << "\n";
        }
        if (dynamic_cast<NiFlipController*>(ctrl) != nullptr) {
            std::cout << "    Texture Slot: " << Field(obj, "Texture Slot")
                      << "  Delta: " << Field(obj, "Delta")
                      << "  Num Sources: " << Field(obj, "Num Sources") << "\n";
            for (const NiObjectRef& r : obj->GetRefs()) {
                if (r == NULL) continue;
                if (auto* src = dynamic_cast<NiSourceTexture*>(static_cast<NiObject*>(r))) {
                    std::cout << "      source: " << src->GetTextureFileName() << "\n";
                }
            }
        }
        if (auto* uv = dynamic_cast<NiUVController*>(ctrl)) {
            std::cout << "    NiUVData present: " << (uv->GetData() != NULL ? "yes" : "no") << "\n";
            if (uv->GetData() != NULL) {
                std::cout << uv->GetData()->asString(false);
            }
        }
        if (auto* vis = dynamic_cast<NiVisController*>(ctrl)) {
            if (vis->GetData() != NULL) {
                std::cout << "    NiVisData keys: " << vis->GetData()->GetKeys().size() << "\n";
            }
        }
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage:\n"
                  << "  measure-material-controllers corpus <input_root>\n"
                  << "  measure-material-controllers dump   <nif_or_kf>\n";
        return 1;
    }
    const std::string mode = argv[1];
    const fs::path arg = argv[2];
    if (mode == "corpus") return RunCorpus(arg);
    if (mode == "dump") return RunDump(arg);
    std::cerr << "unknown mode\n";
    return 1;
}
