#pragma once

// Classifies NiNode / geometry names left behind by the 3ds Max export
// pipeline that produced the Grand Fantasia assets.
//
// GF models ship with the editor's own helper objects still in the file:
// bone gizmos, biped boxes, attachment nubs. They carry real NiTriShape
// geometry -- low-poly, untextured -- and render as blank grey surfaces
// floating around the model.
//
// The classification is deliberately conservative. Measured on the corpus,
// a name match ALONE is not sufficient evidence: 81.5% of all meshes match a
// naive "looks like a Max default name" regex, because the artists left most
// real body parts named "Editable Poly" and "Plane" too. See
// PHASE3_FINDINGS §12 for the full measurement.

#include <string>

namespace gfnif {

/*! What a node's name says about its role. */
enum class NodeRole {
    /*! Real geometry, or a name we have no reason to distrust. Default. */
    Visual,
    /*! An editor helper with no visual value: a bone gizmo, a biped box, a
     *  collision primitive. Its geometry is not exported. */
    CollisionHelper,
    /*! A named anchor point -- weapon/shield/rope attachment. Kept in the
     *  skeleton for later use; any gizmo geometry on it is not exported. */
    AttachPoint,
};

/*! Classifies a node or geometry name.
 *
 *  Name matching is anchored and requires the whole name to be the helper
 *  word plus an optional numeric/duplicate suffix ("Bone", "Bone14",
 *  "Box02", "Biped Object@#3"). A substring match would be far too eager:
 *  real GF geometry is routinely called things like "Editable Poly".
 *
 *  Note this looks at the name only. Callers additionally require the mesh to
 *  be untextured before discarding it -- see ShouldDropGeometry. */
NodeRole ClassifyNodeName(const std::string& name);

/*! Whether a geometry block should be dropped from the export.
 *
 *  Both conditions must hold:
 *    * the name classifies as a helper or attach point, and
 *    * the geometry has no resolved diffuse texture.
 *
 *  The texture test is what makes this safe. 40 meshes in the corpus are
 *  named "Box01"/"Box02" and so on but carry genuine item textures and up to
 *  427 vertices; they are real geometry an artist never renamed, and the
 *  texture is what tells them apart from the 1389 untextured 24-vertex cubes
 *  that are actual gizmos. */
bool ShouldDropGeometry(const std::string& name, bool hasResolvedTexture);

} // namespace gfnif
