#pragma once

#include <string>
#include <vector>

namespace gfnif {

/*! Everything the CLI accepts, after parsing and validation. */
struct Options {
    std::string input;
    std::string output = "out";
    std::vector<std::string> entities;
    unsigned threads = 0; //!< 0 = decide from hardware_concurrency
    bool debug = false;
    int debugLimit = 10;
    bool noverb = false;
    bool dryRun = false;
    bool overwrite = false;
    std::string logFile;
    std::string report;
    bool verifyStrips = false;

    /*! Set when --input names a file rather than a directory: the Phase 2
     *  single-file diagnostic mode, kept because it is how every earlier phase
     *  was debugged. */
    bool singleFileMode = false;
};

/*! Outcome of parsing. `exitCode` is what main should return when
 *  `shouldExit` is set -- 0 for --help/--version, non-zero for a usage error. */
struct ParseOutcome {
    bool shouldExit = false;
    int exitCode = 0;
    std::string error;
};

/*! Parses argv into `options`.
 *
 *  Rejects combinations that cannot be honoured rather than silently picking
 *  one side; see the implementation for which pairs are errors and which are
 *  merely resolved with a warning. */
ParseOutcome ParseArgs(int argc, char** argv, Options& options,
                       std::vector<std::string>& warnings);

/*! Threads actually used, after applying --debug (always 1) and the default. */
unsigned EffectiveThreadCount(const Options& options);

} // namespace gfnif
