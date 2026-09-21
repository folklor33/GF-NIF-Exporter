#pragma once

// Material / UV / visibility animation extraction (Phase 8).
//
// The counterpart of AnimationExtractor for every controller that is NOT a
// bone transform: NiTextureTransformController, NiAlphaController,
// NiMaterialColorController, NiVisController, NiFlipController. Phase 4
// explicitly left these out of scope and nothing picked them up since, which
// is why glow planes, effect quads and pulsing transparencies render frozen.
//
// Kept separate from AnimationExtractor for the reason that file is itself
// separate from MeshExtractor: it answers a different question (how a
// *surface* changes over time, not how a *bone* moves), resolves against a
// different set of targets (geometry and particle systems rather than bones),
// and reads a disjoint part of the format (NiProperty-attached controllers
// rather than NiTransformController).
//
// Both sources are handled, because the corpus uses both and they are not
// interchangeable: the .nif carries the controller with its own data on 42% of
// files, while a companion .kf's NiControllerSequence binds the same controller
// types to an interpolator of its own -- measured, every one of the corpus's
// 2309 NiVisControllers is an inert .nif stub whose curve lives only in the .kf
// (docs/PHASE8_FINDINGS.md Etape 1).

#include "export/SceneModel.hpp"

#include <map>
#include <string>
#include <vector>

namespace Niflib {
class NiAVObject;
class NiControllerSequence;
class NiObject;
} // namespace Niflib

namespace gfnif {

class TextureResolver;

/*! Everything needed to turn a controller's owner into a MaterialTrackTarget.
 *
 *  Built by the caller (ExtractScene) from the maps it already has: the mesh
 *  walk's block-identity index and the particle pass's own. Resolution by
 *  pointer is exact; the name maps exist only for the .kf path, which carries
 *  nothing but a name (see MaterialTrackTarget::name). */
struct MaterialTrackTargetIndex {
    /*! Geometry block -> where the mesh walk put it. Same map Phase 7's
     *  correctif built for mesh emitters. */
    const std::map<Niflib::NiAVObject*, MeshEmitterRef>* geometry = nullptr;
    /*! NiParticleSystem block -> index into SceneData::particleSystems. */
    const std::map<Niflib::NiAVObject*, int>* particleSystems = nullptr;
    /*! Plain node block -> index into SceneData::nodes, when the file exports
     *  a node list at all. */
    const std::map<Niflib::NiAVObject*, int>* nodes = nullptr;

    const SkeletonData* skeleton = nullptr;
    const std::vector<MeshData>* meshes = nullptr;
    const std::vector<MeshData>* emitterMeshes = nullptr;
    const std::vector<ParticleSystemData>* particleSystemData = nullptr;
    const std::vector<SceneNode>* sceneNodes = nullptr;
};

class MaterialAnimationExtractor {
public:
    MaterialAnimationExtractor(const TextureResolver& textures,
                               std::vector<std::string>* warnings);

    /*! Walks every NiObjectNET reachable from the file's blocks and collects
     *  the non-transform controllers that carry their own data.
     *
     *  Emits at most one clip, named "embedded-material" with loop = true: a
     *  controller embedded in the .nif belongs to no NiControllerSequence and
     *  runs permanently in the original engine (a glow that ondulates, a
     *  texture that scrolls forever), so it cannot be attached to one of the
     *  .kf clips without inventing a relationship the file does not state.
     *  Does nothing when no such controller carries data. */
    void ExtractEmbedded(const std::vector<Niflib::NiObject*>& blocks,
                         const MaterialTrackTargetIndex& index, SceneData& scene,
                         MaterialAnimationStats& stats);

    /*! Adds the material tracks of one already-parsed NiControllerSequence to
     *  the AnimationClip the transform pass created for it.
     *
     *  Called from AnimationExtractor's own .kf walk rather than re-reading the
     *  file: `clip` is the clip that pass just appended, so a consumer gets one
     *  clip carrying both its bone tracks and its material tracks. */
    void ExtractFromSequence(Niflib::NiControllerSequence* sequence,
                             const MaterialTrackTargetIndex& index, AnimationClip& clip,
                             MaterialAnimationStats& stats);

private:
    const TextureResolver& textures_;
    std::vector<std::string>* warnings_;
};

} // namespace gfnif
