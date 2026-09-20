// One-off diagnostic (Phase 4 follow-up, Task C): investigates embedded
// NiTransformController data on static (non-skeleton) models, and the
// "mixed" case (skinned file with a controller on a node outside the
// skeleton root's subtree).
//
// Not part of the shipped exporter; not wired into CMakeLists.txt.
//
// Usage:
//   measure_embedded_controllers.exe dump <nif_path>       WA85-style tree dump
//   measure_embedded_controllers.exe corpus <input_root>   corpus-wide scan

#include "niflib.h"
#include "nif_math.h"
#include "obj/NiAVObject.h"
#include "obj/NiBSplineTransformInterpolator.h"
#include "obj/NiInterpolator.h"
#include "obj/NiMultiTargetTransformController.h"
#include "obj/NiNode.h"
#include "obj/NiObjectNET.h"
#include "obj/NiSkinInstance.h"
#include "obj/NiTimeController.h"
#include "obj/NiTransformController.h"
#include "obj/NiTransformData.h"
#include "obj/NiTransformInterpolator.h"
#include "obj/NiTriShape.h"

#include "nif/HeaderNormalizer.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <set>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;
using namespace Niflib;

namespace {

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

bool HasSkinnedMesh(const std::vector<NiObjectRef>& blocks) {
    for (const NiObjectRef& b : blocks) {
        auto* shape = dynamic_cast<NiTriShape*>(static_cast<NiObject*>(b));
        if (shape != nullptr && shape->GetSkinInstance() != nullptr) return true;
    }
    return false;
}

std::string InterpTypeName(NiInterpolator* interp) {
    if (interp == nullptr) return "(null)";
    if (dynamic_cast<NiBSplineTransformInterpolator*>(interp)) return "NiBSplineTransformInterpolator-family:" + interp->GetType().GetTypeName();
    if (dynamic_cast<NiTransformInterpolator*>(interp)) return "NiTransformInterpolator";
    return std::string("other:") + interp->GetType().GetTypeName();
}

bool ClassicHasKeys(NiTransformInterpolator* ti) {
    NiTransformData* data = ti->GetData();
    if (data == nullptr) return false;
    return !data->GetTranslateKeys().empty() || !data->GetQuatRotateKeys().empty() ||
           !data->GetScaleKeys().empty() || data->GetRotateType() == XYZ_ROTATION_KEY;
}

void DumpTree(NiAVObject* obj, int depth, std::set<NiObject*>& visited) {
    if (obj == nullptr || !visited.insert(obj).second) return;
    std::string indent(depth * 2, ' ');
    std::cout << indent << "- " << obj->GetName() << " [" << obj->GetType().GetTypeName() << "]\n";

    for (const auto& ctrlRef : obj->GetControllers()) {
        auto* ctrl = static_cast<NiTimeController*>(ctrlRef);
        std::cout << indent << "    controller: " << ctrl->GetType().GetTypeName();
        if (auto* xform = dynamic_cast<NiTransformController*>(ctrl)) {
            NiInterpolator* interp = xform->GetInterpolator();
            std::cout << " (NiTransformController) interpolator=" << InterpTypeName(interp);
            if (auto* classic = dynamic_cast<NiTransformInterpolator*>(interp)) {
                std::cout << " hasKeyframeData=" << (ClassicHasKeys(classic) ? "yes" : "no");
            } else if (auto* bs = dynamic_cast<NiBSplineTransformInterpolator*>(interp)) {
                std::cout << " startTime=" << bs->GetStartTime() << " stopTime=" << bs->GetStopTime();
            }
        } else if (auto* multi = dynamic_cast<NiMultiTargetTransformController*>(ctrl)) {
            std::cout << " (NiMultiTargetTransformController) extraTargets=" << multi->GetExtraTargets().size();
        }
        std::cout << "\n";
    }

    if (auto* node = dynamic_cast<NiNode*>(obj)) {
        for (const auto& c : node->GetChildren()) {
            DumpTree(static_cast<NiAVObject*>(c), depth + 1, visited);
        }
    }
}

int RunDump(const fs::path& nifPath) {
    std::vector<NiObjectRef> blocks;
    if (!LoadBlocks(nifPath, blocks)) {
        std::cerr << "failed to load " << nifPath << "\n";
        return 1;
    }
    std::cout << "Has skinned mesh: " << (HasSkinnedMesh(blocks) ? "yes" : "no") << "\n\n";

    std::set<NiObject*> referenced;
    for (const NiObjectRef& b : blocks) {
        for (const NiObjectRef& r : static_cast<NiObject*>(b)->GetRefs()) {
            if (r != NULL) referenced.insert(static_cast<NiObject*>(r));
        }
    }
    std::set<NiObject*> visited;
    for (const NiObjectRef& b : blocks) {
        NiObject* obj = static_cast<NiObject*>(b);
        if (referenced.count(obj)) continue;
        if (auto* av = dynamic_cast<NiAVObject*>(obj)) {
            DumpTree(av, 0, visited);
        }
    }
    return 0;
}

// Collects every NiNode under `root`'s subtree (the skeleton subtree), for
// the "mixed case" check: a controller target outside this set.
void CollectSubtree(NiAVObject* obj, std::set<NiObject*>& out) {
    if (obj == nullptr || !out.insert(obj).second) return;
    if (auto* node = dynamic_cast<NiNode*>(obj)) {
        for (const auto& c : node->GetChildren()) CollectSubtree(static_cast<NiAVObject*>(c), out);
    }
}

NiNode* FindSkeletonRoot(const std::vector<NiObjectRef>& blocks) {
    for (const NiObjectRef& b : blocks) {
        auto* shape = dynamic_cast<NiTriShape*>(static_cast<NiObject*>(b));
        if (shape == nullptr) continue;
        NiSkinInstance* skin = shape->GetSkinInstance();
        if (skin == nullptr) continue;
        NiNode* r = skin->GetSkeletonRoot();
        if (r != nullptr) return r;
    }
    return nullptr;
}

int RunCorpus(const fs::path& root) {
    std::vector<fs::path> nifFiles;
    for (auto it = fs::recursive_directory_iterator(root); it != fs::recursive_directory_iterator(); ++it) {
        if (!it->is_regular_file()) continue;
        auto p = it->path();
        std::string ext = p.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == ".nif" && p.parent_path().filename() == "model") nifFiles.push_back(p);
    }
    std::sort(nifFiles.begin(), nifFiles.end());

    int staticFiles = 0;
    int staticFilesWithLiveController = 0;
    std::vector<std::string> sample;
    int mixedFilesFound = 0;
    std::vector<std::string> mixedSample;

    for (const fs::path& nifPath : nifFiles) {
        std::vector<NiObjectRef> blocks;
        if (!LoadBlocks(nifPath, blocks)) continue;

        std::set<NiObject*> referenced;
        for (const NiObjectRef& b : blocks) {
            for (const NiObjectRef& r : static_cast<NiObject*>(b)->GetRefs()) {
                if (r != NULL) referenced.insert(static_cast<NiObject*>(r));
            }
        }

        bool skinned = HasSkinnedMesh(blocks);
        NiNode* skelRoot = skinned ? FindSkeletonRoot(blocks) : nullptr;
        std::set<NiObject*> skelSubtree;
        if (skelRoot != nullptr) CollectSubtree(skelRoot, skelSubtree);

        bool hasLiveController = false;
        bool hasOutsideController = false;

        std::vector<NiAVObject*> stack;
        std::set<NiObject*> visited;
        for (const NiObjectRef& b : blocks) {
            NiObject* obj = static_cast<NiObject*>(b);
            if (referenced.count(obj)) continue;
            if (auto* av = dynamic_cast<NiAVObject*>(obj)) stack.push_back(av);
        }
        while (!stack.empty()) {
            NiAVObject* obj = stack.back();
            stack.pop_back();
            if (obj == nullptr || !visited.insert(obj).second) continue;

            for (const auto& ctrlRef : obj->GetControllers()) {
                auto* xform = dynamic_cast<NiTransformController*>(static_cast<NiTimeController*>(ctrlRef));
                if (xform == nullptr) continue;
                NiInterpolator* interp = xform->GetInterpolator();
                if (interp == nullptr) continue;
                bool hasData = false;
                if (auto* classic = dynamic_cast<NiTransformInterpolator*>(interp)) {
                    hasData = ClassicHasKeys(classic);
                } else if (auto* bs = dynamic_cast<NiBSplineTransformInterpolator*>(interp)) {
                    hasData = (bs->GetStopTime() - bs->GetStartTime()) > 0.0f;
                }
                if (hasData) {
                    hasLiveController = true;
                    if (skinned && skelSubtree.count(obj) == 0) {
                        hasOutsideController = true;
                    }
                }
            }
            if (auto* node = dynamic_cast<NiNode*>(obj)) {
                for (const auto& c : node->GetChildren()) stack.push_back(static_cast<NiAVObject*>(c));
            }
        }

        std::string key = fs::relative(nifPath, root).generic_string();
        if (!skinned) {
            ++staticFiles;
            if (hasLiveController) {
                ++staticFilesWithLiveController;
                if (sample.size() < 40) sample.push_back(key);
            }
        } else if (hasOutsideController) {
            ++mixedFilesFound;
            if (mixedSample.size() < 20) mixedSample.push_back(key);
        }
    }

    std::cout << "=== Static (no-skeleton) model corpus scan ===\n";
    std::cout << "Static model .nif files total: " << staticFiles << "\n";
    std::cout << "Static models with >=1 live NiTransformController (would produce a track if bone "
              << "resolution weren't the blocker): " << staticFilesWithLiveController << "\n";
    std::cout << "Sample:\n";
    for (auto& s : sample) std::cout << "    " << s << "\n";

    std::cout << "\n=== Mixed case: skinned file with controller outside skeleton subtree ===\n";
    std::cout << "Files found: " << mixedFilesFound << "\n";
    for (auto& s : mixedSample) std::cout << "    " << s << "\n";

    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage:\n"
                  << "  measure_embedded_controllers dump   <nif_path>\n"
                  << "  measure_embedded_controllers corpus <input_root>\n";
        return 1;
    }
    std::string mode = argv[1];
    fs::path arg = argv[2];
    if (mode == "dump") return RunDump(arg);
    if (mode == "corpus") return RunCorpus(arg);
    std::cerr << "unknown mode\n";
    return 1;
}
