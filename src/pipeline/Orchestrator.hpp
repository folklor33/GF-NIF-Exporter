#pragma once

#include "cli/Args.hpp"
#include "pipeline/ConversionJob.hpp"

#include <string>
#include <vector>

namespace gfnif {

class Logger;

/*! Corpus-wide totals for the final summary and the JSON report. */
struct RunSummary {
    std::size_t totalFiles = 0;
    std::size_t converted = 0;
    std::size_t failed = 0;
    std::size_t skipped = 0;
    std::size_t dryRun = 0;

    std::size_t vertices = 0;
    std::size_t triangles = 0;
    std::size_t bones = 0;
    std::size_t clips = 0;
    std::size_t particleSystems = 0;
    std::size_t filesWithSkeleton = 0;
    std::size_t filesWithAnimation = 0;
    std::size_t filesWithParticles = 0;
    std::uintmax_t bytesWritten = 0;

    int texturesResolved = 0;
    int texturesMissing = 0;

    double wallSeconds = 0.0;
    unsigned threadsUsed = 1;

    /*! "relative/path: reason", in scan order so two runs agree. */
    std::vector<std::string> failures;
    /*! Slowest files, for the performance section of the findings. */
    std::vector<std::pair<double, std::string>> slowest;
};

/*! Makes niflib's lazy block-type registration safe to use from many threads.
 *
 *  niflib registers its 441 block factories on the first ReadNifList call,
 *  guarded by a plain non-atomic bool. Concurrent first calls would both
 *  re-register while other threads read the same std::map -- a data race.
 *  Calling this once before any worker starts performs the registration and
 *  flips niflib's own flag, so ReadNifList never writes the map again and every
 *  worker only ever reads it. See docs/PHASE6_FINDINGS.md. */
void PrepareNiflibForConcurrentUse();

/*! Runs the whole pipeline and returns the process exit code:
 *  0 when every file converted, 2 when any failed, 1 on a setup error. */
int RunPipeline(const Options& options, Logger& logger, RunSummary& summary);

/*! Writes the JSON run report. Returns false and fills `error` on failure. */
bool WriteJsonReport(const std::string& path, const Options& options, const RunSummary& summary,
                     std::string& error);

} // namespace gfnif
