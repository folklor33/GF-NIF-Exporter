cmake_minimum_required(VERSION 3.19)
project(gf-nif-export CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    # Applied to every target here. Conformance mode (/permissive-) is NOT set
    # globally: niflib needs /permissive, so it is chosen per target below.
    add_compile_options(/W3 /utf-8 /MP)
    # 4996: niflib uses deprecated CRT/stdext calls throughout.
    add_compile_options(/wd4996)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS _SCL_SECURE_NO_WARNINGS NOMINMAX)
endif()

set(NIFLIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/niflib")

if(NOT EXISTS "${NIFLIB_DIR}/include/niflib.h")
    message(FATAL_ERROR
        "niflib sources not found in ${NIFLIB_DIR}.\n"
        "Run: git submodule update --init --recursive")
endif()

# ---------------------------------------------------------------------------
# niflib (built as a static lib from its own sources)
#
# We deliberately do NOT use niflib's bundled CMakeLists: it builds a SHARED
# target, probes for an exact Boost 1.75.0 + Doxygen, and pulls in src/nifqhull.cpp.
# nifqhull.cpp #includes the C sources of the `qhull` git submodule, which is an
# empty directory here (its recorded URL uses the git:// protocol that GitHub has
# disabled). qhull is only reached from Inertia.cpp's mass-property computation
# for Havok collision shapes, which this exporter never invokes, so we drop that
# one translation unit and supply a stub for it (see src/nif/QhullStub.cpp).
# ---------------------------------------------------------------------------
file(GLOB_RECURSE NIFLIB_SOURCES CONFIGURE_DEPENDS
    "${NIFLIB_DIR}/src/*.cpp"
    "${NIFLIB_DIR}/NvTriStrip/*.cpp"
    "${NIFLIB_DIR}/TriStripper/*.cpp"
)
list(REMOVE_ITEM NIFLIB_SOURCES "${NIFLIB_DIR}/src/nifqhull.cpp")

# QhullStub.cpp replaces the excluded src/nifqhull.cpp, so it belongs to this
# target and is compiled under the same C++14 / permissive settings.
add_library(niflib_static STATIC
    ${NIFLIB_SOURCES}
    "${CMAKE_CURRENT_SOURCE_DIR}/src/nif/QhullStub.cpp"
)
target_include_directories(niflib_static SYSTEM PUBLIC "${NIFLIB_DIR}/include")
target_compile_definitions(niflib_static PUBLIC NIFLIB_STATIC_LINK)

# niflib (last released 2021) does not compile as C++17:
#   * its headers do `using namespace std;`, so its own `Niflib::byte` typedef
#     becomes ambiguous with C++17's `std::byte` (C2872/C2556/C2371);
#   * TriStripper uses std::mem_fun_ref and std::binary_function, both removed
#     in C++17 (C2039/C2504).
# Rather than patch the submodule, we compile just this target as C++14. It is a
# separate static library, so the language level does not leak into our own C++17
# sources, and nothing in niflib's public headers depends on a C++17-only type.
set_target_properties(niflib_static PROPERTIES
    CXX_STANDARD 14
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)

if(MSVC)
    # niflib's generated sources are large; several exceed the default limits.
    # It also predates conformant lookup, so it needs /permissive (not /permissive-).
    # 4828: TriStripper's headers carry Latin-1 accented characters in their
    # author comments, which are not valid UTF-8 under the global /utf-8 flag.
    target_compile_options(niflib_static PRIVATE
        /bigobj /permissive /wd4244 /wd4267 /wd4018 /wd4715 /wd4828)
endif()

# ---------------------------------------------------------------------------
# gfnif-export
# ---------------------------------------------------------------------------
add_executable(gfnif-export
    src/main.cpp
    src/cli/Args.cpp
    src/cli/ProgressReporter.cpp
    src/export/GfxFormatWriter.cpp
    src/nif/AnimationExtractor.cpp
    src/nif/HeaderNormalizer.cpp
    src/nif/MaterialExtractor.cpp
    src/nif/MeshExtractor.cpp
    src/nif/NameClassifier.cpp
    src/nif/NifDumper.cpp
    src/nif/ParticleExtractor.cpp
    src/nif/SkeletonExtractor.cpp
    src/pipeline/ConversionJob.cpp
    src/pipeline/Orchestrator.cpp
    src/pipeline/ThreadPool.cpp
    src/scanner/EntityScanner.cpp
    src/texture/TextureResolver.cpp
    src/util/Logger.cpp
    src/util/PathUtils.cpp
)
target_include_directories(gfnif-export PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    # CLI11 2.4.2, vendored as the single header it ships as (BSD 3-clause).
    # SYSTEM so its own warnings do not surface under our /W3 /permissive-.
)
target_include_directories(gfnif-export SYSTEM PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/external/cli11")
target_link_libraries(gfnif-export PRIVATE niflib_static)
find_package(Threads REQUIRED)
target_link_libraries(gfnif-export PRIVATE Threads::Threads)

if(MSVC)
    # Our own code is held to conformance mode, unlike niflib.
    target_compile_options(gfnif-export PRIVATE /permissive-)
endif()

# Opt-in AddressSanitizer build for diagnosing memory errors across a long run:
#   cmake -S . -B build-asan -DGFNIF_ASAN=ON && cmake --build build-asan --config Debug
option(GFNIF_ASAN "Build with AddressSanitizer" OFF)
if(GFNIF_ASAN AND MSVC)
    target_compile_options(gfnif-export PRIVATE /fsanitize=address /Zi)
    target_compile_options(niflib_static PRIVATE /fsanitize=address /Zi)
    target_link_options(gfnif-export PRIVATE /INCREMENTAL:NO)
endif()

if(MSVC)
    set_property(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                 PROPERTY VS_STARTUP_PROJECT gfnif-export)
endif()

# ---------------------------------------------------------------------------
# tools/diag -- one-off corpus measurement probes.
#
# These are not part of the conversion pipeline; each answers a specific
# question a phase's findings document then records. They are built on demand:
#   cmake --build build --config Release --target measure-phase7
# ---------------------------------------------------------------------------
add_executable(measure-phase7 EXCLUDE_FROM_ALL
    tools/diag/measure_phase7.cpp
    src/nif/HeaderNormalizer.cpp
)
target_include_directories(measure-phase7 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(measure-phase7 PRIVATE niflib_static)
if(MSVC)
    target_compile_options(measure-phase7 PRIVATE /permissive-)
endif()
