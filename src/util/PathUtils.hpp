#pragma once

#include <filesystem>
#include <string>

namespace gfnif {

/*! Path helpers for mirroring the input tree under the output root.
 *
 *  The exporter reproduces the relative shape of the input under --output, so
 *  `input/monster/model/M011.nif` becomes `out/monster/model/M011.gfmodel`
 *  plus the sibling `.gfbin`. */

/*! Lowercased extension of `p`, including the dot ("" when there is none). */
std::string LowerExtension(const std::filesystem::path& p);

/*! True if `p` ends in `.nif` (case-insensitive). */
bool IsNifFile(const std::filesystem::path& p);

/*! True if `p` ends in `.kf` (case-insensitive). */
bool IsKfFile(const std::filesystem::path& p);

/*! Path of `file` relative to `root`, falling back to the bare filename when
 *  the two share no common prefix (which would otherwise yield a `..`-laden
 *  path escaping the output root). */
std::filesystem::path RelativeUnder(const std::filesystem::path& file,
                                    const std::filesystem::path& root);

/*! The output base path for `nifFile`: `outRoot / <relative dir> / <stem>`,
 *  with no extension. The writer appends `.gfmodel` and `.gfbin`. */
std::filesystem::path OutputBaseFor(const std::filesystem::path& nifFile,
                                    const std::filesystem::path& inputRoot,
                                    const std::filesystem::path& outRoot);

/*! Creates the parent directory of `p` if missing. Returns false and fills
 *  `error` when the directory cannot be created -- an unwritable output root
 *  must be reported per file, not crash the run. */
bool EnsureParentDirectory(const std::filesystem::path& p, std::string& error);

/*! True when every output of `outBase` exists and is at least as new as
 *  `source`, i.e. the file can be skipped without --overwrite.
 *
 *  Both `.gfmodel` and `.gfbin` must be present: a half-written pair from an
 *  interrupted run is not up to date. A missing or unreadable timestamp counts
 *  as out of date, so the safe answer is always "convert it again". */
bool OutputIsUpToDate(const std::filesystem::path& source,
                      const std::filesystem::path& outBase);

/*! Forward slashes, for stable log and report text across platforms. */
std::string ToPosix(const std::filesystem::path& p);

} // namespace gfnif
