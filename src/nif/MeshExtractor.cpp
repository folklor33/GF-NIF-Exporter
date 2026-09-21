#include "nif/MeshExtractor.hpp"

#include "nif/AnimationExtractor.hpp"
#include "nif/HeaderNormalizer.hpp"
#include "nif/MaterialExtractor.hpp"
#include "nif/NameClassifier.hpp"
#include "nif/ParticleExtractor.hpp"
#include "nif/SkeletonExtractor.hpp"
#include "texture/TextureResolver.hpp"

#include "niflib.h"
#include "nif_math.h"
#include "obj/NiAVObject.h"
#include "obj/NiGeometry.h"
#include "obj/NiGeometryData.h"
#include "obj/NiNode.h"
#include "obj/NiObject.h"
#include "obj/NiSkinInstance.h"
#include "obj/NiTriShape.h"
#include "obj/NiTriShapeData.h"
#include "obj/NiTriStrips.h"
#include "obj/NiTriStripsData.h"

#include <cmath>
#include <exception>
#include <filesystem>
#include <set>
#include <sstream>
#include <vector>

namespace gfnif {
namespace {

using Niflib::Matrix44;
using Niflib::NiObject;
using Niflib::NiObjectRef;

/*! Transforms a position by a niflib Matrix44.
 *
 *  niflib uses the row-vector convention (v * M) with translation in row 3 --
 *  see Matrix44::operator*(Vector3) in niflib's src/nif_math.cpp. */
void TransformPosition(const Matrix44& m, const Niflib::Vector3& in, float (&out)[3]) {
    out[0] = in.x * m[0][0] + in.y * m[1][0] + in.z * m[2][0] + m[3][0];
    out[1] = in.x * m[0][1] + in.y * m[1][1] + in.z * m[2][1] + m[3][1];
    out[2] = in.x * m[0][2] + in.y * m[1][2] + in.z * m[2][2] + m[3][2];
}

/*! Transforms a direction by the rotation/scale part only (translation row
 *  dropped), then renormalises.
 *
 *  Using the plain upper 3x3 rather than its inverse-transpose is correct for
 *  the rigid + uniform-scale transforms NIF nodes carry; GF models use uniform
 *  scale only (NiAVObject stores scale as a single float, so non-uniform scale
 *  cannot even be expressed). */
void TransformNormal(const Matrix44& m, const Niflib::Vector3& in, float (&out)[3]) {
    float x = in.x * m[0][0] + in.y * m[1][0] + in.z * m[2][0];
    float y = in.x * m[0][1] + in.y * m[1][1] + in.z * m[2][1];
    float z = in.x * m[0][2] + in.y * m[1][2] + in.z * m[2][2];
    const float len = std::sqrt(x * x + y * y + z * z);
    if (len > 1e-12f) {
        x /= len;
        y /= len;
        z /= len;
    } else {
        x = 0.0f;
        y = 0.0f;
        z = 1.0f;
    }
    out[0] = x;
    out[1] = y;
    out[2] = z;
}

/*! Converts a triangle strip into an indexed triangle list.
 *
 *  A strip encodes triangle i as (s[i], s[i+1], s[i+2]) with the winding order
 *  alternating every step, so odd-numbered triangles have two indices swapped
 *  to keep a consistent facing.
 *
 *  Degenerate triangles -- ones where two of the three indices are equal -- are
 *  dropped. They are not errors: repeating an index is the standard way to
 *  stitch several strips into one, and they cover zero area, so a renderer
 *  gains nothing from them. `degenerateCount` accumulates how many were seen,
 *  which Phase 2 reports across the corpus as a sanity check.
 *
 *  Indices are widened to uint32_t on output even though NIF stores them as
 *  unsigned short, so the format does not need changing if a later asset
 *  exceeds 65535 vertices. */
void DeStripify(const std::vector<unsigned short>& strip,
                std::vector<uint32_t>& outIndices,
                int& degenerateCount) {
    if (strip.size() < 3) {
        return;
    }
    for (size_t i = 0; i + 2 < strip.size(); ++i) {
        unsigned short a = strip[i];
        unsigned short b = strip[i + 1];
        unsigned short c = strip[i + 2];

        if (a == b || b == c || a == c) {
            ++degenerateCount;
            continue;
        }

        // Odd triangles are wound the other way round; swap to normalise.
        if (i % 2 == 0) {
            outIndices.push_back(a);
            outIndices.push_back(b);
            outIndices.push_back(c);
        } else {
            outIndices.push_back(a);
            outIndices.push_back(c);
            outIndices.push_back(b);
        }
    }
}

/*! Copies the vertex attributes of one geometry, baking `world` into positions
 *  and normals. */
void BuildVertices(Niflib::NiGeometryData* data,
                   const Matrix44& world,
                   bool materialHasVertexColor,
                   std::vector<StaticVertex>& out) {
    const std::vector<Niflib::Vector3> positions = data->GetVertices();
    const std::vector<Niflib::Vector3> normals = data->GetNormals();
    const std::vector<Niflib::Color4> colors = data->GetColors();

    // UV set 0 is the diffuse channel. GetUVSet throws when there are none, so
    // the count is checked first.
    std::vector<Niflib::TexCoord> uvs;
    if (data->GetUVSetCount() > 0) {
        uvs = data->GetUVSet(0);
    }

    out.resize(positions.size());
    for (size_t i = 0; i < positions.size(); ++i) {
        StaticVertex& v = out[i];
        TransformPosition(world, positions[i], v.position);

        if (i < normals.size()) {
            TransformNormal(world, normals[i], v.normal);
        }
        if (i < uvs.size()) {
            v.uv[0] = uvs[i].u;
            v.uv[1] = uvs[i].v;
        }
        // Vertex colors are only meaningful when the material advertises them;
        // otherwise the default opaque white in StaticVertex is kept, so the
        // attribute is always safe to read.
        if (materialHasVertexColor && i < colors.size()) {
            v.color[0] = colors[i].r;
            v.color[1] = colors[i].g;
            v.color[2] = colors[i].b;
            v.color[3] = colors[i].a;
        }
    }
}

/*! Walks the node tree, accumulating transforms and emitting one MeshData per
 *  geometry block found.
 *
 *  `visited` guards against the DAG sharing that is normal in NIF files: a node
 *  can be referenced from more than one parent, and following it twice would
 *  duplicate geometry (or loop forever). */
void WalkNode(Niflib::NiAVObject* obj,
              const Matrix44& parentWorld,
              SceneData& scene,
              MaterialExtractor& materials,
              SkeletonExtractor& skeletons,
              std::set<NiObject*>& visited,
              ExtractionResult& result,
              int depth,
              bool verifyStrips,
              std::vector<Niflib::NiAVObject*>* meshSourceObjects = nullptr) {
    if (obj == nullptr || !visited.insert(obj).second) {
        return;
    }
    if (depth > result.maxDepthSeen) {
        result.maxDepthSeen = depth;
    }
    // Guard against pathological nesting exhausting the stack. NIF hierarchies
    // are shallow in practice (see PHASE2_FINDINGS); anything past this is
    // malformed, and bailing out beats taking the process down.
    if (depth > kMaxNodeDepth) {
        result.warnings.push_back("node hierarchy deeper than " +
                                  std::to_string(kMaxNodeDepth) + "; subtree skipped");
        return;
    }

    // niflib composes as local * parentWorld (row-vector convention).
    const Matrix44 world = obj->GetLocalTransform() * parentWorld;

    if (auto* node = dynamic_cast<Niflib::NiNode*>(obj)) {
        for (const Niflib::Ref<Niflib::NiAVObject>& child : node->GetChildren()) {
            WalkNode(static_cast<Niflib::NiAVObject*>(child), world, scene, materials, skeletons,
                     visited, result, depth + 1, verifyStrips, meshSourceObjects);
        }
        return;
    }

    auto* triShape = dynamic_cast<Niflib::NiTriShape*>(obj);
    auto* triStrips = dynamic_cast<Niflib::NiTriStrips*>(obj);
    if (triShape == nullptr && triStrips == nullptr) {
        return; // Not geometry (a particle system, a camera, ...). Not our phase.
    }

    // NiAVObject::flags bit 0 is the file's own, authoritative visibility
    // flag -- the artist/pipeline explicitly marked this shape not to be
    // rendered, as opposed to the name/texture heuristics used elsewhere in
    // this file. Measured on the full corpus: 10 160 shapes (24.3%) are
    // hidden this way, 590 447 vertices, across 900 files (31.9%); 90.5% of
    // them are untextured leftovers (matching the pattern found on M491),
    // but 461 are large (>100 verts) and textured -- consistent with
    // deliberately-hidden LOD/variant geometry kept in the file rather than
    // deleted. There is no case for exporting what the source file itself
    // says not to draw, so this is unconditional: no texture/name gate
    // needed, unlike NameClassifier's filter. See PHASE3_FINDINGS §13.
    if (!obj->GetVisibility()) {
        ++result.hiddenGeometriesDropped;
        return;
    }

    auto* geom = static_cast<Niflib::NiGeometry*>(obj);
    Niflib::NiGeometryData* data = geom->GetData();
    if (data == nullptr) {
        result.warnings.push_back("geometry '" + obj->GetName() + "' has no data block; skipped");
        ++result.geometriesSkipped;
        return;
    }

    MeshData mesh;
    mesh.name = obj->GetName();
    mesh.materialIndex = materials.ExtractFor(obj, scene.materials);

    // Drop the 3ds Max helper gizmos the exporter left in the file: bone
    // octahedra, biped boxes, attachment nubs. They are real NiTriShapes, so
    // nothing upstream filters them, and they render as blank grey surfaces.
    //
    // The material has to be resolved first, because the test is name AND
    // untextured -- a name match alone would take out real geometry that
    // happens to still be called "Box02". See NameClassifier.hpp.
    const bool hasResolvedTexture = mesh.materialIndex >= 0 &&
                                    scene.materials[mesh.materialIndex].textureFound;
    if (ShouldDropGeometry(mesh.name, hasResolvedTexture)) {
        ++result.helperGeometriesDropped;
        return;
    }

    const bool hasVertexColor = mesh.materialIndex >= 0 &&
                                scene.materials[mesh.materialIndex].hasVertexColor;

    if (triShape != nullptr) {
        mesh.source = MeshData::Source::TriShape;
        // NiTriShapeData already stores an explicit triangle list. GetTriangles
        // is declared on NiTriBasedGeomData, below the NiGeometryData that
        // GetData() hands back, so the data block is narrowed first.
        auto* shapeData = dynamic_cast<Niflib::NiTriShapeData*>(data);
        if (shapeData == nullptr) {
            result.warnings.push_back("NiTriShape '" + mesh.name +
                                      "' has non-trishape data block; skipped");
            ++result.geometriesSkipped;
            return;
        }
        for (const Niflib::Triangle& t : shapeData->GetTriangles()) {
            mesh.indices.push_back(t.v1);
            mesh.indices.push_back(t.v2);
            mesh.indices.push_back(t.v3);
        }
    } else {
        mesh.source = MeshData::Source::TriStrips;
        auto* stripData = dynamic_cast<Niflib::NiTriStripsData*>(data);
        if (stripData == nullptr) {
            result.warnings.push_back("NiTriStrips '" + mesh.name +
                                      "' has non-strip data block; skipped");
            ++result.geometriesSkipped;
            return;
        }
        const int stripCount = stripData->GetStripCount();
        size_t candidateTriangles = 0;
        for (int s = 0; s < stripCount; ++s) {
            const std::vector<unsigned short> strip = stripData->GetStrip(s);
            if (strip.size() >= 3) {
                candidateTriangles += strip.size() - 2;
            }
            DeStripify(strip, mesh.indices, mesh.degenerateTrianglesDropped);
        }
        result.degenerateTrianglesDropped += mesh.degenerateTrianglesDropped;

        // Invariant: a strip of length L yields exactly L-2 candidate triangles,
        // each either kept or dropped as degenerate. A mismatch would mean the
        // de-striping lost or invented geometry.
        const size_t emitted = mesh.indices.size() / 3;
        if (emitted + static_cast<size_t>(mesh.degenerateTrianglesDropped) != candidateTriangles) {
            result.warnings.push_back("de-striping invariant violated on '" + mesh.name + "': " +
                                      std::to_string(emitted) + " kept + " +
                                      std::to_string(mesh.degenerateTrianglesDropped) +
                                      " dropped != " + std::to_string(candidateTriangles) +
                                      " candidates");
        }

        // Cross-check against niflib's own strip expansion. niflib drops
        // degenerate triangles too, so its triangle count must equal the number
        // we keep -- that is the number a renderer actually draws.
        if (verifyStrips) {
            const size_t niflibTriangles = stripData->GetTriangles().size();
            if (niflibTriangles != emitted) {
                result.warnings.push_back(
                    "strip cross-check on '" + mesh.name + "': niflib expands to " +
                    std::to_string(niflibTriangles) + " triangles, we kept " +
                    std::to_string(emitted));
            }
        }
    }

    // Skinned vertices stay raw, in the shape's own local space; unskinned
    // geometry keeps the Phase 2 world-space flattening.
    //
    // This matches niflib's own NiGeometry::GetSkinDeformation, which feeds the
    // untransformed NiGeometryData vertices through `boneOffset * boneWorld`.
    // The bone chain therefore carries the placement, and baking the node's
    // world matrix in here as well would apply it twice.
    //
    // PHASE2_FINDINGS §9.3 established that NiTriStrips is never skinned across
    // the whole corpus, so only the NiTriShape path needs to ask.
    const bool wantsSkin = triShape != nullptr && triShape->GetSkinInstance() != NULL;
    BuildVertices(data, wantsSkin ? Matrix44::IDENTITY : world, hasVertexColor, mesh.vertices);

    if (wantsSkin) {
        // A skin that cannot be resolved falls back to a static export rather
        // than dropping the geometry: a visible mesh in the wrong pose beats a
        // hole, and the warning says which it was. The vertices have to be
        // rebuilt in world space for that, since they were left local above.
        if (!skeletons.ApplySkin(triShape, scene, mesh, result.skinning)) {
            BuildVertices(data, world, hasVertexColor, mesh.vertices);
            ++result.skinsFailed;
        }
    }

    if (mesh.vertices.empty() || mesh.indices.empty()) {
        result.warnings.push_back("geometry '" + mesh.name + "' produced no triangles; skipped");
        ++result.geometriesSkipped;
        return;
    }

    // Guard the export against an out-of-range index rather than letting a
    // malformed file produce a .gfbin that crashes the viewer.
    const uint32_t vertexCount = static_cast<uint32_t>(mesh.vertices.size());
    for (uint32_t idx : mesh.indices) {
        if (idx >= vertexCount) {
            result.warnings.push_back("geometry '" + mesh.name + "' has out-of-range index " +
                                      std::to_string(idx) + " (>= " + std::to_string(vertexCount) +
                                      "); skipped");
            ++result.geometriesSkipped;
            return;
        }
    }

    scene.meshes.push_back(std::move(mesh));
    if (meshSourceObjects != nullptr) {
        meshSourceObjects->push_back(obj);
    }
}

} // namespace

bool ResolveCompanionKf(const std::string& nifPath, std::string& outKfPath) {
    namespace fs = std::filesystem;
    const fs::path nif(nifPath);

    // "<type>/model/NAME.nif" -> "<type>/animation/NAME.kf": walk up from the
    // file to its parent directory name, only proceeding when that parent is
    // literally "model" (case-sensitive, matching the corpus) so a .nif that
    // does not sit under a model/ directory is left alone rather than guessing.
    const fs::path parent = nif.parent_path();
    if (parent.filename() != "model") {
        return false;
    }
    const fs::path candidate =
        parent.parent_path() / "animation" / fs::path(nif.stem()).concat(".kf");

    std::error_code ec;
    if (!fs::exists(candidate, ec) || ec) {
        return false;
    }
    outKfPath = candidate.string();
    return true;
}

ExtractionResult ExtractScene(const std::string& nifPath, SceneData& scene, bool verifyStrips) {
    ExtractionResult result;
    scene = SceneData();
    scene.sourceNifPath = nifPath;

    std::string bytes;
    if (!ReadWholeFile(nifPath, bytes)) {
        result.error = "cannot open file";
        return result;
    }

    HeaderNormalizationReport header;
    NormalizeNifHeader(bytes, header);
    if (header.repairedTruncatedHeaderString) {
        result.warnings.push_back("header string was truncated (\"" + header.originalHeaderString +
                                  "\"); rebuilt in memory");
    }
    if (header.restampedVersion) {
        result.warnings.push_back("version stamped " + FormatNifVersion(header.originalVersion) +
                                  " over a 20.2.0.8 layout; restamped in memory");
    }

    // niflib crashes rather than throwing cleanly on a block type it does not
    // implement, so the header's type table is checked first and the file
    // skipped if anything in it is unsupported. See HeaderNormalizer.cpp.
    const std::string unsupported = FindUnsupportedBlockType(bytes);
    if (!unsupported.empty()) {
        result.error = "unsupported block type '" + unsupported + "' (niflib cannot read it)";
        return result;
    }

    std::vector<NiObjectRef> blocks;
    try {
        std::istringstream stream(bytes, std::ios::binary);
        blocks = Niflib::ReadNifList(stream, nullptr);
    } catch (const std::exception& e) {
        result.error = std::string("niflib parse failed: ") + e.what();
        return result;
    } catch (...) {
        result.error = "niflib parse failed: unknown exception";
        return result;
    }

    if (blocks.empty()) {
        result.error = "file contains no blocks";
        return result;
    }

    TextureResolver resolver(nifPath);
    MaterialExtractor materials(resolver, &result.warnings);
    // One extractor per file, so every skinned shape shares the single
    // skeleton it builds rather than duplicating it per sub-mesh.
    SkeletonExtractor skeletons(&result.warnings);

    // Roots are the blocks nothing else references. Starting from every root
    // (rather than assuming block 0) keeps files with several top-level nodes
    // working.
    std::set<NiObject*> referenced;
    for (const NiObjectRef& b : blocks) {
        for (const NiObjectRef& r : static_cast<NiObject*>(b)->GetRefs()) {
            if (r != NULL) {
                referenced.insert(static_cast<NiObject*>(r));
            }
        }
    }

    // Parallel to scene.meshes, recording which NiAVObject each entry came
    // from -- needed only if this file turns out to need the animated-mesh
    // reattachment pass below (see nodesPtr/scene.nodes), so a mesh can be
    // matched back to its owning node by pointer identity rather than by name
    // (names collide routinely, e.g. WA85's three "Editable Mesh" siblings).
    std::vector<Niflib::NiAVObject*> meshSourceObjects;

    std::set<NiObject*> visited;
    for (const NiObjectRef& b : blocks) {
        NiObject* obj = static_cast<NiObject*>(b);
        if (referenced.find(obj) != referenced.end()) {
            continue;
        }
        if (auto* av = dynamic_cast<Niflib::NiAVObject*>(obj)) {
            WalkNode(av, Matrix44::IDENTITY, scene, materials, skeletons, visited, result, 0,
                     verifyStrips, &meshSourceObjects);
        }
    }

    result.skinning.skeletons = static_cast<int>(scene.skeletons.size());

    if (scene.meshes.empty()) {
        // Distinguish "every shape was explicitly hidden by the source file"
        // from a genuinely empty/malformed file: the former is a real
        // statement about the asset (an unused or trigger-only NPC, for
        // instance -- confirmed on npc/N600.nif, whose 4 shapes are all
        // hidden), not an extraction defect, and should read that way in the
        // summary rather than looking like every other parse failure.
        if (result.hiddenGeometriesDropped > 0) {
            result.error = "no visible geometry (" + std::to_string(result.hiddenGeometriesDropped) +
                           " shape(s) present but all marked hidden in the source file)";
        } else {
            result.error = "no geometry found (" + std::to_string(blocks.size()) + " blocks parsed)";
        }
        return result;
    }

    // Animation, after geometry/skinning has succeeded: embedded controllers
    // are walked from the same set of unreferenced roots the mesh pass used
    // (a file can carry embedded animation on a node with no skinned mesh
    // under it at all), and the companion .kf, if any, is resolved by the
    // <type>/model/NAME.nif <-> <type>/animation/NAME.kf convention.
    //
    // The skeleton used to resolve bone names is whichever one the mesh walk
    // built -- scene.skeletons is a vector for format-generality (see
    // SceneData), but every file in this corpus builds at most one, and
    // SkeletonExtractor's own EnsureSkeleton already warns if a second root is
    // ever named.
    //
    // A file with animation but no skin has no SkeletonData to resolve
    // against. Rather than every such track coming back orphaned, a node
    // hierarchy is built from the same unreferenced roots (see
    // BuildNodeHierarchy / SceneNode) and tried as a fallback target list --
    // covers a plain NiNode a NiTransformController animates directly (a
    // rotating prop, a moving door) with no skin anywhere in the file.
    // Measured corpus-wide: no file mixes a real skeleton with an embedded
    // controller targeting a node outside that skeleton's own subtree, so
    // building this list only when scene.skeletons is empty costs nothing on
    // a skinned file and does not risk the two resolution paths disagreeing
    // on the same file. Kept in scene.nodes (and so written to the .gfmodel)
    // only if the file turns out to have at least one animation track that
    // needed it, so the ~1000 static files with no embedded controller at
    // all pay nothing extra.
    {
        AnimationExtractor animExtractor(&result.warnings);
        const SkeletonData* skeleton =
            scene.skeletons.empty() ? nullptr : &scene.skeletons[0];

        std::vector<SceneNode> nodes;
        // Parallel to `nodes`: which NiAVObject each entry came from, offset
        // exactly the way `nodes` itself is below -- used only by the
        // animated-mesh reattachment pass after clipsBefore/animations are
        // known, to map a mesh (recorded by pointer in meshSourceObjects)
        // back to its node index without relying on name matching.
        std::map<Niflib::NiAVObject*, int> objectToNodeIndex;
        if (skeleton == nullptr) {
            for (const NiObjectRef& b : blocks) {
                NiObject* obj = static_cast<NiObject*>(b);
                if (referenced.find(obj) != referenced.end()) {
                    continue;
                }
                if (auto* av = dynamic_cast<Niflib::NiAVObject*>(obj)) {
                    std::map<Niflib::NiAVObject*, int> rootObjectToIndex;
                    std::vector<SceneNode> rootNodes = BuildNodeHierarchy(av, rootObjectToIndex);
                    // Re-parent every subsequent root's own roots (parentIndex
                    // == -1) onto nothing but keep them appended, exactly as
                    // the mesh walk treats independent unreferenced roots as
                    // siblings -- offsetting each subtree's internal indices
                    // by the running total so cross-references stay correct.
                    const int offset = static_cast<int>(nodes.size());
                    for (SceneNode& n : rootNodes) {
                        if (n.parentIndex >= 0) {
                            n.parentIndex += offset;
                        }
                    }
                    for (auto& [rootObj, idx] : rootObjectToIndex) {
                        objectToNodeIndex[rootObj] = idx + offset;
                    }
                    nodes.insert(nodes.end(), std::make_move_iterator(rootNodes.begin()),
                                 std::make_move_iterator(rootNodes.end()));
                }
            }
        }
        const std::vector<SceneNode>* nodesPtr = nodes.empty() ? nullptr : &nodes;

        const size_t clipsBefore = scene.animations.size();

        for (const NiObjectRef& b : blocks) {
            NiObject* obj = static_cast<NiObject*>(b);
            if (referenced.find(obj) != referenced.end()) {
                continue;
            }
            if (auto* av = dynamic_cast<Niflib::NiAVObject*>(obj)) {
                animExtractor.ExtractEmbedded(av, skeleton, nodesPtr, scene, result.animation);
            }
        }

        std::string kfPath;
        if (ResolveCompanionKf(nifPath, kfPath)) {
            ++result.animation.kfFilesFound;
            if (!animExtractor.ExtractFromKf(kfPath, skeleton, nodesPtr, scene, result.animation)) {
                result.warnings.push_back("companion .kf '" + kfPath +
                                          "' could not be parsed; its animations are missing");
            }
        }

        // Particle systems (Phase 5), resolved against the same
        // skeleton/node list animation just used. Unlike animation, a
        // particle system's attach point needs resolving whether or not
        // anything in the file animates -- a static emitter parented under a
        // plain NiNode is just as real a "needs the node list" case as a
        // moving one (measured: half of the corpus's particle-bearing files
        // are skeleton-less, with or without a companion .kf -- see
        // PHASE5_FINDINGS). nodesPtr/skeleton are already built above
        // unconditionally when skeleton == nullptr, so this costs nothing
        // extra to attempt.
        const size_t particlesBefore = scene.particleSystems.size();
        {
            ParticleExtractor particleExtractor(&result.warnings);
            for (const NiObjectRef& b : blocks) {
                NiObject* obj = static_cast<NiObject*>(b);
                if (referenced.find(obj) != referenced.end()) {
                    continue;
                }
                if (auto* av = dynamic_cast<Niflib::NiAVObject*>(obj)) {
                    particleExtractor.Extract(av, skeleton, nodesPtr, materials, scene,
                                              result.particles);
                }
            }
        }
        if (scene.particleSystems.size() > particlesBefore) {
            result.particles.filesWithParticles = 1;
        }

        // A file needs its node list exported (see SceneData::nodes) when
        // EITHER an animation track resolved against it (PHASE4_FINDINGS'
        // original reason) OR a particle system's attach/emitter/gravity
        // object did (this phase's addition) -- every other skeleton-less
        // file (the large majority) keeps scene.nodes empty.
        if (nodesPtr != nullptr &&
            (scene.animations.size() > clipsBefore || scene.particleSystems.size() > particlesBefore)) {
            // Reattach every mesh whose own node (or an ancestor of it) is
            // actually the target of a resolved track. Without this, a track
            // moves an empty SceneNode pivot while the mesh's geometry --
            // pre-flattened to world space by the walk above, same as every
            // other static mesh -- stays put: WA85's "1.50s / 2 tracks but
            // nothing moves" symptom (see PHASE4_FINDINGS's WA85 section and
            // this session's follow-up). A mesh not covered by this (the
            // overwhelming majority of skeleton-less files, which either have
            // no animation at all or animate a node with no geometry
            // anywhere under it) keeps today's flattened vertices and
            // nodeIndex == -1, unchanged.
            std::vector<bool> nodeIsAnimated(nodes.size(), false);
            for (const AnimationClip& clip : scene.animations) {
                for (const AnimationTrack& track : clip.tracks) {
                    if (track.boneIndex >= 0 &&
                        static_cast<size_t>(track.boneIndex) < nodes.size()) {
                        nodeIsAnimated[track.boneIndex] = true;
                    }
                }
            }
            // Propagate to every descendant: a rigid child of an animated
            // node must move with it even though nothing targets the child
            // directly. Nodes are parent-before-child, so a single forward
            // pass suffices.
            for (size_t i = 0; i < nodes.size(); ++i) {
                const int parent = nodes[i].parentIndex;
                if (parent >= 0 && nodeIsAnimated[parent]) {
                    nodeIsAnimated[i] = true;
                }
            }

            for (size_t i = 0; i < scene.meshes.size(); ++i) {
                if (i >= meshSourceObjects.size()) {
                    continue; // defensive: sizes are kept in lockstep above
                }
                auto it = objectToNodeIndex.find(meshSourceObjects[i]);
                if (it == objectToNodeIndex.end() || !nodeIsAnimated[it->second]) {
                    continue;
                }
                MeshData& mesh = scene.meshes[i];
                if (mesh.isSkinned) {
                    continue; // already node-independent (skin matrices carry placement)
                }
                const Matrix44 nodeWorldInverse =
                    meshSourceObjects[i]->GetWorldTransform().Inverse();
                for (StaticVertex& v : mesh.vertices) {
                    const Niflib::Vector3 worldPos(v.position[0], v.position[1], v.position[2]);
                    const Niflib::Vector3 worldNormal(v.normal[0], v.normal[1], v.normal[2]);
                    TransformPosition(nodeWorldInverse, worldPos, v.position);
                    TransformNormal(nodeWorldInverse, worldNormal, v.normal);
                }
                mesh.nodeIndex = it->second;
            }

            scene.nodes = std::move(nodes);
        }
    }

    result.success = true;
    return result;
}

} // namespace gfnif
