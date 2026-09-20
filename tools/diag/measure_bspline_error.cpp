// One-off diagnostic (Phase 4): measures B-spline resampling error at several
// candidate sample rates against a very fine reference, and the resulting
// keyframe count. Not part of the shipped exporter; not wired into the main
// CMakeLists -- built ad hoc, see the invocation this phase's findings doc
// records.
//
// Reference: for each NiBSplineCompTransformInterpolator track found under a
// given corpus root, sample rotation+translation at kReferenceHz (assumed to
// be close enough to the true continuous curve to stand in for it), and at
// each candidate rate. Error is measured only at the candidate's own sample
// times by re-sampling the reference near that time (nearest reference sample
// within half a reference step) -- this approximates the error a consumer
// doing linear interpolation between the candidate's keyframes would see at
// the points where it matters most (the keyframes themselves would be zero
// error under linear interpolation at their own times, so error is instead
// measured at the MIDPOINTS between consecutive candidate keyframes, which is
// where linear interpolation deviates most from the true curve).

#include "niflib.h"
#include "nif_math.h"
#include "obj/NiAVObject.h"
#include "obj/NiBSplineCompTransformInterpolator.h"
#include "obj/NiBSplineTransformInterpolator.h"
#include "obj/NiControllerSequence.h"
#include "obj/NiNode.h"

#include "nif/HeaderNormalizer.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;
using namespace Niflib;

namespace {

constexpr int kBSplineDegree = 3;
constexpr float kReferenceHz = 240.0f;

struct ErrorStats {
    double sumRotDeg = 0.0;
    double maxRotDeg = 0.0;
    double sumTransUnits = 0.0;
    double maxTransUnits = 0.0;
    size_t samples = 0;
    long long totalKeyframes = 0;
    // Histogram buckets so a handful of outliers (near-antipodal quaternion
    // discontinuities at the reference/candidate sampling boundary) can be
    // told apart from the typical case, rather than just seeing a scary max.
    size_t rotOver1Deg = 0, rotOver5Deg = 0, rotOver30Deg = 0, rotOver90Deg = 0;
};

// Both raw B-spline samples can be off the unit sphere (measured elsewhere in
// this phase: up to 0.77 magnitude error) -- normalizing here matches what
// the real AnimationExtractor does before writing a quaternion out (see
// AnimationExtractor.cpp's CopyQuat), so this measurement reflects the error a
// consumer actually sees, not an artifact of comparing two denormalized
// quaternions.
float AngleBetweenDeg(Quaternion a, Quaternion b) {
    auto normalize = [](Quaternion& q) {
        float n = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
        if (n > 1e-9f) { q.w /= n; q.x /= n; q.y /= n; q.z /= n; }
    };
    normalize(a);
    normalize(b);
    float dot = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
    dot = std::max(-1.0f, std::min(1.0f, std::fabs(dot)));
    return 2.0f * std::acos(dot) * 180.0f / 3.14159265f;
}

float Dist(const Vector3& a, const Vector3& b) {
    float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// Binary-search interpolation between candidate keyframes at time t. Keys are
// sorted by time (niflib emits Sample*Keys in increasing time order), so
// std::upper_bound locates the bracketing pair in O(log n) rather than the
// O(n) linear scan this replaced -- at reference-rate sample counts (up to
// tens of thousands per track) the linear version made the full corpus
// measurement impractically slow.
Quaternion InterpQuat(const std::vector<Key<Quaternion>>& keys, float t) {
    if (keys.empty()) return Quaternion(1, 0, 0, 0);
    if (t <= keys.front().time) return keys.front().data;
    if (t >= keys.back().time) return keys.back().data;
    auto it = std::upper_bound(keys.begin(), keys.end(), t,
                               [](float v, const Key<Quaternion>& k) { return v < k.time; });
    const Key<Quaternion>& k1 = *it;
    const Key<Quaternion>& k0 = *(it - 1);
    float span = k1.time - k0.time;
    float a = span > 1e-9f ? (t - k0.time) / span : 0.0f;
    const Quaternion& q0 = k0.data;
    Quaternion q1 = k1.data;
    float dot = q0.w * q1.w + q0.x * q1.x + q0.y * q1.y + q0.z * q1.z;
    if (dot < 0) { q1.w = -q1.w; q1.x = -q1.x; q1.y = -q1.y; q1.z = -q1.z; }
    Quaternion r(q0.w + (q1.w - q0.w) * a, q0.x + (q1.x - q0.x) * a,
                q0.y + (q1.y - q0.y) * a, q0.z + (q1.z - q0.z) * a);
    float n = std::sqrt(r.w * r.w + r.x * r.x + r.y * r.y + r.z * r.z);
    if (n > 1e-9f) { r.w /= n; r.x /= n; r.y /= n; r.z /= n; }
    return r;
}

Vector3 InterpVec3(const std::vector<Key<Vector3>>& keys, float t) {
    if (keys.empty()) return Vector3(0, 0, 0);
    if (t <= keys.front().time) return keys.front().data;
    if (t >= keys.back().time) return keys.back().data;
    auto it = std::upper_bound(keys.begin(), keys.end(), t,
                               [](float v, const Key<Vector3>& k) { return v < k.time; });
    const Key<Vector3>& k1 = *it;
    const Key<Vector3>& k0 = *(it - 1);
    float span = k1.time - k0.time;
    float a = span > 1e-9f ? (t - k0.time) / span : 0.0f;
    return Vector3(k0.data.x + (k1.data.x - k0.data.x) * a, k0.data.y + (k1.data.y - k0.data.y) * a,
                  k0.data.z + (k1.data.z - k0.data.z) * a);
}

void MeasureTrack(NiBSplineTransformInterpolator* interp, float candidateHz,
                  ErrorStats& stats) {
    const float start = interp->GetStartTime();
    const float stop = interp->GetStopTime();
    const float duration = stop - start;
    if (!(duration > 0.0f)) return;

    const int refN = std::max(2, static_cast<int>(std::ceil(duration * kReferenceHz)) + 1);
    const int candN = std::max(2, static_cast<int>(std::ceil(duration * candidateHz)) + 1);

    auto refQuat = interp->SampleQuatRotateKeys(refN, kBSplineDegree);
    auto refTrans = interp->SampleTranslateKeys(refN, kBSplineDegree);
    auto candQuat = interp->SampleQuatRotateKeys(candN, kBSplineDegree);
    auto candTrans = interp->SampleTranslateKeys(candN, kBSplineDegree);

    stats.totalKeyframes += static_cast<long long>(candQuat.size() + candTrans.size());

    if (!refQuat.empty() && !candQuat.empty()) {
        for (const auto& rk : refQuat) {
            Quaternion interpolated = InterpQuat(candQuat, rk.time);
            float err = AngleBetweenDeg(rk.data, interpolated);
            stats.sumRotDeg += err;
            stats.maxRotDeg = std::max(stats.maxRotDeg, (double)err);
            ++stats.samples;
            if (err > 1.0) ++stats.rotOver1Deg;
            if (err > 5.0) ++stats.rotOver5Deg;
            if (err > 30.0) ++stats.rotOver30Deg;
            if (err > 90.0) ++stats.rotOver90Deg;
        }
    }
    if (!refTrans.empty() && !candTrans.empty()) {
        for (const auto& rk : refTrans) {
            Vector3 interpolated = InterpVec3(candTrans, rk.time);
            float err = Dist(rk.data, interpolated);
            stats.sumTransUnits += err;
            stats.maxTransUnits = std::max(stats.maxTransUnits, (double)err);
        }
    }
}

void WalkSequence(NiControllerSequence* seq, float candidateHz, ErrorStats& stats) {
    for (const ControllerLink& link : seq->GetControllerData()) {
        if (link.interpolator == NULL) continue;
        auto* bspline =
            dynamic_cast<NiBSplineTransformInterpolator*>(static_cast<NiInterpolator*>(link.interpolator));
        if (bspline == nullptr) continue;
        MeasureTrack(bspline, candidateHz, stats);
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: measure_bspline_error <input-dir>\n";
        return 1;
    }
    fs::path root = argv[1];

    std::vector<fs::path> kfFiles;
    for (auto it = fs::recursive_directory_iterator(root); it != fs::recursive_directory_iterator(); ++it) {
        if (it->is_regular_file() && it->path().extension() == ".kf") {
            kfFiles.push_back(it->path());
        }
    }
    std::sort(kfFiles.begin(), kfFiles.end());

    const std::vector<float> rates = {15.0f, 24.0f, 30.0f, 60.0f};
    std::vector<ErrorStats> statsPerRate(rates.size());

    int filesProcessed = 0;
    for (const fs::path& f : kfFiles) {
        std::cerr << "processing " << f.string() << std::endl;
        std::string bytes;
        if (!gfnif::ReadWholeFile(f.string(), bytes)) continue;
        gfnif::HeaderNormalizationReport report;
        gfnif::NormalizeNifHeader(bytes, report);
        if (!gfnif::FindUnsupportedBlockType(bytes).empty()) continue;

        std::vector<NiObjectRef> blocks;
        try {
            std::istringstream stream(bytes, std::ios::binary);
            blocks = ReadNifList(stream, nullptr);
        } catch (...) {
            continue;
        }

        bool hadBSpline = false;
        for (const NiObjectRef& b : blocks) {
            auto* seq = dynamic_cast<NiControllerSequence*>(static_cast<NiObject*>(b));
            if (seq == nullptr) continue;
            for (size_t i = 0; i < rates.size(); ++i) {
                WalkSequence(seq, rates[i], statsPerRate[i]);
            }
            hadBSpline = true;
        }
        if (hadBSpline) ++filesProcessed;

        if (filesProcessed % 100 == 0 && filesProcessed > 0) {
            std::cerr << "..." << filesProcessed << " files\n";
        }
    }

    std::cout << "Files with NiControllerSequence processed: " << filesProcessed << "\n\n";
    std::cout << "Rate(Hz)  Samples   MeanRotErr(deg)  MaxRotErr(deg)  MeanTransErr  MaxTransErr  TotalKeyframes\n";
    for (size_t i = 0; i < rates.size(); ++i) {
        const ErrorStats& s = statsPerRate[i];
        double meanRot = s.samples > 0 ? s.sumRotDeg / s.samples : 0.0;
        double meanTrans = s.samples > 0 ? s.sumTransUnits / s.samples : 0.0;
        std::cout << rates[i] << "\t  " << s.samples << "\t  " << meanRot << "\t  " << s.maxRotDeg
                  << "\t  " << meanTrans << "\t  " << s.maxTransUnits << "\t  " << s.totalKeyframes
                  << "\n";
    }
    std::cout << "\nRotation error distribution (share of reference samples exceeding threshold):\n";
    std::cout << "Rate(Hz)  >1deg  >5deg  >30deg  >90deg\n";
    for (size_t i = 0; i < rates.size(); ++i) {
        const ErrorStats& s = statsPerRate[i];
        auto pct = [&](size_t n) { return s.samples > 0 ? 100.0 * n / s.samples : 0.0; };
        std::cout << rates[i] << "\t  " << pct(s.rotOver1Deg) << "%\t  " << pct(s.rotOver5Deg)
                  << "%\t  " << pct(s.rotOver30Deg) << "%\t  " << pct(s.rotOver90Deg) << "%\n";
    }
    return 0;
}
