#include "pipeline/Orchestrator.hpp"

#include "cli/ProgressReporter.hpp"
#include "pipeline/ThreadPool.hpp"
#include "scanner/EntityScanner.hpp"
#include "util/Logger.hpp"
#include "util/PathUtils.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace fs = std::filesystem;

namespace Niflib {
// Declared, not defined, in any public header -- see HeaderNormalizer.cpp for
// the same approach applied to RegisterObjects(). Both are ordinary
// namespace-scope symbols with external linkage in a statically linked niflib.
void RegisterObjects();
extern bool g_objects_registered;
} // namespace Niflib

namespace gfnif {
namespace {

std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    std::ostringstream os;
                    os << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                       << static_cast<int>(static_cast<unsigned char>(c));
                    out += os.str();
                } else {
                    out += c;
                }
        }
    }
    return out;
}

const char* StatusName(JobStatus s) {
    switch (s) {
        case JobStatus::Converted: return "converted";
        case JobStatus::Skipped:   return "skipped";
        case JobStatus::DryRun:    return "dry-run";
        case JobStatus::Failed:    return "failed";
    }
    return "?";
}

} // namespace

void PrepareNiflibForConcurrentUse() {
    Niflib::RegisterObjects();
    // Flipping niflib's own guard is the part that actually closes the race:
    // without it, every worker's first ReadNifList would re-run RegisterObjects
    // and write the shared factory map while other workers read it.
    Niflib::g_objects_registered = true;
}

int RunPipeline(const Options& options, Logger& logger, RunSummary& summary) {
    const auto runStart = std::chrono::steady_clock::now();

    // ---- discover the work ------------------------------------------------
    std::vector<ScannedModel> models;
    fs::path inputRoot;

    if (options.singleFileMode) {
        const fs::path file = options.input;
        if (!IsNifFile(file)) {
            logger.Error("error: not a .nif file: " + ToPosix(file));
            return 1;
        }
        ScannedModel m;
        m.nifPath = file;
        // A lone file has no entity tree above it, so the output lands flat in
        // the output root -- matching the Phase 2 single-file behaviour.
        m.relativePath = file.filename();
        inputRoot = file.parent_path();
        std::error_code ec;
        const auto size = fs::file_size(file, ec);
        m.sizeBytes = ec ? 0 : size;
        models.push_back(std::move(m));
        logger.Info("single file mode: " + ToPosix(file));
    } else {
        inputRoot = options.input;
        const ScanResult scan = ScanEntities(inputRoot, options.entities);
        if (!scan.success) {
            logger.Error("error: " + scan.error);
            return 1;
        }
        models = scan.models;

        std::ostringstream os;
        os << "scanned " << ToPosix(inputRoot) << ": " << models.size() << " .nif file(s)";
        if (!scan.entityTypesFound.empty()) {
            os << " across " << scan.entityTypesFound.size() << " entity type(s) (";
            for (size_t i = 0; i < scan.entityTypesFound.size(); ++i) {
                os << (i ? ", " : "") << scan.entityTypesFound[i];
            }
            os << ")";
        }
        os << ", " << scan.withKf << " with a companion .kf";
        if (scan.filteredOut > 0) {
            os << ", " << scan.filteredOut << " excluded by --entities";
        }
        if (scan.orphanKf > 0) {
            os << ", " << scan.orphanKf << " orphan .kf";
        }
        logger.Info(os.str());
    }

    if (models.empty()) {
        logger.Error("error: no .nif files to process under " + ToPosix(options.input));
        return 1;
    }

    // The scan is sorted by path; --debug takes the first N of that stable
    // order so repeated debug runs look at the same files.
    if (options.debug && static_cast<int>(models.size()) > options.debugLimit) {
        logger.Info("--debug: limiting to the first " + std::to_string(options.debugLimit) +
                    " of " + std::to_string(models.size()) + " file(s)");
        models.resize(static_cast<std::size_t>(options.debugLimit));
    }

    const unsigned threads = EffectiveThreadCount(options);
    summary.threadsUsed = threads;
    summary.totalFiles = models.size();

    // ---- prepare the output root -----------------------------------------
    if (!options.dryRun) {
        std::error_code ec;
        fs::create_directories(options.output, ec);
        if (ec && !fs::exists(options.output)) {
            logger.Error("error: cannot create output root '" + options.output +
                         "': " + ec.message());
            return 1;
        }
    }

    JobOptions jobOptions;
    jobOptions.inputRoot = inputRoot;
    jobOptions.outputRoot = options.output;
    jobOptions.dryRun = options.dryRun;
    jobOptions.overwrite = options.overwrite;
    jobOptions.verifyStrips = options.verifyStrips;
    jobOptions.collectDebugLines = options.debug;

    // ---- run --------------------------------------------------------------
    PrepareNiflibForConcurrentUse();

    const ReportMode mode = options.debug    ? ReportMode::Verbose
                            : options.noverb ? ReportMode::Compact
                                             : ReportMode::Normal;
    ProgressReporter reporter(mode, models.size(), logger);
    logger.SetConsoleGuard(&ProgressReporter::GuardBefore, &ProgressReporter::GuardAfter,
                           &reporter);

    // Results are written by index, never appended, so the aggregate below is
    // identical no matter which worker finishes first.
    std::vector<JobResult> results(models.size());

    // Jobs are dispatched in scan order, one index at a time. A longest-first
    // dispatch was implemented and measured here, on the theory that the one
    // ~9 s model (chair/C067, whose malformed .kf niflib grinds through before
    // failing) should not be picked up last. It was not kept: over 4 interleaved
    // A/B pairs at 12 threads it was consistently no better and usually worse
    // (median 30.1 s against 29.2 s). The .nif size is a bad cost proxy here --
    // the expensive models are *small* .nif files with broken companion .kf --
    // and the atomic work queue already keeps every worker busy until the tail.
    // See docs/PHASE6_FINDINGS.md §5.
    ParallelFor(models.size(), threads, [&](std::size_t i) {
        const ScannedModel& m = models[i];
        const std::string rel = ToPosix(m.relativePath);

        reporter.BeginFile(rel, i);
        results[i] = RunConversionJob(m, jobOptions);
        const JobResult& r = results[i];

        // Per-file console/log text is emitted here rather than inside the job
        // so the job stays free of any I/O ordering concerns.
        for (const std::string& line : r.debugLines) {
            logger.Debug("    " + line);
        }

        std::ostringstream os;
        os << rel << ": " << StatusName(r.status);
        if (r.status == JobStatus::Converted || r.status == JobStatus::DryRun) {
            os << " (" << r.meshCount << " mesh, " << r.vertexCount << " v, " << r.triangleCount
               << " t";
            if (r.boneCount > 0) {
                os << ", " << r.boneCount << " bones";
            }
            if (r.clipCount > 0) {
                os << ", " << r.clipCount << " clips";
            }
            if (r.particleSystemCount > 0) {
                os << ", " << r.particleSystemCount << " psys";
            }
            os << ")";
        } else if (r.status == JobStatus::Failed) {
            os << ": " << r.error;
        }

        const bool failed = r.status == JobStatus::Failed;
        if (failed) {
            logger.Error("  FAIL " + rel + ": " + r.error);
        } else if (mode == ReportMode::Compact) {
            // Detail belongs in the log file only; the console shows the bar.
            logger.FileOnly(LogLevel::Info, os.str());
        }

        // Warnings are per-file detail: to the log always, to the console only
        // when the user asked for that level of noise.
        for (const std::string& w : r.extraction.warnings) {
            if (options.debug) {
                logger.Debug("    warning: " + w);
            } else {
                logger.FileOnly(LogLevel::Warning, rel + ": " + w);
            }
        }

        reporter.EndFile(rel, (mode == ReportMode::Compact || failed) ? std::string() : os.str(),
                         failed);
    });

    reporter.Finish();
    logger.SetConsoleGuard(nullptr, nullptr, nullptr);

    // ---- aggregate (single-threaded, in index order) ----------------------
    for (std::size_t i = 0; i < results.size(); ++i) {
        const JobResult& r = results[i];
        const std::string rel = ToPosix(models[i].relativePath);

        switch (r.status) {
            case JobStatus::Converted: ++summary.converted; break;
            case JobStatus::Skipped:   ++summary.skipped; break;
            case JobStatus::DryRun:    ++summary.dryRun; break;
            case JobStatus::Failed:
                ++summary.failed;
                summary.failures.push_back(rel + ": " + r.error);
                continue;
        }

        summary.vertices += r.vertexCount;
        summary.triangles += r.triangleCount;
        summary.bones += r.boneCount;
        summary.clips += r.clipCount;
        summary.particleSystems += r.particleSystemCount;
        summary.bytesWritten += r.bytesWritten;
        summary.texturesResolved += r.texturesResolved;
        summary.texturesMissing += r.texturesMissing;
        if (r.skeletonCount > 0) ++summary.filesWithSkeleton;
        if (r.clipCount > 0) ++summary.filesWithAnimation;
        if (r.particleSystemCount > 0) ++summary.filesWithParticles;

        summary.slowest.emplace_back(r.seconds, rel);
    }

    // Stable ordering: slowest first, ties broken by path so the list cannot
    // vary between runs.
    std::sort(summary.slowest.begin(), summary.slowest.end(),
              [](const auto& a, const auto& b) {
                  if (a.first != b.first) return a.first > b.first;
                  return a.second < b.second;
              });
    if (summary.slowest.size() > 10) {
        summary.slowest.resize(10);
    }

    summary.wallSeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - runStart).count();

    return summary.failed == 0 ? 0 : 2;
}

bool WriteJsonReport(const std::string& path, const Options& options, const RunSummary& summary,
                     std::string& error) {
    std::string dirError;
    if (!EnsureParentDirectory(path, dirError)) {
        error = dirError;
        return false;
    }
    std::ofstream out(path, std::ios::out | std::ios::trunc);
    if (!out) {
        error = "cannot open report file '" + path + "'";
        return false;
    }

    out << "{\n";
    out << "  \"tool\": \"gfnif-export\",\n";
    out << "  \"formatVersion\": 3,\n";
    out << "  \"input\": \"" << JsonEscape(options.input) << "\",\n";
    out << "  \"output\": \"" << JsonEscape(options.output) << "\",\n";
    out << "  \"dryRun\": " << (options.dryRun ? "true" : "false") << ",\n";
    out << "  \"threads\": " << summary.threadsUsed << ",\n";
    out << "  \"wallSeconds\": " << std::fixed << std::setprecision(3) << summary.wallSeconds
        << ",\n";
    out << std::defaultfloat;
    out << "  \"counts\": {\n";
    out << "    \"total\": " << summary.totalFiles << ",\n";
    out << "    \"converted\": " << summary.converted << ",\n";
    out << "    \"failed\": " << summary.failed << ",\n";
    out << "    \"skipped\": " << summary.skipped << ",\n";
    out << "    \"dryRun\": " << summary.dryRun << "\n";
    out << "  },\n";
    out << "  \"totals\": {\n";
    out << "    \"vertices\": " << summary.vertices << ",\n";
    out << "    \"triangles\": " << summary.triangles << ",\n";
    out << "    \"bones\": " << summary.bones << ",\n";
    out << "    \"animationClips\": " << summary.clips << ",\n";
    out << "    \"particleSystems\": " << summary.particleSystems << ",\n";
    out << "    \"filesWithSkeleton\": " << summary.filesWithSkeleton << ",\n";
    out << "    \"filesWithAnimation\": " << summary.filesWithAnimation << ",\n";
    out << "    \"filesWithParticles\": " << summary.filesWithParticles << ",\n";
    out << "    \"bytesWritten\": " << summary.bytesWritten << ",\n";
    out << "    \"texturesResolved\": " << summary.texturesResolved << ",\n";
    out << "    \"texturesMissing\": " << summary.texturesMissing << "\n";
    out << "  },\n";

    out << "  \"failures\": [\n";
    for (std::size_t i = 0; i < summary.failures.size(); ++i) {
        out << "    \"" << JsonEscape(summary.failures[i]) << "\""
            << (i + 1 < summary.failures.size() ? "," : "") << "\n";
    }
    out << "  ],\n";

    out << "  \"slowestFiles\": [\n";
    for (std::size_t i = 0; i < summary.slowest.size(); ++i) {
        out << "    { \"path\": \"" << JsonEscape(summary.slowest[i].second)
            << "\", \"seconds\": " << std::fixed << std::setprecision(3)
            << summary.slowest[i].first << " }" << (i + 1 < summary.slowest.size() ? "," : "")
            << "\n";
        out << std::defaultfloat;
    }
    out << "  ]\n";
    out << "}\n";

    if (!out) {
        error = "write failed for report file '" + path + "'";
        return false;
    }
    return true;
}

} // namespace gfnif
