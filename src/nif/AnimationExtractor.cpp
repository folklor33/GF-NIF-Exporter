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
    // quaternion keys. Rare in this corpus (measured, see PHASE4_FINDINGS);
    // rather than reimplement Euler-to-quaternion composition from three
    // independently-timed channels, such a track is reported and its rotation
    // channel left empty -- translation/scale still extract normally.
    if (data->GetRotateType() == Niflib::XYZ_ROTATION_KEY) {
        outUsedEulerRotation = true;
        if (warnings != nullptr) {
            warnings->push_back(context + ": rotation uses XYZ_ROTATION_KEY (separate Euler " +
                                "channels), not supported; rotation track left empty");
        }
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

AnimationExtractor::AnimationExtractor(std::vector<std::string>* warnings) : warnings_(warnings) {}

void AnimationExtractor::ExtractEmbedded(Niflib::NiAVObject* root, const SkeletonData* skeleton,
                                         SceneData& scene, AnimationStats& stats) {
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
            track.boneIndex = FindBoneByName(skeleton, track.boneName);

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
                                        "', not found in the file's skeleton");
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
                                       SceneData& scene, AnimationStats& stats) {
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
            track.boneIndex = FindBoneByName(skeleton, track.boneName);

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
                    warnings_->push_back(context + ": bone not found in the model's skeleton; " +
                                        "track kept orphaned (boneIndex = -1)");
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
