#include "nif/NifDumper.hpp"

#include "niflib.h"
#include "obj/NiObject.h"
#include "obj/NiObjectNET.h"
#include "Type.h"

#include <algorithm>
#include <exception>
#include <fstream>
#include <ostream>
#include <sstream>
#include <set>
#include <unordered_map>
#include <vector>

namespace gfnif {
namespace {

using Niflib::NiObject;
using Niflib::NiObjectRef;

/*! Formats the NIF version dword the way the header string spells it. */
std::string FormatVersion(unsigned int v) {
    return std::to_string((v >> 24) & 0xFF) + "." + std::to_string((v >> 16) & 0xFF) + "." +
           std::to_string((v >> 8) & 0xFF) + "." + std::to_string(v & 0xFF);
}

std::string TypeNameOf(const NiObject* obj) {
    return obj ? obj->GetType().GetTypeName() : std::string("<null>");
}

/*! NiObjectNET carries a name; other blocks do not. Returns "" when unnamed. */
std::string NameOf(NiObject* obj) {
    if (auto* net = dynamic_cast<Niflib::NiObjectNET*>(obj)) {
        return net->GetName();
    }
    return std::string();
}

/*! Renders "12 [NiNode] Bip01" — index, type, and name when present. */
std::string Label(NiObject* obj, const std::unordered_map<NiObject*, int>& index) {
    std::string s;
    auto it = index.find(obj);
    s += (it != index.end()) ? std::to_string(it->second) : std::string("?");
    s += " [" + TypeNameOf(obj) + "]";
    const std::string name = NameOf(obj);
    if (!name.empty()) {
        s += " \"" + name + "\"";
    }
    return s;
}

/*! Recursively prints the block tree reached from `obj` via its Refs.
 *
 *  `prefix` is the indentation carried down from the ancestors; `is_root` marks
 *  the top-level call, which prints no connector. `last` tells whether `obj` is
 *  the final child of its parent, which selects the connector glyph.
 *
 *  Ptrs (non-owning back-references, e.g. a NiSkinInstance's skeleton root or
 *  bone list) are listed inline rather than descended into: they routinely point
 *  back up the tree, and following them would not terminate. `visited` guards
 *  against the DAG sharing that is normal in NIF files (a NiSourceTexture reused
 *  by several materials, for instance) so shared subtrees print once. */
void PrintTree(NiObject* obj,
               const std::unordered_map<NiObject*, int>& index,
               std::set<NiObject*>& visited,
               const std::string& prefix,
               bool last,
               bool is_root,
               std::ostream& out) {
    if (!obj) {
        return;
    }

    out << prefix;
    if (!is_root) {
        out << (last ? "`-- " : "|-- ");
    }
    out << Label(obj, index);

    if (!visited.insert(obj).second) {
        // Shared block: already expanded at its first occurrence.
        out << "  (see above)\n";
        return;
    }

    // Non-owning links, shown but not traversed.
    std::list<NiObject*> ptrs = obj->GetPtrs();
    if (!ptrs.empty()) {
        out << "  -> ptr:";
        for (NiObject* p : ptrs) {
            auto it = index.find(p);
            out << " " << ((it != index.end()) ? std::to_string(it->second) : std::string("?"));
        }
    }
    out << "\n";

    std::vector<NiObject*> children;
    for (const NiObjectRef& r : obj->GetRefs()) {
        if (r != NULL) {
            children.push_back(static_cast<NiObject*>(r));
        }
    }

    const std::string child_prefix =
        is_root ? std::string() : prefix + (last ? "    " : "|   ");
    for (size_t i = 0; i < children.size(); ++i) {
        PrintTree(children[i], index, visited, child_prefix, i + 1 == children.size(), false, out);
    }
}

/*! Reads `path` into `bytes`. Returns false if the file cannot be read. */
bool ReadWholeFile(const std::string& path, std::string& bytes) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    bytes = buf.str();
    return true;
}

/*! Offset of the version dword: it follows the NUL/newline-terminated header
 *  string. Returns 0 if the file does not look like a NIF at all. */
size_t VersionOffset(const std::string& bytes) {
    const size_t nl = bytes.find('\n');
    if (nl == std::string::npos || nl + 5 > bytes.size()) {
        return 0;
    }
    return nl + 1;
}

unsigned int ReadVersionDword(const std::string& bytes, size_t off) {
    return static_cast<unsigned char>(bytes[off]) |
           (static_cast<unsigned char>(bytes[off + 1]) << 8) |
           (static_cast<unsigned char>(bytes[off + 2]) << 16) |
           (static_cast<unsigned char>(bytes[off + 3]) << 24);
}

void WriteVersionDword(std::string& bytes, size_t off, unsigned int v) {
    bytes[off + 0] = static_cast<char>(v & 0xFF);
    bytes[off + 1] = static_cast<char>((v >> 8) & 0xFF);
    bytes[off + 2] = static_cast<char>((v >> 16) & 0xFF);
    bytes[off + 3] = static_cast<char>((v >> 24) & 0xFF);
}

constexpr unsigned int kVer20_2_0_8 = 0x14020008;
constexpr unsigned int kVer20_3_0_9 = 0x14030009;

} // namespace

bool DumpNifFile(const std::string& path, std::ostream& out, BlockTypeTally* tally) {
    out << "==================================================================\n";
    out << "FILE: " << path << "\n";
    out << "==================================================================\n";

    std::string bytes;
    if (!ReadWholeFile(path, bytes)) {
        out << "  !! CANNOT OPEN FILE\n\n";
        return false;
    }

    // --- Grand Fantasia truncated-header fix-up -----------------------------
    //
    // Some GF files open with the bare header string "Gamebryo File Format",
    // omitting the ", Version x.x.x.x" suffix every other file carries. niflib
    // cannot read those: NifStream(HeaderString&) matches the 20-character
    // "Gamebryo File Format" prefix, hard-codes ver_start = 30, then calls
    // header.substr(30) on a 20-character string, which throws
    // std::out_of_range("invalid string position") (NIF_IO.cpp:395-405).
    //
    // The version dword that follows the string is intact, so we read it and
    // splice a well-formed ", Version x.x.x.x" suffix into the in-memory copy.
    // niflib then parses the header string normally. The file on disk is never
    // modified.
    bool repaired_header = false;
    const size_t nl = bytes.find('\n');
    if (nl != std::string::npos && nl + 5 <= bytes.size() &&
        bytes.compare(0, 20, "Gamebryo File Format") == 0 && nl == 20) {
        const unsigned int dword = ReadVersionDword(bytes, nl + 1);
        bytes = "Gamebryo File Format, Version " + FormatVersion(dword) + "\n" +
                bytes.substr(nl + 1);
        repaired_header = true;
    }

    // A file with a truncated header string is additionally mis-stamped: it
    // claims 20.3.0.9 while its header is physically laid out as 20.2.0.8. Per
    // nifxml, versions above 20.2.0.7 carry two extra header fields -- a
    // per-block size array and a string table -- that these files do not have,
    // so niflib would misread the block-type table as those fields. Restamping
    // to 20.2.0.8 selects the layout the bytes actually use.
    //
    // This is deliberately gated on `repaired_header`: the corpus also contains
    // genuine 20.3.0.9 .kf files that DO carry the string table and parse
    // correctly as-is. Restamping those would corrupt them, so only files
    // carrying the truncated-header marker are touched.
    bool restamped = false;
    if (repaired_header) {
        const size_t voff = VersionOffset(bytes);
        if (voff != 0 && ReadVersionDword(bytes, voff) == kVer20_3_0_9) {
            WriteVersionDword(bytes, voff, kVer20_2_0_8);
            // Keep the header string consistent with the dword we just wrote.
            const size_t nl2 = bytes.find('\n');
            if (nl2 != std::string::npos) {
                bytes = "Gamebryo File Format, Version " + FormatVersion(kVer20_2_0_8) + "\n" +
                        bytes.substr(nl2 + 1);
            }
            restamped = true;
        }
    }

    Niflib::NifInfo info;
    std::vector<NiObjectRef> blocks;

    try {
        std::istringstream stream(bytes, std::ios::binary);
        blocks = Niflib::ReadNifList(stream, &info);
    } catch (const std::exception& e) {
        out << "  !! PARSE FAILED: " << e.what() << "\n\n";
        return false;
    } catch (...) {
        out << "  !! PARSE FAILED: unknown exception\n\n";
        return false;
    }

    out << "Version      : " << FormatVersion(info.version) << " (0x" << std::hex << info.version
        << std::dec << ")\n";
    if (repaired_header) {
        out << "               ^ header string lacked the \", Version ...\" suffix;\n"
            << "                 repaired in memory to parse (see NifDumper.cpp)\n";
    }
    if (restamped) {
        out << "               ^ file is stamped 20.3.0.9 but uses the 20.2.0.8 header\n"
            << "                 layout; restamped in memory to parse (see NifDumper.cpp)\n";
    }
    out << "User version : " << info.userVersion << "\n";
    out << "User version2: " << info.userVersion2 << "\n";
    out << "Endian swap  : " << (info.endian ? "no" : "yes") << "\n";
    if (!info.exportInfo1.empty() || !info.creator.empty()) {
        out << "Creator      : " << info.creator << "\n";
        out << "Export info  : " << info.exportInfo1 << " | " << info.exportInfo2 << "\n";
    }
    out << "Block count  : " << blocks.size() << "\n\n";

    // Map each block to its index so links can be printed as numbers.
    std::unordered_map<NiObject*, int> index;
    for (size_t i = 0; i < blocks.size(); ++i) {
        index[static_cast<NiObject*>(blocks[i])] = static_cast<int>(i);
    }

    out << "--- BLOCK LIST ---\n";
    BlockTypeTally local;
    for (size_t i = 0; i < blocks.size(); ++i) {
        NiObject* obj = static_cast<NiObject*>(blocks[i]);
        const std::string type = TypeNameOf(obj);
        ++local[type];
        if (tally) {
            ++(*tally)[type];
        }

        out << "  " << i << "\t" << type;
        const std::string name = NameOf(obj);
        if (!name.empty()) {
            out << "\t\"" << name << "\"";
        }
        out << "\n";
    }

    out << "\n--- BLOCK TYPE SUMMARY (" << local.size() << " distinct) ---\n";
    for (const auto& kv : local) {
        out << "  " << kv.second << "\tx " << kv.first << "\n";
    }

    // Roots are blocks nothing else references.
    std::set<NiObject*> referenced;
    for (const NiObjectRef& b : blocks) {
        for (const NiObjectRef& r : static_cast<NiObject*>(b)->GetRefs()) {
            if (r != NULL) {
                referenced.insert(static_cast<NiObject*>(r));
            }
        }
    }

    out << "\n--- BLOCK TREE ---\n";
    std::set<NiObject*> visited;
    for (const NiObjectRef& b : blocks) {
        NiObject* obj = static_cast<NiObject*>(b);
        if (referenced.find(obj) == referenced.end()) {
            PrintTree(obj, index, visited, "", true, true, out);
        }
    }

    // Anything still unvisited is unreachable from any root (orphan block).
    for (const NiObjectRef& b : blocks) {
        NiObject* obj = static_cast<NiObject*>(b);
        if (visited.find(obj) == visited.end()) {
            out << "[orphan] ";
            PrintTree(obj, index, visited, "", true, true, out);
        }
    }

    out << "\n";
    return true;
}

} // namespace gfnif
