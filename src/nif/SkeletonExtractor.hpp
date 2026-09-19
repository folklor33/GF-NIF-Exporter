#pragma once

// Skeleton and skinning extraction (Phase 3).
//
// Kept separate from MeshExtractor because the two answer different questions:
// MeshExtractor walks the scene graph emitting drawables, while this file turns
// the *same* NiNode hierarchy into a bone list and resolves NiSkinData into
// per-vertex influences. MeshExtractor calls into here when it meets a
// NiSkinInstance.

#include "export/SceneModel.hpp"

#include <map>
#include <set>
#include <string>
#include <vector>

#include "nif_math.h"

namespace Niflib {
class NiAVObject;
class NiNode;
class NiSkinInstance;
class NiTriShape;
} // namespace Niflib

namespace gfnif {

/*! Per-file skinning state, built once and shared by every skinned geometry in
 *  the file.
 *
 *  A NIF can hold several NiSkinInstance blocks, one per skinned NiTriShape,
 *  each naming its own subset of bones. They all point at the same armature
 *  though, so a single SkeletonData is built for the file and each skin's local
 *  bone list is mapped onto it by node pointer. That is what the brief asks for:
 *  one shared skeleton per file, referenced rather than duplicated. */
class SkeletonExtractor {
public:
    /*! \param warnings appended to when skinning data is malformed. */
    explicit SkeletonExtractor(std::vector<std::string>* warnings);

    /*! Builds the file's skeleton from `skeletonRoot` if it has not been built
     *  already, and returns its index in `scene.skeletons`.
     *
     *  Every node in the subtree becomes a bone, not just the ones a skin
     *  actually influences: intermediate nodes are needed to keep parent chains
     *  unbroken, and the .kf files of Phase 4 bind by node name to nodes that
     *  may carry no weights at all.
     *
     *  Returns -1 if the root is unusable. */
    int EnsureSkeleton(Niflib::NiNode* skeletonRoot, SceneData& scene);

    /*! Reads the NiSkinInstance on `shape` and fills `mesh.vertices` with
     *  bone indices and weights, in the order the vertices already sit in.
     *
     *  `mesh.vertices` must already be populated (positions, normals, ...) --
     *  this only adds the influence slots. Sets mesh.isSkinned and
     *  mesh.skeletonIndex on success.
     *
     *  Returns false when the skin cannot be used (no NiSkinData, bone count
     *  mismatch, out-of-range vertex index). The caller keeps the mesh as
     *  static geometry in that case rather than dropping it. */
    bool ApplySkin(Niflib::NiTriShape* shape, SceneData& scene, MeshData& mesh,
                   SkinningStats& stats);

private:
    /*! One bone influence on one vertex, before the 4-slot cap is applied. */
    struct Influence {
        uint16_t bone = 0;
        float weight = 0.0f;
    };

    /*! Fills `perVertex` from NiSkinPartition instead of NiSkinData.
     *
     *  For the handful of files whose NiSkinData has hasVertexWeights = 0 and
     *  so carries bone transforms but no weights at all. Returns false if the
     *  partition cannot supply them either. */
    bool LoadWeightsFromPartition(Niflib::NiSkinInstance* skin,
                                  const std::vector<int>& localToSkeleton, const MeshData& mesh,
                                  std::vector<std::vector<Influence>>& perVertex);

    /*! Compares the weights taken from NiSkinData against the redundant copy in
     *  NiSkinPartition, accumulating agreement statistics into `stats`.
     *  Reporting only: NiSkinData stays the source of truth. */
    void VerifyAgainstPartition(Niflib::NiSkinInstance* skin, const MeshData& mesh,
                                const std::vector<int>& localToSkeleton,
                                const std::vector<std::set<int>>& preTruncation,
                                SkinningStats& stats);

    std::vector<std::string>* warnings_;
    /*! The one skeleton built for this file, or -1 before the first skinned
     *  shape is met. */
    int skeletonIndex_ = -1;
    /*! Node pointer -> bone index in scene.skeletons[skeletonIndex_].bones.
     *  Pointers, not names: NiNode names are not unique in this corpus, and the
     *  skin's bone list refers to the node objects directly. */
    std::map<Niflib::NiNode*, int> boneByNode_;
    /*! The root the skeleton was built from, so a second NiSkinInstance naming
     *  a different root can be reported rather than silently mixed in. */
    Niflib::NiNode* skeletonRoot_ = nullptr;
    /*! Bindings seen on earlier skins of this file, used only to measure how
     *  often two skins disagree about a shared bone's inverse bind matrix. */
    std::vector<SkinBinding> priorBindings_;
};

} // namespace gfnif
