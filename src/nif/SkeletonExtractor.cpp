#include "nif/SkeletonExtractor.hpp"

#include "niflib.h"
#include "nif_math.h"
#include "nif/NameClassifier.hpp"

#include "obj/NiNode.h"
#include "obj/NiSkinData.h"
#include "obj/NiSkinInstance.h"
#include "obj/NiSkinPartition.h"
#include "obj/NiTriShape.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace gfnif {
namespace {

using Niflib::Matrix44;

/*! Writes a niflib Matrix44 into a column-major float[16].
 *
 *  niflib stores row-vector matrices (v * M), so m[row][col] with translation
 *  in row 3. glTF and Three.js use column-vector, column-major storage, where
 *  translation sits at elements 12..14. Transposing while copying converts
 *  between the two: out[col*4 + row] = m[row][col] with the roles swapped, i.e.
 *  out[i*4 + j] = m[i][j] lays niflib's row i down column i, which is exactly
 *  the transpose the convention change needs.
 *
 *  No axis conversion happens here. NIF is Z-up and the export stays Z-up, for
 *  mesh and skeleton alike (PHASE2_FINDINGS §9). Rotating one and not the other
 *  is precisely the desynchronisation this phase has to avoid. */
void ToColumnMajor(const Matrix44& m, float (&out)[16]) {
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            out[i * 4 + j] = m[i][j];
        }
    }
}

/*! Reads a column-major float[16] (glTF/Three.js order, as ToColumnMajor
 *  writes it) back into a row-vector niflib Matrix44 -- the inverse of
 *  ToColumnMajor, needed to read back a bone's *uncorrected* local transform
 *  when reconstructing the bind pose. */
Matrix44 FromColumnMajor(const float (&m)[16]) {
    Matrix44 out;
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            out[i][j] = m[i * 4 + j];
        }
    }
    return out;
}

/*! Depth-first walk collecting every NiNode under `node` into `bones`, parent
 *  before child.
 *
 *  Only NiNode is collected. Geometry (NiTriShape/NiTriStrips) and other
 *  NiAVObject leaves are not bones and are skipped, but the walk does not
 *  descend past them either -- in this corpus geometry is always a leaf. */
void CollectBones(Niflib::NiNode* node, int parentIndex, SkeletonData& skeleton,
                  std::map<Niflib::NiNode*, int>& byNode, int depth,
                  std::vector<std::string>* warnings) {
    if (node == nullptr) {
        return;
    }
    // A node reachable from two parents would otherwise be emitted twice and
    // get an ambiguous parent. The first path wins; NIF DAG sharing is real
    // (PHASE2_FINDINGS §6) but a bone needs exactly one parent.
    if (byNode.find(node) != byNode.end()) {
        return;
    }
    if (depth > 256) {
        if (warnings != nullptr) {
            warnings->push_back("bone hierarchy deeper than 256; subtree skipped");
        }
        return;
    }

    const int index = static_cast<int>(skeleton.bones.size());
    byNode[node] = index;

    BoneData bone;
    bone.name = node->GetName();
    bone.parentIndex = parentIndex;
    // Every node is kept, helper-named or not: measured on the corpus, 1565
    // helper-named bones are the parent of a genuinely named bone, so pruning
    // them would break the hierarchy. Attach points are merely flagged.
    bone.isAttachPoint = ClassifyNodeName(bone.name) == NodeRole::AttachPoint;
    ToColumnMajor(node->GetLocalTransform(), bone.bindMatrixLocal);
    skeleton.bones.push_back(std::move(bone));

    for (const Niflib::Ref<Niflib::NiAVObject>& child : node->GetChildren()) {
        if (auto* childNode = dynamic_cast<Niflib::NiNode*>(static_cast<Niflib::NiAVObject*>(child))) {
            CollectBones(childNode, index, skeleton, byNode, depth + 1, warnings);
        }
    }
}

} // namespace

SkeletonExtractor::SkeletonExtractor(std::vector<std::string>* warnings) : warnings_(warnings) {}

int SkeletonExtractor::EnsureSkeleton(Niflib::NiNode* skeletonRoot, SceneData& scene) {
    if (skeletonRoot == nullptr) {
        return -1;
    }
    if (skeletonIndex_ >= 0) {
        // Every NiSkinInstance in a file is expected to name the same armature
        // root. If one does not, the bones it references are looked up in the
        // skeleton already built; ApplySkin reports any that are missing.
        if (skeletonRoot != skeletonRoot_ && warnings_ != nullptr) {
            warnings_->push_back("a second skeleton root '" + skeletonRoot->GetName() +
                                 "' was named; reusing '" + skeletonRoot_->GetName() + "'");
        }
        return skeletonIndex_;
    }

    SkeletonData skeleton;
    skeleton.rootName = skeletonRoot->GetName();
    CollectBones(skeletonRoot, -1, skeleton, boneByNode_, 0, warnings_);
    if (skeleton.bones.empty()) {
        return -1;
    }

    // CollectBones seeds the root from its *local* transform, but the bone
    // world matrices the skinning is expressed against come from niflib's
    // GetWorldTransform, which also includes whatever sits above the skeleton
    // root. Seeding the root with its full world transform makes the exported
    // bindMatrixLocal chain reproduce GetWorldTransform for every bone.
    ToColumnMajor(skeletonRoot->GetWorldTransform(), skeleton.bones[0].bindMatrixLocal);

    skeletonRoot_ = skeletonRoot;
    skeletonIndex_ = static_cast<int>(scene.skeletons.size());
    scene.skeletons.push_back(std::move(skeleton));
    return skeletonIndex_;
}

bool SkeletonExtractor::ApplySkin(Niflib::NiTriShape* shape, SceneData& scene, MeshData& mesh,
                                  SkinningStats& stats) {
    Niflib::NiSkinInstance* skin = shape->GetSkinInstance();
    if (skin == nullptr) {
        return false;
    }

    Niflib::NiSkinData* skinData = skin->GetSkinData();
    if (skinData == nullptr) {
        if (warnings_ != nullptr) {
            warnings_->push_back("skinned geometry '" + mesh.name +
                                 "' has no NiSkinData; exported unskinned");
        }
        return false;
    }

    // GetSkeletonRoot is the armature root the bone transforms are relative to.
    Niflib::NiNode* root = skin->GetSkeletonRoot();
    const int skeletonIndex = EnsureSkeleton(root, scene);
    if (skeletonIndex < 0) {
        if (warnings_ != nullptr) {
            warnings_->push_back("skinned geometry '" + mesh.name +
                                 "' has no usable skeleton root; exported unskinned");
        }
        return false;
    }
    const std::vector<Niflib::Ref<Niflib::NiNode>> bones = skin->GetBones();
    const unsigned int skinBoneCount = skinData->GetBoneCount();
    if (bones.size() != skinBoneCount) {
        if (warnings_ != nullptr) {
            warnings_->push_back("skinned geometry '" + mesh.name + "': NiSkinInstance lists " +
                                 std::to_string(bones.size()) + " bones but NiSkinData has " +
                                 std::to_string(skinBoneCount) + "; exported unskinned");
        }
        return false;
    }

    // Map this skin's local bone slots onto the file-wide skeleton, recording
    // each one's bind-pose skin matrix as we go.
    std::vector<int> localToSkeleton(bones.size(), -1);
    for (size_t b = 0; b < bones.size(); ++b) {
        auto* node = static_cast<Niflib::NiNode*>(bones[b]);
        if (node == nullptr) {
            if (warnings_ != nullptr) {
                warnings_->push_back("skinned geometry '" + mesh.name + "': bone slot " +
                                     std::to_string(b) + " is null; exported unskinned");
            }
            return false;
        }
        auto it = boneByNode_.find(node);
        if (it == boneByNode_.end()) {
            // The influencing node is not under the skeleton root we walked.
            // Unrecoverable for this mesh: we cannot place it in the hierarchy.
            if (warnings_ != nullptr) {
                warnings_->push_back("skinned geometry '" + mesh.name + "': bone '" +
                                     node->GetName() +
                                     "' is not under the skeleton root; exported unskinned");
            }
            return false;
        }
        localToSkeleton[b] = static_cast<int>(mesh.skinBindings.size());

        // GetWorldTransform walks the node's own parent chain, so it is
        // authoritative regardless of how our bone list was collected.
        const Matrix44 boneWorld = node->GetWorldTransform();

        SkinBinding binding;
        binding.boneIndex = it->second;

        // The complete bind-pose skin matrix: the one taking a raw vertex
        // straight to its bind-pose world position. This is exactly what
        // niflib's own NiGeometry::GetSkinDeformation computes,
        //     vertexWorld = v * (boneOffset * boneWorld)     (row-vector)
        // and a direct comparison against it reproduces every NiSkinData-weighted
        // mesh in the corpus to 1e-6.
        //
        // Two things are deliberately NOT in this chain, because niflib uses
        // neither and measuring showed both make it worse: NiSkinData's overall
        // skinTransform, and the shape's own world matrix. On the R773/R774
        // family the former is the inverse of the latter, so including either
        // double-counts the shape's placement.
        //
        // GetSkinDeformation ends by multiplying through geomWorld.Inverse() to
        // hand its result back in the geometry's local space; that last step is
        // skipped on purpose so the skinned result lands in world space, beside
        // the unskinned meshes.
        //
        // The row-vector product is formed FIRST and converted ONCE. Converting
        // the two factors separately would transpose each without reversing
        // their order, which is the subtle way this goes wrong.
        //
        // It is stored whole rather than as a classical inverse bind matrix so
        // a consumer never has to rebuild boneWorld from the bone chain, nor
        // match our multiplication convention, just to draw the bind pose.
        // Phase 4 needs the chain anyway for animation, where bone-local
        // transforms are replaced per frame; at bind pose this is the same
        // product with none of the ambiguity.
        ToColumnMajor(skinData->GetBoneTransform(static_cast<unsigned int>(b)) * boneWorld,
                      binding.skinMatrix);

        // Still measured, purely to document how often the per-skin transform
        // actually diverges (see PHASE3_FINDINGS).
        for (const SkinBinding& prior : priorBindings_) {
            if (prior.boneIndex != binding.boneIndex) {
                continue;
            }
            float d = 0.0f;
            for (int k = 0; k < 16; ++k) {
                d = std::max(d, std::fabs(prior.skinMatrix[k] - binding.skinMatrix[k]));
            }
            if (d > 1e-3f) {
                ++stats.boneSkinMatrixConflicts;
                stats.maxSkinMatrixConflict = std::max(stats.maxSkinMatrixConflict, d);
            }
            break;
        }

        priorBindings_.push_back(binding);
        mesh.skinBindings.push_back(binding);
    }

    // --- gather influences per vertex ---------------------------------------
    //
    // NiSkinData is stored bone-major: each bone lists the vertices it touches.
    // The export needs the transpose, so it is accumulated per vertex first and
    // truncated/normalised afterwards.
    const size_t vertexCount = mesh.vertices.size();
    std::vector<std::vector<SkeletonExtractor::Influence>> perVertex(vertexCount);

    for (size_t b = 0; b < bones.size(); ++b) {
        const std::vector<Niflib::SkinWeight> weights =
            skinData->GetBoneWeights(static_cast<unsigned int>(b));
        for (const Niflib::SkinWeight& w : weights) {
            if (w.index >= vertexCount) {
                if (warnings_ != nullptr) {
                    warnings_->push_back("skinned geometry '" + mesh.name + "': bone '" +
                                         static_cast<Niflib::NiNode*>(bones[b])->GetName() +
                                         "' weights out-of-range vertex " +
                                         std::to_string(w.index) + " (>= " +
                                         std::to_string(vertexCount) + "); exported unskinned");
                }
                return false;
            }
            if (w.weight <= 0.0f) {
                continue; // A zero weight is no influence at all.
            }
            SkeletonExtractor::Influence inf;
            inf.bone = static_cast<uint16_t>(localToSkeleton[b]);
            inf.weight = w.weight;
            perVertex[w.index].push_back(inf);
        }
    }

    // Some files store bone transforms in NiSkinData but no weights at all
    // (its hasVertexWeights flag is 0), keeping the weights only in the
    // GPU-oriented NiSkinPartition. Measured on this corpus: 5 files, all of
    // them among the truncated-header vintage. Falling back to the partition is
    // the only way to skin them, and it is the same data -- the corpus-wide
    // cross-check finds zero cases where the two disagree about a bone on a
    // vertex NiSkinData does weight.
    size_t weightedVertices = 0;
    for (const std::vector<SkeletonExtractor::Influence>& infs : perVertex) {
        if (!infs.empty()) {
            ++weightedVertices;
        }
    }
    // Applied whenever ANY drawn vertex lacks weights, not only when the whole
    // mesh does: a few files weight most vertices from NiSkinData and leave a
    // handful to the partition alone. LoadWeightsFromPartition only fills
    // vertices that are still empty, so vertices NiSkinData did weight keep
    // their authoritative values.
    size_t unweightedDrawn = 0;
    for (uint32_t idx : mesh.indices) {
        if (idx < vertexCount && perVertex[idx].empty()) {
            ++unweightedDrawn;
        }
    }
    if (unweightedDrawn > 0) {
        if (LoadWeightsFromPartition(skin, localToSkeleton, mesh, perVertex)) {
            ++stats.skinsFromPartition;
            if (warnings_ != nullptr) {
                warnings_->push_back("skinned geometry '" + mesh.name + "': " +
                                     std::to_string(unweightedDrawn) +
                                     " drawn vertex/vertices unweighted in NiSkinData; "
                                     "filled from NiSkinPartition");
            }
        }
    }

    // An unweighted vertex only matters if something draws it. NIF geometry
    // routinely carries vertices no triangle references, and those are
    // harmless; one that *is* drawn but has no bone would collapse to the
    // origin under GPU skinning, so the two cases are counted apart.
    std::vector<bool> usedByTriangle(vertexCount, false);
    for (uint32_t idx : mesh.indices) {
        if (idx < vertexCount) {
            usedByTriangle[idx] = true;
        }
    }

    // Every bone the source named for each vertex, before the 4-slot cap, so
    // the partition cross-check can tell "we never read this bone" apart from
    // "we read it and truncation dropped it".
    std::vector<std::set<int>> preTruncation(vertexCount);
    for (size_t v = 0; v < vertexCount; ++v) {
        for (const SkeletonExtractor::Influence& inf : perVertex[v]) {
            preTruncation[v].insert(inf.bone);
        }
    }

    // --- truncate to 4, renormalise -----------------------------------------
    for (size_t v = 0; v < vertexCount; ++v) {
        std::vector<SkeletonExtractor::Influence>& infs = perVertex[v];

        const int realCount = static_cast<int>(infs.size());
        if (realCount == 0 && usedByTriangle[v]) {
            ++stats.unweightedVerticesInUse;
        }
        stats.maxInfluencesSeen = std::max(stats.maxInfluencesSeen, realCount);
        stats.influenceHistogram[std::min(realCount, SkinningStats::kMaxTrackedInfluences)] += 1;

        // Descending by weight, so truncation drops the least significant.
        // stable_sort keeps the bone order of equal weights reproducible across
        // runs, which matters for byte-identical re-exports.
        std::stable_sort(infs.begin(), infs.end(),
                         [](const SkeletonExtractor::Influence& a, const SkeletonExtractor::Influence& b) { return a.weight > b.weight; });

        if (realCount > kInfluencesPerVertex) {
            ++stats.verticesTruncated;
            for (size_t i = kInfluencesPerVertex; i < infs.size(); ++i) {
                stats.weightDiscarded += infs[i].weight;
                stats.maxWeightDiscarded = std::max(stats.maxWeightDiscarded, infs[i].weight);
            }
            infs.resize(kInfluencesPerVertex);
        }

        float sum = 0.0f;
        for (const SkeletonExtractor::Influence& inf : infs) {
            sum += inf.weight;
        }

        Vertex& vert = mesh.vertices[v];
        if (sum > 1e-8f) {
            // Renormalise unconditionally: it repairs truncation loss and any
            // drift already present in the source, and is a no-op when the
            // weights already sum to 1.
            for (size_t i = 0; i < infs.size(); ++i) {
                vert.boneIndex[i] = infs[i].bone;
                vert.weight[i] = infs[i].weight / sum;
            }
        } else if (usedByTriangle[v]) {
            // A drawn vertex with no SkeletonExtractor::Influence would be multiplied by a zero
            // weight matrix and collapse onto the origin, tearing the mesh
            // open. Pinning it to the skeleton root with full weight keeps it
            // exactly where the bind pose puts it, which is the best available
            // answer and visually correct at rest.
            vert.boneIndex[0] = 0;
            vert.weight[0] = 1.0f;
        }
        // Remaining slots keep the Vertex defaults: bone 0, weight 0.
    }

    // Ground-truth check (opt-in via GFNIF_VERIFYSKIN): niflib's own skinning,
    // brought into world space by undoing the geometry-local step
    // GetSkinDeformation ends with. Our exported data must reproduce it.
    //
    // Skipped for meshes skinned from the partition: GetSkinDeformation reads
    // NiSkinData, which for exactly those files holds no weights, so it has
    // nothing to compare against.
    if (getenv("GFNIF_VERIFYSKIN") != nullptr && weightedVertices > 0) {
        std::vector<Niflib::Vector3> nv, nn;
        try {
            shape->GetSkinDeformation(nv, nn);
            const Matrix44 gw = shape->GetWorldTransform();

            // Rebuild each vertex the way a consumer will: blend the bindings'
            // skin matrices by our exported weights and apply to our exported
            // (raw, local-space) position.
            float worst = 0.0f;
            for (size_t v = 0; v < vertexCount && v < nv.size(); ++v) {
                const Vertex& ours = mesh.vertices[v];
                Niflib::Vector3 acc(0.0f, 0.0f, 0.0f);
                const Niflib::Vector3 p(ours.position[0], ours.position[1], ours.position[2]);
                for (int k = 0; k < kInfluencesPerVertex; ++k) {
                    if (ours.weight[k] <= 0.0f) {
                        continue;
                    }
                    // Binding order mirrors the skin's own bone list one-for-one,
                    // so the slot index recovers both the node and the offset
                    // straight from niflib -- no transpose round-trip, which
                    // isolates whether the *weights* we exported are right.
                    const unsigned int slot = ours.boneIndex[k];
                    const Matrix44 boneWorld =
                        static_cast<Niflib::NiNode*>(bones[slot])->GetWorldTransform();
                    const Matrix44 off = skinData->GetBoneTransform(slot);
                    acc += ((off * boneWorld) * p) * ours.weight[k];
                }
                const Niflib::Vector3 ref = gw * nv[v];
                const float d = std::max({std::fabs(ref.x - acc.x), std::fabs(ref.y - acc.y),
                                          std::fabs(ref.z - acc.z)});
                worst = std::max(worst, d);
            }
            fprintf(stderr, "VERIFYSKIN %s maxDelta=%.6f bones=%u\n", mesh.name.c_str(), worst,
                    skinBoneCount);
        } catch (const std::exception& e) {
            fprintf(stderr, "VERIFYSKIN %s threw: %s\n", mesh.name.c_str(), e.what());
        }
    }

    stats.skinnedVertices += vertexCount;
    ++stats.skinnedMeshes;

    VerifyAgainstPartition(skin, mesh, localToSkeleton, preTruncation, stats);

    mesh.isSkinned = true;
    mesh.skeletonIndex = skeletonIndex;
    return true;
}

bool SkeletonExtractor::LoadWeightsFromPartition(Niflib::NiSkinInstance* skin,
                                                 const std::vector<int>& localToSkeleton,
                                                 const MeshData& mesh,
                                                 std::vector<std::vector<SkeletonExtractor::Influence>>& perVertex) {
    Niflib::NiSkinPartition* part = skin->GetSkinPartition();
    if (part == nullptr) {
        return false;
    }
    const size_t vertexCount = mesh.vertices.size();
    bool any = false;

    const int partitionCount = part->GetNumPartitions();
    for (int p = 0; p < partitionCount; ++p) {
        if (!part->HasVertexWeights(p) || !part->HasVertexBoneIndices(p)) {
            continue;
        }
        const std::vector<unsigned short> vertexMap = part->GetVertexMap(p);
        const std::vector<unsigned short> boneMap = part->GetBoneMap(p);
        const int numVertices = static_cast<int>(part->GetNumVertices(p));

        for (int i = 0; i < numVertices; ++i) {
            // As in the cross-check: no vertex map means partition-local index
            // i is mesh vertex i.
            unsigned short target = static_cast<unsigned short>(i);
            if (!vertexMap.empty()) {
                if (i >= static_cast<int>(vertexMap.size())) {
                    break;
                }
                target = vertexMap[i];
            }
            // NiSkinData stays the source of truth: only vertices it left
            // entirely unweighted are filled from the partition.
            if (target >= vertexCount || !perVertex[target].empty()) {
                continue;
            }
            const std::vector<float> pw = part->GetVertexWeights(p, i);
            const std::vector<unsigned short> pb = part->GetVertexBoneIndices(p, i);
            for (size_t s = 0; s < pw.size() && s < pb.size(); ++s) {
                if (pw[s] <= 0.0f || pb[s] >= boneMap.size()) {
                    continue;
                }
                const unsigned short localBone = boneMap[pb[s]];
                if (localBone >= localToSkeleton.size()) {
                    continue;
                }
                SkeletonExtractor::Influence inf;
                inf.bone = static_cast<uint16_t>(localToSkeleton[localBone]);
                inf.weight = pw[s];
                perVertex[target].push_back(inf);
                any = true;
            }
        }
    }
    return any;
}

/*! Cross-checks the weights we took from NiSkinData against the ones
 *  NiSkinPartition carries.
 *
 *  NiSkinPartition is Gamebryo's GPU-ready redundant copy of the same skinning:
 *  the geometry is split into partitions of at most N bones, with weights
 *  already padded to 4 per vertex. It is a genuinely independent encoding of
 *  the same data, so agreement is good evidence that NiSkinData was read
 *  correctly.
 *
 *  NiSkinData stays the source of truth regardless -- it is the unquantised,
 *  untruncated original, and the partition has already applied the same 4-slot
 *  limit we are applying. A mismatch is reported, never acted on. */
void SkeletonExtractor::VerifyAgainstPartition(Niflib::NiSkinInstance* skin, const MeshData& mesh,
                                               const std::vector<int>& localToSkeleton,
                                               const std::vector<std::set<int>>& preTruncation,
                                               SkinningStats& stats) {
    Niflib::NiSkinPartition* part = skin->GetSkinPartition();
    if (part == nullptr) {
        ++stats.partitionsMissing;
        return;
    }
    ++stats.partitionsChecked;

    const int partitionCount = static_cast<int>(part->GetNumPartitions());
    for (int p = 0; p < partitionCount; ++p) {
        if (!part->HasVertexWeights(p) || !part->HasVertexBoneIndices(p)) {
            continue;
        }
        const std::vector<unsigned short> vertexMap = part->GetVertexMap(p);
        const std::vector<unsigned short> boneMap = part->GetBoneMap(p);
        const int numVertices = static_cast<int>(part->GetNumVertices(p));

        for (int i = 0; i < numVertices; ++i) {
            // Without a vertex map, a partition's vertices are the mesh's own,
            // in order. Skipping those partitions would quietly shrink the
            // sample the cross-check is based on.
            unsigned short target = static_cast<unsigned short>(i);
            if (!vertexMap.empty()) {
                if (i >= static_cast<int>(vertexMap.size())) {
                    break;
                }
                target = vertexMap[i];
            }
            if (target >= mesh.vertices.size()) {
                continue;
            }
            const std::vector<float> pw = part->GetVertexWeights(p, i);
            const std::vector<unsigned short> pb = part->GetVertexBoneIndices(p, i);
            const Vertex& v = mesh.vertices[target];

            // Compare as sets: the partition need not order its slots the way
            // we do (we sort by descending weight), so each of its non-zero
            // influences is looked for among ours.
            for (size_t s = 0; s < pw.size() && s < pb.size(); ++s) {
                if (pw[s] <= 1e-6f) {
                    continue;
                }
                if (pb[s] >= boneMap.size()) {
                    continue;
                }
                const unsigned short localBone = boneMap[pb[s]];
                if (localBone >= localToSkeleton.size()) {
                    continue;
                }
                const int skeletonBone = localToSkeleton[localBone];

                ++stats.partitionVerticesCompared;
                float ours = 0.0f;
                for (int k = 0; k < kInfluencesPerVertex; ++k) {
                    if (v.weight[k] > 0.0f && v.boneIndex[k] == skeletonBone) {
                        ours = v.weight[k];
                        break;
                    }
                }
                const float delta = std::fabs(ours - pw[s]);
                stats.maxPartitionWeightDelta = std::max(stats.maxPartitionWeightDelta, delta);
                // Two different questions, tracked separately.
                //
                // Bone agreement is the one that must hold: if the partition
                // says a bone influences this vertex, NiSkinData must name that
                // bone too. `ours == 0` means we have no such SkeletonExtractor::Influence at all,
                // which would be a real structural disagreement.
                if (ours <= 0.0f) {
                    // Distinguish the two reasons we might not have this bone.
                    // Our slots hold the post-truncation four, so a bone the
                    // source did weight but truncation dropped is expected and
                    // not a read error. Anything else is a genuine gap.
                    if (preTruncation[target].count(skeletonBone) != 0) {
                        ++stats.partitionBoneTruncatedAway;
                    } else {
                        ++stats.partitionBoneMissing;
                        if (preTruncation[target].empty()) {
                            // The vertex has no NiSkinData SkeletonExtractor::Influence at all --
                            // the same population as `unweightedVerticesInUse`,
                            // counted here in SkeletonExtractor::Influence terms.
                            ++stats.partitionBoneMissingOnUnweighted;
                        }
                    }
                }
                // Weight equality is NOT expected to hold. A partition caps its
                // influences and renormalises what survives, so its weights are
                // systematically >= ours. Counted for the record, not treated
                // as an error; NiSkinData stays the source of truth precisely
                // because it is the un-capped original.
                if (delta > 1e-3f) {
                    ++stats.partitionWeightMismatches;
                    if (pw[s] + 1e-4f < ours) {
                        stats.maxPartitionBelowDelta =
                            std::max(stats.maxPartitionBelowDelta, ours - pw[s]);
                        // The partition having a *smaller* weight than ours is
                        // the direction the subset+renormalise story cannot
                        // explain, so it is the one worth flagging.
                        ++stats.partitionWeightBelowOurs;
                    }
                }
            }
        }
    }
}

} // namespace gfnif
