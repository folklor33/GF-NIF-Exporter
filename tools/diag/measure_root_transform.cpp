// One-off diagnostic (Phase 4 follow-up, Task B): dumps skeleton root bone[0]
// bindMatrixLocal (== skeletonRoot->GetWorldTransform(), per
// SkeletonExtractor::EnsureSkeleton) decomposed into translation/rotation/scale
// for every skinned .nif in the corpus, plus the raw ancestor chain from the
// file's root block down to the skeleton root for named "flagged" files.
//
// Not part of the shipped exporter; not wired into CMakeLists.txt.
//
// Usage:
//   measure_root_transform.exe corpus <input_root>          corpus-wide scan
//   measure_root_transform.exe chain  <nif_path>             ancestor chain dump for one file
//   measure_root_transform.exe static <nif_path>             unskinned mesh world-position sample vs skinned path

#include "niflib.h"
#include "nif_math.h"
#include "obj/NiAVObject.h"
#include "obj/NiGeometry.h"
#include "obj/NiGeometryData.h"
#include "obj/NiNode.h"
#include "obj/NiSkinData.h"
#include "obj/NiSkinInstance.h"
#include "obj/NiTriShape.h"

#include "nif/HeaderNormalizer.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <set>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;
using namespace Niflib;

namespace {

struct Decomposed {
    float t[3];
    float rotDeg[3]; // Euler XYZ extraction, informational only
    float scale[3];
    float maxOffDiagRotDeg; // angle from identity via trace method
};

// Extract translation from row-vector Matrix44 (translation in row 3),
// per SkeletonExtractor::ToColumnMajor's own documented convention.
Decomposed Decompose(const Matrix44& m) {
    Decomposed d{};
    d.t[0] = m[3][0]; d.t[1] = m[3][1]; d.t[2] = m[3][2];
    // Column norms give per-axis scale (row-vector basis vectors are rows 0..2).
    for (int r = 0; r < 3; ++r) {
        float len = std::sqrt(m[r][0]*m[r][0] + m[r][1]*m[r][1] + m[r][2]*m[r][2]);
        d.scale[r] = len;
    }
    // Normalize the 3x3 to get pure rotation, then compute angle-from-identity
    // via the trace formula: angle = acos((trace-1)/2).
    float R[3][3];
    for (int r = 0; r < 3; ++r) {
        float len = d.scale[r] > 1e-8f ? d.scale[r] : 1.0f;
        for (int c = 0; c < 3; ++c) R[r][c] = m[r][c] / len;
    }
    float trace = R[0][0] + R[1][1] + R[2][2];
    float cosang = std::max(-1.0f, std::min(1.0f, (trace - 1.0f) * 0.5f));
    float angle = std::acos(cosang) * 180.0f / 3.14159265f;
    d.maxOffDiagRotDeg = angle;
    // Rough Euler XYZ (informational, not used for the threshold decision)
    d.rotDeg[0] = std::atan2(R[2][1], R[2][2]) * 180.0f / 3.14159265f;
    d.rotDeg[1] = std::atan2(-R[2][0], std::sqrt(R[2][1]*R[2][1]+R[2][2]*R[2][2])) * 180.0f / 3.14159265f;
    d.rotDeg[2] = std::atan2(R[1][0], R[0][0]) * 180.0f / 3.14159265f;
    return d;
}

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

// Finds the first skinned NiTriShape's skeleton root, walking all root blocks.
NiNode* FindSkeletonRoot(const std::vector<NiObjectRef>& blocks, NiTriShape** outShape = nullptr) {
    for (const NiObjectRef& b : blocks) {
        auto* shape = dynamic_cast<NiTriShape*>(static_cast<NiObject*>(b));
        if (shape == nullptr) continue;
        NiSkinInstance* skin = shape->GetSkinInstance();
        if (skin == nullptr) continue;
        NiNode* root = skin->GetSkeletonRoot();
        if (root != nullptr) {
            if (outShape) *outShape = shape;
            return root;
        }
    }
    return nullptr;
}

void PrintDecomposed(const char* label, const Matrix44& m) {
    Decomposed d = Decompose(m);
    std::cout << label << " translation=(" << d.t[0] << ", " << d.t[1] << ", " << d.t[2] << ")"
              << " scale=(" << d.scale[0] << ", " << d.scale[1] << ", " << d.scale[2] << ")"
              << " angleFromIdentityDeg=" << d.maxOffDiagRotDeg
              << " eulerXYZdeg=(" << d.rotDeg[0] << ", " << d.rotDeg[1] << ", " << d.rotDeg[2] << ")\n";
}

int RunCorpus(const fs::path& root) {
    std::vector<fs::path> nifFiles;
    for (auto it = fs::recursive_directory_iterator(root); it != fs::recursive_directory_iterator(); ++it) {
        if (!it->is_regular_file()) continue;
        auto p = it->path();
        std::string ext = p.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == ".nif" && p.parent_path().filename() == "model") nifFiles.push_back(p);
    }
    std::sort(nifFiles.begin(), nifFiles.end());

    int skinnedCount = 0;
    std::vector<std::string> nonIdentityRotation;
    std::vector<std::string> largeTranslation;
    constexpr float kAngleThresholdDeg = 5.0f;
    constexpr float kTranslationThreshold = 5.0f;

    for (const fs::path& nifPath : nifFiles) {
        std::vector<NiObjectRef> blocks;
        if (!LoadBlocks(nifPath, blocks)) continue;
        NiNode* skelRoot = FindSkeletonRoot(blocks);
        if (skelRoot == nullptr) continue;
        ++skinnedCount;

        Matrix44 world = skelRoot->GetWorldTransform();
        Decomposed d = Decompose(world);
        std::string key = fs::relative(nifPath, root).generic_string();

        std::cout << key << "\troot=" << skelRoot->GetName()
                  << "\tt=(" << d.t[0] << "," << d.t[1] << "," << d.t[2] << ")"
                  << "\tscale=(" << d.scale[0] << "," << d.scale[1] << "," << d.scale[2] << ")"
                  << "\tangleDeg=" << d.maxOffDiagRotDeg << "\n";

        if (d.maxOffDiagRotDeg > kAngleThresholdDeg) nonIdentityRotation.push_back(key);
        if (std::fabs(d.t[1]) > kTranslationThreshold || std::fabs(d.t[2]) > kTranslationThreshold) {
            largeTranslation.push_back(key);
        }
    }

    std::cerr << "\n=== SUMMARY ===\n";
    std::cerr << "Skinned models scanned: " << skinnedCount << "\n";
    std::cerr << "Non-identity rotation (angle > " << kAngleThresholdDeg << " deg): "
              << nonIdentityRotation.size() << "\n";
    for (auto& f : nonIdentityRotation) std::cerr << "    ROT  " << f << "\n";
    std::cerr << "Large translation.y or .z (> " << kTranslationThreshold << " units): "
              << largeTranslation.size() << "\n";
    for (auto& f : largeTranslation) std::cerr << "    TR   " << f << "\n";
    return 0;
}

void WalkChainPrint(NiAVObject* obj, int depth, NiNode* stopAt) {
    if (obj == nullptr) return;
    std::string indent(depth * 2, ' ');
    Matrix44 local = obj->GetLocalTransform();
    Decomposed d = Decompose(local);
    std::cout << indent << "- " << obj->GetName() << " [" << obj->GetType().GetTypeName() << "]"
              << " localT=(" << d.t[0] << "," << d.t[1] << "," << d.t[2] << ")"
              << " localScale=(" << d.scale[0] << "," << d.scale[1] << "," << d.scale[2] << ")"
              << " localAngleDeg=" << d.maxOffDiagRotDeg << "\n";
    if (obj == stopAt) {
        std::cout << indent << "  ^^^ SKELETON ROOT ^^^\n";
        return;
    }
    if (auto* node = dynamic_cast<NiNode*>(obj)) {
        for (const auto& c : node->GetChildren()) {
            WalkChainPrint(static_cast<NiAVObject*>(c), depth + 1, stopAt);
        }
    }
}

int RunChain(const fs::path& nifPath) {
    std::vector<NiObjectRef> blocks;
    if (!LoadBlocks(nifPath, blocks)) {
        std::cerr << "failed to load " << nifPath << "\n";
        return 1;
    }
    NiTriShape* shape = nullptr;
    NiNode* skelRoot = FindSkeletonRoot(blocks, &shape);
    if (skelRoot == nullptr) {
        std::cerr << "no skinned shape found in " << nifPath << "\n";
        return 1;
    }
    std::cout << "Skeleton root: " << skelRoot->GetName() << "\n";
    PrintDecomposed("  world transform of skeleton root:", skelRoot->GetWorldTransform());

    std::set<NiObject*> referenced;
    for (const NiObjectRef& b : blocks) {
        for (const NiObjectRef& r : static_cast<NiObject*>(b)->GetRefs()) {
            if (r != NULL) referenced.insert(static_cast<NiObject*>(r));
        }
    }
    std::cout << "\nFull tree from unreferenced root(s) down (stopping recursion at skeleton root):\n";
    for (const NiObjectRef& b : blocks) {
        NiObject* obj = static_cast<NiObject*>(b);
        if (referenced.count(obj)) continue;
        if (auto* av = dynamic_cast<NiAVObject*>(obj)) {
            WalkChainPrint(av, 0, skelRoot);
        }
    }
    return 0;
}

int RunStaticVsSkinned(const fs::path& nifPath) {
    std::vector<NiObjectRef> blocks;
    if (!LoadBlocks(nifPath, blocks)) {
        std::cerr << "failed to load " << nifPath << "\n";
        return 1;
    }
    NiTriShape* skinnedShape = nullptr;
    NiNode* skelRoot = FindSkeletonRoot(blocks, &skinnedShape);
    if (skelRoot == nullptr) {
        std::cerr << "no skinned shape found\n";
        return 1;
    }
    std::cout << "Skeleton root world transform:\n";
    PrintDecomposed("  ", skelRoot->GetWorldTransform());

    bool foundUnskinned = false;
    for (const NiObjectRef& b : blocks) {
        auto* shape = dynamic_cast<NiTriShape*>(static_cast<NiObject*>(b));
        if (shape == nullptr) continue;
        if (shape->GetSkinInstance() != nullptr) continue; // skip skinned ones
        foundUnskinned = true;
        Matrix44 shapeWorld = shape->GetWorldTransform();
        std::cout << "Unskinned shape '" << shape->GetName() << "' own world transform:\n";
        PrintDecomposed("  ", shapeWorld);
        // Sample first vertex world position via this shape's own world transform.
        NiGeometryData* gdata = shape->GetData();
        std::vector<Vector3> verts;
        if (gdata != nullptr) verts = gdata->GetVertices();
        if (!verts.empty()) {
            Vector3 wp = shapeWorld * verts[0];
            std::cout << "  first vertex local=(" << verts[0].x << "," << verts[0].y << "," << verts[0].z
                      << ") world=(" << wp.x << "," << wp.y << "," << wp.z << ")\n";
        }
    }
    if (!foundUnskinned) {
        std::cout << "No unskinned NiTriShape found in this file -- cannot cross-check static vs skinned path.\n";
    }

    // Also report skinned shape's first bone binding world position for comparison.
    if (skinnedShape != nullptr) {
        NiSkinInstance* skin = skinnedShape->GetSkinInstance();
        auto bones = skin->GetBones();
        if (!bones.empty()) {
            auto* b0 = static_cast<NiNode*>(bones[0]);
            std::cout << "Skinned shape's bone[0] ('" << b0->GetName() << "') world transform:\n";
            PrintDecomposed("  ", b0->GetWorldTransform());
        }
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage:\n"
                  << "  measure_root_transform corpus <input_root>\n"
                  << "  measure_root_transform chain  <nif_path>\n"
                  << "  measure_root_transform static <nif_path>\n";
        return 1;
    }
    std::string mode = argv[1];
    fs::path arg = argv[2];
    if (mode == "corpus") return RunCorpus(arg);
    if (mode == "chain") return RunChain(arg);
    if (mode == "static") return RunStaticVsSkinned(arg);
    std::cerr << "unknown mode " << mode << "\n";
    return 1;
}
