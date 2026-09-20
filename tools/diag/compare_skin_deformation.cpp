// One-off diagnostic: compares niflib's own NiGeometry::GetSkinDeformation
// (ground truth) against the exact math our exporter's ApplySkin uses
// (vertexWorld = v_row * (boneOffset * boneWorld), skipping the final
// geomWorld.Inverse() step GetSkinDeformation applies) to check whether our
// stored skinMatrix reproduces the correct bind-pose world position.
//
// Not part of the shipped exporter; not wired into CMakeLists.txt.
//
// Usage: compare_skin_deformation.exe <nif_path>

#include "niflib.h"
#include "nif_math.h"
#include "obj/NiAVObject.h"
#include "obj/NiGeometry.h"
#include "obj/NiGeometryData.h"
#include "obj/NiNode.h"
#include "obj/NiSkinData.h"
#include "obj/NiSkinInstance.h"
#include "obj/NiTriShape.h"
#include "obj/NiControllerSequence.h"
#include "obj/NiTransformInterpolator.h"
#include "obj/NiTransformData.h"
#include "obj/NiObjectNET.h"
#include "Key.h"

#include <iostream>
#include <cmath>
#include <vector>
#include <filesystem>
#include <algorithm>

using namespace Niflib;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: compare_skin_deformation.exe <nif_path>|measure <root>\n";
        return 1;
    }
    // Corpus-wide measurement: for each skinned NiTriShape, check whether the
    // node hierarchy's own transforms agree with the pose implied by the
    // skin's bind data. Disagreement means bindMatrixLocal (sourced from
    // GetLocalTransform()) will NOT match the pose the mesh was actually
    // skinned against -- the exact quirk blender_niftools_addon's
    // store_bind_matrices() silently corrects for display.
    //
    // Measure: for bone b, boneOffset^-1 (the skin's own claim of bone-world
    // at bind time, since boneOffset = bindWorld^-1 * geomWorld roughly) vs
    // the node hierarchy's boneWorld. If they match, no mismatch. Compare via
    // the vertex-position route instead, which is unambiguous: does using
    // boneWorld from the CURRENT node hierarchy reproduce a shape close to
    // itself when checked bone-by-bone for internal consistency (same bone
    // used by >1 mesh should agree)? Simpler and directly meaningful:
    // recompute skinMatrix's implied bone-world (boneOffset^-1) and diff
    // against the node hierarchy's actual boneWorld; a mismatch beyond
    // tolerance flags the file.
    if (std::string(argv[1]) == "measure") {
        if (argc < 3) { std::cerr << "usage: measure <root>\n"; return 1; }
        std::string root = argv[2];
        namespace fs = std::filesystem;
        size_t total = 0, mismatched = 0;
        double worstDiff = 0.0;
        std::string worstFile;
        for (auto& entry : fs::recursive_directory_iterator(root)) {
            if (entry.path().extension() != ".nif") continue;
            std::vector<NiObjectRef> fblocks;
            try {
                fblocks = ReadNifList(entry.path().string());
            } catch (...) { continue; }
            bool fileMismatched = false;
            for (auto& obj : fblocks) {
                auto* shape = dynamic_cast<NiTriShape*>(obj.operator->());
                if (shape == nullptr) continue;
                NiSkinInstanceRef skin = shape->GetSkinInstance();
                if (skin == nullptr) continue;
                NiSkinDataRef skinData = skin->GetSkinData();
                if (skinData == nullptr) continue;
                std::vector<NiNodeRef> bones = skin->GetBones();
                ++total;
                for (unsigned int b = 0; b < skinData->GetBoneCount(); ++b) {
                    if (bones[b] == nullptr) continue;
                    Matrix44 boneWorld = bones[b]->GetWorldTransform();
                    Matrix44 boneOffset = skinData->GetBoneTransform(b);
                    // Row-vector convention (v * M): the addon's own
                    // get_skin_bind() computes bind = boneOffset^-1 * geomWorld,
                    // i.e. apply boneOffset^-1 first, then geomWorld -- so in
                    // niflib's left-to-right operator*, that is
                    // boneOffset.Inverse() * geomWorld, NOT the reverse.
                    Matrix44 geomWorld = shape->GetWorldTransform();
                    Matrix44 impliedBoneWorld = boneOffset.Inverse() * geomWorld;
                    double diff = 0.0;
                    for (int r = 0; r < 4; ++r)
                        for (int c = 0; c < 4; ++c)
                            diff = std::max(diff, (double)std::fabs(impliedBoneWorld[r][c] - boneWorld[r][c]));
                    if (diff > 0.05) {
                        fileMismatched = true;
                        if (diff > worstDiff) { worstDiff = diff; worstFile = entry.path().string(); }
                    }
                }
            }
            if (fileMismatched) {
                ++mismatched;
                std::cout << "MISMATCH: " << entry.path().string() << "\n";
            }
        }
        std::cout << "\nTotal skinned shapes checked (files may have several): " << total << "\n";
        std::cout << "Files with node/skin bind-pose mismatch: " << mismatched << "\n";
        std::cout << "Worst diff: " << worstDiff << " in " << worstFile << "\n";
        return 0;
    }

    if (std::string(argv[1]) == "kf") {
        // Dumps raw rotate-type + first keys for every NiTransformData in a
        // .kf, for the M009 tilt investigation: is Bip01 NonAccum's t=0 key
        // in stand01/magic01 a plain quaternion (unaffected by the
        // XYZ_ROTATION_KEY Euler-composition fix) or Euler (affected)?
        std::string kfPath = argv[2];
        std::vector<NiObjectRef> kfBlocks = ReadNifList(kfPath);
        for (auto& obj : kfBlocks) {
            auto* seq = dynamic_cast<NiControllerSequence*>(obj.operator->());
            if (seq == nullptr) continue;
            std::cout << "=== sequence: " << seq->GetName() << " ===\n";
            for (const ControllerLink& link : seq->GetControllerData()) {
                std::cout << "  [target] " << std::string(link.nodeName)
                          << (link.interpolator == NULL ? "  <-- NULL INTERPOLATOR" : "") << "\n";
                if (link.nodeName.find("NonAccum") == std::string::npos) continue;
                auto* ti = dynamic_cast<NiTransformInterpolator*>(link.interpolator.operator->());
                if (ti == nullptr) { std::cout << "  " << link.nodeName << ": not a NiTransformInterpolator\n"; continue; }
                NiTransformDataRef td = ti->GetData();
                if (td == nullptr) { std::cout << "  " << link.nodeName << ": no NiTransformData (static pose)\n"; continue; }
                const bool isEuler = (td->GetRotateType() == XYZ_ROTATION_KEY);
                std::cout << "  " << link.nodeName << ": rotateType="
                          << (isEuler ? "XYZ_ROTATION_KEY" : "quaternion") << "\n";
                if (!isEuler) {
                    auto keys = td->GetQuatRotateKeys();
                    for (size_t i = 0; i < std::min<size_t>(3, keys.size()); ++i) {
                        std::cout << "    quat key[" << i << "] t=" << keys[i].time
                                  << " (" << keys[i].data.x << "," << keys[i].data.y << ","
                                  << keys[i].data.z << "," << keys[i].data.w << ")\n";
                    }
                }
                auto tkeys = td->GetTranslateKeys();
                std::cout << "    translate keys: " << tkeys.size() << "\n";
                for (size_t i = 0; i < std::min<size_t>(3, tkeys.size()); ++i) {
                    std::cout << "      t=" << tkeys[i].time << " ("
                              << tkeys[i].data.x << "," << tkeys[i].data.y << "," << tkeys[i].data.z << ")\n";
                }
            }
        }
        return 0;
    }

    std::string path = argv[1];
    std::vector<NiObjectRef> blocks = ReadNifList(path);
    for (auto& obj : blocks) {
        auto* shape = dynamic_cast<NiTriShape*>(obj.operator->());
        if (shape == nullptr) continue;
        NiSkinInstanceRef skin = shape->GetSkinInstance();
        if (skin == nullptr) continue;

        std::cout << "=== shape: " << shape->GetName() << " ===\n";

        NiNodeRef skelRootCheck = skin->GetSkeletonRoot();
        Matrix44 skelRootWorld = skelRootCheck->GetWorldTransform();
        std::cout << "  skeleton root '" << skelRootCheck->GetName() << "' world transform:\n";
        for (int r = 0; r < 4; ++r) {
            std::cout << "    [" << skelRootWorld[r][0] << ", " << skelRootWorld[r][1] << ", "
                      << skelRootWorld[r][2] << ", " << skelRootWorld[r][3] << "]\n";
        }

        // Ground truth: niflib's own reference implementation.
        std::vector<Vector3> refVerts, refNorms;
        shape->GetSkinDeformation(refVerts, refNorms);

        // Our exporter's math, reproduced inline (see SkeletonExtractor::ApplySkin):
        // per-bone vert_trans = boneOffset * boneWorld, accumulate weighted,
        // but WITHOUT the final geomWorld.Inverse() -- our export keeps world space.
        NiSkinDataRef skinData = skin->GetSkinData();
        std::vector<NiNodeRef> bones = skin->GetBones();
        NiGeometryDataRef geomData = shape->GetData();
        std::vector<Vector3> inVerts = geomData->GetVertices();
        std::vector<Vector3> ourVerts(inVerts.size(), Vector3(0,0,0));
        std::vector<float> weightSum(inVerts.size(), 0.0f);

        for (unsigned int b = 0; b < skinData->GetBoneCount(); ++b) {
            Matrix44 boneWorld = bones[b]->GetWorldTransform();
            Matrix44 boneOffset = skinData->GetBoneTransform(b);
            Matrix44 skinMatrix = boneOffset * boneWorld;
            std::vector<SkinWeight> weights = skinData->GetBoneWeights(b);
            for (auto& w : weights) {
                if (w.index >= ourVerts.size()) continue;
                ourVerts[w.index] += (skinMatrix * inVerts[w.index]) * w.weight;
                weightSum[w.index] += w.weight;
            }
        }

        // Ground truth ends by multiplying through geomWorld.Inverse() to land
        // in the geometry's local space; undo that so both sides are in world
        // space for a fair comparison (matches ApplySkin's own documented
        // deliberate omission of that last step).
        Matrix44 geomWorld = shape->GetWorldTransform();
        std::cout << "  geomWorld (row-major, translation row3):\n";
        for (int r = 0; r < 4; ++r) {
            std::cout << "    [" << geomWorld[r][0] << ", " << geomWorld[r][1] << ", "
                      << geomWorld[r][2] << ", " << geomWorld[r][3] << "]\n";
        }
        std::vector<Vector3> refVertsWorld(refVerts.size());
        for (size_t i = 0; i < refVerts.size(); ++i) {
            refVertsWorld[i] = geomWorld * refVerts[i];
        }

        // Hypothesis test: does including NiSkinData's overall transform
        // change the bind-pose shape for THIS file? (exporter deliberately
        // omits it based on R773/R774 measurement -- re-check per-file.)
        Matrix44 overall = skinData->GetOverallTransform();
        std::cout << "  overall transform (row-major, translation row 3):\n";
        for (int r = 0; r < 4; ++r) {
            std::cout << "    [" << overall[r][0] << ", " << overall[r][1] << ", "
                      << overall[r][2] << ", " << overall[r][3] << "]\n";
        }
        bool isIdentity = true;
        for (int r = 0; r < 4 && isIdentity; ++r)
            for (int c = 0; c < 4; ++c)
                if (std::fabs(overall[r][c] - (r==c?1.0f:0.0f)) > 1e-4f) { isIdentity = false; break; }
        std::cout << "  overall transform is identity: " << (isIdentity ? "YES" : "NO") << "\n";

        double maxDiff = 0.0;
        size_t maxDiffIdx = 0;
        for (size_t i = 0; i < ourVerts.size(); ++i) {
            Vector3 diff = ourVerts[i] - refVertsWorld[i];
            double d = std::sqrt(diff.x*diff.x + diff.y*diff.y + diff.z*diff.z);
            if (d > maxDiff) { maxDiff = d; maxDiffIdx = i; }
        }
        std::cout << "  vertices: " << ourVerts.size() << "\n";
        std::cout << "  max |ourVerts - refVerts(world)| = " << maxDiff
                  << " at vertex " << maxDiffIdx << "\n";
        for (int vi : {0, 1, 2, 3, 4}) {
            if ((size_t)vi >= ourVerts.size()) continue;
            std::cout << "    v" << vi << " ourVerts(world) = ("
                      << ourVerts[vi].x << ", " << ourVerts[vi].y << ", " << ourVerts[vi].z << ")\n";
        }
        if (maxDiff > 1e-3) {
            std::cout << "    ourVerts[" << maxDiffIdx << "] = ("
                      << ourVerts[maxDiffIdx].x << ", " << ourVerts[maxDiffIdx].y << ", " << ourVerts[maxDiffIdx].z << ")\n";
            std::cout << "    refVerts[" << maxDiffIdx << "] = ("
                      << refVertsWorld[maxDiffIdx].x << ", " << refVertsWorld[maxDiffIdx].y << ", " << refVertsWorld[maxDiffIdx].z << ")\n";
        }
    }
    return 0;
}
