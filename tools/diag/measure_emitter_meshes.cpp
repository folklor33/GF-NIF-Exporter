// Phase 5 correctif: measures whether NiPSysMeshEmitter::meshEmitterMeshNames
// resolve to geometry actually present in the .gfmodel export, and whether
// the referenced source meshes are hidden (NiAVObject::GetVisibility() ==
// false) in the .nif. Confirms/refutes the "emitter mesh filtered out by
// the Phase 3 visibility filter" hypothesis (docs/PHASE5_FINDINGS.md).
//
// Not wired into CMake; built ad hoc like the other tools/diag/*.cpp files.
//
// Usage:
//   measure_emitter_meshes.exe <input_root>

#include "niflib.h"
#include "obj/NiAVObject.h"
#include "obj/NiNode.h"
#include "obj/NiParticleSystem.h"
#include "obj/NiPSysModifier.h"
#include "obj/NiPSysEmitter.h"
#include "obj/NiPSysMeshEmitter.h"
#include "obj/NiTriBasedGeom.h"

#include "nif/HeaderNormalizer.hpp"

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
    int totalRefs = 0;
    int refsResolvedInFile = 0;   // name found among the file's own NiTriBasedGeom blocks
    int refsResolvedHidden = 0;   // resolved AND GetVisibility() == false
    int refsResolvedVisible = 0;  // resolved AND GetVisibility() == true
    int refsUnresolved = 0;       // name not found anywhere in the file
    int filesWithMeshEmitters = 0;
    std::vector<std::string> unresolvedSamples;
    std::vector<std::string> resolvedVisibleSamples;
};

void CollectGeomByName(NiAVObject* obj, std::map<std::string, NiAVObject*>& byName,
                        std::set<NiObject*>& visited) {
    if (obj == nullptr || !visited.insert(obj).second) return;
    if (auto* node = dynamic_cast<NiNode*>(obj)) {
        for (auto& c : node->GetChildren()) {
            CollectGeomByName(static_cast<NiAVObject*>(c), byName, visited);
        }
        return;
    }
    if (dynamic_cast<NiTriBasedGeom*>(obj) != nullptr) {
        byName[obj->GetName()] = obj;
    }
}

void CollectParticleSystems(NiAVObject* obj, std::vector<NiParticleSystem*>& out,
                             std::set<NiObject*>& visited) {
    if (obj == nullptr || !visited.insert(obj).second) return;
    if (auto* ps = dynamic_cast<NiParticleSystem*>(obj)) {
        out.push_back(ps);
    }
    if (auto* node = dynamic_cast<NiNode*>(obj)) {
        for (auto& c : node->GetChildren()) {
            CollectParticleSystems(static_cast<NiAVObject*>(c), out, visited);
        }
    }
}

void ProcessFile(const fs::path& path, Stats& stats) {
    // niflib crashes (not throws) on a block type it does not implement
    // (e.g. NiPhysXScene) -- same guard MeshExtractor.cpp uses before
    // calling ReadNifList.
    std::ifstream file(path, std::ios::binary);
    if (!file) return;
    std::ostringstream ss;
    ss << file.rdbuf();
    std::string bytes = ss.str();
    if (!gfnif::FindUnsupportedBlockType(bytes).empty()) return;

    std::vector<NiObjectRef> objects;
    try {
        std::istringstream stream(bytes, std::ios::binary);
        objects = ReadNifList(stream, nullptr);
    } catch (...) {
        return;
    }
    if (objects.empty()) return;

    std::vector<NiAVObject*> roots;
    for (auto& o : objects) {
        if (auto* av = dynamic_cast<NiAVObject*>(static_cast<NiObject*>(o))) {
            // heuristic: roots are objects with no parent among the block list
            roots.push_back(av);
        }
    }
    // Simplify: just scan every NiAVObject in the file for particle systems
    // and build one name->object map from every NiTriBasedGeom in the file,
    // regardless of hierarchy position (name collisions are rare and
    // visibility is a per-object flag either way).
    std::map<std::string, NiAVObject*> byName;
    std::vector<NiParticleSystem*> systems;
    for (auto& o : objects) {
        auto* av = dynamic_cast<NiAVObject*>(static_cast<NiObject*>(o));
        if (av == nullptr) continue;
        if (dynamic_cast<NiTriBasedGeom*>(av) != nullptr) {
            byName[av->GetName()] = av;
        }
        if (auto* ps = dynamic_cast<NiParticleSystem*>(av)) {
            systems.push_back(ps);
        }
    }

    bool fileHasMeshEmitter = false;
    for (auto* ps : systems) {
        for (auto& r : ps->GetRefs()) {
            if (r == NULL) continue;
            auto* mod = dynamic_cast<NiPSysModifier*>(static_cast<NiObject*>(r));
            if (mod == nullptr) continue;
            auto* meshEmitter = dynamic_cast<NiPSysMeshEmitter*>(mod);
            if (meshEmitter == nullptr) continue;
            fileHasMeshEmitter = true;
            for (auto& mr : meshEmitter->GetRefs()) {
                if (mr == NULL) continue;
                auto* geo = dynamic_cast<NiTriBasedGeom*>(static_cast<NiObject*>(mr));
                if (geo == nullptr) continue;
                ++stats.totalRefs;
                // Prefer resolving by direct object identity (the ref IS the
                // geometry) -- name lookup is a fallback cross-check for
                // whether ParticleExtractor's name-based path (matching the
                // exporter's actual mechanism) would find it too.
                bool hidden = !geo->GetVisibility();
                ++stats.refsResolvedInFile;
                if (hidden) {
                    ++stats.refsResolvedHidden;
                } else {
                    ++stats.refsResolvedVisible;
                    if (stats.resolvedVisibleSamples.size() < 10) {
                        stats.resolvedVisibleSamples.push_back(path.string() + " : " + geo->GetName());
                    }
                }

                // Cross-check: does the name resolve via byName map too
                // (mirrors what a name-based lookup, like the export-side
                // marker matching, would see)?
                auto it = byName.find(geo->GetName());
                if (it == byName.end()) {
                    ++stats.refsUnresolved;
                    if (stats.unresolvedSamples.size() < 10) {
                        stats.unresolvedSamples.push_back(path.string() + " : '" + geo->GetName() + "'");
                    }
                }
            }
        }
    }
    if (fileHasMeshEmitter) ++stats.filesWithMeshEmitters;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: measure_emitter_meshes <input_root>\n";
        return 1;
    }
    fs::path root = argv[1];
    Stats stats;
    int fileCount = 0;
    for (auto& entry : fs::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".nif") continue;
        ++fileCount;
        std::cerr << "processing: " << entry.path().string() << std::endl;
        ProcessFile(entry.path(), stats);
    }

    std::cout << "Scanned " << fileCount << " .nif files\n";
    std::cout << "Files with >=1 NiPSysMeshEmitter: " << stats.filesWithMeshEmitters << "\n";
    std::cout << "Total meshEmitterMeshNames refs: " << stats.totalRefs << "\n";
    std::cout << "  resolved (ref points directly at a NiTriBasedGeom in the file): "
              << stats.refsResolvedInFile << "\n";
    std::cout << "    of which HIDDEN (GetVisibility()==false): " << stats.refsResolvedHidden << "\n";
    std::cout << "    of which VISIBLE: " << stats.refsResolvedVisible << "\n";
    std::cout << "  unresolved by NAME lookup (name not found among file's NiTriBasedGeom blocks): "
              << stats.refsUnresolved << "\n";

    std::cout << "\nSample unresolved-by-name refs:\n";
    for (auto& s : stats.unresolvedSamples) std::cout << "  " << s << "\n";
    std::cout << "\nSample VISIBLE emitter-mesh refs (should still export fine):\n";
    for (auto& s : stats.resolvedVisibleSamples) std::cout << "  " << s << "\n";

    return 0;
}
