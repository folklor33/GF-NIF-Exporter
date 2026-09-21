// tsprobe -- does niflib's lazy block-type registration actually race?
//
// Phase 6 needed a measured answer, not an assumption, before converting files
// on 12 threads. niflib registers its 441 block factories on the first
// ReadNifList call, guarded by a plain non-atomic bool (src/niflib.cpp:162):
//
//     if ( g_objects_registered == false ) {
//         g_objects_registered = true;
//         RegisterObjects();
//     }
//
// Two threads can both observe `false`; worse, one can set the flag and still
// be midway through 441 inserts into ObjectRegistry::object_map while another
// thread, seeing the flag already true, starts reading that same std::map.
//
// This probe reproduces exactly that guard from 12 threads and then does what
// ReadNifList does for every block: look a type up in the registry. A lookup
// that comes back null is a block niflib would have silently dropped.
//
// Measured on a 12-core machine, 20 runs each:
//   without --fix : 19/20 runs reported 1149-5736 failed lookups; 1 run died
//                   with an access violation (0xC0000005).
//   with    --fix : 0/20 runs reported any failure, no crashes.
//
// The fix is what Orchestrator::PrepareNiflibForConcurrentUse() does: register
// once up front and set niflib's own flag, so ReadNifList never writes the map
// again and every worker only ever reads it.
//
// Build (from the repo root, after the main build has produced niflib_static):
//   cl /nologo /EHsc /std:c++14 /permissive /MD /O2 /I external\niflib\include ^
//      /DNIFLIB_STATIC_LINK tools\tsprobe\tsprobe.cpp ^
//      /link build\Release\niflib_static.lib
//
// Run:  tsprobe.exe        (racy path)
//       tsprobe.exe --fix  (the fix applied)

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "ObjectRegistry.h"
#include "obj/NiObject.h"

namespace Niflib {
void RegisterObjects();
extern bool g_objects_registered;
} // namespace Niflib

namespace {
constexpr int kThreads = 12;
constexpr int kLookupsPerThread = 4000;
} // namespace

int main(int argc, char** argv) {
    const bool applyFix = (argc > 1 && std::string(argv[1]) == "--fix");
    if (applyFix) {
        Niflib::RegisterObjects();
        Niflib::g_objects_registered = true;
    }

    std::atomic<int> failedLookups{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&] {
            // Mimic ReadNifList's guard exactly, including its non-atomicity.
            if (Niflib::g_objects_registered == false) {
                Niflib::g_objects_registered = true;
                Niflib::RegisterObjects();
            }
            for (int i = 0; i < kLookupsPerThread; ++i) {
                Niflib::NiObject* o = Niflib::ObjectRegistry::CreateObject("NiTriShape");
                if (o == nullptr) {
                    ++failedLookups;
                } else {
                    // CreateObject returns refcount 0; a Ref takes and releases it.
                    Niflib::Ref<Niflib::NiObject> owner(o);
                }
            }
        });
    }
    for (std::thread& t : threads) {
        t.join();
    }

    std::printf("%s: lookups that failed = %d\n", applyFix ? "WITH FIX" : "NO FIX",
                failedLookups.load());
    return failedLookups.load() == 0 ? 0 : 1;
}
