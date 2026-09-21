#pragma once

// Particle-system extraction (Phase 5): NiParticleSystem, its one emitter
// (NiPSysBoxEmitter / NiPSysMeshEmitter -- the only two found in the corpus,
// see PHASE5_FINDINGS Etape 1) and the modifiers actually varying per effect
// (gravity, rotation, grow/fade, color-over-life).
//
// This phase extracts PARAMETERS only -- no simulation, no rendering. See
// SceneModel.hpp's ParticleSystemData for the format and PHASE5_FINDINGS for
// why niflib's particle classes need a different reading strategy from every
// other block this exporter reads (no public getters for their scalar
// fields; only asString() exposes them, see ParticleExtractor.cpp).

#include "export/SceneModel.hpp"
#include "nif/MaterialExtractor.hpp"

#include <map>
#include <string>
#include <vector>

namespace Niflib {
class NiAVObject;
class NiParticleSystem;
} // namespace Niflib

namespace gfnif {

/*! Extracts every NiParticleSystem reachable from a file's roots.
 *
 *  Mirrors AnimationExtractor's shape: constructed once per file, resolves
 *  attach nodes against whichever of `skeleton`/`nodes` the file populated
 *  (never both, see SceneData::nodes), and reports corpus-wide measurements
 *  through ParticleStats. */
class ParticleExtractor {
public:
    explicit ParticleExtractor(std::vector<std::string>* warnings);

    /*! Walks every NiParticleSystem reachable from `root` and appends one
     *  ParticleSystemData per system to `scene.particleSystems`.
     *
     *  `skeleton` is scene.skeletons[0] if the file has one, or nullptr
     *  otherwise. `nodes`, when non-null, is the node hierarchy built for a
     *  skeleton-less file (see MeshExtractor.cpp) -- unlike Phase 4's
     *  animation path, this one is needed whenever the file has particle
     *  systems at all, not only when something animates (see
     *  PHASE5_FINDINGS: half the corpus's particle-bearing files are
     *  skeleton-less, with or without a companion .kf).
     *
     *  A system whose own emitter type is not recognised, or whose data
     *  block is missing, is skipped with a warning -- never fails the file. */
    void Extract(Niflib::NiAVObject* root, const SkeletonData* skeleton,
                 const std::vector<SceneNode>* nodes, MaterialExtractor& materials,
                 SceneData& scene, ParticleStats& stats);

private:
    std::vector<std::string>* warnings_;
};

} // namespace gfnif
