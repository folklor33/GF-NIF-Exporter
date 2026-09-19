// gfnif-export -- CLI entry point.
//
// Phase 1 added `dump`: parse a .nif/.kf and print its block structure.
// Phase 2 adds `export`: convert a .nif's static geometry and materials into
// a .gfmodel + .gfbin pair.
//
// The multi-file scanner and parallel pipeline are Phase 6; for now a directory
// argument is walked serially, which is enough for the 17-file test corpus.

#include "export/GfxFormatWriter.hpp"
#include "export/SceneModel.hpp"
#include "nif/MeshExtractor.hpp"
#include "nif/NifDumper.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

void PrintUsage() {
    std::cout
        << "gfnif-export\n\n"
        << "Usage:\n"
        << "  gfnif-export dump   <file|dir> [--summary]   block structure (phase 1 diagnostic)\n"
        << "  gfnif-export export <file|dir> -o <outDir>   .nif -> .gfmodel + .gfbin\n"
        << "\n"
        << "  export options:\n"
        << "    -o, --out <dir>   output root (default: ./out)\n"
        << "    --input-root <d>  strip this prefix when mirroring the input tree\n"
        << "    -v, --verbose     list every warning instead of counting them\n"
        << "    --verify-strips   cross-check de-striping against niflib's own expansion\n"
        << "\n"
        << "With no command, a bare path is treated as `dump` for compatibility.\n";
}

bool HasExtension(const fs::path& p, const char* ext) {
    std::string e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return e == ext;
}

bool IsNifLike(const fs::path& p) { return HasExtension(p, ".nif") || HasExtension(p, ".kf"); }

/*! Collects the files to process under `target`, or just `target` itself. */
std::vector<fs::path> CollectFiles(const fs::path& target, bool (*accept)(const fs::path&)) {
    std::vector<fs::path> files;
    std::error_code ec;
    if (fs::is_directory(target, ec)) {
        for (auto it = fs::recursive_directory_iterator(target, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) {
                break;
            }
            if (it->is_regular_file(ec) && accept(it->path())) {
                files.push_back(it->path());
            }
        }
        std::sort(files.begin(), files.end());
    } else if (accept(target)) {
        files.push_back(target);
    }
    return files;
}

int RunDump(const std::vector<std::string>& args) {
    if (args.empty()) {
        PrintUsage();
        return 1;
    }
    const fs::path target = args[0];
    bool summaryOnly = false;
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--summary") {
            summaryOnly = true;
        }
    }

    std::error_code ec;
    if (!fs::exists(target, ec)) {
        std::cerr << "error: path not found: " << target.string() << "\n";
        return 1;
    }

    const std::vector<fs::path> files = CollectFiles(target, IsNifLike);
    if (files.empty()) {
        std::cerr << "error: no .nif/.kf files under " << target.string() << "\n";
        return 1;
    }

    std::ostringstream sink;
    gfnif::BlockTypeTally tally;
    int ok = 0;
    std::vector<std::string> failures;

    for (const fs::path& f : files) {
        std::ostream& out = summaryOnly ? static_cast<std::ostream&>(sink) : std::cout;
        if (gfnif::DumpNifFile(f.string(), out, &tally)) {
            ++ok;
        } else {
            failures.push_back(f.string());
        }
        if (summaryOnly) {
            sink.str(std::string());
        }
    }

    std::cout << "\n==================================================================\n";
    std::cout << "CORPUS SUMMARY: " << ok << "/" << files.size() << " file(s) parsed\n";
    std::cout << "==================================================================\n";
    std::cout << "Distinct block types: " << tally.size() << "\n";
    for (const auto& kv : tally) {
        std::cout << "  " << kv.second << "\tx " << kv.first << "\n";
    }
    if (!failures.empty()) {
        std::cout << "\nFAILED FILES (" << failures.size() << "):\n";
        for (const std::string& f : failures) {
            std::cout << "  " << f << "\n";
        }
    }
    return failures.empty() ? 0 : 2;
}

int RunExport(const std::vector<std::string>& args) {
    if (args.empty()) {
        PrintUsage();
        return 1;
    }

    const fs::path target = args[0];
    fs::path outRoot = "out";
    fs::path inputRoot;
    bool verbose = false;
    bool verifyStrips = false;

    for (size_t i = 1; i < args.size(); ++i) {
        const std::string& a = args[i];
        if ((a == "-o" || a == "--out") && i + 1 < args.size()) {
            outRoot = args[++i];
        } else if (a == "--input-root" && i + 1 < args.size()) {
            inputRoot = args[++i];
        } else if (a == "-v" || a == "--verbose") {
            verbose = true;
        } else if (a == "--verify-strips") {
            verifyStrips = true;
        } else {
            std::cerr << "error: unknown option '" << a << "'\n";
            return 1;
        }
    }

    std::error_code ec;
    if (!fs::exists(target, ec)) {
        std::cerr << "error: path not found: " << target.string() << "\n";
        return 1;
    }
    // Mirror the input tree under the output root. Without an explicit
    // --input-root, a directory target is its own root and a single file lands
    // flat in the output directory.
    if (inputRoot.empty() && fs::is_directory(target, ec)) {
        inputRoot = target;
    }

    const std::vector<fs::path> files =
        CollectFiles(target, [](const fs::path& p) { return HasExtension(p, ".nif"); });
    if (files.empty()) {
        std::cerr << "error: no .nif files under " << target.string() << "\n";
        return 1;
    }

    int converted = 0;
    size_t totalVertices = 0, totalTriangles = 0;
    int totalDegenerate = 0, totalSkipped = 0, totalWarnings = 0, maxDepth = 0;
    int texturesResolved = 0, texturesMissing = 0;
    std::vector<std::string> failures;
    std::vector<std::string> unresolvedTextures;

    for (const fs::path& f : files) {
        gfnif::SceneData scene;
        const gfnif::ExtractionResult res = gfnif::ExtractScene(f.string(), scene, verifyStrips);

        std::cout << f.string() << "\n";

        if (!res.success) {
            std::cout << "  ERROR: " << res.error << "\n";
            failures.push_back(f.string() + ": " + res.error);
            continue;
        }

        // Mirror the relative path of the input under the output root.
        fs::path relative = inputRoot.empty() ? f.filename() : fs::relative(f, inputRoot, ec);
        if (ec || relative.empty()) {
            relative = f.filename();
        }
        const fs::path outBase = (outRoot / relative).replace_extension();

        std::string writeError;
        if (!gfnif::WriteSceneFiles(scene, outBase.string(), writeError)) {
            std::cout << "  ERROR: " << writeError << "\n";
            failures.push_back(f.string() + ": " + writeError);
            continue;
        }

        for (const gfnif::MaterialData& m : scene.materials) {
            if (m.sourceTextureName.empty()) {
                continue;
            }
            if (m.textureFound) {
                ++texturesResolved;
            } else {
                ++texturesMissing;
                unresolvedTextures.push_back(f.filename().string() + " -> " + m.sourceTextureName);
            }
        }

        ++converted;
        totalVertices += scene.TotalVertices();
        totalTriangles += scene.TotalTriangles();
        totalDegenerate += res.degenerateTrianglesDropped;
        totalSkipped += res.geometriesSkipped;
        totalWarnings += static_cast<int>(res.warnings.size());
        maxDepth = std::max(maxDepth, res.maxDepthSeen);

        std::cout << "  -> " << outBase.string() << ".gfmodel/.gfbin\n";
        std::cout << "     " << scene.meshes.size() << " mesh(es), " << scene.materials.size()
                  << " material(s), " << scene.TotalVertices() << " verts, "
                  << scene.TotalTriangles() << " tris";
        if (res.degenerateTrianglesDropped > 0) {
            std::cout << ", " << res.degenerateTrianglesDropped << " degenerate dropped";
        }
        std::cout << "\n";

        if (!res.warnings.empty()) {
            if (verbose) {
                for (const std::string& w : res.warnings) {
                    std::cout << "     warning: " << w << "\n";
                }
            } else {
                std::cout << "     " << res.warnings.size()
                          << " warning(s) (use -v to list)\n";
            }
        }
    }

    std::cout << "\n==================================================================\n";
    std::cout << "EXPORT SUMMARY: " << converted << "/" << files.size() << " file(s) converted\n";
    std::cout << "==================================================================\n";
    std::cout << "Vertices          : " << totalVertices << "\n";
    std::cout << "Triangles         : " << totalTriangles << "\n";
    std::cout << "Degenerate dropped: " << totalDegenerate << "\n";
    std::cout << "Geometries skipped: " << totalSkipped << "\n";
    std::cout << "Warnings          : " << totalWarnings << "\n";
    std::cout << "Max node depth    : " << maxDepth << "\n";

    const int texTotal = texturesResolved + texturesMissing;
    std::cout << "Textures resolved : " << texturesResolved << "/" << texTotal;
    if (texTotal > 0) {
        std::cout << " (" << (100 * texturesResolved / texTotal) << "%)";
    }
    std::cout << "\n";

    if (!unresolvedTextures.empty()) {
        std::cout << "\nUNRESOLVED TEXTURES (" << unresolvedTextures.size() << "):\n";
        for (const std::string& t : unresolvedTextures) {
            std::cout << "  " << t << "\n";
        }
    }
    if (!failures.empty()) {
        std::cout << "\nFAILED FILES (" << failures.size() << "):\n";
        for (const std::string& f : failures) {
            std::cout << "  " << f << "\n";
        }
    }

    return failures.empty() ? 0 : 2;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        PrintUsage();
        return 1;
    }

    const std::string command = argv[1];
    std::vector<std::string> rest(argv + 2, argv + argc);

    if (command == "dump") {
        return RunDump(rest);
    }
    if (command == "export") {
        return RunExport(rest);
    }
    if (command == "-h" || command == "--help") {
        PrintUsage();
        return 0;
    }

    // Compatibility with the Phase 1 invocation: a bare path means dump.
    std::vector<std::string> legacy(argv + 1, argv + argc);
    return RunDump(legacy);
}
