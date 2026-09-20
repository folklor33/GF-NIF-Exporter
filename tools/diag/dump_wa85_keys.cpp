// Follow-up ad hoc diagnostic: WA85's Plane03/Plane04 controllers report
// hasKeyframeData=yes (NiTransformData present) but the exported .gfmodel shows
// 0 keys in every channel for those tracks. Dumps the raw key counts and rotate
// type to find out why.
#include "niflib.h"
#include "obj/NiAVObject.h"
#include "obj/NiNode.h"
#include "obj/NiTimeController.h"
#include "obj/NiTransformController.h"
#include "obj/NiTransformData.h"
#include "obj/NiTransformInterpolator.h"

#include "nif/HeaderNormalizer.hpp"

#include <filesystem>
#include <iostream>
#include <set>
#include <sstream>

using namespace Niflib;
namespace fs = std::filesystem;

void Walk(NiAVObject* obj, std::set<NiObject*>& visited) {
    if (obj == nullptr || !visited.insert(obj).second) return;
    for (const auto& ctrlRef : obj->GetControllers()) {
        auto* xform = dynamic_cast<NiTransformController*>(static_cast<NiTimeController*>(ctrlRef));
        if (xform == nullptr) continue;
        auto* interp = dynamic_cast<NiTransformInterpolator*>(static_cast<NiInterpolator*>(xform->GetInterpolator()));
        if (interp == nullptr) continue;
        NiTransformData* data = interp->GetData();
        if (data == nullptr) { std::cout << obj->GetName() << ": data=null\n"; continue; }
        std::cout << obj->GetName() << ": rotateType=" << (int)data->GetRotateType()
                  << " quatKeys=" << data->GetQuatRotateKeys().size()
                  << " translateKeys=" << data->GetTranslateKeys().size()
                  << " scaleKeys=" << data->GetScaleKeys().size()
                  << " xyzX=" << data->GetXRotateKeys().size()
                  << " xyzY=" << data->GetYRotateKeys().size()
                  << " xyzZ=" << data->GetZRotateKeys().size()
                  << "\n";
    }
    if (auto* node = dynamic_cast<NiNode*>(obj)) {
        for (const auto& c : node->GetChildren()) Walk(static_cast<NiAVObject*>(c), visited);
    }
}

int main(int argc, char** argv) {
    if (argc < 2) return 1;
    std::string bytes;
    gfnif::ReadWholeFile(argv[1], bytes);
    gfnif::HeaderNormalizationReport rep;
    gfnif::NormalizeNifHeader(bytes, rep);
    std::vector<NiObjectRef> blocks;
    std::istringstream stream(bytes, std::ios::binary);
    blocks = ReadNifList(stream, nullptr);

    std::set<NiObject*> referenced;
    for (const NiObjectRef& b : blocks)
        for (const NiObjectRef& r : static_cast<NiObject*>(b)->GetRefs())
            if (r != NULL) referenced.insert(static_cast<NiObject*>(r));

    std::set<NiObject*> visited;
    for (const NiObjectRef& b : blocks) {
        NiObject* obj = static_cast<NiObject*>(b);
        if (referenced.count(obj)) continue;
        if (auto* av = dynamic_cast<NiAVObject*>(obj)) Walk(av, visited);
    }
    return 0;
}
