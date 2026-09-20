// One-off diagnostic (Phase 4 XYZ_ROTATION_KEY correctif, Piste 2): empirical
// discriminant for the Euler composition order used by XYZ_ROTATION_KEY.
//
// Method (see the correctif brief): the overwhelming majority of animation
// clips start at, or very near, the rest pose of the bone they target. For
// every XYZ_ROTATION_KEY track, this tool:
//   1. Samples the X/Y/Z rotation channels at t=0 (nearest-key / first-key,
//      since channels are independently timed and t=0 need not be an exact
//      key on every axis).
//   2. Composes those three angles into a rotation matrix under all 6
//      possible axis orders (and, for the winning order, checks handedness
//      is not an issue since a proper rotation matrix has no sign ambiguity
//      to test separately).
//   3. Compares each candidate's rotation against the target bone's own
//      local bind-pose rotation (NiNode::GetLocalTransform(), the same local
//      frame AnimationTrack keys apply against -- see PHASE4_FINDINGS §8).
//   4. Accumulates mean/median angular error per candidate, corpus-wide.
//
// The candidate that converges to near-zero error corpus-wide (while the
// other 5 do not) identifies the composition order empirically, independent
// of the blender_niftools_addon source-reading finding (Piste 1) -- if both
// agree, the result is solid.
//
// Not part of the shipped exporter; not wired into CMakeLists.txt.

#include "niflib.h"
#include "nif_math.h"
#include "obj/NiAVObject.h"
#include "obj/NiControllerSequence.h"
#include "obj/NiNode.h"
#include "obj/NiTransformData.h"
#include "obj/NiTransformInterpolator.h"

#include "nif/HeaderNormalizer.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;
using namespace Niflib;

namespace {

// --- minimal 3x3 matrix / quaternion helpers, independent of niflib's own
// (niflib exposes no axis-rotation-matrix constructor -- confirmed absent
// from nif_math.h) ---

struct Mat3 {
    // row-major: m[row][col]
    double m[3][3];
};

Mat3 Identity3() {
    Mat3 r{};
    r.m[0][0] = r.m[1][1] = r.m[2][2] = 1.0;
    return r;
}

Mat3 RotX(double a) {
    double c = std::cos(a), s = std::sin(a);
    Mat3 r = Identity3();
    r.m[1][1] = c; r.m[1][2] = -s;
    r.m[2][1] = s; r.m[2][2] = c;
    return r;
}
Mat3 RotY(double a) {
    double c = std::cos(a), s = std::sin(a);
    Mat3 r = Identity3();
    r.m[0][0] = c; r.m[0][2] = s;
    r.m[2][0] = -s; r.m[2][2] = c;
    return r;
}
Mat3 RotZ(double a) {
    double c = std::cos(a), s = std::sin(a);
    Mat3 r = Identity3();
    r.m[0][0] = c; r.m[0][1] = -s;
    r.m[1][0] = s; r.m[1][1] = c;
    return r;
}

Mat3 Mul(const Mat3& a, const Mat3& b) {
    Mat3 r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            double s = 0;
            for (int k = 0; k < 3; ++k) s += a.m[i][k] * b.m[k][j];
            r.m[i][j] = s;
        }
    return r;
}

// Quaternion (w,x,y,z) from a rotation matrix, standard Shepperd's method.
struct Quat { double w, x, y, z; };

Quat MatToQuat(const Mat3& m) {
    double tr = m.m[0][0] + m.m[1][1] + m.m[2][2];
    Quat q;
    if (tr > 0) {
        double s = std::sqrt(tr + 1.0) * 2;
        q.w = 0.25 * s;
        q.x = (m.m[2][1] - m.m[1][2]) / s;
        q.y = (m.m[0][2] - m.m[2][0]) / s;
        q.z = (m.m[1][0] - m.m[0][1]) / s;
    } else if (m.m[0][0] > m.m[1][1] && m.m[0][0] > m.m[2][2]) {
        double s = std::sqrt(1.0 + m.m[0][0] - m.m[1][1] - m.m[2][2]) * 2;
        q.w = (m.m[2][1] - m.m[1][2]) / s;
        q.x = 0.25 * s;
        q.y = (m.m[0][1] + m.m[1][0]) / s;
        q.z = (m.m[0][2] + m.m[2][0]) / s;
    } else if (m.m[1][1] > m.m[2][2]) {
        double s = std::sqrt(1.0 + m.m[1][1] - m.m[0][0] - m.m[2][2]) * 2;
        q.w = (m.m[0][2] - m.m[2][0]) / s;
        q.x = (m.m[0][1] + m.m[1][0]) / s;
        q.y = 0.25 * s;
        q.z = (m.m[1][2] + m.m[2][1]) / s;
    } else {
        double s = std::sqrt(1.0 + m.m[2][2] - m.m[0][0] - m.m[1][1]) * 2;
        q.w = (m.m[1][0] - m.m[0][1]) / s;
        q.x = (m.m[0][2] + m.m[2][0]) / s;
        q.y = (m.m[1][2] + m.m[2][1]) / s;
        q.z = 0.25 * s;
    }
    double n = std::sqrt(q.w*q.w + q.x*q.x + q.y*q.y + q.z*q.z);
    if (n > 1e-12) { q.w/=n; q.x/=n; q.y/=n; q.z/=n; }
    return q;
}

double QuatAngleDiffDeg(const Quat& a, const Quat& b) {
    double dot = a.w*b.w + a.x*b.x + a.y*b.y + a.z*b.z;
    dot = std::min(1.0, std::max(-1.0, std::fabs(dot)));
    return 2.0 * std::acos(dot) * 180.0 / 3.14159265358979323846;
}

enum Order { XYZ, XZY, YXZ, YZX, ZXY, ZYX, kNumOrders };
const char* kOrderNames[kNumOrders] = {"XYZ", "XZY", "YXZ", "YZX", "ZXY", "ZYX"};

// Composes a matrix for the given order: name "ABC" means apply A first, then
// B, then C, i.e. matrix = Rc * Rb * Ra (standard intrinsic/extrinsic
// equivalence for fixed-axis composition).
Mat3 Compose(Order order, double x, double y, double z) {
    Mat3 rx = RotX(x), ry = RotY(y), rz = RotZ(z);
    switch (order) {
        case XYZ: return Mul(rz, Mul(ry, rx));
        case XZY: return Mul(ry, Mul(rz, rx));
        case YXZ: return Mul(rz, Mul(rx, ry));
        case YZX: return Mul(rx, Mul(rz, ry));
        case ZXY: return Mul(ry, Mul(rx, rz));
        case ZYX: return Mul(rx, Mul(ry, rz));
        default: return Identity3();
    }
}

bool ResolveCompanionKf(const fs::path& nifPath, fs::path& outKfPath) {
    const fs::path parent = nifPath.parent_path();
    if (parent.filename() != "model") return false;
    const fs::path candidate = parent.parent_path() / "animation" / fs::path(nifPath.stem()).concat(".kf");
    std::error_code ec;
    if (!fs::exists(candidate, ec) || ec) return false;
    outKfPath = candidate;
    return true;
}

bool LoadBlocks(const fs::path& path, std::vector<NiObjectRef>& blocks) {
    std::string bytes;
    if (!gfnif::ReadWholeFile(path.string(), bytes)) return false;
    gfnif::HeaderNormalizationReport rep;
    gfnif::NormalizeNifHeader(bytes, rep);
    if (!gfnif::FindUnsupportedBlockType(bytes).empty()) return false;
    try {
        std::istringstream stream(bytes, std::ios::binary);
        blocks = ReadNifList(stream, nullptr);
    } catch (...) {
        return false;
    }
    return !blocks.empty();
}

// First (or nearest-to-zero-time) key value of a Key<float> vector.
float SampleAtZero(const std::vector<Key<float>>& keys) {
    if (keys.empty()) return 0.0f;
    // Keys are time-sorted in the source; take the one closest to t=0 (usually
    // keys[0], but be defensive).
    float best = keys[0].data;
    float bestDt = std::fabs(keys[0].time);
    for (const auto& k : keys) {
        float dt = std::fabs(k.time);
        if (dt < bestDt) { bestDt = dt; best = k.data; }
    }
    return best;
}

// Collects every NiNode in the .nif, keyed by name, so a track's target bone
// name can be resolved to its own local bind rotation (NiNode::GetLocalTransform).
void CollectNodesByName(NiAVObject* root, std::map<std::string, NiNode*>& out) {
    std::vector<NiAVObject*> stack{root};
    std::set<NiObject*> visited;
    while (!stack.empty()) {
        NiAVObject* obj = stack.back();
        stack.pop_back();
        if (obj == nullptr || !visited.insert(obj).second) continue;
        if (auto* node = dynamic_cast<NiNode*>(obj)) {
            out[node->GetName()] = node;
            for (const Ref<NiAVObject>& child : node->GetChildren()) {
                stack.push_back(static_cast<NiAVObject*>(child));
            }
        }
    }
}

Mat3 ToMat3(const Matrix33& m) {
    Mat3 r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            r.m[i][j] = m[i][j];
    return r;
}

struct CandidateStats {
    double sumErrDeg = 0.0;
    double sumSqErrDeg = 0.0;
    std::vector<double> errs; // for median
    long long count = 0;

    void Add(double errDeg) {
        sumErrDeg += errDeg;
        sumSqErrDeg += errDeg * errDeg;
        errs.push_back(errDeg);
        ++count;
    }
    double Mean() const { return count ? sumErrDeg / count : 0.0; }
    double Median() {
        if (errs.empty()) return 0.0;
        std::sort(errs.begin(), errs.end());
        return errs[errs.size() / 2];
    }
    // Share of samples under a threshold -- reveals a bimodal "exact match /
    // unrelated pose" split that a single mean/median would hide.
    double ShareUnder(double thresholdDeg) {
        if (errs.empty()) return 0.0;
        std::sort(errs.begin(), errs.end());
        size_t n = std::lower_bound(errs.begin(), errs.end(), thresholdDeg) - errs.begin();
        return 100.0 * n / errs.size();
    }
};

// Resamples a Key<float> vector at a fixed set of time stamps via linear
// interpolation (same convention this project settled on for classic tracks
// -- see AnimationExtractor.cpp's ExtractClassicTrack comment).
float LerpAt(const std::vector<Key<float>>& keys, float t) {
    if (keys.empty()) return 0.0f;
    if (t <= keys.front().time) return keys.front().data;
    if (t >= keys.back().time) return keys.back().data;
    for (size_t i = 1; i < keys.size(); ++i) {
        if (t <= keys[i].time) {
            float t0 = keys[i - 1].time, t1 = keys[i].time;
            float a = (t1 > t0) ? (t - t0) / (t1 - t0) : 0.0f;
            return keys[i - 1].data + a * (keys[i].data - keys[i - 1].data);
        }
    }
    return keys.back().data;
}

// Hamilton-product quaternion composition matching AnimationExtractor.cpp's
// ComposeXyzEuler exactly (kept as an independent copy here so this file has
// no dependency on the exporter's own sources) -- used to cross-check that
// implementation's arithmetic against the matrix-based Compose(XYZ, ...) this
// tool already validated corpus-wide, before trusting it in the real
// extractor.
Quat ComposeXyzEulerQuat(double xRad, double yRad, double zRad) {
    const double hx = xRad * 0.5, hy = yRad * 0.5, hz = zRad * 0.5;
    const double qx_w = std::cos(hx), qx_x = std::sin(hx);
    const double qy_w = std::cos(hy), qy_y = std::sin(hy);
    const double qz_w = std::cos(hz), qz_z = std::sin(hz);

    const double qyx_w = qy_w * qx_w;
    const double qyx_x = qy_w * qx_x;
    const double qyx_y = qy_y * qx_w;
    const double qyx_z = -qy_y * qx_x;

    const double w = qz_w * qyx_w - qz_z * qyx_z;
    const double x = qz_w * qyx_x - qz_z * qyx_y;
    const double y = qz_w * qyx_y + qz_z * qyx_x;
    const double z = qz_w * qyx_z + qz_z * qyx_w;
    return Quat{w, x, y, z};
}

// Smoothness discriminant (Piste 2, third signal): a wrong composition order
// should produce visible jumps/reversals between consecutive samples on a
// track that is otherwise smooth; the right order should not. This does NOT
// depend on the "clip starts at rest pose" assumption at all, so it is an
// independent cross-check on the t=0-vs-bind-pose result above.
struct SmoothnessStats {
    double sumStepDeg = 0.0;
    long long steps = 0;
    void Add(double stepDeg) { sumStepDeg += stepDeg; ++steps; }
    double MeanStep() const { return steps ? sumStepDeg / steps : 0.0; }
};

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: measure_xyz_euler_order <input_root>\n";
        return 1;
    }
    fs::path root = argv[1];

    // Self-check: ComposeXyzEulerQuat vs. the matrix-based Compose(XYZ, ...)
    // must agree on random angle triples before this tool's numbers can be
    // trusted to validate the real extractor's implementation.
    {
        double maxSelfCheckErr = 0.0;
        unsigned seed = 12345;
        auto rnd = [&seed]() {
            seed = seed * 1103515245u + 12345u;
            return static_cast<double>((seed >> 8) & 0xFFFFFF) / 0xFFFFFF * 6.28318530718 - 3.14159265359;
        };
        for (int i = 0; i < 10000; ++i) {
            double x = rnd(), y = rnd(), z = rnd();
            Quat viaMatrix = MatToQuat(Compose(XYZ, x, y, z));
            Quat viaHamilton = ComposeXyzEulerQuat(x, y, z);
            maxSelfCheckErr = std::max(maxSelfCheckErr, QuatAngleDiffDeg(viaMatrix, viaHamilton));
        }
        std::cerr << "Self-check: ComposeXyzEulerQuat vs matrix Compose(XYZ,...) max error over "
                     "10000 random angle triples: " << maxSelfCheckErr << " deg\n";
        if (maxSelfCheckErr > 0.01) {
            std::cerr << "SELF-CHECK FAILED -- the two implementations disagree; do not trust "
                         "downstream results until this is fixed.\n";
            return 2;
        }
    }

    std::vector<fs::path> nifFiles;
    for (auto it = fs::recursive_directory_iterator(root); it != fs::recursive_directory_iterator(); ++it) {
        if (!it->is_regular_file()) continue;
        auto p = it->path();
        std::string ext = p.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == ".nif" && p.parent_path().filename() == "model") {
            nifFiles.push_back(p);
        }
    }
    std::sort(nifFiles.begin(), nifFiles.end());
    std::cerr << "Found " << nifFiles.size() << " model .nif files\n";

    CandidateStats stats[kNumOrders];
    CandidateStats statsMultiAxis[kNumOrders];
    SmoothnessStats smoothStats[kNumOrders];
    long long tracksChecked = 0;
    long long tracksMultiAxis = 0;
    long long tracksSmoothChecked = 0;
    long long tracksSkippedNoBone = 0;
    long long filesChecked = 0;

    for (const fs::path& nifPath : nifFiles) {
        fs::path kfPath;
        if (!ResolveCompanionKf(nifPath, kfPath)) continue;

        std::vector<NiObjectRef> nifBlocks;
        if (!LoadBlocks(nifPath, nifBlocks)) continue;
        std::vector<NiObjectRef> kfBlocks;
        if (!LoadBlocks(kfPath, kfBlocks)) continue;

        // Find the .nif's root NiAVObject(s) (unreferenced roots, same
        // convention as MeshExtractor::ExtractScene -- GetRefs() only, walking
        // every non-referenced root rather than assuming a single one) to
        // collect every NiNode name in the file.
        std::set<NiObject*> referenced;
        for (const NiObjectRef& b : nifBlocks) {
            for (const NiObjectRef& r : static_cast<NiObject*>(b)->GetRefs()) {
                if (r != NULL) referenced.insert(static_cast<NiObject*>(r));
            }
        }
        std::map<std::string, NiNode*> nodesByName;
        for (const NiObjectRef& b : nifBlocks) {
            NiObject* obj = static_cast<NiObject*>(b);
            if (referenced.count(obj)) continue;
            if (auto* av = dynamic_cast<NiAVObject*>(obj)) {
                CollectNodesByName(av, nodesByName);
            }
        }
        if (nodesByName.empty()) continue;

        bool fileHadTrack = false;

        for (const NiObjectRef& b : kfBlocks) {
            auto* seq = dynamic_cast<NiControllerSequence*>(static_cast<NiObject*>(b));
            if (seq == nullptr) continue;

            for (const ControllerLink& link : seq->GetControllerData()) {
                if (link.interpolator == NULL) continue;
                auto* classic = dynamic_cast<NiTransformInterpolator*>(static_cast<NiInterpolator*>(link.interpolator));
                if (classic == nullptr) continue;
                NiTransformData* data = classic->GetData();
                if (data == nullptr) continue;
                if (data->GetRotateType() != XYZ_ROTATION_KEY) continue;

                const std::string boneName = link.nodeName;
                auto it = nodesByName.find(boneName);
                if (it == nodesByName.end()) { ++tracksSkippedNoBone; continue; }

                const float x = SampleAtZero(data->GetXRotateKeys());
                const float y = SampleAtZero(data->GetYRotateKeys());
                const float z = SampleAtZero(data->GetZRotateKeys());
                // Skip degenerate all-zero tracks: they trivially match every
                // order (identity), which would dilute the discriminant rather
                // than inform it.
                if (std::fabs(x) < 1e-6f && std::fabs(y) < 1e-6f && std::fabs(z) < 1e-6f) continue;

                const Matrix44 localBind = it->second->GetLocalTransform();
                Matrix33 rot33 = localBind.GetRotation();
                Mat3 bindRot = ToMat3(rot33);
                Quat bindQ = MatToQuat(bindRot);

                // Composition order only matters when at least two axes carry
                // a non-trivial rotation -- a single-axis rotation commutes
                // with the identity regardless of order, and near-parallel
                // small rotations barely distinguish orders either (first-order
                // commutativity). This split isolates the samples that can
                // actually discriminate.
                const double degX = std::fabs(x) * 180.0 / 3.14159265358979323846;
                const double degY = std::fabs(y) * 180.0 / 3.14159265358979323846;
                const double degZ = std::fabs(z) * 180.0 / 3.14159265358979323846;
                int axesAbove5 = (degX > 5.0) + (degY > 5.0) + (degZ > 5.0);
                const bool multiAxis = axesAbove5 >= 2;

                for (int o = 0; o < kNumOrders; ++o) {
                    Mat3 candidate = Compose(static_cast<Order>(o), x, y, z);
                    Quat candQ = MatToQuat(candidate);
                    double err = QuatAngleDiffDeg(candQ, bindQ);
                    stats[o].Add(err);
                    if (multiAxis) statsMultiAxis[o].Add(err);
                }
                ++tracksChecked;
                if (multiAxis) ++tracksMultiAxis;
                fileHadTrack = true;

                // Smoothness discriminant: resample all three axis channels at
                // a uniform 30 Hz rate across the track's own time span (union
                // of the three channels' key ranges), compose each order at
                // every sample, and accumulate the angular step between
                // consecutive samples. Only tracks with at least two channels
                // carrying 3+ keys and a real time span are worth checking --
                // otherwise there's nothing to resample.
                const auto& xk = data->GetXRotateKeys();
                const auto& yk = data->GetYRotateKeys();
                const auto& zk = data->GetZRotateKeys();
                float tMin = 1e30f, tMax = -1e30f;
                int nonTrivialChannels = 0;
                for (const auto* chan : {&xk, &yk, &zk}) {
                    if (chan->size() >= 2) {
                        ++nonTrivialChannels;
                        tMin = std::min(tMin, chan->front().time);
                        tMax = std::max(tMax, chan->back().time);
                    }
                }
                if (nonTrivialChannels >= 2 && tMax > tMin) {
                    const float durationS = tMax - tMin;
                    const int nSamples = std::max(4, static_cast<int>(durationS * 30.0f));
                    Quat prev[kNumOrders];
                    bool havePrev = false;
                    for (int s = 0; s <= nSamples; ++s) {
                        float t = tMin + durationS * (static_cast<float>(s) / nSamples);
                        float sx = LerpAt(xk, t), sy = LerpAt(yk, t), sz = LerpAt(zk, t);
                        for (int o = 0; o < kNumOrders; ++o) {
                            Quat q = MatToQuat(Compose(static_cast<Order>(o), sx, sy, sz));
                            if (havePrev) {
                                smoothStats[o].Add(QuatAngleDiffDeg(q, prev[o]));
                            }
                            prev[o] = q;
                        }
                        havePrev = true;
                    }
                    ++tracksSmoothChecked;
                }
            }
        }
        if (fileHadTrack) ++filesChecked;
    }

    std::cout << "=== XYZ_ROTATION_KEY Euler order empirical discriminant ===\n";
    std::cout << "Files contributing at least one track: " << filesChecked << "\n";
    std::cout << "Tracks checked (t=0 vs bind pose): " << tracksChecked << "\n";
    std::cout << "Tracks skipped (bone name not found in .nif): " << tracksSkippedNoBone << "\n\n";
    std::cout << "All tracks:\n";
    std::cout << "Order   MeanErr(deg)   MedianErr(deg)   %<1deg   %<5deg   %<30deg\n";
    for (int o = 0; o < kNumOrders; ++o) {
        std::cout << kOrderNames[o] << "     " << stats[o].Mean() << "        " << stats[o].Median()
                  << "        " << stats[o].ShareUnder(1.0) << "   " << stats[o].ShareUnder(5.0)
                  << "   " << stats[o].ShareUnder(30.0) << "\n";
    }
    std::cout << "\nMulti-axis tracks only (>=2 axes with |angle|>5deg at t=0), n=" << tracksMultiAxis << ":\n";
    std::cout << "Order   MeanErr(deg)   MedianErr(deg)   %<1deg   %<5deg   %<30deg\n";
    for (int o = 0; o < kNumOrders; ++o) {
        std::cout << kOrderNames[o] << "     " << statsMultiAxis[o].Mean() << "        " << statsMultiAxis[o].Median()
                  << "        " << statsMultiAxis[o].ShareUnder(1.0) << "   " << statsMultiAxis[o].ShareUnder(5.0)
                  << "   " << statsMultiAxis[o].ShareUnder(30.0) << "\n";
    }

    std::cout << "\nSmoothness discriminant (mean angular step deg between consecutive 30Hz\n"
                  "samples along each track's own timeline; independent of the rest-pose\n"
                  "assumption above -- a wrong order should show larger/jumpier steps),\n"
              << "tracks contributing: " << tracksSmoothChecked << ":\n";
    std::cout << "Order   MeanStepDeg\n";
    for (int o = 0; o < kNumOrders; ++o) {
        std::cout << kOrderNames[o] << "     " << smoothStats[o].MeanStep() << "\n";
    }
    return 0;
}
