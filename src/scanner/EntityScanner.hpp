#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace gfnif {

/*! One discovered model and everything the scanner knows about it up front. */
struct ScannedModel {
    /*! Absolute (or as-given) path to the .nif. */
    std::filesystem::path nifPath;
    /*! Path relative to the input root, used to mirror the output tree. */
    std::filesystem::path relativePath;
    /*! Entity type directory this model lives under ("monster", "npc", ...),
     *  or "" when the file is not under a recognised type directory. */
    std::string entityType;
    /*! Companion `<type>/animation/NAME.kf`, empty when there is none.
     *
     *  Resolved here only so the scan can report pairing coverage; extraction
     *  re-resolves it internally (MeshExtractor::ResolveCompanionKf), and that
     *  remains the authority. Duplicating the lookup keeps the scanner free of
     *  any influence on what gets exported. */
    std::filesystem::path kfPath;
    /*! Size of the .nif in bytes, used to order the work queue largest-first. */
    std::uintmax_t sizeBytes = 0;
};

/*! What a scan found, including the reasons it rejected things. */
struct ScanResult {
    bool success = false;
    std::string error;

    std::vector<ScannedModel> models;

    /*! Entity type directories actually seen under the input root. */
    std::vector<std::string> entityTypesFound;
    /*! .nif files excluded by the --entities filter. */
    int filteredOut = 0;
    /*! Models that resolved a companion .kf. */
    int withKf = 0;
    /*! .kf files with no same-named .nif. The Phase 1 convention says there are
     *  none; a non-zero count here means the corpus changed. */
    int orphanKf = 0;
};

/*! The entity type directories the tool recognises, per docs/NAMING_CONVENTIONS.md. */
const std::vector<std::string>& KnownEntityTypes();

/*! Walks `inputRoot` for .nif files.
 *
 *  `entityFilter` restricts the walk to those entity type directories; an empty
 *  filter accepts everything, including files outside any known type directory
 *  (so a flat or partial tree still scans). Results are sorted by relative path
 *  so a run's job list is deterministic regardless of directory iteration
 *  order, which is what makes two runs comparable. */
ScanResult ScanEntities(const std::filesystem::path& inputRoot,
                        const std::vector<std::string>& entityFilter);

/*! Splits a comma-separated --entities value, lowercased and trimmed. */
std::vector<std::string> ParseEntityList(const std::string& csv);

/*! Names an entity in `filter` that is not a known type, or "" if all are
 *  known. Lets the CLI reject a typo instead of silently scanning nothing. */
std::string FirstUnknownEntity(const std::vector<std::string>& filter);

} // namespace gfnif
