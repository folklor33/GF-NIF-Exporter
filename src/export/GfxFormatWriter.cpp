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

size_t AppendUint16(std::string& buffer, const std::vector<uint16_t>& values) {
    const size_t offset = buffer.size();
    buffer.append(reinterpret_cast<const char*>(values.data()), values.size() * sizeof(uint16_t));
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

/*! Serialises a column-major 4x4 as a flat JSON array, in the element order
 *  THREE.Matrix4.fromArray expects. */
std::string Mat4Json(const float (&m)[16]) {
    std::ostringstream out;
    out << "[";
    for (int i = 0; i < 16; ++i) {
        out << (i ? ", " : "") << Num(m[i]);
    }
    out << "]";
    return out.str();
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
        size_t skinIndex = 0, skinWeight = 0;
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

        // Skin attributes are written only for a skinned mesh, so an unskinned
        // one costs nothing in the .gfbin even though every Vertex carries the
        // slots in memory. uint16 bone indices match THREE's skinIndex
        // expectations and halve the space against uint32.
        if (mesh.isSkinned) {
            std::vector<uint16_t> boneIdx;
            boneIdx.reserve(mesh.vertices.size() * kInfluencesPerVertex);
            for (const Vertex& v : mesh.vertices) {
                for (int k = 0; k < kInfluencesPerVertex; ++k) {
                    boneIdx.push_back(v.boneIndex[k]);
                }
            }
            r.skinIndex = AppendUint16(bin, boneIdx);

            // uint16 leaves the buffer 2-byte aligned on an odd vertex count;
            // pad so the float32 weights that follow start 4-byte aligned, as
            // a Float32Array view over the range requires.
            while (bin.size() % 4 != 0) {
                bin.push_back('\0');
            }

            scratch.clear();
            scratch.reserve(mesh.vertices.size() * kInfluencesPerVertex);
            for (const Vertex& v : mesh.vertices) {
                for (int k = 0; k < kInfluencesPerVertex; ++k) {
                    scratch.push_back(v.weight[k]);
                }
            }
            r.skinWeight = AppendFloats(bin, scratch.data(), scratch.size());
        }

        // Uint32Array views need 4-byte alignment too. Everything above ends on
        // a 4-byte boundary already, but the padding is kept explicit so a
        // future attribute of another width cannot silently break it.
        while (bin.size() % 4 != 0) {
            bin.push_back('\0');
        }
        r.index = AppendUint32(bin, mesh.indices);
        ranges.push_back(r);
    }

    // Animation keyframes go in the same buffer, after every mesh's data --
    // same principle as the mesh attributes: bulk numeric data lives in the
    // .gfbin, addressed by byte offset from the .gfmodel JSON, so a consumer
    // reads it straight into a typed array with no parsing.
    //
    // Each channel (translation/rotation/scale) of each track is stored as two
    // parallel arrays: times (float32, one per key) and values (float32,
    // 3 or 4 per key). Keeping times separate from values, rather than
    // interleaving [time, x, y, z], lets a consumer binary-search the time
    // array directly without a stride.
    struct ChannelRange {
        size_t timeOffset = 0, valueOffset = 0;
        size_t count = 0;
    };
    struct TrackRanges {
        ChannelRange translations, rotations, scales;
    };
    std::vector<std::vector<TrackRanges>> animRanges(scene.animations.size());

    for (size_t ci = 0; ci < scene.animations.size(); ++ci) {
        const AnimationClip& clip = scene.animations[ci];
        animRanges[ci].resize(clip.tracks.size());
        for (size_t ti = 0; ti < clip.tracks.size(); ++ti) {
            const AnimationTrack& t = clip.tracks[ti];
            TrackRanges& r = animRanges[ci][ti];
            std::vector<float> scratch;

            r.translations.count = t.translations.size();
            scratch.clear();
            for (const VectorKey& k : t.translations) scratch.push_back(k.time);
            r.translations.timeOffset = AppendFloats(bin, scratch.data(), scratch.size());
            scratch.clear();
            for (const VectorKey& k : t.translations) {
                scratch.insert(scratch.end(), {k.value[0], k.value[1], k.value[2]});
            }
            r.translations.valueOffset = AppendFloats(bin, scratch.data(), scratch.size());

            r.rotations.count = t.rotations.size();
            scratch.clear();
            for (const QuatKey& k : t.rotations) scratch.push_back(k.time);
            r.rotations.timeOffset = AppendFloats(bin, scratch.data(), scratch.size());
            scratch.clear();
            for (const QuatKey& k : t.rotations) {
                scratch.insert(scratch.end(), {k.value[0], k.value[1], k.value[2], k.value[3]});
            }
            r.rotations.valueOffset = AppendFloats(bin, scratch.data(), scratch.size());

            r.scales.count = t.scales.size();
            scratch.clear();
            for (const VectorKey& k : t.scales) scratch.push_back(k.time);
            r.scales.timeOffset = AppendFloats(bin, scratch.data(), scratch.size());
            scratch.clear();
            for (const VectorKey& k : t.scales) scratch.push_back(k.value[0]);
            r.scales.valueOffset = AppendFloats(bin, scratch.data(), scratch.size());
        }
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
    js << "  \"generator\": \"gfnif-export (phase 4)\",\n";
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
        js << "      \"isSkinned\": " << (m.isSkinned ? "true" : "false") << ",\n";
        js << "      \"skeletonIndex\": " << m.skeletonIndex << ",\n";
        js << "      \"nodeIndex\": " << m.nodeIndex << ",\n";
        // glTF "joints" + "inverseBindMatrices", folded into one list: entry j
        // is the bone that this mesh's skinIndex value j refers to, with the
        // inverse bind matrix *this* skin supplies for it.
        if (m.isSkinned) {
            js << "      \"skinBindings\": [\n";
            for (size_t b = 0; b < m.skinBindings.size(); ++b) {
                const SkinBinding& sb = m.skinBindings[b];
                js << "        {\"bone\": " << sb.boneIndex << ", \"skinMatrix\": "
                   << Mat4Json(sb.skinMatrix) << "}"
                   << (b + 1 < m.skinBindings.size() ? "," : "") << "\n";
            }
            js << "      ],\n";
        }
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
           << ", \"itemSize\": 4, \"count\": " << vcount << ", \"type\": \"float32\"}";
        // Present only on a skinned mesh. The names match THREE's
        // 'skinIndex'/'skinWeight' buffer attributes, so a consumer can set
        // them straight onto a SkinnedMesh geometry.
        if (m.isSkinned) {
            js << ",\n";
            js << "        \"skinIndex\": {\"byteOffset\": " << r.skinIndex << ", \"itemSize\": "
               << kInfluencesPerVertex << ", \"count\": " << vcount
               << ", \"type\": \"uint16\"},\n";
            js << "        \"skinWeight\": {\"byteOffset\": " << r.skinWeight << ", \"itemSize\": "
               << kInfluencesPerVertex << ", \"count\": " << vcount
               << ", \"type\": \"float32\"}\n";
        } else {
            js << "\n";
        }
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
        js << "      \"alpha\": " << Num(m.alpha) << ",\n";
        // Raw NIF NiAlphaProperty state -- see MaterialData::srcBlendMode for
        // why these stay as NIF's own enum values rather than a render
        // engine's constants. hasAlphaProperty=false means every other field
        // here is meaningless and the material is plain opaque, matching
        // Phase 2's behaviour exactly.
        js << "      \"hasAlphaProperty\": " << (m.hasAlphaProperty ? "true" : "false") << ",\n";
        js << "      \"alphaBlendEnabled\": " << (m.alphaBlendEnabled ? "true" : "false") << ",\n";
        js << "      \"srcBlendMode\": " << static_cast<int>(m.srcBlendMode) << ",\n";
        js << "      \"dstBlendMode\": " << static_cast<int>(m.dstBlendMode) << ",\n";
        js << "      \"alphaTestEnabled\": " << (m.alphaTestEnabled ? "true" : "false") << ",\n";
        js << "      \"alphaTestFunc\": " << static_cast<int>(m.alphaTestFunc) << ",\n";
        js << "      \"alphaTestThreshold\": " << static_cast<int>(m.alphaTestThreshold) << "\n";
        js << "    }" << (i + 1 < scene.materials.size() ? "," : "") << "\n";
    }
    js << "  ],\n";

    // Bones are emitted parent-before-child, so a consumer can build the
    // hierarchy in one forward pass without sorting or deferring.
    js << "  \"skeletons\": [\n";
    for (size_t i = 0; i < scene.skeletons.size(); ++i) {
        const SkeletonData& s = scene.skeletons[i];
        js << "    {\n";
        js << "      \"rootName\": \"" << JsonEscape(s.rootName) << "\",\n";
        js << "      \"boneCount\": " << s.bones.size() << ",\n";
        js << "      \"bones\": [\n";
        for (size_t b = 0; b < s.bones.size(); ++b) {
            const BoneData& bone = s.bones[b];
            js << "        {\"name\": \"" << JsonEscape(bone.name) << "\""
               << ", \"parent\": " << bone.parentIndex
               << ", \"isAttachPoint\": " << (bone.isAttachPoint ? "true" : "false")
               << ",\n         \"bindMatrixLocal\": " << Mat4Json(bone.bindMatrixLocal) << "}"
               << (b + 1 < s.bones.size() ? "," : "") << "\n";
        }
        js << "      ]\n";
        js << "    }" << (i + 1 < scene.skeletons.size() ? "," : "") << "\n";
    }
    js << "  ],\n";

    // Only populated for a skeleton-less file that has an embedded animation
    // track targeting a plain node -- see SceneData::nodes. Same shape and
    // parent-before-child ordering as "skeletons" above, minus the
    // isAttachPoint flag, which is meaningless off a skeleton.
    js << "  \"nodes\": [\n";
    for (size_t i = 0; i < scene.nodes.size(); ++i) {
        const SceneNode& n = scene.nodes[i];
        js << "    {\"name\": \"" << JsonEscape(n.name) << "\""
           << ", \"parent\": " << n.parentIndex
           << ",\n     \"localMatrix\": " << Mat4Json(n.localMatrix) << "}"
           << (i + 1 < scene.nodes.size() ? "," : "") << "\n";
    }
    js << "  ],\n";

    // Each channel is {times: accessor, values: accessor, count}, empty
    // (count 0) when the source did not animate that property -- a consumer
    // keeps the bone's bind-pose value in that case. itemSize is baked into
    // the accessor rather than stated separately: 3 for translation/scale, 4
    // (quaternion xyzw) for rotation.
    const auto channelJson = [](const char* indent, const ChannelRange& r, int itemSize) {
        std::ostringstream out;
        out << indent << "{\"count\": " << r.count << ", \"times\": {\"byteOffset\": "
            << r.timeOffset << ", \"itemSize\": 1, \"count\": " << r.count
            << ", \"type\": \"float32\"}, \"values\": {\"byteOffset\": " << r.valueOffset
            << ", \"itemSize\": " << itemSize << ", \"count\": " << r.count
            << ", \"type\": \"float32\"}}";
        return out.str();
    };

    js << "  \"animations\": [\n";
    for (size_t ci = 0; ci < scene.animations.size(); ++ci) {
        const AnimationClip& clip = scene.animations[ci];
        js << "    {\n";
        js << "      \"name\": \"" << JsonEscape(clip.name) << "\",\n";
        js << "      \"originFile\": \"" << JsonEscape(clip.originFile) << "\",\n";
        js << "      \"durationSeconds\": " << Num(clip.durationSeconds) << ",\n";
        js << "      \"wasResampledFromBSpline\": "
           << (clip.wasResampledFromBSpline ? "true" : "false") << ",\n";
        js << "      \"bSplineSampleRate\": " << Num(clip.bSplineSampleRate) << ",\n";
        js << "      \"tracks\": [\n";
        for (size_t ti = 0; ti < clip.tracks.size(); ++ti) {
            const AnimationTrack& t = clip.tracks[ti];
            const TrackRanges& r = animRanges[ci][ti];
            js << "        {\n";
            js << "          \"boneName\": \"" << JsonEscape(t.boneName) << "\",\n";
            js << "          \"boneIndex\": " << t.boneIndex << ",\n";
            js << "          \"translation\": " << channelJson("", r.translations, 3) << ",\n";
            js << "          \"rotation\": " << channelJson("", r.rotations, 4) << ",\n";
            // Scale is stored as a single float per key (uniform scale only --
            // NiAVObject cannot even express non-uniform scale, see
            // PHASE3_FINDINGS), not a 3-component vector like translation.
            js << "          \"scale\": " << channelJson("", r.scales, 1) << "\n";
            js << "        }" << (ti + 1 < clip.tracks.size() ? "," : "") << "\n";
        }
        js << "      ]\n";
        js << "    }" << (ci + 1 < scene.animations.size() ? "," : "") << "\n";
    }
    js << "  ],\n";

    // Reserved for a later phase. Emitted as empty rather than omitted so the
    // schema keeps its shape, but deliberately NOT pre-filled with a guessed
    // structure -- phase 5 will define this.
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
