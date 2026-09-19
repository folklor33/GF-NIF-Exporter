// Stub for Niflib::NifQHull::compute_convex_hull.
//
// niflib's real implementation (src/nifqhull.cpp) textually #includes the C
// sources of the bundled `qhull` git submodule. That submodule is empty in this
// checkout and its recorded URL uses the git:// protocol GitHub no longer serves,
// so we exclude that translation unit from the build (see CMakeLists.txt).
//
// The only caller is Inertia.cpp's CalcMassPropertiesPolyhedron, reached solely
// when computing Havok rigid-body mass properties for a collision shape that
// supplies vertices but no triangles. The GF NIF exporter reads meshes, skins,
// materials, animations and particle systems -- it never touches bhk* collision
// mass properties -- so this path is unreachable in practice. Returning an empty
// triangle list degrades that computation to a zero-volume result rather than
// silently producing wrong geometry, and we assert in debug builds so that any
// future use of this path is noticed immediately.

#include "nifqhull.h"

#include <cassert>

namespace Niflib {

vector<Triangle> NifQHull::compute_convex_hull(const vector<Vector3>& verts) {
    (void)verts;
    assert(false && "NifQHull::compute_convex_hull is stubbed out in gf-nif-export "
                    "(qhull excluded from the build); no convex hull is available.");
    return vector<Triangle>();
}

} // namespace Niflib
