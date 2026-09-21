#pragma once

#include "nif/MeshExtractor.hpp"
#include "scanner/EntityScanner.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace gfnif {

/*! Why a job produced no output. */
enum class JobStatus {
    Converted,  //!< .gfmodel + .gfbin written
    Skipped,    //!< output already up to date (no --overwrite)
    DryRun,     //!< would have been converted, but --dry-run
    Failed      //!< extraction or writing failed; counted and logged, never fatal
};

/*! The outcome of converting one .nif.
 *
 *  Indexed by job number and written only by the worker that owns that index,
 *  so no locking is needed and the aggregate is independent of the order the
 *  workers happened to finish in. */
struct JobResult {
    JobStatus status = JobStatus::Failed;
    std::string error;

    std::filesystem::path nifPath;
    std::filesystem::path outBase;

    /*! Per-file detail lines, emitted by the reporter after the job returns so
     *  that a --debug log reads in job order rather than completion order. */
    std::vector<std::string> debugLines;

    /*! Statistics merged into the run totals. Only meaningful when status is
     *  Converted. */
    ExtractionResult extraction;
    std::size_t meshCount = 0;
    std::size_t materialCount = 0;
    std::size_t vertexCount = 0;
    std::size_t triangleCount = 0;
    std::size_t boneCount = 0;
    std::size_t clipCount = 0;
    std::size_t particleSystemCount = 0;
    std::size_t skeletonCount = 0;
    /*! Bytes written (.gfmodel + .gfbin), for the throughput measurement. */
    std::uintmax_t bytesWritten = 0;
    /*! Texture references that did and did not resolve. */
    int texturesResolved = 0;
    int texturesMissing = 0;
    std::vector<std::string> unresolvedTextures;
    bool hadCompanionKf = false;
    /*! Wall time for this file, used to find the slowest files in a run. */
    double seconds = 0.0;
};

/*! What the job needs that is not per-file. Read-only and shared by all workers. */
struct JobOptions {
    std::filesystem::path inputRoot;
    std::filesystem::path outputRoot;
    bool dryRun = false;
    bool overwrite = false;
    bool verifyStrips = false;
    /*! Collect the per-file detail lines. Off outside --debug so a 2825-file
     *  run does not retain thousands of strings it will never print. */
    bool collectDebugLines = false;
};

/*! Converts one scanned model. Never throws: any exception escaping the
 *  extractor becomes a Failed result, because one bad file must not take down
 *  a worker and with it the whole run. */
JobResult RunConversionJob(const ScannedModel& model, const JobOptions& options);

} // namespace gfnif
