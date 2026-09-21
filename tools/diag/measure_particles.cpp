// Phase 5, Etape 1: corpus-wide inventory of particle-system blocks, BEFORE
// any extractor is written. Not part of the shipped exporter; not wired into
// CMakeLists.txt (build ad hoc, see comment at bottom of file).
//
// Answers the five questions the Phase 5 brief asks for:
//   1. which particle-related block types exist, and in how many files
//   2. which emitter types are actually used, and in what proportion
//   3. which modifiers are actually attached, and in what proportion
//   4. which files/entity-types carry particle systems
//   5. are particle-system nodes targeted by an animation controller
//
// Usage:
//   measure_particles.exe corpus <input_root>

#include "niflib.h"
#include "obj/NiAVObject.h"
#include "obj/NiNode.h"
#include "obj/NiParticleSystem.h"
#include "obj/NiParticles.h"
#include "obj/NiParticlesData.h"
#include "obj/NiPSysData.h"
#include "obj/NiPSysModifier.h"
#include "obj/NiPSysEmitter.h"
#include "obj/NiPSysVolumeEmitter.h"
#include "obj/NiPSysBoxEmitter.h"
#include "obj/NiPSysMeshEmitter.h"
#include "obj/NiPSysSphereEmitter.h"
#include "obj/NiPSysCylinderEmitter.h"
#include "obj/NiTimeController.h"
#include "obj/NiTransformController.h"
#include "obj/NiMultiTargetTransformController.h"
#include "obj/NiControllerSequence.h"
#include "obj/NiControllerManager.h"
#include "obj/NiTriBasedGeom.h"
#include "obj/NiProperty.h"
#include "obj/NiTexturingProperty.h"
#include "obj/NiSourceTexture.h"
#include "obj/NiSkinInstance.h"
#include "obj/NiTriShape.h"

#include "nif/HeaderNormalizer.hpp"
#include "nif/MeshExtractor.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;
using namespace Niflib;

namespace {

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

// Walks NiAVObject::GetControllers() across the whole reachable graph and
// returns true if `target` is ever the object a live NiTransformController/
// NiMultiTargetTransformController animates (same "live controller" test
// PHASE4's measure_embedded_controllers.cpp uses).
bool IsAnimatedByEmbeddedController(NiAVObject* target,
                                     const std::vector<NiObjectRef>& blocks) {
    for (const NiObjectRef& b : blocks) {
        auto* obj = static_cast<NiObject*>(b);
        auto* av = dynamic_cast<NiAVObject*>(obj);
        if (av == nullptr) continue;
        for (const auto& ctrlRef : av->GetControllers()) {
            auto* ctrl = static_cast<NiTimeController*>(ctrlRef);
            if (dynamic_cast<NiTransformController*>(ctrl) != nullptr && av == target) {
                return true;
            }
            if (auto* multi = dynamic_cast<NiMultiTargetTransformController*>(ctrl)) {
                for (const auto& extra : multi->GetExtraTargets()) {
                    if (static_cast<NiAVObject*>(extra) == target) return true;
                }
            }
        }
    }
    return false;
}

struct Stats {
    int filesWithParticles = 0;
    int totalSystems = 0;
    std::map<std::string, int> filesByEntityType;
    std::map<std::string, int> systemsByEntityType;

    std::map<std::string, int> emitterTypeCounts;
    std::map<std::string, int> modifierTypeCounts;

    int systemsWithAnimatedAttachNode = 0;
    int systemsAttachedToSkinnedFileNode = 0;
    std::vector<std::string> animatedAttachSample;

    int systemsWithNoEmitter = 0;
    int systemsWithMultipleEmitters = 0;
    std::vector<std::string> multiEmitterSample;

    int meshEmitterMeshRefsTotal = 0;
    int meshEmitterZeroMeshRefs = 0;

    // Parent-chain checks: does the ancestor a particle system is parented
    // under sit inside a skinned skeleton, or share a name with something a
    // companion .kf actually animates?
    int systemsUnderSkinnedAncestor = 0;
    int filesWithParticlesAndCompanionKf = 0;
    int systemsWithAncestorNamedInKf = 0;
    std::vector<std::string> ancestorNamedInKfSample;

    // Does the particle-bearing file itself have any skinned mesh at all
    // (i.e. would scene.skeletons be non-empty today)? If not, particle
    // attach-node resolution needs a node hierarchy independent of whether
    // the file has animation, since today's SceneData::nodes is only built
    // when animation needs it.
    int filesWithParticlesAndSkin = 0;
    int filesWithParticlesNoSkin = 0;
};

std::string EntityTypeOf(const fs::path& relPath) {
    // input/<type>/model/NAME.nif -> <type>
    auto it = relPath.begin();
    if (it == relPath.end()) return "?";
    return it->string();
}

// Ancestor chain names, root-first, from `obj` up to (not including) the
// scene root -- i.e. every NiNode name a particle system is parented under.
std::vector<std::string> AncestorNames(NiAVObject* obj) {
    std::vector<std::string> names;
    Ref<NiNode> parent = obj->GetParent();
    while (parent != NULL) {
        names.push_back(parent->GetName());
        parent = parent->GetParent();
    }
    return names;
}

bool AnyAncestorSkinned(const std::vector<std::string>& ancestorNames,
                        const std::vector<NiObjectRef>& blocks) {
    std::set<std::string> skinnedBoneNames;
    for (const NiObjectRef& b : blocks) {
        auto* shape = dynamic_cast<NiTriShape*>(static_cast<NiObject*>(b));
        if (shape == nullptr) continue;
        NiSkinInstance* skin = shape->GetSkinInstance();
        if (skin == nullptr) continue;
        for (const auto& bone : skin->GetBones()) {
            if (bone != NULL) skinnedBoneNames.insert(static_cast<NiAVObject*>(bone)->GetName());
        }
    }
    for (const std::string& name : ancestorNames) {
        if (skinnedBoneNames.count(name)) return true;
    }
    return false;
}

bool LoadKfTargetNames(const fs::path& kfPath, std::set<std::string>& outNames) {
    std::vector<NiObjectRef> blocks;
    if (!LoadBlocks(kfPath, blocks)) return false;
    for (const NiObjectRef& b : blocks) {
        auto* seq = dynamic_cast<NiControllerSequence*>(static_cast<NiObject*>(b));
        if (seq == nullptr) continue;
        for (const auto& link : seq->GetControllerData()) {
            std::string name = link.nodeName;
            if (!name.empty()) outNames.insert(name);
        }
    }
    return true;
}

void ScanFile(const fs::path& nifPath, const fs::path& root, Stats& stats) {
    std::vector<NiObjectRef> blocks;
    if (!LoadBlocks(nifPath, blocks)) return;

    std::vector<NiParticleSystem*> systems;
    for (const NiObjectRef& b : blocks) {
        if (auto* ps = dynamic_cast<NiParticleSystem*>(static_cast<NiObject*>(b))) {
            systems.push_back(ps);
        }
    }
    if (systems.empty()) return;

    const std::string entityType = EntityTypeOf(fs::relative(nifPath, root));
    ++stats.filesWithParticles;
    stats.filesByEntityType[entityType]++;

    bool hasSkin = false;
    for (const NiObjectRef& b : blocks) {
        auto* shape = dynamic_cast<NiTriShape*>(static_cast<NiObject*>(b));
        if (shape != nullptr && shape->GetSkinInstance() != nullptr) {
            hasSkin = true;
            break;
        }
    }
    if (hasSkin) {
        ++stats.filesWithParticlesAndSkin;
    } else {
        ++stats.filesWithParticlesNoSkin;
    }
    stats.totalSystems += static_cast<int>(systems.size());
    stats.systemsByEntityType[entityType] += static_cast<int>(systems.size());

    std::set<std::string> kfTargetNames;
    std::string kfPath;
    bool hasKf = gfnif::ResolveCompanionKf(nifPath.string(), kfPath);
    if (hasKf) {
        ++stats.filesWithParticlesAndCompanionKf;
        LoadKfTargetNames(kfPath, kfTargetNames);
    }

    for (NiParticleSystem* ps : systems) {
        // Enumerate this system's modifiers via GetRefs() (confirmed in
        // NiParticleSystem.cpp: GetRefs() returns exactly the modifiers
        // vector -- no separate getter exists, see PHASE5_FINDINGS).
        std::vector<NiPSysModifier*> modifiers;
        for (const NiObjectRef& r : ps->GetRefs()) {
            if (r == NULL) continue;
            if (auto* mod = dynamic_cast<NiPSysModifier*>(static_cast<NiObject*>(r))) {
                modifiers.push_back(mod);
            }
        }

        int emitterCount = 0;
        for (NiPSysModifier* mod : modifiers) {
            stats.modifierTypeCounts[mod->GetType().GetTypeName()]++;
            if (auto* emitter = dynamic_cast<NiPSysEmitter*>(mod)) {
                ++emitterCount;
                stats.emitterTypeCounts[emitter->GetType().GetTypeName()]++;
                if (auto* meshEmit = dynamic_cast<NiPSysMeshEmitter*>(emitter)) {
                    // No getter for emitterMeshes either; count via GetRefs().
                    int meshRefs = 0;
                    for (const NiObjectRef& r : meshEmit->GetRefs()) {
                        if (r != NULL && dynamic_cast<NiTriBasedGeom*>(static_cast<NiObject*>(r))) {
                            ++meshRefs;
                        }
                    }
                    stats.meshEmitterMeshRefsTotal += meshRefs;
                    if (meshRefs == 0) ++stats.meshEmitterZeroMeshRefs;
                }
            }
        }
        if (emitterCount == 0) ++stats.systemsWithNoEmitter;
        if (emitterCount > 1) {
            ++stats.systemsWithMultipleEmitters;
            if (stats.multiEmitterSample.size() < 20) {
                stats.multiEmitterSample.push_back(fs::relative(nifPath, root).generic_string() +
                                                    " \"" + ps->GetName() + "\"");
            }
        }

        // Attach-node animation check: is `ps` itself (a NiAVObject sitting
        // in the scene graph) the target of a live embedded controller?
        // Bones/animated-.kf targets are resolved by name in the real
        // extractor; this coarse check only answers "does this happen at
        // all in the corpus".
        if (IsAnimatedByEmbeddedController(ps, blocks)) {
            ++stats.systemsWithAnimatedAttachNode;
            if (stats.animatedAttachSample.size() < 20) {
                stats.animatedAttachSample.push_back(fs::relative(nifPath, root).generic_string() +
                                                      " \"" + ps->GetName() + "\"");
            }
        }

        std::vector<std::string> ancestors = AncestorNames(ps);
        if (AnyAncestorSkinned(ancestors, blocks)) {
            ++stats.systemsUnderSkinnedAncestor;
        }
        if (hasKf) {
            for (const std::string& name : ancestors) {
                if (kfTargetNames.count(name)) {
                    ++stats.systemsWithAncestorNamedInKf;
                    if (stats.ancestorNamedInKfSample.size() < 20) {
                        stats.ancestorNamedInKfSample.push_back(
                            fs::relative(nifPath, root).generic_string() + " \"" + ps->GetName() +
                            "\" under \"" + name + "\"");
                    }
                    break;
                }
            }
        }
    }
}

int RunCorpus(const fs::path& root) {
    std::vector<fs::path> nifFiles;
    for (auto it = fs::recursive_directory_iterator(root); it != fs::recursive_directory_iterator(); ++it) {
        if (!it->is_regular_file()) continue;
        auto p = it->path();
        std::string ext = p.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        // Both .nif (models) and .kf (external animation) can in principle
        // carry particle blocks; the brief only cares about what a model
        // exports, so restrict to model/ .nif files.
        if (ext == ".nif" && p.parent_path().filename() == "model") nifFiles.push_back(p);
    }
    std::sort(nifFiles.begin(), nifFiles.end());

    Stats stats;
    int scanned = 0;
    for (const fs::path& nifPath : nifFiles) {
        ScanFile(nifPath, root, stats);
        ++scanned;
        if (scanned % 500 == 0) std::cerr << "  scanned " << scanned << "/" << nifFiles.size() << "\n";
    }

    std::cout << "=== Particle system corpus inventory (" << nifFiles.size()
              << " model/ .nif files scanned) ===\n\n";

    std::cout << "Files with >=1 NiParticleSystem : " << stats.filesWithParticles << "\n";
    std::cout << "  of which skinned (have SkeletonData today) : " << stats.filesWithParticlesAndSkin
              << "\n";
    std::cout << "  of which skeleton-less                     : " << stats.filesWithParticlesNoSkin
              << "\n";
    std::cout << "Total NiParticleSystem blocks    : " << stats.totalSystems << "\n\n";

    std::cout << "By entity type (files with particles / systems):\n";
    for (auto& [type, n] : stats.filesByEntityType) {
        std::cout << "  " << type << ": " << n << " files, " << stats.systemsByEntityType[type]
                  << " systems\n";
    }

    std::cout << "\nEmitter types actually used:\n";
    for (auto& [type, n] : stats.emitterTypeCounts) {
        std::cout << "  " << n << "\tx " << type << "\n";
    }
    std::cout << "Systems with 0 emitters found : " << stats.systemsWithNoEmitter << "\n";
    std::cout << "Systems with >1 emitter found : " << stats.systemsWithMultipleEmitters << "\n";
    for (auto& s : stats.multiEmitterSample) std::cout << "    " << s << "\n";

    std::cout << "\nModifier types actually attached:\n";
    for (auto& [type, n] : stats.modifierTypeCounts) {
        std::cout << "  " << n << "\tx " << type << "\n";
    }

    std::cout << "\nNiPSysMeshEmitter mesh references: " << stats.meshEmitterMeshRefsTotal
              << " total, " << stats.meshEmitterZeroMeshRefs << " emitters with 0 refs\n";

    std::cout << "\nParticle systems whose own scene-graph node is targeted by a live embedded\n"
              << "NiTransformController/NiMultiTargetTransformController (i.e. an emitter that\n"
              << "moves with an animated attach point): " << stats.systemsWithAnimatedAttachNode
              << "\n";
    for (auto& s : stats.animatedAttachSample) std::cout << "    " << s << "\n";

    std::cout << "\nFiles with particles that have a companion .kf : "
              << stats.filesWithParticlesAndCompanionKf << " / " << stats.filesWithParticles << "\n";
    std::cout << "Systems parented (anywhere up the chain) under a skinned skeleton bone : "
              << stats.systemsUnderSkinnedAncestor << " / " << stats.totalSystems << "\n";
    std::cout << "Systems parented under a node the companion .kf actually names as a\n"
              << "controller target (i.e. genuinely rides an animated bone) : "
              << stats.systemsWithAncestorNamedInKf << " / " << stats.totalSystems << "\n";
    for (auto& s : stats.ancestorNamedInKfSample) std::cout << "    " << s << "\n";

    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3 || std::string(argv[1]) != "corpus") {
        std::cerr << "usage: measure_particles corpus <input_root>\n";
        return 1;
    }
    return RunCorpus(argv[2]);
}
