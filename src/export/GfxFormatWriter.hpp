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
 *  a way a consumer must notice. */
constexpr int kGfModelFormatVersion = 1;

} // namespace gfnif
