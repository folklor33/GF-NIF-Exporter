#pragma once

#include "export/SceneModel.hpp"

#include <string>
#include <vector>

namespace gfnif {

/*! Outcome of converting one .nif into a SceneData. */
struct ExtractionResult {
    bool success = false;
    /*! Fatal reason when success is false. */
    std::string error;
    /*! Non-fatal problems: unresolved textures, skipped geometry, missing data
     *  blocks. The conversion still produced output. */
    std::vector<std::string> warnings;

    /*! Degenerate triangles dropped while de-stripifying, across the file. */
    int degenerateTrianglesDropped = 0;
    /*! Geometry blocks skipped because their data block was missing or their
     *  topology was not understood. */
    int geometriesSkipped = 0;
    /*! Deepest node nesting reached while walking, for diagnostics. */
    int maxDepthSeen = 0;
};

/*! Depth past which the node walk gives up on a subtree rather than risking the
 *  stack. Real GF hierarchies are far shallower. */
constexpr int kMaxNodeDepth = 256;

/*! Reads `nifPath` and fills `scene` with its static (bind-pose) geometry and
 *  materials.
 *
 *  The NiNode hierarchy is walked from the root and each node's local transform
 *  accumulated, so every vertex comes out in world space. Phase 3 will replace
 *  this flattening with a proper skeleton/bind-pose for skinned meshes; for a
 *  static pose, baking the transform in is sufficient and keeps the viewer
 *  simple.
 *
 *  A file that cannot be parsed at all returns success = false. Individual
 *  geometry blocks that cannot be read are skipped with a warning rather than
 *  failing the whole file. */
ExtractionResult ExtractScene(const std::string& nifPath, SceneData& scene,
                              bool verifyStrips = false);

} // namespace gfnif
