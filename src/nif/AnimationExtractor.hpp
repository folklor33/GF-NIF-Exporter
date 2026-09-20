#pragma once

// Animation extraction (Phase 4): embedded NiTransformController /
// NiMultiTargetTransformController pose data, plus whatever NiControllerSequence
// clips the file's companion .kf carries.
//
// Kept separate from MeshExtractor and SkeletonExtractor for the same reason
// those two are split: this answers a different question (how a bone moves
// over time) from a completely different, largely disjoint part of the format
// (NiTimeController/NiInterpolator, rather than NiGeometry/NiSkinInstance), and
// needs the SkeletonData built by SkeletonExtractor already in hand to resolve
// bone names against.

#include "export/SceneModel.hpp"

#include <string>
#include <vector>

namespace Niflib {
class NiAVObject;
class NiControllerSequence;
class NiInterpolator;
class NiNode;
class NiObjectNET;
} // namespace Niflib

namespace gfnif {

/*! Extracts animation clips for one .nif, given the SkeletonData already built
 *  for it (or an empty one, for a file with animation controllers but no
 *  skin -- bones are still resolved by name against the node hierarchy). */
class AnimationExtractor {
public:
    explicit AnimationExtractor(std::vector<std::string>* warnings);

    /*! Walks every NiTransformController / NiMultiTargetTransformController
     *  reachable from `root` and appends one AnimationClip per controller that
     *  carries usable interpolator data. A controller with a null interpolator
     *  (common: NiMultiTargetTransformController's extra targets are often
     *  registered without their own per-bone data in this corpus, see
     *  PHASE4_FINDINGS) is skipped silently -- that is normal, not a failure.
     *
     *  `skeleton` is scene.skeletons[skeletonIndex] if the file has one, or
     *  nullptr if it does not; bones are resolved by exact name match against
     *  it either way (a file can carry embedded animation without any skin). */
    void ExtractEmbedded(Niflib::NiAVObject* root, const SkeletonData* skeleton, SceneData& scene,
                         AnimationStats& stats);

    /*! Loads `kfPath` (already resolved by the caller from the
     *  <type>/animation/NAME.kf convention) and appends one AnimationClip per
     *  NiControllerSequence root block it contains.
     *
     *  Every ControllerLink's nodeName is resolved against `skeleton` by exact
     *  string match; an unresolved name produces an orphaned track (boneIndex
     *  = -1, kept and reported, never dropped silently) and a warning, per the
     *  brief's "warn, don't fail" discipline. Returns false only when the file
     *  itself cannot be parsed at all -- a bad individual track never fails the
     *  whole load. */
    bool ExtractFromKf(const std::string& kfPath, const SkeletonData* skeleton, SceneData& scene,
                       AnimationStats& stats);

private:
    std::vector<std::string>* warnings_;
};

} // namespace gfnif
