#pragma once

#include <iosfwd>
#include <map>
#include <string>

namespace gfnif {

/*! Aggregate counts of the block types seen across one or more files.
 *  Used to build the corpus-wide inventory for the Phase 1 findings doc. */
using BlockTypeTally = std::map<std::string, int>;

/*! Parses `path` (a .nif or .kf) with niflib and writes a NifSkope-style
 *  "Block List" to `out`: every block with its file index, type and name,
 *  followed by the block tree reconstructed from the Ref/Ptr links.
 *
 *  \param path  file to read
 *  \param out   stream that receives the human-readable dump
 *  \param tally if non-null, block type occurrences are added to it
 *  \return true if the file parsed successfully */
bool DumpNifFile(const std::string& path, std::ostream& out, BlockTypeTally* tally = nullptr);

} // namespace gfnif
