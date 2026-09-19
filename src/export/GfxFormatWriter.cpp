#include "export/GfxFormatWriter.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;

namespace gfnif {
namespace {

/*! Appends `count` floats to `buffer` and returns the byte offset they start
 *  at. Values are written in the host's little-endian layout, which every
 *  target of this tool (x64 Windows) and every consumer (a browser typed array)
 *  shares. */
size_t AppendFloats(std::string& buffer, const float* values, size_t count) {
    const size_t offset = buffer.size();
    buffer.append(reinterpret_cast<const char*>(values), count * sizeof(float));
    return offset;
}

size_t AppendUint32(std::string& buffer, const std::vector<uint32_t>& values) {
    const size_t offset = buffer.size();
    buffer.append(reinterpret_cast<const char*>(values.data()), values.size() * sizeof(uint32_t));
    return offset;
}

/*! Escapes a string for JSON. Texture paths and mesh names come from game data,
 *  so backslashes and stray control characters are both possible. */
std::string JsonEscape(const std::string& s) {
    std::ostringstream out;
    for (unsigned char c : s) {
        switch (c) {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\b': out << "\\b"; break;
            case '\f': out << "\\f"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (c < 0x20) {
                    out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                        << static_cast<int>(c) << std::dec;
                } else {
                    out << static_cast<char>(c);
                }
        }
    }
    return out.str();
}

/*! Formats a float with enough precision to round-trip through float32. */
std::string Num(float v) {
    std::ostringstream out;
    out << std::setprecision(9) << v;
    return out.str();
}

std::string Vec3Json(const float (&v)[3]) {
    return "[" + Num(v[0]) + ", " + Num(v[1]) + ", " + Num(v[2]) + "]";
}

} // namespace

bool WriteSceneFiles(const SceneData& scene, const std::string& outBase, std::string& error) {
    error.clear();

    const fs::path modelPath = outBase + ".gfmodel";
    const fs::path binPath = outBase + ".gfbin";

    std::error_code ec;
    fs::create_directories(modelPath.parent_path(), ec);

    // --- binary buffer ------------------------------------------------------
    //
    // Attributes are written per mesh, de-interleaved: all positions, then all
    // normals, then UVs, colors and finally indices. De-interleaving keeps each
    // range directly usable as a THREE.BufferAttribute without a stride, and
    // lets a consumer skip an attribute it does not need.
    std::string bin;
    struct MeshRanges {
        size_t position = 0, normal = 0, uv = 0, color = 0, index = 0;
    };
    std::vector<MeshRanges> ranges;
    ranges.reserve(scene.meshes.size());

    for (const MeshData& mesh : scene.meshes) {
        MeshRanges r;
        std::vector<float> scratch;

        scratch.clear();
        scratch.reserve(mesh.vertices.size() * 3);
        for (const StaticVertex& v : mesh.vertices) {
            scratch.insert(scratch.end(), {v.position[0], v.position[1], v.position[2]});
        }
        r.position = AppendFloats(bin, scratch.data(), scratch.size());

        scratch.clear();
        for (const StaticVertex& v : mesh.vertices) {
            scratch.insert(scratch.end(), {v.normal[0], v.normal[1], v.normal[2]});
        }
        r.normal = AppendFloats(bin, scratch.data(), scratch.size());

        scratch.clear();
        for (const StaticVertex& v : mesh.vertices) {
            scratch.insert(scratch.end(), {v.uv[0], v.uv[1]});
        }
        r.uv = AppendFloats(bin, scratch.data(), scratch.size());

        scratch.clear();
        for (const StaticVertex& v : mesh.vertices) {
            scratch.insert(scratch.end(), {v.color[0], v.color[1], v.color[2], v.color[3]});
        }
        r.color = AppendFloats(bin, scratch.data(), scratch.size());

        r.index = AppendUint32(bin, mesh.indices);
        ranges.push_back(r);
    }

    {
        std::ofstream out(binPath, std::ios::binary);
        if (!out) {
            error = "cannot write " + binPath.string();
            return false;
        }
        out.write(bin.data(), static_cast<std::streamsize>(bin.size()));
        if (!out) {
            error = "failed while writing " + binPath.string();
            return false;
        }
    }

    // --- JSON ---------------------------------------------------------------
    std::ostringstream js;
    js << "{\n";
    js << "  \"formatVersion\": " << kGfModelFormatVersion << ",\n";
    js << "  \"generator\": \"gfnif-export (phase 2)\",\n";
    js << "  \"sourceNif\": \"" << JsonEscape(scene.sourceNifPath) << "\",\n";
    js << "  \"binary\": \"" << JsonEscape(binPath.filename().string()) << "\",\n";
    js << "  \"binaryByteLength\": " << bin.size() << ",\n";

    js << "  \"meshes\": [\n";
    for (size_t i = 0; i < scene.meshes.size(); ++i) {
        const MeshData& m = scene.meshes[i];
        const MeshRanges& r = ranges[i];
        const size_t vcount = m.vertices.size();

        js << "    {\n";
        js << "      \"name\": \"" << JsonEscape(m.name) << "\",\n";
        js << "      \"materialIndex\": " << m.materialIndex << ",\n";
        js << "      \"vertexCount\": " << vcount << ",\n";
        js << "      \"indexCount\": " << m.indices.size() << ",\n";
        js << "      \"sourceTopology\": \""
           << (m.source == MeshData::Source::TriStrips ? "NiTriStrips" : "NiTriShape") << "\",\n";
        js << "      \"degenerateTrianglesDropped\": " << m.degenerateTrianglesDropped << ",\n";
        // Each accessor is {byteOffset, componentCount, count, type}, enough to
        // build a THREE.BufferAttribute with no further arithmetic.
        js << "      \"attributes\": {\n";
        js << "        \"position\": {\"byteOffset\": " << r.position
           << ", \"itemSize\": 3, \"count\": " << vcount << ", \"type\": \"float32\"},\n";
        js << "        \"normal\": {\"byteOffset\": " << r.normal
           << ", \"itemSize\": 3, \"count\": " << vcount << ", \"type\": \"float32\"},\n";
        js << "        \"uv\": {\"byteOffset\": " << r.uv << ", \"itemSize\": 2, \"count\": "
           << vcount << ", \"type\": \"float32\"},\n";
        js << "        \"color\": {\"byteOffset\": " << r.color
           << ", \"itemSize\": 4, \"count\": " << vcount << ", \"type\": \"float32\"}\n";
        js << "      },\n";
        js << "      \"indices\": {\"byteOffset\": " << r.index << ", \"count\": "
           << m.indices.size() << ", \"type\": \"uint32\"}\n";
        js << "    }" << (i + 1 < scene.meshes.size() ? "," : "") << "\n";
    }
    js << "  ],\n";

    js << "  \"materials\": [\n";
    for (size_t i = 0; i < scene.materials.size(); ++i) {
        const MaterialData& m = scene.materials[i];
        js << "    {\n";
        js << "      \"diffuseTexturePath\": \"" << JsonEscape(m.diffuseTexturePath) << "\",\n";
        js << "      \"sourceTextureName\": \"" << JsonEscape(m.sourceTextureName) << "\",\n";
        js << "      \"textureFound\": " << (m.textureFound ? "true" : "false") << ",\n";
        js << "      \"hasVertexColor\": " << (m.hasVertexColor ? "true" : "false") << ",\n";
        js << "      \"ambient\": " << Vec3Json(m.ambient) << ",\n";
        js << "      \"diffuse\": " << Vec3Json(m.diffuse) << ",\n";
        js << "      \"specular\": " << Vec3Json(m.specular) << ",\n";
        js << "      \"emissive\": " << Vec3Json(m.emissive) << ",\n";
        js << "      \"glossiness\": " << Num(m.glossiness) << ",\n";
        js << "      \"alpha\": " << Num(m.alpha) << "\n";
        js << "    }" << (i + 1 < scene.materials.size() ? "," : "") << "\n";
    }
    js << "  ],\n";

    // Reserved for later phases. Emitted as empty rather than omitted so the
    // schema keeps its shape, but deliberately NOT pre-filled with a guessed
    // structure -- phases 3 to 5 will define these.
    js << "  \"skeleton\": null,\n";
    js << "  \"animations\": [],\n";
    js << "  \"particleSystems\": []\n";
    js << "}\n";

    {
        std::ofstream out(modelPath, std::ios::binary);
        if (!out) {
            error = "cannot write " + modelPath.string();
            return false;
        }
        const std::string text = js.str();
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!out) {
            error = "failed while writing " + modelPath.string();
            return false;
        }
    }

    return true;
}

} // namespace gfnif
