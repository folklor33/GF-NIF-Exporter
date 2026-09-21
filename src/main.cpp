// gfnif-export -- CLI entry point.
//
// Phase 1 added `dump`: parse a .nif/.kf and print its block structure.
// Phase 2 added `export`: convert a .nif's geometry and materials.
// Phase 6 makes the flag-driven pipeline the primary interface: scan an entity
// tree, convert in parallel, mirror the tree, and report.
//
//   gfnif-export --input <dir> --output <dir> [options]   the pipeline
//   gfnif-export dump <file|dir> [--summary]              phase 1 diagnostic
//   gfnif-export export <file|dir> -o <dir> ...           phase 2 diagnostic
//
// The two subcommands are kept because every earlier phase was debugged through
// them, and `--input` pointing at a single file is the supported way to run one
// model through the real pipeline.

#include "cli/Args.hpp"
#include "nif/NifDumper.hpp"
#include "pipeline/Orchestrator.hpp"
#include "util/Logger.hpp"
#include "util/PathUtils.hpp"

#include <algorithm>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::vector<fs::path> CollectNifLike(const fs::path& target) {
    std::vector<fs::path> files;
    std::error_code ec;
    if (fs::is_directory(target, ec)) {
        for (auto it = fs::recursive_directory_iterator(target, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) {
                break;
            }
            if (it->is_regular_file(ec) &&
                (gfnif::IsNifFile(it->path()) || gfnif::IsKfFile(it->path()))) {
                files.push_back(it->path());
            }
        }
        std::sort(files.begin(), files.end());
    } else if (gfnif::IsNifFile(target) || gfnif::IsKfFile(target)) {
        files.push_back(target);
    }
    return files;
}

int RunDump(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cerr << "usage: gfnif-export dump <file|dir> [--summary]\n";
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

    const std::vector<fs::path> files = CollectNifLike(target);
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

void PrintSummary(const gfnif::Options& options, const gfnif::RunSummary& s) {
    std::ostringstream os;
    os << "\n==================================================================\n";
    os << "EXPORT SUMMARY: " << s.converted << "/" << s.totalFiles << " file(s) converted\n";
    os << "==================================================================\n";
    if (s.skipped > 0) {
        os << "Skipped (up to date): " << s.skipped << "\n";
    }
    if (s.dryRun > 0) {
        os << "Would convert       : " << s.dryRun << " (dry run, nothing written)\n";
    }
    os << "Failed              : " << s.failed << "\n";
    os << "Vertices            : " << s.vertices << "\n";
    os << "Triangles           : " << s.triangles << "\n";
    os << "Files with skeleton : " << s.filesWithSkeleton << " (" << s.bones << " bones)\n";
    os << "Files with animation: " << s.filesWithAnimation << " (" << s.clips << " clips)\n";
    os << "Files with particles: " << s.filesWithParticles << " (" << s.particleSystems
       << " systems)\n";

    const int texTotal = s.texturesResolved + s.texturesMissing;
    os << "Textures resolved   : " << s.texturesResolved << "/" << texTotal;
    if (texTotal > 0) {
        os << " (" << (100 * s.texturesResolved / texTotal) << "%)";
    }
    os << "\n";

    os << "\n--- performance --------------------------------------------------\n";
    os << "Threads             : " << s.threadsUsed << "\n";
    os << "Wall time           : " << std::fixed << std::setprecision(2) << s.wallSeconds
       << " s\n";
    if (s.wallSeconds > 0.0) {
        os << "Throughput          : " << std::setprecision(1)
           << (static_cast<double>(s.totalFiles) / s.wallSeconds) << " file/s";
        if (s.bytesWritten > 0) {
            os << ", " << std::setprecision(1)
               << (static_cast<double>(s.bytesWritten) / (1024.0 * 1024.0) / s.wallSeconds)
               << " MB/s written";
        }
        os << "\n";
    }
    if (s.bytesWritten > 0) {
        os << "Bytes written       : " << std::setprecision(2)
           << (static_cast<double>(s.bytesWritten) / (1024.0 * 1024.0 * 1024.0)) << " GB\n";
    }
    os << std::defaultfloat;

    if (!s.slowest.empty()) {
        os << "Slowest files       :\n";
        for (const auto& p : s.slowest) {
            os << "  " << std::fixed << std::setprecision(2) << p.first << " s  " << p.second
               << "\n";
        }
        os << std::defaultfloat;
    }

    if (!s.failures.empty()) {
        os << "\nFAILED FILES (" << s.failures.size() << "):\n";
        for (const std::string& f : s.failures) {
            os << "  " << f << "\n";
        }
    }

    std::cout << os.str();
    (void)options;
}

int RunPipelineCommand(int argc, char** argv) {
    gfnif::Options options;
    std::vector<std::string> warnings;
    const gfnif::ParseOutcome outcome = gfnif::ParseArgs(argc, argv, options, warnings);
    if (outcome.shouldExit) {
        if (!outcome.error.empty()) {
            std::cerr << "error: " << outcome.error << "\n";
        }
        return outcome.exitCode;
    }

    gfnif::Logger logger;
    logger.SetConsoleLevel(options.debug ? gfnif::LogLevel::Debug
                           : options.noverb ? gfnif::LogLevel::Error
                                            : gfnif::LogLevel::Info);

    if (!options.logFile.empty()) {
        std::string dirError;
        if (!gfnif::EnsureParentDirectory(options.logFile, dirError)) {
            std::cerr << "error: " << dirError << "\n";
            return 1;
        }
        std::string logError;
        if (!logger.OpenFile(options.logFile, logError)) {
            std::cerr << "error: " << logError << "\n";
            return 1;
        }
    }

    for (const std::string& w : warnings) {
        logger.Warning("warning: " + w);
    }

    gfnif::RunSummary summary;
    const int code = gfnif::RunPipeline(options, logger, summary);
    if (code == 1) {
        return code; // setup failure; the pipeline already reported it
    }

    PrintSummary(options, summary);

    if (!options.report.empty()) {
        std::string reportError;
        if (!gfnif::WriteJsonReport(options.report, options, summary, reportError)) {
            std::cerr << "error: " << reportError << "\n";
            return 1;
        }
        std::cout << "\nReport written to " << options.report << "\n";
    }

    logger.Flush();
    return code;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "gfnif-export: missing arguments (try --help)\n";
        return 1;
    }

    const std::string first = argv[1];
    if (first == "dump") {
        return RunDump(std::vector<std::string>(argv + 2, argv + argc));
    }

    // `export <path> ...` was the Phase 2 spelling and is documented in the
    // findings of phases 2 to 5. Rewrite it onto the pipeline so those
    // invocations keep working: the bare positional becomes --input, and
    // --input-root is dropped (the pipeline derives it from --input).
    if (first == "export") {
        std::vector<std::string> owned;
        owned.push_back(argv[0]);
        bool tookInput = false;
        for (int i = 2; i < argc; ++i) {
            const std::string a = argv[i];
            if (a == "--input-root" && i + 1 < argc) {
                ++i; // the pipeline mirrors from --input; this is now implicit
                continue;
            }
            // The Phase 2 form always led with the path, before any option.
            if (!tookInput && !a.empty() && a[0] != '-') {
                owned.push_back("--input=" + a);
                tookInput = true;
                continue;
            }
            owned.push_back(a);
        }
        std::vector<char*> forwarded;
        forwarded.reserve(owned.size());
        for (std::string& s : owned) {
            forwarded.push_back(s.data());
        }
        return RunPipelineCommand(static_cast<int>(forwarded.size()), forwarded.data());
    }

    // Everything else is the Phase 6 pipeline, including --help and --version,
    // which CLI11 handles.
    return RunPipelineCommand(argc, argv);
}
