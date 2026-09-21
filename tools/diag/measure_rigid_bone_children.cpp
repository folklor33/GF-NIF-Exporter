// Sujet A measurement: rigid (non-skinned) meshes whose ancestor node is a
// bone (i.e. referenced by some NiSkinInstance's bone list in the same file).
// Extended per user follow-up: measure how many non-bone nodes sit between
// the mesh and its nearest bone ancestor, and whether any such intermediate
// node is itself animated (has a NiTimeController) or is a NiBillboardNode --
// either would make "flatten straight to nearest bone" wrong.
// Two modes:
//   single file arg  -> verbose inventory (name, skinned?, parent chain, bone?)
//   --corpus <dir>   -> aggregate count across all .nif files
#include "niflib.h"
#include "obj/NiAVObject.h"
#include "obj/NiNode.h"
#include "obj/NiBillboardNode.h"
#include "obj/NiTriBasedGeom.h"
#include "obj/NiSkinInstance.h"
#include "obj/NiTimeController.h"
#include "obj/NiTransformController.h"

#include "nif/HeaderNormalizer.hpp"

#include <filesystem>
#include <iostream>
#include <set>
#include <sstream>
#include <vector>

using namespace Niflib;
namespace fs = std::filesystem;

struct FileResult {
    int rigidMeshes = 0;
    int rigidMeshesUnderBone = 0;
    int rigidMeshesDirectBoneParent = 0;   // 0 intermediate nodes
    int rigidMeshesWithIntermediates = 0;  // >=1 intermediate non-bone node
    int rigidMeshesWithAnimatedIntermediate = 0;
    int rigidMeshesWithBillboardIntermediate = 0;
    bool hasSkeleton = false;
};

static NiNode* ParentOf(NiAVObject* obj) {
    return obj->GetParent();
}

// Walks up from obj (exclusive) to the nearest bone ancestor (exclusive).
// Returns false if no bone ancestor exists. Fills `intermediates` with every
// non-bone node passed through, nearest-first.
static bool FindNearestBoneAncestor(NiAVObject* obj, const std::set<NiObject*>& bones,
                                     std::vector<NiNode*>& intermediates) {
    NiNode* p = ParentOf(obj);
    while (p != nullptr) {
        if (bones.count(static_cast<NiObject*>(p))) return true;
        intermediates.push_back(p);
        p = ParentOf(static_cast<NiAVObject*>(p));
    }
    return false;
}

static bool HasTransformController(NiAVObject* obj) {
    for (const auto& ctrlRef : obj->GetControllers()) {
        if (dynamic_cast<NiTransformController*>(static_cast<NiTimeController*>(ctrlRef)) != nullptr) {
            return true;
        }
    }
    return false;
}

static FileResult AnalyzeFile(const std::string& path, bool verbose) {
    FileResult res;
    std::string bytes;
    gfnif::ReadWholeFile(path, bytes);
    gfnif::HeaderNormalizationReport rep;
    gfnif::NormalizeNifHeader(bytes, rep);
    std::vector<NiObjectRef> blocks;
    std::istringstream stream(bytes, std::ios::binary);
    try {
        blocks = ReadNifList(stream, nullptr);
    } catch (...) {
        return res;
    }

    std::set<NiObject*> bones;
    std::vector<NiTriBasedGeom*> shapes;
    for (const NiObjectRef& b : blocks) {
        NiObject* obj = static_cast<NiObject*>(b);
        if (auto* shape = dynamic_cast<NiTriBasedGeom*>(obj)) {
            shapes.push_back(shape);
            if (NiSkinInstance* skin = shape->GetSkinInstance()) {
                for (const auto& bone : skin->GetBones()) {
                    if (bone != NULL) bones.insert(static_cast<NiObject*>(bone));
                }
            }
        }
    }
    res.hasSkeleton = !bones.empty();
    if (!res.hasSkeleton) return res;

    for (NiTriBasedGeom* shape : shapes) {
        bool skinned = shape->GetSkinInstance() != nullptr;
        if (skinned) continue;
        res.rigidMeshes++;
        std::vector<NiNode*> intermediates;
        bool underBone = FindNearestBoneAncestor(static_cast<NiAVObject*>(shape), bones, intermediates);
        if (!underBone) continue;
        res.rigidMeshesUnderBone++;
        if (intermediates.empty()) {
            res.rigidMeshesDirectBoneParent++;
        } else {
            res.rigidMeshesWithIntermediates++;
        }
        bool anyAnimated = false;
        bool anyBillboard = false;
        for (NiNode* n : intermediates) {
            if (HasTransformController(static_cast<NiAVObject*>(n))) anyAnimated = true;
            if (dynamic_cast<NiBillboardNode*>(n) != nullptr) anyBillboard = true;
        }
        if (anyAnimated) res.rigidMeshesWithAnimatedIntermediate++;
        if (anyBillboard) res.rigidMeshesWithBillboardIntermediate++;

        if (verbose) {
            std::cout << shape->GetName() << ": rigid, parent="
                      << (ParentOf(static_cast<NiAVObject*>(shape))
                              ? ParentOf(static_cast<NiAVObject*>(shape))->GetName()
                              : "<none>")
                      << ", intermediates=" << intermediates.size();
            if (!intermediates.empty()) {
                std::cout << " [";
                for (size_t i = 0; i < intermediates.size(); ++i) {
                    if (i) std::cout << " > ";
                    std::cout << intermediates[i]->GetName();
                    if (HasTransformController(static_cast<NiAVObject*>(intermediates[i]))) std::cout << "(animated)";
                    if (dynamic_cast<NiBillboardNode*>(intermediates[i]) != nullptr) std::cout << "(billboard)";
                }
                std::cout << "]";
            }
            std::cout << "\n";
        }
    }
    return res;
}

int main(int argc, char** argv) {
    if (argc < 2) return 1;
    std::string arg1 = argv[1];
    if (arg1 == "--corpus" && argc >= 3) {
        int filesWithSkeleton = 0, filesWithRigidUnderBone = 0;
        int totalRigidUnderBone = 0, totalRigid = 0;
        int totalDirect = 0, totalWithIntermediates = 0;
        int totalAnimatedIntermediate = 0, totalBillboardIntermediate = 0;
        for (auto& entry : fs::recursive_directory_iterator(argv[2])) {
            if (entry.path().extension() != ".nif") continue;
            const std::string stem = entry.path().stem().string();
            // Permanent known-skip files (unsupported version/blocks) --
            // documented corpus-wide skips, not part of this measurement.
            if (stem == "WF20" || stem == "M156" || stem == "N600") continue;
            std::cerr << entry.path().string() << std::endl;
            FileResult r = AnalyzeFile(entry.path().string(), false);
            if (!r.hasSkeleton) continue;
            filesWithSkeleton++;
            totalRigid += r.rigidMeshes;
            totalRigidUnderBone += r.rigidMeshesUnderBone;
            totalDirect += r.rigidMeshesDirectBoneParent;
            totalWithIntermediates += r.rigidMeshesWithIntermediates;
            totalAnimatedIntermediate += r.rigidMeshesWithAnimatedIntermediate;
            totalBillboardIntermediate += r.rigidMeshesWithBillboardIntermediate;
            if (r.rigidMeshesUnderBone > 0) filesWithRigidUnderBone++;
        }
        std::cout << "Files with a skeleton: " << filesWithSkeleton << "\n";
        std::cout << "  of which >=1 rigid mesh has a bone ancestor: " << filesWithRigidUnderBone << "\n";
        std::cout << "Total rigid meshes in skeleton files: " << totalRigid << "\n";
        std::cout << "Total rigid meshes with a bone ancestor: " << totalRigidUnderBone << "\n";
        std::cout << "  direct bone parent (0 intermediates): " << totalDirect << "\n";
        std::cout << "  with >=1 intermediate non-bone node:  " << totalWithIntermediates << "\n";
        std::cout << "    of which >=1 intermediate is animated (has NiTransformController): "
                  << totalAnimatedIntermediate << "\n";
        std::cout << "    of which >=1 intermediate is a NiBillboardNode: "
                  << totalBillboardIntermediate << "\n";
        return 0;
    }
    AnalyzeFile(arg1, true);
    return 0;
}
