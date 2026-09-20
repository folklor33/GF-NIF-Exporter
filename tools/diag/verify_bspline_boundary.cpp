// One-off diagnostic (Phase 4): validates niflib's own B-spline evaluation
// (SampleQuatRotateKeys / SampleTranslateKeys), independent of the sampling
// rate this phase chose, by checking a property that must hold for ANY
// correct clamped B-spline evaluator: sampling at the interpolator's own
// start/stop time must reproduce its first/last control point, since a
// clamped (open) uniform B-spline is defined to pass through its endpoint
// control points exactly.
//
// This is the closest available substitute for Phase 3's GetSkinDeformation
// cross-check: niflib exposes no independent second implementation of B-spline
// evaluation to compare against (see PHASE4_FINDINGS for why), so instead this
// checks niflib's own evaluator against a mathematical invariant of the curve
// family it claims to implement. A discrepancy here would mean either the
// degree/parameterization assumption (cubic, clamped) is wrong, or the
// dequantization is being misapplied.

#include "niflib.h"
#include "nif_math.h"
#include "Key.h"
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

// Control points are stored as raw quaternion coordinates, not necessarily
// unit length (they are dequantized linearly from int16 storage, with no
// constraint that the result lands back on the unit sphere) -- normalizing
// here isolates a genuine curve-evaluation discrepancy from what would
// otherwise look like one but is really just comparing two differently-scaled
// quaternions.
float QuatAngleDeg(Quaternion a, Quaternion b) {
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

float Vec3Dist(const Vector3& a, const Vector3& b) {
    float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: verify_bspline_boundary <input-dir>\n";
        return 1;
    }
    fs::path root = argv[1];

    int tracksChecked = 0;
    double maxRotErrDeg = 0.0, sumRotErrDeg = 0.0;
    double maxTransErr = 0.0, sumTransErr = 0.0;
    int rotSamples = 0, transSamples = 0;

    for (auto it = fs::recursive_directory_iterator(root); it != fs::recursive_directory_iterator(); ++it) {
        if (!it->is_regular_file() || it->path().extension() != ".kf") continue;

        std::string bytes;
        if (!gfnif::ReadWholeFile(it->path().string(), bytes)) continue;
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

        for (const NiObjectRef& b : blocks) {
            auto* seq = dynamic_cast<NiControllerSequence*>(static_cast<NiObject*>(b));
            if (seq == nullptr) continue;

            for (const ControllerLink& link : seq->GetControllerData()) {
                if (link.interpolator == NULL) continue;
                auto* bspline = dynamic_cast<NiBSplineTransformInterpolator*>(
                    static_cast<NiInterpolator*>(link.interpolator));
                if (bspline == nullptr) continue;

                const float start = bspline->GetStartTime();
                const float stop = bspline->GetStopTime();
                if (!(stop > start)) continue;

                ++tracksChecked;

                // Endpoint control points, straight from the (dequantized)
                // control data -- the ground truth the sampled curve must hit
                // exactly at t = start and t = stop.
                std::vector<Quaternion> quatControl = bspline->GetQuatRotateControlData();
                std::vector<Vector3> transControl = bspline->GetTranslateControlData();

                // Sample densely enough that the first/last output sample
                // lands extremely close to start/stop (within one sub-step).
                const int npoints = 200;
                auto sampledQuat = bspline->SampleQuatRotateKeys(npoints, kBSplineDegree);
                auto sampledTrans = bspline->SampleTranslateKeys(npoints, kBSplineDegree);

                if (!quatControl.empty() && !sampledQuat.empty()) {
                    float err = QuatAngleDeg(quatControl.front(), sampledQuat.front().data);
                    sumRotErrDeg += err;
                    maxRotErrDeg = std::max(maxRotErrDeg, (double)err);
                    ++rotSamples;
                    err = QuatAngleDeg(quatControl.back(), sampledQuat.back().data);
                    sumRotErrDeg += err;
                    maxRotErrDeg = std::max(maxRotErrDeg, (double)err);
                    ++rotSamples;
                }
                if (!transControl.empty() && !sampledTrans.empty()) {
                    float err = Vec3Dist(transControl.front(), sampledTrans.front().data);
                    sumTransErr += err;
                    maxTransErr = std::max(maxTransErr, (double)err);
                    ++transSamples;
                    err = Vec3Dist(transControl.back(), sampledTrans.back().data);
                    sumTransErr += err;
                    maxTransErr = std::max(maxTransErr, (double)err);
                    ++transSamples;
                }
            }
        }
    }

    std::cout << "B-spline tracks checked: " << tracksChecked << "\n";
    std::cout << "Rotation endpoint error (deg): mean "
              << (rotSamples ? sumRotErrDeg / rotSamples : 0.0) << ", max " << maxRotErrDeg
              << " (" << rotSamples << " endpoint samples)\n";
    std::cout << "Translation endpoint error (units): mean "
              << (transSamples ? sumTransErr / transSamples : 0.0) << ", max " << maxTransErr
              << " (" << transSamples << " endpoint samples)\n";
    return 0;
}
