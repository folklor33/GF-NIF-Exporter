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

/*! Number of bone influences carried per vertex.
 *
 *  Four is what glTF, Three.js and the GPU skinning path all expect, and it is
 *  also what NiSkinPartition already normalises to. Vertices with fewer real
 *  influences pad with weight 0; vertices with more are truncated to the four
 *  largest and renormalised (see PHASE3_FINDINGS for the measured rate). */
constexpr int kInfluencesPerVertex = 4;

/*! One vertex of a mesh.
 *
 *  Static (non-skinned) geometry has positions and normals in world space, with
 *  the accumulated NiNode transform baked in as in Phase 2. Skinned geometry
 *  instead keeps them *raw*, in the shape's own local space, because the skin
 *  matrices carry the placement -- baking a node transform in as well would
 *  apply it twice. See PHASE3_FINDINGS §6.
 *
 *  The influence slots are present on every vertex, skinned or not. Carrying
 *  one vertex type rather than a StaticVertex/SkinnedVertex pair means
 *  GfxFormatWriter, MeshExtractor and the viewer each have a single code path;
 *  MeshData::isSkinned says whether the slots hold real data, and the writer
 *  simply omits the two attributes for an unskinned mesh, so nothing reaches
 *  the .gfbin that a consumer would have to skip over. */
struct Vertex {
    float position[3] = {0.0f, 0.0f, 0.0f};
    float normal[3] = {0.0f, 0.0f, 1.0f};
    float uv[2] = {0.0f, 0.0f};
    /*! Opaque white when the source has no NiVertexColorProperty / vertex
     *  colors, so a consumer can always read this attribute unconditionally. */
    float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};

    /*! Index into the owning MeshData::skinBindings (glTF "joints"
     *  semantics), not directly into the skeleton. Meaningless where the
     *  matching weight is 0. */
    uint16_t boneIndex[kInfluencesPerVertex] = {0, 0, 0, 0};
    /*! Influence weights, sorted descending, summing to 1 on a skinned vertex
     *  and to 0 on an unskinned one. */
    float weight[kInfluencesPerVertex] = {0.0f, 0.0f, 0.0f, 0.0f};
};

/*! Kept as an alias so the Phase 2 vocabulary still reads correctly at call
 *  sites that only touch the static attributes. */
using StaticVertex = Vertex;

/*! One bone of a skeleton: an entry in the NiNode hierarchy of the file. */
struct BoneData {
    /*! The NiNode name, stored exactly as it appears in the file -- no case
     *  folding, no whitespace trimming. Phase 4 binds .kf tracks to bones by
     *  this string, and the .kf spells it the same way the .nif does. */
    std::string name;
    /*! Index of the parent in SkeletonData::bones, or -1 for the root.
     *  Parents always precede their children, so a consumer can compute world
     *  matrices in a single forward pass. */
    int parentIndex = -1;
    /*! The bone's rest transform in its parent's space, straight from the
     *  NiNode's local transform. Column-major (Three.js / glTF order). */
    float bindMatrixLocal[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    /*! True for a named anchor point -- "Shield Nub", "Right Rope Nub" and
     *  friends -- where a consumer may want to attach a weapon, shield or
     *  rope. Advisory only: the bone is an ordinary bone in every other way.
     *
     *  Helper-named bones are NEVER removed from the skeleton, whatever their
     *  role: 1565 of them are the parent of a genuinely named bone, so
     *  dropping them would break the hierarchy. Only their gizmo *geometry*
     *  is filtered, in MeshExtractor. */
    bool isAttachPoint = false;
};

/*! One bone as used by one particular skinned mesh.
 *
 *  The bind matrix lives here rather than on BoneData because it is a property
 *  of the *skin*, not of the bone: measured on this corpus, two NiSkinInstances
 *  in the same file routinely give the same NiNode different NiSkinData
 *  transforms (8227 such conflicts corpus-wide, differing by up to 14.45; N920
 *  alone has 8). Storing one per bone on the shared skeleton would force us to
 *  pick a winner and misplace every mesh that wanted the other. glTF splits
 *  joints and inverseBindMatrices the same way, for the same reason. */
struct SkinBinding {
    /*! Index into SkeletonData::bones. */
    int boneIndex = -1;
    /*! The complete bind-pose skin matrix for this bone: it takes a vertex of
     *  this mesh, in the raw local space the positions are stored in, straight
     *  to its bind-pose world position. Column-major.
     *
     *  Equivalent to `boneWorldAtBind * inverseBind` already multiplied out.
     *  Storing the product rather than the two halves means a consumer never
     *  has to rebuild the bone's world matrix from the hierarchy just to draw
     *  the bind pose, and cannot get the multiplication order wrong. */
    float skinMatrix[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
};

/*! The bone hierarchy of one file. Bones are in parent-before-child order. */
struct SkeletonData {
    /*! Name of the NiNode the hierarchy was walked from. */
    std::string rootName;
    std::vector<BoneData> bones;
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

    /*! True when the geometry carried a usable NiSkinInstance, i.e. the
     *  boneIndex/weight slots on its vertices hold real data and its positions
     *  are raw shape-local rather than world space. */
    bool isSkinned = false;
    /*! Index into SceneData::skeletons, or -1 when not skinned. A file may mix
     *  skinned and unskinned meshes freely; the unskinned ones keep -1 and are
     *  drawn with their baked world transform as in Phase 2. */
    int skeletonIndex = -1;
    /*! This mesh's bones, in the order Vertex::boneIndex refers to them, each
     *  with the bind-pose skin matrix this particular skin supplies. Empty when
     *  not skinned. */
    std::vector<SkinBinding> skinBindings;
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
    /*! NiMaterialProperty's transparency scalar, 1.0 = opaque.
     *
     *  Clamped to [0, 1] on read: 228 shapes in the corpus store a small
     *  negative value here (down to -0.88), always alongside a real
     *  NiAlphaProperty that carries the material's actual blend state -- see
     *  PHASE2_FINDINGS §12. The field is malformed authoring data on those
     *  shapes, not a signal to act on; clamping keeps it from producing a
     *  negative opacity in a consumer that reads it standalone. */
    float alpha = 1.0f;

    /*! True when a NiVertexColorProperty is attached, i.e. the vertex color
     *  attribute is meaningful rather than the default white. */
    bool hasVertexColor = false;
    /*! False when the .png for sourceTextureName was not found on disk. The
     *  conversion still succeeds; the consumer falls back to the flat colors. */
    bool textureFound = false;

    /*! False when the geometry has no NiAlphaProperty at all. The material is
     *  then plain opaque, exactly Phase 2's behaviour -- every field below is
     *  meaningless in that case and left at its default. */
    bool hasAlphaProperty = false;
    /*! Alpha blending on/off, from NiAlphaProperty's flags bit 0. */
    bool alphaBlendEnabled = false;
    /*! Source/destination blend factors, kept as NIF's own raw enum values
     *  (NiAlphaProperty::BlendFunc: 0=ONE, 1=ZERO, 2=SRC_COLOR, ...,
     *  6=SRC_ALPHA, 7=ONE_MINUS_SRC_ALPHA, ...) rather than translated to a
     *  render engine's constants. Same principle as the Z-up axis convention:
     *  the export stays faithful to the source, and a consumer's loader does
     *  the translation to whatever it renders with. */
    uint8_t srcBlendMode = 0;
    uint8_t dstBlendMode = 0;
    /*! Alpha testing on/off, from flags bit 9. */
    bool alphaTestEnabled = false;
    /*! Alpha test comparison function, NIF's raw TestFunc enum (0=ALWAYS,
     *  1=LESS, ..., 4=GREATER, ...). Measured on the corpus: almost always 4
     *  (GREATER) when testing is enabled at all. */
    uint8_t alphaTestFunc = 0;
    /*! NiAlphaProperty.threshold, 0-255, compared against a pixel's alpha by
     *  alphaTestFunc. */
    uint8_t alphaTestThreshold = 0;
};

/*! Corpus-wide measurements about skinning, accumulated across a run.
 *
 *  The influence histogram is the evidence for the truncation policy: the
 *  format allows 4 influences per vertex, and this says how many vertices the
 *  source actually pushes past that. */
struct SkinningStats {
    int skinnedMeshes = 0;
    int skeletons = 0;
    size_t skinnedVertices = 0;
    /*! influenceHistogram[n] = vertices whose source had exactly n non-zero
     *  influences. Index 0 means a skinned mesh's vertex that no bone weighted
     *  at all, which is itself worth reporting. Sized to hold the observed
     *  maximum; index kMaxTrackedInfluences is an overflow bucket. */
    static constexpr int kMaxTrackedInfluences = 16;
    size_t influenceHistogram[kMaxTrackedInfluences + 1] = {};
    /*! Highest influence count seen on any single vertex. */
    int maxInfluencesSeen = 0;
    /*! Vertices whose influences had to be truncated to kInfluencesPerVertex. */
    size_t verticesTruncated = 0;
    /*! Total weight discarded by truncation, to show it is negligible. */
    double weightDiscarded = 0.0;
    /*! Largest single weight thrown away by truncation. */
    float maxWeightDiscarded = 0.0f;
    /*! Vertices with no bone influence at all that a triangle actually draws.
     *  Unreferenced unweighted vertices are ignored -- nothing renders them. */
    size_t unweightedVerticesInUse = 0;

    /*! Meshes whose NiSkinData carried no vertex weights (hasVertexWeights = 0)
     *  and were skinned from NiSkinPartition instead. */
    int skinsFromPartition = 0;

    /*! Bones that two NiSkinInstances of the same file gave materially
     *  different inverse bind matrices. A single shared skeleton can only hold
     *  one, so a non-zero count means the per-skin transform matters. */
    size_t boneSkinMatrixConflicts = 0;
    float maxSkinMatrixConflict = 0.0f;

    /*! NiSkinPartition cross-check (see PHASE3_FINDINGS). */
    int partitionsChecked = 0;
    int partitionsMissing = 0;
    size_t partitionVerticesCompared = 0;
    /*! Influences the partition asserts that NiSkinData does not name at all.
     *  This is the cross-check that must stay at/near zero: it would mean we
     *  read the wrong bone. */
    size_t partitionBoneMissing = 0;
    /*! Influences the partition names that NiSkinData did have, but that our
     *  own 4-slot truncation dropped. Expected, not an error. */
    size_t partitionBoneTruncatedAway = 0;
    /*! Of partitionBoneMissing, those on a vertex NiSkinData weights not at
     *  all -- i.e. the partition is the only place the influence exists. */
    size_t partitionBoneMissingOnUnweighted = 0;
    /*! Influences whose weight differs by more than 1e-3. Expected to be large
     *  and benign: a partition caps influences and renormalises, so its weights
     *  are systematically higher than the unc-apped NiSkinData ones. */
    size_t partitionWeightMismatches = 0;
    /*! Of those, the ones where the partition weight is *lower* than ours --
     *  the direction the subset+renormalise explanation cannot account for. */
    size_t partitionWeightBelowOurs = 0;
    /*! Largest such shortfall, to show whether they are rounding or structural. */
    float maxPartitionBelowDelta = 0.0f;
    float maxPartitionWeightDelta = 0.0f;

    void Merge(const SkinningStats& o) {
        skinnedMeshes += o.skinnedMeshes;
        skeletons += o.skeletons;
        skinnedVertices += o.skinnedVertices;
        for (int i = 0; i <= kMaxTrackedInfluences; ++i) {
            influenceHistogram[i] += o.influenceHistogram[i];
        }
        if (o.maxInfluencesSeen > maxInfluencesSeen) maxInfluencesSeen = o.maxInfluencesSeen;
        verticesTruncated += o.verticesTruncated;
        weightDiscarded += o.weightDiscarded;
        if (o.maxWeightDiscarded > maxWeightDiscarded) maxWeightDiscarded = o.maxWeightDiscarded;
        unweightedVerticesInUse += o.unweightedVerticesInUse;
        skinsFromPartition += o.skinsFromPartition;
        boneSkinMatrixConflicts += o.boneSkinMatrixConflicts;
        if (o.maxSkinMatrixConflict > maxSkinMatrixConflict) {
            maxSkinMatrixConflict = o.maxSkinMatrixConflict;
        }
        partitionsChecked += o.partitionsChecked;
        partitionsMissing += o.partitionsMissing;
        partitionVerticesCompared += o.partitionVerticesCompared;
        partitionBoneMissing += o.partitionBoneMissing;
        partitionBoneTruncatedAway += o.partitionBoneTruncatedAway;
        partitionBoneMissingOnUnweighted += o.partitionBoneMissingOnUnweighted;
        partitionWeightMismatches += o.partitionWeightMismatches;
        partitionWeightBelowOurs += o.partitionWeightBelowOurs;
        if (o.maxPartitionBelowDelta > maxPartitionBelowDelta) {
            maxPartitionBelowDelta = o.maxPartitionBelowDelta;
        }
        if (o.maxPartitionWeightDelta > maxPartitionWeightDelta) {
            maxPartitionWeightDelta = o.maxPartitionWeightDelta;
        }
    }
};

/*! Corpus-wide measurements about animation extraction, accumulated across a
 *  run -- the evidence for the B-spline sampling-rate decision and the .kf
 *  track resolution rate. See PHASE4_FINDINGS. */
struct AnimationStats {
    int embeddedClips = 0;
    int kfClips = 0;
    /*! .nif files that had a companion .kf resolved and loaded. Files with no
     *  companion are not counted anywhere -- "no .kf" is normal for a model
     *  with no external animation, not a gap to report (see
     *  docs/NAMING_CONVENTIONS.md §2). */
    int kfFilesFound = 0;
    int tracksTotal = 0;
    int tracksResolved = 0;
    int tracksOrphaned = 0;

    int classicTracks = 0;
    int bSplineTracks = 0;
    /*! tracksTotal - classicTracks - bSplineTracks - tracksSkippedOutOfScope,
     *  i.e. a track this phase should have handled but could not. Kept
     *  separate from tracksSkippedOutOfScope so the two read differently in
     *  the summary: an out-of-scope track (a property/UV controller riding
     *  the same ControllerLink array as bone tracks) is expected corpus noise,
     *  not a defect. */
    int tracksFailed = 0;
    /*! ControllerLink entries whose interpolator is not one of the two
     *  bone-transform families this phase reads (NiTransformInterpolator /
     *  NiBSplineTransformInterpolator) -- material, UV or visibility
     *  controllers sharing the same NiControllerSequence. Not a gap: this
     *  phase only extracts bone transforms. */
    int tracksSkippedOutOfScope = 0;
    /*! Classic tracks whose rotation channel used XYZ_ROTATION_KEY (separate
     *  Euler sub-channels) rather than quaternion keys. Translation/scale
     *  still extract; only the rotation channel is left empty. Counted here
     *  so its corpus prevalence is visible without grepping warnings. */
    int tracksWithUnsupportedEulerRotation = 0;
    /*! NiTransformInterpolator links whose GetData() is null -- the bone has a
     *  static pose value for this clip (see NiTransformInterpolator's own
     *  translation/rotation/scale fields) but no keyframe timeline at all.
     *  Not a failure: the bone simply does not move in this particular clip.
     *  Distinct from tracksFailed, which is a track this phase should have
     *  been able to read but could not. */
    int tracksStaticPoseOnly = 0;
    /*! NiBSplineTransformInterpolator links with zero/negative duration or with
     *  every channel offset unset (USHRT_MAX) -- no translation, rotation, or
     *  scale data at all. The B-spline equivalent of tracksStaticPoseOnly. */
    int tracksBSplineEmpty = 0;

    long long totalKeyframesWritten = 0;
    /*! Tracks whose source animated the scale channel at all (more than the
     *  single bind-pose value). Answers brief question 6. */
    int tracksWithScaleAnimated = 0;

    void Merge(const AnimationStats& o) {
        embeddedClips += o.embeddedClips;
        kfClips += o.kfClips;
        kfFilesFound += o.kfFilesFound;
        tracksTotal += o.tracksTotal;
        tracksResolved += o.tracksResolved;
        tracksOrphaned += o.tracksOrphaned;
        classicTracks += o.classicTracks;
        bSplineTracks += o.bSplineTracks;
        tracksFailed += o.tracksFailed;
        tracksSkippedOutOfScope += o.tracksSkippedOutOfScope;
        tracksWithUnsupportedEulerRotation += o.tracksWithUnsupportedEulerRotation;
        tracksStaticPoseOnly += o.tracksStaticPoseOnly;
        tracksBSplineEmpty += o.tracksBSplineEmpty;
        totalKeyframesWritten += o.totalKeyframesWritten;
        tracksWithScaleAnimated += o.tracksWithScaleAnimated;
    }
};

/*! One translation/scale keyframe: a time and a 3-component value. */
struct VectorKey {
    float time = 0.0f;
    float value[3] = {0.0f, 0.0f, 0.0f};
};

/*! One rotation keyframe: a time and a quaternion, stored [x, y, z, w] (the
 *  glTF/Three.js component order). Component values are copied straight from
 *  niflib's own {w, x, y, z} with no axis remap -- see AnimationExtractor.cpp
 *  for why that is safe (no handedness change anywhere in this exporter). */
struct QuatKey {
    float time = 0.0f;
    float value[4] = {0.0f, 0.0f, 0.0f, 1.0f};
};

/*! Animation of a single bone within a clip.
 *
 *  Channels are independent and may have different key counts/timings, exactly
 *  as NiTransformData stores them -- a consumer interpolates each channel on
 *  its own timeline rather than assuming a shared keyframe grid. An empty
 *  channel means the source did not animate that property; the consumer keeps
 *  the bone's bind-pose value for it. */
struct AnimationTrack {
    /*! Bone name exactly as the source spells it, matching BoneData::name --
     *  the same string this track was resolved against. Kept even when
     *  boneIndex resolved successfully, so a consumer can re-bind without
     *  reparsing the source. */
    std::string boneName;
    /*! Index into the clip's target SkeletonData::bones, or -1 when the name
     *  could not be resolved (an orphaned .kf track). An orphaned track is
     *  still emitted -- see AnimationClip -- so the corpus-wide resolution
     *  rate can be measured from the exported data itself. */
    int boneIndex = -1;
    std::vector<VectorKey> translations;
    std::vector<QuatKey> rotations;
    std::vector<VectorKey> scales;
};

/*! One animation clip: either an embedded NiControllerSequence-less
 *  NiTransformController/NiMultiTargetTransformController pass (originFile ==
 *  "embedded"), or a whole NiControllerSequence loaded from a .kf file
 *  (originFile == the .kf's filename). */
struct AnimationClip {
    /*! Sequence name (from NiControllerSequence) or a synthesised name for the
     *  embedded case. Not guaranteed unique across clips of one file. */
    std::string name;
    /*! "embedded" or the source .kf filename (no directory), so a consumer or
     *  a report can tell the two paths apart without re-deriving it. */
    std::string originFile;
    float durationSeconds = 0.0f;
    /*! Sampling rate used to resample any NiBSplineCompTransformInterpolator
     *  track in this clip, in Hz. 0 when the clip has no B-spline track (every
     *  track kept its native NiTransformData keys, see PHASE4_FINDINGS). */
    float bSplineSampleRate = 0.0f;
    /*! True if at least one track in this clip came from a
     *  NiBSplineCompTransformInterpolator and was resampled rather than kept
     *  as native keys. */
    bool wasResampledFromBSpline = false;
    std::vector<AnimationTrack> tracks;
};

/*! One converted .nif.
 *
 *  Phase 5 will add particleSystems. The writer already emits that JSON key
 *  as empty so the schema does not change shape later. */
struct SceneData {
    std::string sourceNifPath;
    std::vector<MeshData> meshes;
    std::vector<MaterialData> materials;
    /*! Typically 0 or 1 per file -- every NiSkinInstance in the corpus shares
     *  one armature root. Kept as a vector so a file with genuinely separate
     *  armatures would not need a format change. */
    std::vector<SkeletonData> skeletons;
    /*! Embedded animations plus every .kf found for this file under the
     *  <type>/animation/NAME.kf convention. Empty when the file has neither. */
    std::vector<AnimationClip> animations;

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
