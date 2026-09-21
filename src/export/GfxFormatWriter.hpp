#pragma once

#include "export/SceneModel.hpp"

#include <string>

namespace gfnif {

/*! Serialises a SceneData to `<outBase>.gfmodel` (JSON structure) and
 *  `<outBase>.gfbin` (concatenated binary buffers).
 *
 *  The split exists so the Angular/Three.js consumer can fetch the small JSON,
 *  decide what it needs, and then pull the big buffer straight into typed
 *  arrays without any parsing. Every attribute is stored as tightly packed
 *  little-endian float32 (or uint32 for indices) at a byte offset the JSON
 *  states, which maps directly onto THREE.BufferAttribute.
 *
 *  \param outBase output path WITHOUT extension.
 *  \param error   set when the function returns false.
 *  \return true on success. */
bool WriteSceneFiles(const SceneData& scene, const std::string& outBase, std::string& error);

/*! Format version written into the .gfmodel. Bumped when the schema changes in
 *  a way a consumer must notice.
 *
 *  5 (Phase 8) adds, on each animation clip:
 *    - "materialTracks": UV scroll/rotate/scale, opacity, animated colour,
 *      flipbook and visibility, from the controllers Phase 4 declared out of
 *      scope and nothing picked up since. Each track names the geometry,
 *      particle system or node it drives (never a material index --
 *      SceneData::materials is deduplicated, see MaterialTrackTarget) and the
 *      one property it animates, with keys in the .gfbin like every other
 *      animation channel.
 *    - "loop": whether the clip repeats, from the sequence's cycle type. A
 *      clip named "embedded-material" is the implicit, always-looping clip
 *      carrying the controllers the .nif embeds outside any sequence.
 *  Nothing existing moves or changes meaning, so a version-4 consumer reads a
 *  version-5 file exactly as before, minus the new animation.
 *
 *  4 (Phase 7 correctif) adds, on each particle system's emitter:
 *    - "meshEmitterMeshes": the emission-surface references resolved to array
 *      indices by block identity. "meshEmitterMeshNames" stays, unchanged and
 *      parallel, but is diagnostic only: a name does not identify a mesh
 *      (see MeshEmitterRef).
 *    - "birthRate" / "birthRateKeys": the authored emission density, read off
 *      the system's NiPSysEmitterCtlr. -1 when the source does not state it.
 *  Nothing else moves, and no existing field changes meaning, so a version-3
 *  consumer still reads a version-4 file correctly apart from the new fields. */
constexpr int kGfModelFormatVersion = 5;

} // namespace gfnif
