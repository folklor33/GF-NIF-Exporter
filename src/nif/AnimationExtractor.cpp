#include "nif/AnimationExtractor.hpp"

#include "nif/HeaderNormalizer.hpp"

#include "niflib.h"
#include "nif_math.h"
#include "obj/NiAVObject.h"
#include "obj/NiBSplineCompTransformInterpolator.h"
#include "obj/NiBSplineTransformInterpolator.h"
#include "obj/NiControllerSequence.h"
#include "obj/NiKeyframeData.h"
#include "obj/NiMultiTargetTransformController.h"
#include "obj/NiNode.h"
#include "obj/NiObjectNET.h"
#include "obj/NiTimeController.h"
#include "obj/NiTransformController.h"
#include "obj/NiTransformData.h"
#include "obj/NiTransformInterpolator.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <sstream>

namespace gfnif {
namespace {

using Niflib::NiObject;
using Niflib::NiObjectRef;

/*! Cubic, uniform non-rational B-spline -- the convention niflib's own
 *  bspline() helper (and every other NIF tool) assumes when a file does not
 *  say otherwise. Passed straight through to niflib's Sample*Keys(); it clamps
 *  internally when a track has fewer than 4 control points. */
constexpr int kBSplineDegree = 3;

/*! Points per second used to resample a NiBSplineCompTransformInterpolator
 *  track. See PHASE4_FINDINGS for the error/size measurements this rests on. */
constexpr float kBSplineSampleRateHz = 30.0f;

/*! True if a track's scale channel actually varies -- as opposed to holding
 *  the same value at every key, which happens whenever the source authored a
 *  redundant single-value channel rather than truly animating scale. Answers
 *  the brief's question 6 (does the corpus ever animate bone scale at all). */
bool ScaleActuallyAnimates(const std::vector<VectorKey>& scales) {
    for (size_t i = 1; i < scales.size(); ++i) {
        if (scales[i].value[0] != scales[0].value[0]) {
            return true;
        }
    }
    return false;
}

int FindBoneByName(const SkeletonData* skeleton, const std::string& name) {
    if (skeleton == nullptr) {
        return -1;
    }
    for (size_t i = 0; i < skeleton->bones.size(); ++i) {
        if (skeleton->bones[i].name == name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int FindNodeByName(const std::vector<SceneNode>* nodes, const std::string& name) {
    if (nodes == nullptr) {
        return -1;
    }
    for (size_t i = 0; i < nodes->size(); ++i) {
        if ((*nodes)[i].name == name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

/*! Resolves a track's target name against whichever of the two a file has --
 *  see AnimationExtractor.hpp: a file is never both skinned and carrying a
 *  standalone node list, so exactly one of `skeleton`/`nodes` is non-null in
 *  practice, but both are tried defensively rather than assuming that. */
int ResolveTargetIndex(const SkeletonData* skeleton, const std::vector<SceneNode>* nodes,
                       const std::string& name) {
    const int boneIndex = FindBoneByName(skeleton, name);
    if (boneIndex >= 0) {
        return boneIndex;
    }
    return FindNodeByName(nodes, name);
}

/*! Copies a niflib quaternion {w, x, y, z} into the glTF/Three.js component
 *  order [x, y, z, w], renormalizing in the process.
 *
 *  No axis remap is applied, matching PHASE3_FINDINGS §6: this exporter stays
 *  Z-up end to end and never flips a handedness, so a quaternion's components
 *  carry over unchanged -- the same rotation, expressed in the same frame,
 *  just relabelled into the order a consumer's quaternion type expects.
 *
 *  The renormalization matters for the B-spline path: a B-spline is
 *  interpolated component-wise through the quaternion's four coordinates,
 *  which does not generally stay on the unit sphere between control points --
 *  measured on the corpus, uncorrected norms drift up to 0.77 from 1.0 on some
 *  resampled tracks. A classic NiTransformData key is expected to already be
 *  unit length, but renormalizing costs nothing and guards against any source
 *  data that is not quite unit either way; a consumer's slerp requires unit
 *  input. */
void CopyQuat(const Niflib::Quaternion& q, float (&out)[4]) {
    float x = q.x, y = q.y, z = q.z, w = q.w;
    const float norm = std::sqrt(x * x + y * y + z * z + w * w);
    if (norm > 1e-9f) {
        x /= norm;
        y /= norm;
        z /= norm;
        w /= norm;
    } else {
        x = y = z = 0.0f;
        w = 1.0f;
    }
    out[0] = x;
    out[1] = y;
    out[2] = z;
    out[3] = w;
}

void CopyVec3(const Niflib::Vector3& v, float (&out)[3]) {
    out[0] = v.x;
    out[1] = v.y;
    out[2] = v.z;
}

/*! Composes three Euler angles (radians) into a quaternion using the order
 *  XYZ_ROTATION_KEY implies: rotate about X first, then Y, then Z, each about
 *  the fixed/world axes -- i.e. q = qz (x) qy (x) qx (Hamilton product, right
 *  operand applied first). This is not a guess: niflib itself has no
 *  composition helper for this key type (PHASE4_FINDINGS §12.2, confirmed by
 *  reading nif_math.cpp/.h, kfm.cpp and every obj/*.cpp -- no FromEuler, no
 *  XYZ-composition code anywhere), so the order comes from two independent,
 *  convergent sources instead:
 *
 *   1. blender_niftools_addon's animation importer (same NifTools umbrella as
 *      niflib and nif.xml, the closest thing to an authoritative reference)
 *      composes exactly this way: it builds a `mathutils.Euler(n_val)` with no
 *      explicit order argument, and Blender's own default Euler order is
 *      documented as 'XYZ' (blender_python_api docs, "create a new euler with
 *      default axis rotation order" example). Deriving Blender's own
 *      eul_to_mat3() by hand against the standard Rx/Ry/Rz matrices confirms
 *      'XYZ' means R = Rz(z)*Ry(y)*Rx(x) -- apply X first.
 *   2. An empirical corpus-wide measurement (tools/diag/measure_xyz_euler_order.cpp)
 *      tested all 6 possible orders two ways: comparing the composed rotation
 *      at each track's t=0 against its target bone's bind pose (264403 tracks,
 *      1029 files), and a smoothness check across each track's own resampled
 *      timeline that does not depend on any rest-pose assumption at all
 *      (264401 tracks). Both independently pick this same order: it is the
 *      unique best performer on the t=0 check (tied only with YXZ, expected
 *      since X and Y rotations nearly commute at small angles) and the clear,
 *      unambiguous winner on the smoothness check (mean 1.42 deg/step vs.
 *      1.58-1.70 deg/step for the other five candidates) -- see
 *      PHASE4_FINDINGS for the full numbers. */
Niflib::Quaternion ComposeXyzEuler(float xRad, float yRad, float zRad) {
    const float hx = xRad * 0.5f, hy = yRad * 0.5f, hz = zRad * 0.5f;
    // Single-axis quaternions, {w,x,y,z}.
    const float qx_w = std::cos(hx), qx_x = std::sin(hx);
    const float qy_w = std::cos(hy), qy_y = std::sin(hy);
    const float qz_w = std::cos(hz), qz_z = std::sin(hz);

    // qyx = qy * qx (Hamilton product).
    const float qyx_w = qy_w * qx_w;
    const float qyx_x = qy_w * qx_x;
    const float qyx_y = qy_y * qx_w;
    const float qyx_z = -qy_y * qx_x;

    // q = qz * qyx.
    const float w = qz_w * qyx_w - qz_z * qyx_z;
    const float x = qz_w * qyx_x - qz_z * qyx_y;
    const float y = qz_w * qyx_y + qz_z * qyx_x;
    const float z = qz_w * qyx_z + qz_z * qyx_w;

    return Niflib::Quaternion(w, x, y, z);
}

/*! Linearly samples a Key<float> vector at time `t`, clamping to the first/
 *  last key outside its range. Matches this project's existing convention for
 *  classic tracks: tangent/TBC data is dropped everywhere, not just here (see
 *  ExtractClassicTrack's own comment) -- the blender_niftools_addon reference
 *  importer does the same for XYZ_ROTATION_KEY specifically (resamples onto
 *  the union of the three channels' key times via plain linear interpolation,
 *  regardless of each axis's own KeyType), so this is consistent with both
 *  this codebase's precedent and the reference implementation, not a new,
 *  unverified choice. */
float LerpKeyAt(const std::vector<Niflib::Key<float>>& keys, float t) {
    if (keys.empty()) {
        return 0.0f;
    }
    if (t <= keys.front().time) {
        return keys.front().data;
    }
    if (t >= keys.back().time) {
        return keys.back().data;
    }
    for (size_t i = 1; i < keys.size(); ++i) {
        if (t <= keys[i].time) {
            const float t0 = keys[i - 1].time;
            const float t1 = keys[i].time;
            const float a = (t1 > t0) ? (t - t0) / (t1 - t0) : 0.0f;
            return keys[i - 1].data + a * (keys[i].data - keys[i - 1].data);
        }
    }
    return keys.back().data;
}

/*! Composes a NiKeyframeData's independent X/Y/Z rotation channels
 *  (XYZ_ROTATION_KEY) into a single quaternion timeline and appends it to
 *  `track.rotations`.
 *
 *  The three channels are independently timed (their own key counts, times,
 *  and per-axis KeyType -- confirmed structurally in NiKeyframeData.h; see
 *  PHASE4_FINDINGS §12.2), so they are first resampled onto the union of all
 *  three channels' key times (matching the reference importer's own
 *  approach), then composed per sample with ComposeXyzEuler and written as a
 *  regular quaternion key. A channel with fewer than 1 key is treated as a
 *  constant 0 (no rotation contribution from that axis) via LerpKeyAt's
 *  empty-vector fallback -- consistent with "this axis was never keyed", not
 *  an error. */
void ExtractXyzRotation(Niflib::NiTransformData* data, AnimationTrack& track) {
    const std::vector<Niflib::Key<float>> xKeys = data->GetXRotateKeys();
    const std::vector<Niflib::Key<float>> yKeys = data->GetYRotateKeys();
    const std::vector<Niflib::Key<float>> zKeys = data->GetZRotateKeys();

    std::vector<float> allTimes;
    allTimes.reserve(xKeys.size() + yKeys.size() + zKeys.size());
    for (const auto& k : xKeys) allTimes.push_back(k.time);
    for (const auto& k : yKeys) allTimes.push_back(k.time);
    for (const auto& k : zKeys) allTimes.push_back(k.time);
    if (allTimes.empty()) {
        return; // No axis carries any key at all: nothing to compose.
    }
    std::sort(allTimes.begin(), allTimes.end());
    allTimes.erase(std::unique(allTimes.begin(), allTimes.end()), allTimes.end());

    track.rotations.reserve(track.rotations.size() + allTimes.size());
    for (float t : allTimes) {
        const float x = LerpKeyAt(xKeys, t);
        const float y = LerpKeyAt(yKeys, t);
        const float z = LerpKeyAt(zKeys, t);
        QuatKey qk;
        qk.time = t;
        CopyQuat(ComposeXyzEuler(x, y, z), qk.value);
        track.rotations.push_back(qk);
    }
}

/*! Writes a niflib Matrix44 into a column-major float[16] -- identical
 *  convention and transpose to SkeletonExtractor.cpp's ToColumnMajor (kept as
 *  a separate copy since that one is file-local); see that file's comment for
 *  the full row-vector/column-major derivation. Used for SceneNode::localMatrix,
 *  the node-hierarchy equivalent of BoneData::bindMatrixLocal. */
void ToColumnMajorLocal(const Niflib::Matrix44& m, float (&out)[16]) {
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            out[i * 4 + j] = m[i][j];
        }
    }
}

/*! Reads a NiTransformInterpolator's NiTransformData keys as-is, keeping their
 *  native times. Tangent/TCB data is dropped -- the consumer interpolates
 *  linearly between keys -- see PHASE4_FINDINGS for how often that discards
 *  real easing versus how often the source was already linear. */
bool ExtractClassicTrack(Niflib::NiTransformInterpolator* interp, AnimationTrack& track,
                         std::vector<std::string>* warnings, const std::string& context,
                         bool& outUsedEulerRotation, bool& outStaticPoseOnly) {
    outUsedEulerRotation = false;
    outStaticPoseOnly = false;
    Niflib::NiTransformData* data = interp->GetData();
    if (data == nullptr) {
        outStaticPoseOnly = true;
        return false; // Pose-only interpolator, no timeline. Not an error.
    }

    // XYZ_ROTATION_KEY stores separate per-axis Euler tracks instead of
    // quaternion keys -- the majority encoding for classic rotation in this
    // corpus (68.0% of classic tracks, PHASE4_FINDINGS §3.1), not a rare case.
    // ExtractXyzRotation composes the three independently-timed channels into
    // a single quaternion timeline; see ComposeXyzEuler for the composition
    // order and its justification (external reference + corpus-wide
    // measurement, PHASE4_FINDINGS §12.2 follow-up).
    if (data->GetRotateType() == Niflib::XYZ_ROTATION_KEY) {
        outUsedEulerRotation = true;
        ExtractXyzRotation(data, track);
    } else {
        for (const Niflib::Key<Niflib::Quaternion>& k : data->GetQuatRotateKeys()) {
            QuatKey qk;
            qk.time = k.time;
            CopyQuat(k.data, qk.value);
            track.rotations.push_back(qk);
        }
    }

    for (const Niflib::Key<Niflib::Vector3>& k : data->GetTranslateKeys()) {
        VectorKey vk;
        vk.time = k.time;
        CopyVec3(k.data, vk.value);
        track.translations.push_back(vk);
    }

    for (const Niflib::Key<float>& k : data->GetScaleKeys()) {
        VectorKey vk;
        vk.time = k.time;
        vk.value[0] = vk.value[1] = vk.value[2] = k.data;
        track.scales.push_back(vk);
    }

    return true;
}

/*! Samples a NiBSplineTransformInterpolator (covers the plain and compressed
 *  forms, via virtual dispatch) at a uniform rate and fills `track`.
 *
 *  Returns false only when the interpolator carries no channel data at all
 *  (npoints computes to 0) -- a channel the source did not animate produces
 *  an empty vector for that channel specifically, which is normal (see
 *  ExtractClassicTrack's translation/scale being independently optional). */
bool ExtractBSplineTrack(Niflib::NiBSplineTransformInterpolator* interp, AnimationTrack& track,
                         float sampleRateHz, float& outDuration, bool& outEmpty) {
    outEmpty = false;
    const float start = interp->GetStartTime();
    const float stop = interp->GetStopTime();
    const float duration = stop - start;
    outDuration = duration;
    if (!(duration > 0.0f)) {
        outEmpty = true;
        return false;
    }

    const int npoints = std::max(2, static_cast<int>(std::ceil(duration * sampleRateHz)) + 1);

    for (const Niflib::Key<Niflib::Quaternion>& k :
         interp->SampleQuatRotateKeys(npoints, kBSplineDegree)) {
        QuatKey qk;
        qk.time = k.time;
        CopyQuat(k.data, qk.value);
        track.rotations.push_back(qk);
    }
    for (const Niflib::Key<Niflib::Vector3>& k :
         interp->SampleTranslateKeys(npoints, kBSplineDegree)) {
        VectorKey vk;
        vk.time = k.time;
        CopyVec3(k.data, vk.value);
        track.translations.push_back(vk);
    }
    for (const Niflib::Key<float>& k : interp->SampleScaleKeys(npoints, kBSplineDegree)) {
        VectorKey vk;
        vk.time = k.time;
        vk.value[0] = vk.value[1] = vk.value[2] = k.data;
        track.scales.push_back(vk);
    }

    const bool ok = !track.rotations.empty() || !track.translations.empty() || !track.scales.empty();
    outEmpty = !ok;
    return ok;
}

/*! Fills `track` from `interp`, dispatching on its concrete type. Returns
 *  false when the interpolator type is not one this phase handles (e.g. a
 *  bare NiKeyBasedInterpolator with no data) or carries no usable data.
 *
 *  `outGenuineFailure` is set to true only when the interpolator IS one of the
 *  two bone-transform families this phase reads but its data could not be
 *  turned into a track for a reason not already covered by a dedicated
 *  counter (out-of-scope type, static pose, empty B-spline, Euler rotation).
 *  Every caller should count a failure only when this comes back true --
 *  otherwise a normal "this bone simply doesn't move here" case gets
 *  misreported as an error. */
bool ExtractInterpolator(Niflib::NiInterpolator* interp, AnimationTrack& track,
                         AnimationClip& clip, AnimationStats& stats,
                         std::vector<std::string>* warnings, const std::string& context,
                         bool& outGenuineFailure) {
    outGenuineFailure = false;
    if (interp == nullptr) {
        return false;
    }
    if (auto* bspline = dynamic_cast<Niflib::NiBSplineTransformInterpolator*>(interp)) {
        float duration = 0.0f;
        bool empty = false;
        const bool ok =
            ExtractBSplineTrack(bspline, track, kBSplineSampleRateHz, duration, empty);
        if (empty) {
            ++stats.tracksBSplineEmpty;
        }
        if (ok) {
            ++stats.bSplineTracks;
            clip.wasResampledFromBSpline = true;
            clip.bSplineSampleRate = kBSplineSampleRateHz;
            clip.durationSeconds = std::max(clip.durationSeconds, duration);
        } else {
            outGenuineFailure = !empty;
        }
        return ok;
    }
    if (auto* classic = dynamic_cast<Niflib::NiTransformInterpolator*>(interp)) {
        bool usedEuler = false;
        bool staticPoseOnly = false;
        const bool ok =
            ExtractClassicTrack(classic, track, warnings, context, usedEuler, staticPoseOnly);
        if (usedEuler) {
            ++stats.tracksWithUnsupportedEulerRotation;
        }
        if (staticPoseOnly) {
            ++stats.tracksStaticPoseOnly;
        }
        if (ok) {
            ++stats.classicTracks;
            for (const QuatKey& k : track.rotations) {
                clip.durationSeconds = std::max(clip.durationSeconds, k.time);
            }
            for (const VectorKey& k : track.translations) {
                clip.durationSeconds = std::max(clip.durationSeconds, k.time);
            }
            for (const VectorKey& k : track.scales) {
                clip.durationSeconds = std::max(clip.durationSeconds, k.time);
            }
        } else {
            outGenuineFailure = !staticPoseOnly;
        }
        return ok;
    }
    // Some other interpolator family: a NiFloatInterpolator/NiPoint3Interpolator
    // riding the same NiControllerSequence for a material/UV/visibility
    // controller (measured: this is the dominant case, not a rare fallback --
    // see PHASE4_FINDINGS). Out of scope for this phase, which extracts bone
    // transforms only; counted separately from a genuine extraction failure so
    // the corpus summary does not read as though something went wrong.
    ++stats.tracksSkippedOutOfScope;
    if (warnings != nullptr) {
        warnings->push_back(context + ": interpolator type not handled by Phase 4 (not a " +
                            "bone-transform track)");
    }
    return false;
}

} // namespace

std::vector<SceneNode> BuildNodeHierarchy(Niflib::NiAVObject* root) {
    std::vector<SceneNode> nodes;
    if (root == nullptr) {
        return nodes;
    }

    // Same shape as SkeletonExtractor's CollectBones -- see SceneNode -- but
    // walking NiAVObject rather than NiNode specifically, since a
    // skeleton-less file's animated target can be any node in the tree, not
    // only ones with children (a leaf NiTriShape's own NiNode-typed parent
    // is what SkeletonExtractor would require; this does not need that
    // restriction because it is never asked to carry child bones' weights).
    struct Item {
        Niflib::NiAVObject* obj;
        int parentIndex;
    };
    std::vector<Item> stack{{root, -1}};
    std::set<Niflib::NiObject*> visited;

    while (!stack.empty()) {
        Item item = stack.back();
        stack.pop_back();
        if (item.obj == nullptr || !visited.insert(item.obj).second) {
            continue;
        }

        const int index = static_cast<int>(nodes.size());
        SceneNode n;
        n.name = item.obj->GetName();
        n.parentIndex = item.parentIndex;
        ToColumnMajorLocal(item.obj->GetLocalTransform(), n.localMatrix);
        nodes.push_back(std::move(n));

        if (auto* node = dynamic_cast<Niflib::NiNode*>(item.obj)) {
            for (const Niflib::Ref<Niflib::NiAVObject>& child : node->GetChildren()) {
                stack.push_back({static_cast<Niflib::NiAVObject*>(child), index});
            }
        }
    }

    return nodes;
}

AnimationExtractor::AnimationExtractor(std::vector<std::string>* warnings) : warnings_(warnings) {}

void AnimationExtractor::ExtractEmbedded(Niflib::NiAVObject* root, const SkeletonData* skeleton,
                                         const std::vector<SceneNode>* nodes, SceneData& scene,
                                         AnimationStats& stats) {
    if (root == nullptr) {
        return;
    }

    // Every NiAVObject in the subtree may carry its own controller chain, so
    // the whole tree is walked rather than assuming controllers only sit on
    // the skeleton root -- M491's NiMultiTargetTransformController does, but
    // nothing in the format guarantees that in general.
    std::vector<Niflib::NiAVObject*> stack{root};
    std::set<Niflib::NiObject*> visited;
    AnimationClip clip;
    clip.originFile = "embedded";
    bool clipHasTracks = false;

    while (!stack.empty()) {
        Niflib::NiAVObject* obj = stack.back();
        stack.pop_back();
        if (obj == nullptr || !visited.insert(obj).second) {
            continue;
        }

        for (const Niflib::Ref<Niflib::NiTimeController>& ctrl : obj->GetControllers()) {
            auto* xform = dynamic_cast<Niflib::NiTransformController*>(
                static_cast<Niflib::NiTimeController*>(ctrl));
            if (xform == nullptr) {
                continue; // NiTextureTransformController, NiAlphaController, ...: out of scope.
            }
            Niflib::NiInterpolator* interp = xform->GetInterpolator();
            if (interp == nullptr) {
                continue; // Registered but inert -- see AnimationExtractor.hpp.
            }

            AnimationTrack track;
            track.boneName = obj->GetName();
            track.boneIndex = ResolveTargetIndex(skeleton, nodes, track.boneName);

            ++stats.tracksTotal;
            bool genuineFailure = false;
            if (!ExtractInterpolator(interp, track, clip, stats, warnings_,
                                     "embedded track on '" + track.boneName + "'",
                                     genuineFailure)) {
                if (genuineFailure) {
                    ++stats.tracksFailed;
                    if (warnings_ != nullptr) {
                        warnings_->push_back("embedded track on '" + track.boneName +
                                            "': recognized transform interpolator produced no "
                                            "usable keyframes");
                    }
                }
                continue;
            }
            if (track.boneIndex >= 0) {
                ++stats.tracksResolved;
            } else {
                ++stats.tracksOrphaned;
                if (warnings_ != nullptr) {
                    warnings_->push_back("embedded animation track targets '" + track.boneName +
                                        "', not found in the file's " +
                                        (skeleton != nullptr ? "skeleton" : "node hierarchy"));
                }
            }
            if (ScaleActuallyAnimates(track.scales)) {
                ++stats.tracksWithScaleAnimated;
            }
            clip.tracks.push_back(std::move(track));
            clipHasTracks = true;
        }

        if (auto* node = dynamic_cast<Niflib::NiNode*>(obj)) {
            for (const Niflib::Ref<Niflib::NiAVObject>& child : node->GetChildren()) {
                stack.push_back(static_cast<Niflib::NiAVObject*>(child));
            }
        }
    }

    if (clipHasTracks) {
        clip.name = "embedded";
        for (const AnimationTrack& t : clip.tracks) {
            stats.totalKeyframesWritten += static_cast<long long>(
                t.translations.size() + t.rotations.size() + t.scales.size());
        }
        scene.animations.push_back(std::move(clip));
        ++stats.embeddedClips;
    }
}

bool AnimationExtractor::ExtractFromKf(const std::string& kfPath, const SkeletonData* skeleton,
                                       const std::vector<SceneNode>* nodes, SceneData& scene,
                                       AnimationStats& stats) {
    std::string bytes;
    if (!ReadWholeFile(kfPath, bytes)) {
        return false;
    }

    // Same load path as ExtractScene for .nif: normalize the header, refuse a
    // block type niflib cannot construct (it crashes rather than throwing --
    // see HeaderNormalizer.cpp), then parse. A .kf is structurally a NIF file,
    // so nothing here is .kf-specific.
    HeaderNormalizationReport header;
    NormalizeNifHeader(bytes, header);
    const std::string unsupported = FindUnsupportedBlockType(bytes);
    if (!unsupported.empty()) {
        if (warnings_ != nullptr) {
            warnings_->push_back(kfPath + ": unsupported block type '" + unsupported +
                                "'; animations from this .kf skipped");
        }
        return false;
    }

    std::vector<NiObjectRef> blocks;
    try {
        std::istringstream stream(bytes, std::ios::binary);
        blocks = Niflib::ReadNifList(stream, nullptr);
    } catch (const std::exception& e) {
        if (warnings_ != nullptr) {
            warnings_->push_back(kfPath + ": niflib parse failed: " + std::string(e.what()));
        }
        return false;
    } catch (...) {
        if (warnings_ != nullptr) {
            warnings_->push_back(kfPath + ": niflib parse failed: unknown exception");
        }
        return false;
    }

    const std::string kfFileName = [&]() {
        const size_t slash = kfPath.find_last_of("/\\");
        return slash == std::string::npos ? kfPath : kfPath.substr(slash + 1);
    }();

    for (const NiObjectRef& b : blocks) {
        auto* seq = dynamic_cast<Niflib::NiControllerSequence*>(static_cast<NiObject*>(b));
        if (seq == nullptr) {
            continue;
        }

        AnimationClip clip;
        clip.originFile = kfFileName;
        clip.name = seq->GetName();
        clip.durationSeconds = std::max(0.0f, seq->GetStopTime() - seq->GetStartTime());

        for (const Niflib::ControllerLink& link : seq->GetControllerData()) {
            if (link.interpolator == NULL) {
                continue; // A link with no interpolator carries nothing to extract.
            }

            AnimationTrack track;
            track.boneName = link.nodeName;
            track.boneIndex = ResolveTargetIndex(skeleton, nodes, track.boneName);

            ++stats.tracksTotal;
            const std::string context =
                kfFileName + " '" + clip.name + "': track on '" + track.boneName + "'";
            bool genuineFailure = false;
            if (!ExtractInterpolator(static_cast<Niflib::NiInterpolator*>(link.interpolator), track,
                                     clip, stats, warnings_, context, genuineFailure)) {
                if (genuineFailure) {
                    ++stats.tracksFailed;
                    if (warnings_ != nullptr) {
                        warnings_->push_back(context +
                                            ": recognized transform interpolator produced no "
                                            "usable keyframes");
                    }
                }
                continue;
            }
            if (track.boneIndex >= 0) {
                ++stats.tracksResolved;
            } else {
                ++stats.tracksOrphaned;
                if (warnings_ != nullptr) {
                    warnings_->push_back(context + ": target not found in the model's " +
                                        (skeleton != nullptr ? "skeleton" : "node hierarchy") +
                                        "; track kept orphaned (boneIndex = -1)");
                }
            }
            if (ScaleActuallyAnimates(track.scales)) {
                ++stats.tracksWithScaleAnimated;
            }
            clip.tracks.push_back(std::move(track));
        }

        if (!clip.tracks.empty()) {
            for (const AnimationTrack& t : clip.tracks) {
                stats.totalKeyframesWritten += static_cast<long long>(
                    t.translations.size() + t.rotations.size() + t.scales.size());
            }
            scene.animations.push_back(std::move(clip));
            ++stats.kfClips;
        }
    }

    return true;
}

} // namespace gfnif
