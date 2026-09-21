// Phase 7 correctif: the three measurements the correctif brief asks for,
// in one pass over the corpus, so that each hypothesis is decided on data
// rather than on reading.
//
//   A. Birth rate -- WHERE is it actually stored? The Phase 7 report
//      (docs/PHASE7_FINDINGS.md 6.1) suggests reading "Birth Rate" off
//      NiPSysEmitter the asString() way. Reading niflib's generated
//      NiPSysEmitter.cpp shows it prints no such field: at this NIF version
//      (20.2.0.8) the birth rate is not an emitter field at all, it is the
//      value driven by the NiPSysEmitterCtlr controller attached to the
//      NiParticleSystem. This measures that: how many systems carry such a
//      controller, what interpolator it uses, and what value it yields.
//
//   B. Mesh emitters and skinning -- correctif point 3. An emitter that
//      samples a skinned mesh samples its BIND POSE buffer, so it cannot
//      follow the animation. Counts how many emitter meshes are skinned,
//      and among those how many are rigid (every vertex influenced by one
//      bone only), which is the cheap fix the brief asks to size.
//
//   C. Colour source -- correctif point 1. For every system whose
//      NiPSysColorModifier keys are all black in RGB, reports what its own
//      NiMaterialProperty carries in emissive/diffuse, to say whether the
//      colour is simply held elsewhere.
//
// Usage:
//   measure-phase7.exe <input_root>

#include "niflib.h"
#include "obj/NiAVObject.h"
#include "obj/NiColorData.h"
#include "obj/NiFloatData.h"
#include "obj/NiFloatInterpolator.h"
#include "obj/NiInterpolator.h"
#include "obj/NiMaterialProperty.h"
#include "obj/NiNode.h"
#include "obj/NiParticleSystem.h"
#include "obj/NiPSysColorModifier.h"
#include "obj/NiPSysEmitter.h"
#include "obj/NiPSysEmitterCtlr.h"
#include "obj/NiPSysEmitterCtlrData.h"
#include "obj/NiPSysMeshEmitter.h"
#include "obj/NiPSysModifier.h"
#include "obj/NiProperty.h"
#include "obj/NiSingleInterpController.h"
#include "obj/NiSkinData.h"
#include "obj/NiSkinInstance.h"
#include "obj/NiTimeController.h"
#include "obj/NiTriBasedGeom.h"
#include "obj/NiTriBasedGeomData.h"

#include "nif/HeaderNormalizer.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace Niflib;

namespace {

struct Stats {
    int files = 0;
    int filesWithParticles = 0;
    int systems = 0;

    // --- A. birth rate ---
    int systemsWithEmitterCtlr = 0;
    std::map<std::string, int> emitterCtlrInterpolatorTypes;
    int birthRateConstant = 0;   // NiFloatInterpolator with no data block
    int birthRateKeyed = 0;      // NiFloatInterpolator with a NiFloatData timeline
    int birthRateUnreadable = 0; // some other interpolator type
    int ctlrNoInterpolator = 0;  // of those, the ones with no interpolator at all
    int ctlrWithCtlrData = 0;    // ... but carrying a NiPSysEmitterCtlrData instead
    std::vector<std::string> ctlrDataSamples;
    double birthRateSum = 0.0;
    float birthRateMin = 1e30f;
    float birthRateMax = -1e30f;
    int birthRateZero = 0;
    std::vector<std::string> birthRateSamples;

    // --- B. emitter meshes / skinning ---
    int emitterMeshRefs = 0;
    int emitterMeshRefsSkinned = 0;
    int emitterMeshRefsSkinnedRigid = 0; // one influence per vertex, everywhere
    int emitterMeshRefsSkinnedNoData = 0;
    std::vector<std::string> skinnedEmitterSamples;

    // --- C. colour ---
    int systemsWithColorModifier = 0;
    int systemsColorKeysAllBlack = 0;
    int blackKeysWithColouredEmissive = 0;
    int blackKeysWithColouredDiffuse = 0;
    int blackKeysWithNoMaterial = 0;
    int blackKeysAllSourcesGrey = 0;
    std::vector<std::string> blackKeySamples;
};

bool IsColoured(const Color3& c) {
    const float mx = std::max({c.r, c.g, c.b});
    const float mn = std::min({c.r, c.g, c.b});
    // "Coloured" = not on the grey axis, with a margin well above float noise.
    return (mx - mn) > 0.02f;
}

std::string Col3(const Color3& c) {
    std::ostringstream o;
    o << "[" << c.r << ", " << c.g << ", " << c.b << "]";
    return o.str();
}

/*! Reads the birth rate a NiPSysEmitterCtlr drives, if it can. Returns false
 *  when the controller's interpolator is not one this probe understands. */
bool ReadBirthRate(NiPSysEmitterCtlr* ctlr, Stats& stats, float& outValue, bool& outKeyed) {
    Ref<NiInterpolator> interp = ctlr->GetInterpolator();
    if (interp == NULL) return false;
    const std::string typeName = interp->GetType().GetTypeName();
    ++stats.emitterCtlrInterpolatorTypes[typeName];

    auto* fi = dynamic_cast<NiFloatInterpolator*>(static_cast<NiObject*>(interp));
    if (fi == nullptr) return false;

    Ref<NiFloatData> data = fi->GetData();
    if (data == NULL) {
        outKeyed = false;
        outValue = fi->GetFloatValue();
        return true;
    }
    const std::vector<Key<float>> keys = data->GetKeys();
    if (keys.empty()) {
        outKeyed = false;
        outValue = fi->GetFloatValue();
        return true;
    }
    outKeyed = true;
    // Report the mean of the timeline; the export will keep the whole curve,
    // this is only to characterise the magnitudes.
    double sum = 0.0;
    for (const Key<float>& k : keys) sum += k.data;
    outValue = static_cast<float>(sum / static_cast<double>(keys.size()));
    return true;
}

/*! True when every vertex the skin weights is influenced by exactly one bone
 *  -- the "rigidly bound to a single bone" case the brief asks to size. */
bool IsRigidSkin(NiSkinInstance* skin, int vertexCount) {
    Ref<NiSkinData> data = skin->GetSkinData();
    if (data == NULL) return false;
    std::vector<int> influences(static_cast<size_t>(std::max(vertexCount, 0)), 0);
    const unsigned int boneCount = data->GetBoneCount();
    for (unsigned int b = 0; b < boneCount; ++b) {
        for (const SkinWeight& w : data->GetBoneWeights(b)) {
            if (w.weight <= 0.0f) continue;
            if (w.index < influences.size()) ++influences[w.index];
        }
    }
    for (int n : influences) {
        if (n > 1) return false;
    }
    return true;
}

NiMaterialProperty* FindMaterial(NiAVObject* obj) {
    for (const Ref<NiProperty>& p : obj->GetProperties()) {
        if (p == NULL) continue;
        if (auto* mat = dynamic_cast<NiMaterialProperty*>(static_cast<NiObject*>(p))) {
            return mat;
        }
    }
    return nullptr;
}

void ProcessFile(const fs::path& path, Stats& stats) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return;
    std::ostringstream ss;
    ss << file.rdbuf();
    std::string bytes = ss.str();

    gfnif::HeaderNormalizationReport header;
    gfnif::NormalizeNifHeader(bytes, header);
    if (!gfnif::FindUnsupportedBlockType(bytes).empty()) return;

    std::vector<NiObjectRef> objects;
    try {
        std::istringstream stream(bytes, std::ios::binary);
        objects = ReadNifList(stream, nullptr);
    } catch (...) {
        return;
    }
    if (objects.empty()) return;
    ++stats.files;

    std::vector<NiParticleSystem*> systems;
    for (const NiObjectRef& o : objects) {
        if (auto* ps = dynamic_cast<NiParticleSystem*>(static_cast<NiObject*>(o))) {
            systems.push_back(ps);
        }
    }
    if (systems.empty()) return;
    ++stats.filesWithParticles;

    for (NiParticleSystem* ps : systems) {
        ++stats.systems;
        const std::string where = path.filename().string() + " / " + ps->GetName();

        // --- A. birth rate, off the NiPSysEmitterCtlr controller chain ---
        for (const Ref<NiTimeController>& c : ps->GetControllers()) {
            if (c == NULL) continue;
            auto* emitterCtlr = dynamic_cast<NiPSysEmitterCtlr*>(static_cast<NiObject*>(c));
            if (emitterCtlr == nullptr) continue;
            ++stats.systemsWithEmitterCtlr;
            float rate = 0.0f;
            bool keyed = false;
            if (!ReadBirthRate(emitterCtlr, stats, rate, keyed)) {
                ++stats.birthRateUnreadable;
                if (emitterCtlr->GetInterpolator() == NULL) ++stats.ctlrNoInterpolator;
                Ref<NiPSysEmitterCtlrData> cd = emitterCtlr->GetData();
                if (cd != NULL) {
                    ++stats.ctlrWithCtlrData;
                    if (stats.ctlrDataSamples.size() < 6) {
                        std::string text =
                            static_cast<NiObject*>(cd)->asString(true).substr(0, 400);
                        for (char& ch : text) {
                            if (ch == '\n') ch = '|';
                        }
                        stats.ctlrDataSamples.push_back(where + " : " + text);
                    }
                }
                continue;
            }
            if (keyed) {
                ++stats.birthRateKeyed;
            } else {
                ++stats.birthRateConstant;
            }
            if (rate == 0.0f) ++stats.birthRateZero;
            stats.birthRateSum += rate;
            stats.birthRateMin = std::min(stats.birthRateMin, rate);
            stats.birthRateMax = std::max(stats.birthRateMax, rate);
            if (stats.birthRateSamples.size() < 12) {
                std::ostringstream o;
                o << where << " : " << rate << (keyed ? " (keyed)" : " (constant)");
                stats.birthRateSamples.push_back(o.str());
            }
        }

        // --- B/C. walk this system's modifiers ---
        bool colorKeysPresent = false;
        bool colorKeysAllBlack = true;
        for (const NiObjectRef& r : ps->GetRefs()) {
            if (r == NULL) continue;
            auto* mod = dynamic_cast<NiPSysModifier*>(static_cast<NiObject*>(r));
            if (mod == nullptr) continue;

            if (auto* meshEmitter = dynamic_cast<NiPSysMeshEmitter*>(mod)) {
                for (const NiObjectRef& mr : meshEmitter->GetRefs()) {
                    if (mr == NULL) continue;
                    auto* geo = dynamic_cast<NiTriBasedGeom*>(static_cast<NiObject*>(mr));
                    if (geo == nullptr) continue;
                    ++stats.emitterMeshRefs;
                    Ref<NiSkinInstance> skin = geo->GetSkinInstance();
                    if (skin == NULL) continue;
                    ++stats.emitterMeshRefsSkinned;
                    int vertexCount = 0;
                    if (auto* gd = dynamic_cast<NiTriBasedGeomData*>(
                            static_cast<NiObject*>(geo->GetData()))) {
                        vertexCount = static_cast<int>(gd->GetVertexCount());
                    }
                    if (skin->GetSkinData() == NULL) {
                        ++stats.emitterMeshRefsSkinnedNoData;
                    } else if (IsRigidSkin(static_cast<NiSkinInstance*>(skin), vertexCount)) {
                        ++stats.emitterMeshRefsSkinnedRigid;
                    }
                    if (stats.skinnedEmitterSamples.size() < 12) {
                        std::ostringstream o;
                        o << where << " -> '" << geo->GetName() << "' ("
                          << (skin->GetSkinData() != NULL
                                  ? std::to_string(skin->GetSkinData()->GetBoneCount())
                                  : std::string("?"))
                          << " bones, " << vertexCount << " verts)";
                        stats.skinnedEmitterSamples.push_back(o.str());
                    }
                }
                continue;
            }

            if (auto* color = dynamic_cast<NiPSysColorModifier*>(mod)) {
                for (const NiObjectRef& cr : color->GetRefs()) {
                    if (cr == NULL) continue;
                    auto* cd = dynamic_cast<NiColorData*>(static_cast<NiObject*>(cr));
                    if (cd == nullptr) continue;
                    for (const Key<Color4>& k : cd->GetKeys()) {
                        colorKeysPresent = true;
                        if (k.data.r > 0.004f || k.data.g > 0.004f || k.data.b > 0.004f) {
                            colorKeysAllBlack = false;
                        }
                    }
                }
            }
        }

        if (!colorKeysPresent) continue;
        ++stats.systemsWithColorModifier;
        if (!colorKeysAllBlack) continue;
        ++stats.systemsColorKeysAllBlack;

        NiMaterialProperty* mat = FindMaterial(ps);
        if (mat == nullptr) {
            ++stats.blackKeysWithNoMaterial;
            continue;
        }
        const Color3 emissive = mat->GetEmissiveColor();
        const Color3 diffuse = mat->GetDiffuseColor();
        const bool emiColoured = IsColoured(emissive);
        const bool difColoured = IsColoured(diffuse);
        if (emiColoured) ++stats.blackKeysWithColouredEmissive;
        if (difColoured) ++stats.blackKeysWithColouredDiffuse;
        if (!emiColoured && !difColoured) ++stats.blackKeysAllSourcesGrey;
        if (stats.blackKeySamples.size() < 15) {
            stats.blackKeySamples.push_back(where + " : emissive " + Col3(emissive) +
                                            " diffuse " + Col3(diffuse));
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: measure-phase7 <input_root>\n";
        return 1;
    }
    Stats s;
    for (const auto& entry : fs::recursive_directory_iterator(argv[1])) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".nif") continue;
        ProcessFile(entry.path(), s);
    }

    std::cout << "Files parsed                        : " << s.files << "\n";
    std::cout << "  with >=1 NiParticleSystem         : " << s.filesWithParticles << "\n";
    std::cout << "Particle systems                    : " << s.systems << "\n";

    std::cout << "\n--- A. birth rate -------------------------------------\n";
    std::cout << "systems with a NiPSysEmitterCtlr    : " << s.systemsWithEmitterCtlr << "\n";
    std::cout << "  readable, constant value          : " << s.birthRateConstant << "\n";
    std::cout << "  readable, keyed timeline          : " << s.birthRateKeyed << "\n";
    std::cout << "  interpolator not readable         : " << s.birthRateUnreadable << "\n";
    std::cout << "    of those, no interpolator at all: " << s.ctlrNoInterpolator << "\n";
    std::cout << "    of those, a NiPSysEmitterCtlrData: " << s.ctlrWithCtlrData << "\n";
    for (const std::string& sample : s.ctlrDataSamples) std::cout << "      " << sample << "\n";
    const int readable = s.birthRateConstant + s.birthRateKeyed;
    if (readable > 0) {
        std::cout << "  value  min / mean / max           : " << s.birthRateMin << " / "
                  << (s.birthRateSum / readable) << " / " << s.birthRateMax << "\n";
        std::cout << "  exactly zero                      : " << s.birthRateZero << "\n";
    }
    std::cout << "  interpolator types seen:\n";
    for (const auto& [name, n] : s.emitterCtlrInterpolatorTypes) {
        std::cout << "    " << name << " : " << n << "\n";
    }
    for (const std::string& sample : s.birthRateSamples) std::cout << "    " << sample << "\n";

    std::cout << "\n--- B. emitter meshes and skinning --------------------\n";
    std::cout << "mesh-emitter geometry references    : " << s.emitterMeshRefs << "\n";
    std::cout << "  skinned (NiSkinInstance present)  : " << s.emitterMeshRefsSkinned << "\n";
    std::cout << "    rigid, 1 influence per vertex   : " << s.emitterMeshRefsSkinnedRigid << "\n";
    std::cout << "    skinned but no NiSkinData       : " << s.emitterMeshRefsSkinnedNoData << "\n";
    for (const std::string& sample : s.skinnedEmitterSamples) std::cout << "    " << sample << "\n";

    std::cout << "\n--- C. colour source ----------------------------------\n";
    std::cout << "systems with colour keys            : " << s.systemsWithColorModifier << "\n";
    std::cout << "  whose keys are all black in RGB   : " << s.systemsColorKeysAllBlack << "\n";
    std::cout << "    with a COLOURED emissive        : " << s.blackKeysWithColouredEmissive << "\n";
    std::cout << "    with a COLOURED diffuse         : " << s.blackKeysWithColouredDiffuse << "\n";
    std::cout << "    with neither (grey everywhere)  : " << s.blackKeysAllSourcesGrey << "\n";
    std::cout << "    with no NiMaterialProperty      : " << s.blackKeysWithNoMaterial << "\n";
    for (const std::string& sample : s.blackKeySamples) std::cout << "    " << sample << "\n";

    return 0;
}
