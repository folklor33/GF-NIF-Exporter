#pragma once

#include <string>
#include <vector>

namespace gfnif {

/*! What NormalizeNifHeader had to change to make niflib accept a file.
 *
 *  Phase 1 found two GF-specific header anomalies, both on the same file
 *  (monster/model/M903.nif). They are reported separately so that a corpus-wide
 *  run can tell whether they really always co-occur -- the Phase 1 findings flag
 *  that as an open question, because the version restamp is gated on the
 *  truncation marker and that gate is only safe while the two travel together. */
struct HeaderNormalizationReport {
    /*! The header string lacked its ", Version x.x.x.x" suffix and was rebuilt
     *  from the version dword that follows it. */
    bool repairedTruncatedHeaderString = false;
    /*! The file claimed 20.3.0.9 but is laid out as 20.2.0.8, and was restamped.
     *  Only ever set when repairedTruncatedHeaderString is also set. */
    bool restampedVersion = false;
    /*! Version dword as found on disk, before any restamp. 0 if unreadable. */
    unsigned int originalVersion = 0;
    /*! The header string as found on disk, up to but excluding the newline. */
    std::string originalHeaderString;

    bool AnyChange() const { return repairedTruncatedHeaderString || restampedVersion; }
};

/*! Formats a NIF version dword as "20.2.0.8". */
std::string FormatNifVersion(unsigned int version);

/*! Rewrites `bytes` in place so niflib can parse it, and describes what changed.
 *
 *  `bytes` is the whole file read in binary. Files that need no change are left
 *  untouched and the report comes back all-false. The file on disk is never
 *  modified -- callers pass an in-memory copy.
 *
 *  See the implementation for why each fix-up exists and why the version
 *  restamp is gated on the truncation marker. */
void NormalizeNifHeader(std::string& bytes, HeaderNormalizationReport& report);

/*! Reads the whole file at `path` into `bytes`. Returns false if unreadable. */
bool ReadWholeFile(const std::string& path, std::string& bytes);

/*! Reads the block-type name table out of a NIF header without invoking niflib.
 *
 *  Returns false when the header cannot be walked (not a NIF, truncated, or a
 *  version whose header layout differs). `bytes` must already have been passed
 *  through NormalizeNifHeader. */
bool ReadBlockTypeNames(const std::string& bytes, std::vector<std::string>& outTypes);

/*! Names a block type present in the file that niflib cannot construct, or ""
 *  when every type is supported.
 *
 *  niflib throws on an unregistered block type, but the throw happens with a
 *  partially linked object graph in flight and the ensuing cleanup dereferences
 *  freed memory, taking the process down with an access violation rather than a
 *  catchable exception. Checking the header's own type table first lets the
 *  caller skip such a file cleanly. See PHASE2_FINDINGS.md. */
std::string FindUnsupportedBlockType(const std::string& bytes);

} // namespace gfnif
