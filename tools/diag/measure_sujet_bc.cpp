// Sujet B/C measurement (billboard prevalence + controller-target type +
// morph controllers). Two modes:
//   --file <path>     -> verbose dump for one file
//   --corpus <dir>    -> aggregate stats across all .nif files
#include "niflib.h"
#include "obj/NiAVObject.h"
#include "obj/NiNode.h"
#include "obj/NiBillboardNode.h"
#include "obj/NiTriBasedGeom.h"
#include "obj/NiTriShape.h"
#include "obj/NiTriShapeData.h"
#include "obj/NiSkinInstance.h"
#include "obj/NiTimeController.h"
#include "obj/NiTransformController.h"
#include "obj/NiTransformInterpolator.h"
#include "obj/NiTransformData.h"
#include "obj/NiGeomMorpherController.h"
#include "obj/NiMorphData.h"
#include "obj/NiInterpolator.h"

#include "nif/HeaderNormalizer.hpp"

#include <filesystem>
#include <iostream>
#include <set>
#include <sstream>
#include <vector>

using namespace Niflib;
namespace fs = std::filesystem;

static int CountDescendantShapes(NiAVObject* obj, std::set<NiObject*>& visited) {
    if (obj == nullptr || !visited.insert(obj).second) return 0;
    int count = dynamic_cast<NiTriBasedGeom*>(obj) != nullptr ? 1 : 0;
    if (auto* node = dynamic_cast<NiNode*>(obj)) {
        for (const auto& c : node->GetChildren()) {
            count += CountDescendantShapes(static_cast<NiAVObject*>(c), visited);
        }
    }
    return count;
}

struct FileStats {
    bool parsed = false;
    int billboardNodes = 0;
    int billboardMeshesUnderneath = 0;
    std::vector<int> billboardModes;
    int transformControllersOnShape = 0;
    int transformControllersOnNode = 0;
    int morphControllers = 0;
};

static void WalkAll(NiAVObject* obj, std::set<NiObject*>& visited, FileStats& fs_) {
    if (obj == nullptr || !visited.insert(obj).second) return;

    if (auto* bb = dynamic_cast<NiBillboardNode*>(obj)) {
        fs_.billboardNodes++;
        fs_.billboardModes.push_back(static_cast<int>(bb->GetBillboardMode()));
        std::set<NiObject*> v2;
        fs_.billboardMeshesUnderneath += CountDescendantShapes(obj, v2);
    }

    for (const Ref<NiTimeController>& ctrlRef : obj->GetControllers()) {
        NiTimeController* ctrl = ctrlRef;
        if (dynamic_cast<NiTransformController*>(ctrl) != nullptr) {
            if (dynamic_cast<NiTriBasedGeom*>(obj) != nullptr) {
                fs_.transformControllersOnShape++;
            } else {
                fs_.transformControllersOnNode++;
            }
        }
        if (dynamic_cast<NiGeomMorpherController*>(ctrl) != nullptr) {
            fs_.morphControllers++;
        }
    }

    if (auto* node = dynamic_cast<NiNode*>(obj)) {
        for (const auto& c : node->GetChildren()) {
            WalkAll(static_cast<NiAVObject*>(c), visited, fs_);
        }
    }
}

static FileStats AnalyzeFile(const std::string& path) {
    FileStats fs_;
    std::string bytes;
    gfnif::ReadWholeFile(path, bytes);
    gfnif::HeaderNormalizationReport rep;
    gfnif::NormalizeNifHeader(bytes, rep);
    std::vector<NiObjectRef> blocks;
    std::istringstream stream(bytes, std::ios::binary);
    try {
        blocks = ReadNifList(stream, nullptr);
    } catch (...) {
        return fs_;
    }
    fs_.parsed = true;

    std::set<NiObject*> referenced;
    for (const NiObjectRef& b : blocks)
        for (const NiObjectRef& r : static_cast<NiObject*>(b)->GetRefs())
            if (r != NULL) referenced.insert(static_cast<NiObject*>(r));

    std::set<NiObject*> visited;
    for (const NiObjectRef& b : blocks) {
        NiObject* obj = static_cast<NiObject*>(b);
        if (referenced.count(obj)) continue;
        if (auto* av = dynamic_cast<NiAVObject*>(obj)) WalkAll(av, visited, fs_);
    }
    return fs_;
}

static void DumpFileVerbose(const std::string& path) {
    std::string bytes;
    gfnif::ReadWholeFile(path, bytes);
    gfnif::HeaderNormalizationReport rep;
    gfnif::NormalizeNifHeader(bytes, rep);
    std::vector<NiObjectRef> blocks;
    std::istringstream stream(bytes, std::ios::binary);
    blocks = ReadNifList(stream, nullptr);

    for (const NiObjectRef& b : blocks) {
        NiObject* obj = static_cast<NiObject*>(b);
        auto* av = dynamic_cast<NiAVObject*>(obj);
        if (av == nullptr) continue;
        for (const Ref<NiTimeController>& ctrlRef : av->GetControllers()) {
            NiTimeController* ctrl = ctrlRef;
            std::cout << "Object '" << av->GetName()
                      << "' (" << obj->GetType().GetTypeName() << ") has controller of type "
                      << ctrl->GetType().GetTypeName() << "\n";
            if (auto* xform = dynamic_cast<NiTransformController*>(ctrl)) {
                NiInterpolator* rawInterp = xform->GetInterpolator();
                std::cout << "    interpolator type: "
                          << (rawInterp ? rawInterp->GetType().GetTypeName() : "null") << "\n";
                std::cout << "    startTime=" << ctrl->GetStartTime() << " stopTime=" << ctrl->GetStopTime()
                          << " frequency=" << ctrl->GetFrequency() << " phase=" << ctrl->GetPhase() << "\n";
                auto* interp = dynamic_cast<NiTransformInterpolator*>(rawInterp);
                if (interp != nullptr) {
                    NiTransformData* data = interp->GetData();
                    if (data != nullptr) {
                        std::cout << "    translateKeys=" << data->GetTranslateKeys().size()
                                  << " quatKeys=" << data->GetQuatRotateKeys().size()
                                  << " scaleKeys=" << data->GetScaleKeys().size() << "\n";
                        for (const auto& k : data->GetTranslateKeys()) {
                            std::cout << "      t=" << k.time << " pos=(" << k.data.x << "," << k.data.y << "," << k.data.z << ")\n";
                        }
                    }
                }
            }
            if (dynamic_cast<NiGeomMorpherController*>(ctrl) != nullptr) {
                std::cout << "    ** GeomMorpherController present **\n";
            }
        }
        // Also dump the shape's own data-block baked transform, if any --
        // some old-format NiTriShapeData carries a rotation/translation/scale
        // separate from the parent NiAVObject's local transform.
        if (auto* shape = dynamic_cast<NiTriShape*>(obj)) {
            std::cout << "Shape '" << shape->GetName() << "' local transform: T=("
                      << shape->GetLocalTranslation().x << "," << shape->GetLocalTranslation().y << ","
                      << shape->GetLocalTranslation().z << ")\n";
        }
    }
}

int main(int argc, char** argv) {
    if (argc < 3) return 1;
    std::string mode = argv[1];
    if (mode == "--file") {
        DumpFileVerbose(argv[2]);
        return 0;
    }
    if (mode == "--corpus") {
        int filesParsed = 0;
        int filesWithBillboard = 0, totalBillboardNodes = 0, totalBillboardMeshes = 0;
        int totalCtrlOnShape = 0, totalCtrlOnNode = 0, totalMorph = 0, filesWithMorph = 0;
        std::vector<int> modeHist(16, 0);
        for (auto& entry : fs::recursive_directory_iterator(argv[2])) {
            if (entry.path().extension() != ".nif") continue;
            const std::string stem = entry.path().stem().string();
            if (stem == "WF20" || stem == "M156" || stem == "N600") continue;
            FileStats fs_ = AnalyzeFile(entry.path().string());
            if (!fs_.parsed) continue;
            filesParsed++;
            if (fs_.billboardNodes > 0) {
                filesWithBillboard++;
                totalBillboardNodes += fs_.billboardNodes;
                totalBillboardMeshes += fs_.billboardMeshesUnderneath;
                for (int m : fs_.billboardModes) if (m >= 0 && m < 16) modeHist[m]++;
            }
            totalCtrlOnShape += fs_.transformControllersOnShape;
            totalCtrlOnNode += fs_.transformControllersOnNode;
            if (fs_.morphControllers > 0) { filesWithMorph++; totalMorph += fs_.morphControllers; }
        }
        std::cout << "Files parsed: " << filesParsed << "\n";
        std::cout << "Files with >=1 NiBillboardNode: " << filesWithBillboard << "\n";
        std::cout << "  total NiBillboardNode blocks: " << totalBillboardNodes << "\n";
        std::cout << "  total mesh descendants under billboard nodes: " << totalBillboardMeshes << "\n";
        std::cout << "  billboard mode histogram: ";
        for (size_t i = 0; i < modeHist.size(); ++i) if (modeHist[i]) std::cout << i << "=" << modeHist[i] << " ";
        std::cout << "\n";
        std::cout << "NiTransformController targeting a NiTriBasedGeom shape directly: " << totalCtrlOnShape << "\n";
        std::cout << "NiTransformController targeting a NiNode: " << totalCtrlOnNode << "\n";
        std::cout << "Files with >=1 NiGeomMorpherController: " << filesWithMorph << " (total " << totalMorph << ")\n";
        return 0;
    }
    return 1;
}
