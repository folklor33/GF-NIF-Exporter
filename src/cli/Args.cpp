#include "cli/Args.hpp"

#include "pipeline/ThreadPool.hpp"
#include "scanner/EntityScanner.hpp"

#include "CLI11.hpp"

#include <filesystem>

namespace fs = std::filesystem;

namespace gfnif {

unsigned EffectiveThreadCount(const Options& options) {
    // --debug is defined as sequential: its whole purpose is a log that reads in
    // file order. It therefore wins over --threads (with a warning at parse time).
    if (options.debug) {
        return 1;
    }
    return options.threads == 0 ? DefaultThreadCount() : options.threads;
}

ParseOutcome ParseArgs(int argc, char** argv, Options& options,
                       std::vector<std::string>& warnings) {
    ParseOutcome outcome;

    CLI::App app{"gfnif-export -- Grand Fantasia .nif/.kf to .gfmodel/.gfbin converter"};
    app.set_version_flag("--version", "gfnif-export 6.0 (format version 3)");

    std::string entitiesCsv;

    app.add_option("-i,--input", options.input,
                   "Input root holding the entity directories (monster/, npc/, ...), "
                   "or a single .nif")
        ->required();
    app.add_option("-o,--output", options.output, "Output root; the input tree is mirrored here")
        ->capture_default_str();
    app.add_option("--entities", entitiesCsv,
                   "Restrict to these entity types, comma separated (e.g. monster,npc)");
    app.add_option("-t,--threads", options.threads,
                   "Worker threads (default: hardware_concurrency)");
    app.add_flag("--debug", options.debug,
                 "Verbose per-file logging, sequential, limited to --debug-limit files");
    app.add_option("--debug-limit", options.debugLimit, "Max files processed in --debug")
        ->capture_default_str();
    app.add_flag("--noverb", options.noverb, "Single repainting progress line, full parallelism");
    app.add_flag("--dry-run", options.dryRun, "Report what would be done, write nothing");
    app.add_flag("--overwrite", options.overwrite,
                 "Re-convert even when the output is newer than the source");
    app.add_option("--log-file", options.logFile,
                   "Detailed log, written in every mode including --noverb");
    app.add_option("--report", options.report, "Write a JSON run report to this path");
    app.add_flag("--verify-strips", options.verifyStrips,
                 "Cross-check de-striping against niflib's own expansion (slow)");

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        // CLI11 prints help/version through this path too, where exit code is 0.
        outcome.shouldExit = true;
        outcome.exitCode = app.exit(e);
        return outcome;
    }

    if (!entitiesCsv.empty()) {
        options.entities = ParseEntityList(entitiesCsv);
        const std::string unknown = FirstUnknownEntity(options.entities);
        if (!unknown.empty()) {
            std::string known;
            for (const std::string& t : KnownEntityTypes()) {
                known += (known.empty() ? "" : ", ") + t;
            }
            outcome.shouldExit = true;
            outcome.exitCode = 1;
            outcome.error = "unknown entity type '" + unknown + "'; known types are: " + known;
            return outcome;
        }
    }

    if (options.debug && options.noverb) {
        // Genuinely contradictory: one wants a line-by-line log, the other wants
        // no per-file console output at all. Refuse rather than pick.
        outcome.shouldExit = true;
        outcome.exitCode = 1;
        outcome.error = "--debug and --noverb are mutually exclusive";
        return outcome;
    }

    if (options.debug && options.threads > 1) {
        warnings.push_back("--debug forces sequential processing; ignoring --threads " +
                           std::to_string(options.threads));
    }
    if (options.debugLimit <= 0) {
        outcome.shouldExit = true;
        outcome.exitCode = 1;
        outcome.error = "--debug-limit must be at least 1";
        return outcome;
    }
    if (options.threads > 256) {
        outcome.shouldExit = true;
        outcome.exitCode = 1;
        outcome.error = "--threads above 256 is refused as a typo guard";
        return outcome;
    }

    if (options.dryRun && options.overwrite) {
        // Not contradictory, just redundant: --dry-run writes nothing either
        // way. Say so and continue, since --overwrite still changes which files
        // the dry run reports as work to do.
        warnings.push_back(
            "--dry-run writes nothing; --overwrite only affects which files are listed");
    }

    std::error_code ec;
    if (!fs::exists(options.input, ec) || ec) {
        outcome.shouldExit = true;
        outcome.exitCode = 1;
        outcome.error = "input path not found: " + options.input;
        return outcome;
    }
    options.singleFileMode = !fs::is_directory(options.input, ec) || ec;

    return outcome;
}

} // namespace gfnif
