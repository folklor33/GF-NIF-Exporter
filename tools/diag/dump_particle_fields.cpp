// Phase 5 diagnostic: prints asString(true) for every particle-related block
// in one file, to validate the asString()-parsing approach and show real
// field values (used to compare against NifSkope's own detail panel).
//
// Usage: dump_particle_fields.exe <nif_path>

#include "niflib.h"
#include "obj/NiObject.h"
#include "obj/NiParticleSystem.h"
#include "obj/NiPSysModifier.h"
#include "obj/NiPSysData.h"

#include "nif/HeaderNormalizer.hpp"

#include <filesystem>
#include <iostream>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;
using namespace Niflib;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: dump_particle_fields <nif_path>\n";
        return 1;
    }
    fs::path path = argv[1];
    std::string bytes;
    if (!gfnif::ReadWholeFile(path.string(), bytes)) {
        std::cerr << "read failed\n";
        return 1;
    }
    gfnif::HeaderNormalizationReport rep;
    gfnif::NormalizeNifHeader(bytes, rep);
    std::vector<NiObjectRef> blocks;
    try {
        std::istringstream stream(bytes, std::ios::binary);
        blocks = ReadNifList(stream, nullptr);
    } catch (...) {
        std::cerr << "parse failed\n";
        return 1;
    }

    for (const NiObjectRef& b : blocks) {
        auto* obj = static_cast<NiObject*>(b);
        bool interesting = dynamic_cast<NiParticleSystem*>(obj) != nullptr ||
                            dynamic_cast<NiPSysModifier*>(obj) != nullptr ||
                            dynamic_cast<NiPSysData*>(obj) != nullptr;
        if (!interesting) continue;
        std::cout << "===== [" << obj->GetType().GetTypeName() << "] =====\n";
        std::cout << obj->asString(true) << "\n";
    }
    return 0;
}
