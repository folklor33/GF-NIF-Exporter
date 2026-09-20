// One-off diagnostic (Phase 4 follow-up, Task A): measures XYZ_ROTATION_KEY
// prevalence corpus-wide, cross-referencing which (file, clip, bone) triples
// are affected and whether the affected bone's translation moves it far from
// bind pose while rotation stays frozen ("looks broken" signature).
//
// Mirrors AnimationExtractor::ExtractClassicTrack's own dispatch (classic
// NiTransformInterpolator only; B-spline tracks are quaternion-based and
// unaffected) rather than re-deriving anything, per PHASE4_FINDINGS §3.1.
// Not part of the shipped exporter; not wired into CMakeLists.txt.
//
// Build (same pattern as measure_bspline_error.cpp): add a temporary
// add_executable linking niflib_static, or invoke MSVC directly.
//
// Usage: measure_xyz_rotation.exe <input_root>
//   Walks <input_root>/<type>/model/*.nif, resolves each companion
//   <type>/animation/NAME.kf (same convention as MeshExtractor::ResolveCompanionKf),
//   and for every NiControllerSequence's ControllerLink with a
//   NiTransformInterpolator whose GetRotateType() == XYZ_ROTATION_KEY, checks
//   whether that same bone's translation keys range (max-min per axis) exceeds
//   a threshold.

#include "niflib.h"
#include "nif_math.h"
#include "obj/NiAVObject.h"
#include "obj/NiControllerSequence.h"
#include "obj/NiNode.h"
#include "obj/NiTransformData.h"
#include "obj/NiTransformInterpolator.h"

#include "nif/HeaderNormalizer.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;
using namespace Niflib;

namespace {

constexpr float kTranslationRangeThreshold = 0.5f;

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

// Classify a file/clip-name as "idle/pose-like" vs "motion-like" by simple
// substring heuristics matching the corpus naming (stand/idle/magic-style
// pose clips vs move/walk/run/attack/death motion clips), for the brief's
// disproportionate-effect question.
bool LooksLikeIdleClip(const std::string& name) {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    static const char* idleTokens[] = {"stand", "idle", "magic", "wait", "pose"};
    for (const char* t : idleTokens) {
        if (lower.find(t) != std::string::npos) return true;
    }
    return false;
}
bool LooksLikeMotionClip(const std::string& name) {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    static const char* motionTokens[] = {"move", "walk", "run", "attack", "death",
                                          "dash", "jump", "hit", "skill"};
    for (const char* t : motionTokens) {
        if (lower.find(t) != std::string::npos) return true;
    }
    return false;
}

struct Totals {
    long long xyzOccurrences = 0;                 // raw warning-equivalent count
    std::set<std::pair<std::string,std::string>> affectedClips; // (file, clipname)
    std::set<std::string> affectedFiles;
    long long idleClipOccurrences = 0;
    long long motionClipOccurrences = 0;
    long long otherClipOccurrences = 0;
    std::set<std::pair<std::string,std::string>> idleAffectedClips;
    std::set<std::pair<std::string,std::string>> motionAffectedClips;
    long long largeTranslationWithFrozenRotation = 0; // bone+clip meeting the "looks broken" proxy
    std::set<std::string> filesWithLargeTranslationFrozenRotation;
};

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: measure_xyz_rotation <input_root>\n";
        return 1;
    }
    fs::path root = argv[1];

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

    Totals totals;
    int filesWithKf = 0;
    int m009StandFound = 0, m009MagicFound = 0;

    for (const fs::path& nifPath : nifFiles) {
        fs::path kfPath;
        if (!ResolveCompanionKf(nifPath, kfPath)) continue;
        ++filesWithKf;

        std::vector<NiObjectRef> blocks;
        if (!LoadBlocks(kfPath, blocks)) continue;

        const std::string fileKey = fs::relative(nifPath, root).generic_string();
        const bool isM009 = nifPath.stem().string() == "M009";

        for (const NiObjectRef& b : blocks) {
            auto* seq = dynamic_cast<NiControllerSequence*>(static_cast<NiObject*>(b));
            if (seq == nullptr) continue;
            const std::string clipName = seq->GetName();
            const bool idle = LooksLikeIdleClip(clipName);
            const bool motion = LooksLikeMotionClip(clipName);

            for (const ControllerLink& link : seq->GetControllerData()) {
                if (link.interpolator == NULL) continue;
                auto* classic = dynamic_cast<NiTransformInterpolator*>(static_cast<NiInterpolator*>(link.interpolator));
                if (classic == nullptr) continue; // B-spline or out-of-scope: unaffected by this bug
                NiTransformData* data = classic->GetData();
                if (data == nullptr) continue; // static pose only

                if (isM009 && (clipName == "stand01" || clipName == "magic01")) {
                    bool isXyz = (data->GetRotateType() == XYZ_ROTATION_KEY);
                    if (clipName == "stand01" && isXyz) ++m009StandFound;
                    if (clipName == "magic01" && isXyz) ++m009MagicFound;
                }

                if (data->GetRotateType() != XYZ_ROTATION_KEY) continue;

                ++totals.xyzOccurrences;
                totals.affectedClips.insert({fileKey, clipName});
                totals.affectedFiles.insert(fileKey);
                if (idle) { ++totals.idleClipOccurrences; totals.idleAffectedClips.insert({fileKey, clipName}); }
                else if (motion) { ++totals.motionClipOccurrences; totals.motionAffectedClips.insert({fileKey, clipName}); }
                else { ++totals.otherClipOccurrences; }

                // Translation range check: max-min per axis across this track's
                // translate keys (the bone this XYZ_ROTATION_KEY link governs).
                const std::vector<Key<Vector3>> tkeys = data->GetTranslateKeys();
                if (tkeys.size() >= 2) {
                    float minv[3] = {1e30f,1e30f,1e30f}, maxv[3] = {-1e30f,-1e30f,-1e30f};
                    for (const auto& k : tkeys) {
                        float v[3] = {k.data.x, k.data.y, k.data.z};
                        for (int a = 0; a < 3; ++a) { minv[a] = std::min(minv[a], v[a]); maxv[a] = std::max(maxv[a], v[a]); }
                    }
                    float range = std::max({maxv[0]-minv[0], maxv[1]-minv[1], maxv[2]-minv[2]});
                    if (range > kTranslationRangeThreshold) {
                        ++totals.largeTranslationWithFrozenRotation;
                        totals.filesWithLargeTranslationFrozenRotation.insert(fileKey);
                    }
                }
            }
        }
    }

    std::cout << "=== XYZ_ROTATION_KEY corpus scan ===\n";
    std::cout << "Model .nif files with companion .kf resolved: " << filesWithKf << "\n";
    std::cout << "M009 stand01: XYZ_ROTATION_KEY tracks found = " << m009StandFound << "\n";
    std::cout << "M009 magic01: XYZ_ROTATION_KEY tracks found = " << m009MagicFound << "\n";
    std::cout << "Raw XYZ_ROTATION_KEY occurrences (classic tracks): " << totals.xyzOccurrences << "\n";
    std::cout << "Unique (file, clipname) pairs affected: " << totals.affectedClips.size() << "\n";
    std::cout << "Unique files affected: " << totals.affectedFiles.size() << "\n";
    std::cout << "  idle/pose-like clip occurrences: " << totals.idleClipOccurrences
              << " (" << totals.idleAffectedClips.size() << " unique clips)\n";
    std::cout << "  motion-like clip occurrences: " << totals.motionClipOccurrences
              << " (" << totals.motionAffectedClips.size() << " unique clips)\n";
    std::cout << "  other/unclassified clip occurrences: " << totals.otherClipOccurrences << "\n";
    std::cout << "Bone+clip cases with translation range > " << kTranslationRangeThreshold
              << " while rotation frozen: " << totals.largeTranslationWithFrozenRotation << "\n";
    std::cout << "Files exhibiting that signature: " << totals.filesWithLargeTranslationFrozenRotation.size() << "\n";
    for (const auto& f : totals.filesWithLargeTranslationFrozenRotation) {
        std::cout << "    " << f << "\n";
    }
    return 0;
}
