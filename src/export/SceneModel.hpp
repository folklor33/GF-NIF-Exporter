#pragma once

// Intermediate scene representation, deliberately free of any niflib type.
//
// Everything niflib-specific is confined to the extractors in src/nif/. This
// keeps the export format decoupled from the parsing library and gives the
// later phases (skeleton, animations, particles) a stable place to grow: new
// data hangs off SceneData as additional vectors rather than changing what is
// already here.

#include <cstdint>
#include <string>
#include <vector>

namespace gfnif {

/*! One vertex of a static (bind-pose) mesh, already in world space.
 *
 *  Phase 2 exports a flattened static pose, so positions and normals have the
 *  accumulated NiNode transform baked in. Phase 3 will keep vertices in their
 *  local/bind space instead and carry the transform on the skeleton. */
struct StaticVertex {
    float position[3] = {0.0f, 0.0f, 0.0f};
    float normal[3] = {0.0f, 0.0f, 1.0f};
    float uv[2] = {0.0f, 0.0f};
    /*! Opaque white when the source has no NiVertexColorProperty / vertex
     *  colors, so a consumer can always read this attribute unconditionally. */
    float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
};

/*! A single drawable. Both NiTriShape and NiTriStrips collapse into this:
 *  strips are de-stripified into a plain indexed triangle list. */
struct MeshData {
    std::string name;
    std::vector<StaticVertex> vertices;
    std::vector<uint32_t> indices;
    /*! Index into SceneData::materials, or -1 when the geometry carries no
     *  NiMaterialProperty. */
    int materialIndex = -1;

    /*! Source topology, kept for reporting/diagnostics. */
    enum class Source { TriShape, TriStrips };
    Source source = Source::TriShape;

    /*! Degenerate triangles dropped while de-stripifying (always 0 for
     *  TriShape). Reported per corpus as a sanity check on the de-striping. */
    int degenerateTrianglesDropped = 0;
};

/*! Material parameters, flattened from the NiProperty list attached to a
 *  geometry block. */
struct MaterialData {
    /*! Texture path resolved to an on-disk .png, relative to the input root and
     *  using forward slashes. Empty when the geometry has no texture at all. */
    std::string diffuseTexturePath;
    /*! The original .dds path as written in the NIF, kept for diagnostics and
     *  for reporting unresolved references. */
    std::string sourceTextureName;

    float ambient[3] = {1.0f, 1.0f, 1.0f};
    float diffuse[3] = {1.0f, 1.0f, 1.0f};
    float specular[3] = {0.0f, 0.0f, 0.0f};
    float emissive[3] = {0.0f, 0.0f, 0.0f};
    float glossiness = 0.0f;
    float alpha = 1.0f;

    /*! True when a NiVertexColorProperty is attached, i.e. the vertex color
     *  attribute is meaningful rather than the default white. */
    bool hasVertexColor = false;
    /*! False when the .png for sourceTextureName was not found on disk. The
     *  conversion still succeeds; the consumer falls back to the flat colors. */
    bool textureFound = false;
};

/*! One converted .nif.
 *
 *  Phase 3+ will add skeleton / animations / particleSystems members here. The
 *  writer already emits those JSON keys as empty so the schema does not change
 *  shape later. */
struct SceneData {
    std::string sourceNifPath;
    std::vector<MeshData> meshes;
    std::vector<MaterialData> materials;

    /*! Total vertices/triangles across all meshes, for logging. */
    size_t TotalVertices() const {
        size_t n = 0;
        for (const MeshData& m : meshes) {
            n += m.vertices.size();
        }
        return n;
    }
    size_t TotalTriangles() const {
        size_t n = 0;
        for (const MeshData& m : meshes) {
            n += m.indices.size() / 3;
        }
        return n;
    }
};

} // namespace gfnif
