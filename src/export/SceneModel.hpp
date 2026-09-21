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

/*! One node of a static (skeleton-less) file's scene graph, exported only for
 *  files that actually need it -- see SceneData::nodes.
 *
 *  A skinned file's animation targets bones in SkeletonData, which already
 *  carries every node in the skeleton root's subtree (not just weighted
 *  ones, see BoneData). A file with NO skin has no SkeletonData at all, so an
 *  embedded NiTransformController on a plain NiNode -- a rotating prop, a
 *  moving door, a billboard pivot -- had nothing to resolve its target
 *  against and was silently orphaned (see PHASE4_FINDINGS, the WA85 case).
 *  SceneNode is the minimal fix: the same {name, parent, local transform}
 *  shape as BoneData, so AnimationTrack can resolve by name against it with
 *  the identical mechanism, and a consumer builds a THREE.Object3D chain
 *  from it exactly as it builds a THREE.Bone chain from SkeletonData. */
struct SceneNode {
    /*! The NiNode/NiAVObject name, stored exactly as it appears in the file. */
    std::string name;
    /*! Index of the parent in SceneData::nodes, or -1 for a root. Parents
     *  always precede their children. */
    int parentIndex = -1;
    /*! The node's rest transform in its parent's space. Column-major. */
    float localMatrix[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
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

    /*! Index into SceneData::nodes, or -1 when the mesh keeps its default
     *  world-space-flattened vertices (see SceneData::nodes and PHASE4
     *  follow-up: the WA85 case).
     *
     *  Set only for the rare skeleton-less file where this mesh's own
     *  NiAVObject (or an ancestor) is the target of a real embedded/`.kf`
     *  animation track -- i.e. scene.nodes is populated AND something in it
     *  actually moves. In that case `vertices` are node-local (relative to
     *  this node, matching the skinned-mesh convention of never baking a
     *  transform that would move at runtime) instead of pre-flattened to
     *  world space, so a consumer parents the mesh under
     *  SceneData::nodes[nodeIndex] and lets the animated node hierarchy carry
     *  it -- otherwise the animation moves an empty pivot while the baked,
     *  disconnected geometry stays put. Every other mesh in the corpus keeps
     *  -1 and today's flattened-world-space vertices unchanged. */
    int nodeIndex = -1;
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
     *  Euler sub-channels) rather than quaternion keys -- composed into a
     *  regular quaternion timeline by ExtractXyzRotation (see
     *  AnimationExtractor.cpp's ComposeXyzEuler for the composition order and
     *  its justification). Counted here so its corpus prevalence (68.0% of
     *  classic tracks, PHASE4_FINDINGS §3.1) is visible without grepping
     *  warnings, not because it is still a gap. */
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

/*! Corpus-wide measurements about particle extraction (Phase 5), accumulated
 *  across a run -- the evidence for the emitter/modifier coverage decisions.
 *  See PHASE5_FINDINGS. */
struct ParticleStats {
    int filesWithParticles = 0;
    int systemsTotal = 0;
    int systemsAttachNodeResolved = 0;
    int systemsAttachNodeOrphaned = 0;
    int systemsMaterialResolved = 0;
    int systemsTextureFound = 0;

    int emittersBox = 0;
    int emittersMesh = 0;
    /*! An emitter type this exporter does not recognise -- none measured in
     *  the corpus (only Box/Mesh occur, PHASE5_FINDINGS Etape 1), kept so a
     *  future corpus update is visible rather than silently mis-extracted. */
    int emittersUnsupported = 0;
    int emitterObjectResolved = 0;

    int modifiersGravity = 0;
    int modifiersRotation = 0;
    int modifiersGrowFade = 0;
    int modifiersColor = 0;
    int modifiersColorKeysMissing = 0;
    int gravityObjectResolved = 0;
    /*! A modifier type not in this exporter's known set at all (not even as
     *  a type-only boilerplate entry) -- skipped with a warning. */
    int modifiersUnsupported = 0;

    /*! meshEmitterMeshNames references across all NiPSysMeshEmitters --
     *  correctif measurement, see docs/PHASE5_FINDINGS.md. */
    int meshEmitterNameRefs = 0;
    /*! Of those, references to a shape the source file marks hidden --
     *  kept in scene.emitterMeshes instead of dropped (WalkNode's
     *  isEmitterSurface exception). */
    int meshEmitterNameRefsHiddenKept = 0;
    /*! Of those, references to a name not found among the file's own
     *  geometry at all -- a genuinely orphaned reference, reported as a
     *  warning, never a failure. */
    int meshEmitterNameRefsUnresolved = 0;

    void Merge(const ParticleStats& o) {
        filesWithParticles += o.filesWithParticles;
        systemsTotal += o.systemsTotal;
        systemsAttachNodeResolved += o.systemsAttachNodeResolved;
        systemsAttachNodeOrphaned += o.systemsAttachNodeOrphaned;
        systemsMaterialResolved += o.systemsMaterialResolved;
        systemsTextureFound += o.systemsTextureFound;
        emittersBox += o.emittersBox;
        emittersMesh += o.emittersMesh;
        emittersUnsupported += o.emittersUnsupported;
        emitterObjectResolved += o.emitterObjectResolved;
        modifiersGravity += o.modifiersGravity;
        modifiersRotation += o.modifiersRotation;
        modifiersGrowFade += o.modifiersGrowFade;
        modifiersColor += o.modifiersColor;
        modifiersColorKeysMissing += o.modifiersColorKeysMissing;
        gravityObjectResolved += o.gravityObjectResolved;
        modifiersUnsupported += o.modifiersUnsupported;
        meshEmitterNameRefs += o.meshEmitterNameRefs;
        meshEmitterNameRefsHiddenKept += o.meshEmitterNameRefsHiddenKept;
        meshEmitterNameRefsUnresolved += o.meshEmitterNameRefsUnresolved;
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
    /*! Target name exactly as the source spells it, matching BoneData::name
     *  or SceneNode::name -- the same string this track was resolved
     *  against. Kept even when boneIndex resolved successfully, so a
     *  consumer can re-bind without reparsing the source. */
    std::string boneName;
    /*! Index into the clip's target SkeletonData::bones (when the file has a
     *  skeleton) or SceneData::nodes (when it does not -- see SceneNode),
     *  or -1 when the name could not be resolved in either. Which array it
     *  indexes is unambiguous per file: a file never populates both (see
     *  SceneData::nodes). An orphaned track is still emitted -- see
     *  AnimationClip -- so the corpus-wide resolution rate can be measured
     *  from the exported data itself. */
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

/*! One color/alpha keyframe of a particle system's color-over-life curve
 *  (NiPSysColorModifier's NiColorData). `time` is normalised 0..1 across a
 *  particle's own lifespan, exactly as NiColorData stores it for this use
 *  (see PHASE5_FINDINGS) -- not a scene/clip time. */
struct ParticleColorKey {
    float time = 0.0f;
    float value[4] = {1.0f, 1.0f, 1.0f, 1.0f};
};

/*! One particle emitter. Every NiParticleSystem in the corpus has exactly one
 *  emitter (measured, see PHASE5_FINDINGS Etape 1) so this is not a vector.
 *
 *  Values are copied straight from the NIF's own fields/units -- angles in
 *  radians as NiPSysEmitter stores them, no conversion to a render engine's
 *  convention -- per this project's standing principle (Z-up, raw alpha
 *  blend enums) of keeping translation to the loader's job. */
struct ParticleEmitterData {
    /*! Raw NIF type name: "NiPSysBoxEmitter" or "NiPSysMeshEmitter" -- the
     *  only two emitter types found in the corpus (PHASE5_FINDINGS Etape 1).
     *  A future emitter type this exporter does not recognise is skipped
     *  with a warning rather than guessed at; see ParticleExtractor. */
    std::string type;

    // --- NiPSysEmitter base fields (both emitter types) ---
    float speed = 0.0f;
    float speedVariation = 0.0f;
    /*! Radians, first emission cone axis. */
    float declination = 0.0f;
    float declinationVariation = 0.0f;
    /*! Radians, second emission cone axis. */
    float planarAngle = 0.0f;
    float planarAngleVariation = 0.0f;
    float initialColor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float initialRadius = 0.0f;
    float radiusVariation = 0.0f;
    float lifeSpan = 0.0f;
    float lifeSpanVariation = 0.0f;

    // --- NiPSysBoxEmitter fields (type == "NiPSysBoxEmitter") ---
    float boxWidth = 0.0f;
    float boxHeight = 0.0f;
    float boxDepth = 0.0f;

    // --- NiPSysMeshEmitter fields (type == "NiPSysMeshEmitter") ---
    /*! Raw NIF enum values (Niflib::VelocityType / EmitFrom) -- kept as ints
     *  rather than translated, same principle as MaterialData's blend enums. */
    int meshInitialVelocityType = 0;
    int meshEmissionType = 0;
    float meshEmissionAxis[3] = {1.0f, 0.0f, 0.0f};
    /*! Names of the NiTriShape/NiTriStrips this emitter emits from, kept for
     *  diagnostics -- a consumer emits from the *attached* geometry's own
     *  exported mesh, not from this list directly. */
    std::vector<std::string> meshEmitterMeshNames;

    /*! Index into SceneData::skeletons[skeletonIndex].bones or
     *  SceneData::nodes (whichever the owning ParticleSystemData resolves
     *  against), or -1 when unresolved. This is NiPSysVolumeEmitter's own
     *  "Emitter Object" -- the node the emitter's shape (box dimensions, or
     *  the mesh-emitter's emission origin) is expressed relative to, which
     *  is routinely a different node from the NiParticleSystem's own parent
     *  (a sibling "<name>-Emitter" NiNode, see PHASE5_FINDINGS). Falls back
     *  to the particle system's own attachNodeIndex when the emitter object
     *  cannot be resolved. */
    int emitterObjectNodeIndex = -1;
};

/*! One particle modifier attached to a NiParticleSystem, other than the
 *  emitter itself (see ParticleSystemData::emitter).
 *
 *  Only the types actually varying in the corpus carry real fields here --
 *  NiPSysAgeDeathModifier/BoundUpdateModifier/PositionModifier/SpawnModifier
 *  are universal engine boilerplate attached to every system (measured
 *  corpus-wide, PHASE5_FINDINGS Etape 1) with no per-effect authored
 *  variation worth extracting, so `type` alone documents their presence. */
struct ParticleModifierData {
    /*! Raw NIF type name, e.g. "NiPSysGravityModifier". */
    std::string type;

    // --- NiPSysGravityModifier (type == "NiPSysGravityModifier") ---
    float gravityAxis[3] = {0.0f, 0.0f, 1.0f};
    float gravityDecay = 0.0f;
    float gravityStrength = 0.0f;
    /*! Raw NiPSysGravityModifier::ForceType enum value (0=Planar,
     *  1=Spherical, 2=Unknown). */
    int gravityForceType = 0;
    float gravityTurbulence = 0.0f;
    float gravityTurbulenceScale = 1.0f;
    /*! Index into the same node/bone list as ParticleSystemData::attachNodeIndex,
     *  or -1 when the modifier has no gravity object (a global/world-axis
     *  force) or it could not be resolved. */
    int gravityObjectNodeIndex = -1;

    // --- NiPSysRotationModifier (type == "NiPSysRotationModifier") ---
    float rotationInitialSpeed = 0.0f;
    float rotationInitialSpeedVariation = 0.0f;
    float rotationInitialAngle = 0.0f;
    float rotationInitialAngleVariation = 0.0f;
    bool rotationRandomSpeedSign = false;
    bool rotationRandomInitialAxis = true;
    float rotationInitialAxis[3] = {1.0f, 0.0f, 0.0f};

    // --- NiPSysGrowFadeModifier (type == "NiPSysGrowFadeModifier") ---
    float growTime = 0.0f;
    float fadeTime = 0.0f;

    // --- NiPSysColorModifier (type == "NiPSysColorModifier") ---
    /*! From this modifier's NiColorData, keys normalised 0..1 across a
     *  particle's lifespan. Empty when the modifier had no data block. */
    std::vector<ParticleColorKey> colorKeys;
};

/*! One particle system (NiParticleSystem), as needed to reproduce it with a
 *  JS particle library (three.quarks or similar) in the Angular viewer --
 *  see PHASE5_FINDINGS for the corpus measurements behind every field here.
 *
 *  This phase extracts parameters only: no simulation, no rendering. Values
 *  stay in the NIF's own units/axes (Z-up, radians, NIF blend enums) exactly
 *  like every other phase's data -- translation to a specific particle
 *  library's conventions is the Angular loader's job. */
struct ParticleSystemData {
    /*! The NiParticleSystem's own name. */
    std::string name;

    /*! Index into SceneData::skeletons[0].bones or SceneData::nodes (never
     *  both -- see SceneData::nodes), or -1 when the name could not be
     *  resolved. This is the system's own PARENT node -- where it sits in
     *  the scene graph -- which may be different from the emitter's own
     *  shape-origin node (see ParticleEmitterData::emitterObjectNodeIndex).
     *  A consumer parents the emitter under this node so it follows an
     *  animated bone (PHASE5_FINDINGS: ~30% of systems in the corpus are
     *  parented, directly or via an ancestor, under a node a companion .kf
     *  actually animates). */
    int attachNodeIndex = -1;
    /*! True when attachNodeIndex resolves into SceneData::skeletons rather
     *  than SceneData::nodes -- a file never has both populated (see
     *  SceneData::nodes), but this makes which one unambiguous without the
     *  consumer having to check both vectors' sizes. */
    bool attachNodeIsBone = false;

    /*! This system's own local transform in attachNodeIndex's space
     *  (NiParticleSystem::GetLocalTransform() -- particle systems have their
     *  own translation/rotation/scale like any NiAVObject, on top of
     *  whichever node they are parented under). Column-major. */
    float localMatrix[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    /*! NiPSysData::GetVertexCount() (inherited from NiGeometryData) -- the
     *  size the source pre-allocated its particle arrays to, i.e. the
     *  maximum number of simultaneously live particles this system was
     *  authored for. A real, typed getter (unlike most particle fields --
     *  see PHASE5_FINDINGS Etape 1), since NiPSysData shares NiGeometryData
     *  with ordinary meshes. */
    int maxParticles = 0;

    /*! Material/texture, reusing MaterialExtractor exactly as a mesh does --
     *  a NiParticleSystem carries the same NiTexturingProperty/
     *  NiMaterialProperty/NiAlphaProperty list a NiTriShape does. Index into
     *  SceneData::materials, or -1 when the system carries no
     *  material-bearing property at all. */
    int materialIndex = -1;

    ParticleEmitterData emitter;
    /*! AgeDeath/BoundUpdate/Position/Spawn modifiers ARE included here (as
     *  type-only entries, see ParticleModifierData) so a consumer can see
     *  the full authored modifier stack, even though this phase does not
     *  extract per-effect fields for them (measured corpus-wide as
     *  boilerplate, PHASE5_FINDINGS). A modifier type this exporter does not
     *  recognise at all is skipped with a warning -- see ParticleExtractor. */
    std::vector<ParticleModifierData> modifiers;
};

/*! One converted .nif. */
struct SceneData {
    std::string sourceNifPath;
    std::vector<MeshData> meshes;
    std::vector<MaterialData> materials;
    /*! Typically 0 or 1 per file -- every NiSkinInstance in the corpus shares
     *  one armature root. Kept as a vector so a file with genuinely separate
     *  armatures would not need a format change. */
    std::vector<SkeletonData> skeletons;
    /*! The file's node hierarchy, exported ONLY when skeletons is empty AND
     *  the file has at least one embedded animation track that needs it (see
     *  SceneNode) -- kept empty for the overwhelming majority of static
     *  files to avoid any change to the validated Phase 2 flattened-vertex
     *  path. A skinned file never populates this: SkeletonData already
     *  covers every node in its skeleton root's subtree. */
    std::vector<SceneNode> nodes;
    /*! Embedded animations plus every .kf found for this file under the
     *  <type>/animation/NAME.kf convention. Empty when the file has neither. */
    std::vector<AnimationClip> animations;

    /*! Particle systems (NiParticleSystem), Phase 5. Empty when the file has
     *  none -- measured at 55% of the corpus (PHASE5_FINDINGS), so this is
     *  routinely non-empty, unlike the rarer skeleton/animation cases. */
    std::vector<ParticleSystemData> particleSystems;

    /*! Geometry kept only as an emission surface for a NiPSysMeshEmitter
     *  (ParticleSystemData::emitter.meshEmitterMeshNames), never for
     *  rendering. Separate from `meshes` on purpose: these shapes are
     *  routinely marked hidden in the source file (measured: 33% of
     *  meshEmitterMeshNames references corpus-wide resolve to a hidden
     *  shape -- see docs/PHASE5_FINDINGS.md correctif), so the ordinary
     *  visibility filter would otherwise drop geometry a particle system
     *  still needs. Kept out of `meshes` so no existing render path shows
     *  them by accident; a consumer looks a name up here only when
     *  resolving a mesh emitter's surface. */
    std::vector<MeshData> emitterMeshes;

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
