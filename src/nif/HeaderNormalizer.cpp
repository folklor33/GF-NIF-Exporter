#include "nif/HeaderNormalizer.hpp"

#include "ObjectRegistry.h"
#include "obj/NiObject.h"

#include <fstream>
#include <sstream>

namespace Niflib {
// niflib registers its block-type factories lazily, from inside ReadNifList,
// and does not declare the function in any public header. We probe the registry
// before any read happens, so the registration is forced here. The symbol is
// an ordinary function in namespace Niflib (src/gen/register.cpp) and we link
// niflib statically, so declaring it is enough.
void RegisterObjects();
} // namespace Niflib

namespace gfnif {
namespace {

/*! Ensures niflib's object registry is populated before it is queried. */
void EnsureNiflibObjectsRegistered() {
    static const bool once = [] {
        Niflib::RegisterObjects();
        return true;
    }();
    (void)once;
}

constexpr unsigned int kVer20_2_0_8 = 0x14020008;
constexpr unsigned int kVer20_3_0_9 = 0x14030009;

/*! The prefix niflib matches on, and the exact length it hard-codes past it. */
constexpr char kGamebryoPrefix[] = "Gamebryo File Format";
constexpr size_t kGamebryoPrefixLen = 20;

unsigned int ReadDword(const std::string& bytes, size_t off) {
    return static_cast<unsigned char>(bytes[off]) |
           (static_cast<unsigned char>(bytes[off + 1]) << 8) |
           (static_cast<unsigned char>(bytes[off + 2]) << 16) |
           (static_cast<unsigned char>(bytes[off + 3]) << 24);
}

void WriteDword(std::string& bytes, size_t off, unsigned int v) {
    bytes[off + 0] = static_cast<char>(v & 0xFF);
    bytes[off + 1] = static_cast<char>((v >> 8) & 0xFF);
    bytes[off + 2] = static_cast<char>((v >> 16) & 0xFF);
    bytes[off + 3] = static_cast<char>((v >> 24) & 0xFF);
}

} // namespace

std::string FormatNifVersion(unsigned int v) {
    return std::to_string((v >> 24) & 0xFF) + "." + std::to_string((v >> 16) & 0xFF) + "." +
           std::to_string((v >> 8) & 0xFF) + "." + std::to_string(v & 0xFF);
}

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

void NormalizeNifHeader(std::string& bytes, HeaderNormalizationReport& report) {
    report = HeaderNormalizationReport();

    const size_t nl = bytes.find('\n');
    if (nl == std::string::npos || nl + 5 > bytes.size()) {
        return; // Not a NIF, or too short to hold a version dword. Leave as-is.
    }
    report.originalHeaderString = bytes.substr(0, nl);
    report.originalVersion = ReadDword(bytes, nl + 1);

    // --- Fix 1: truncated header string ------------------------------------
    //
    // Most GF files open with "Gamebryo File Format, Version 20.2.0.8\n".
    // Some open with the bare "Gamebryo File Format\n". niflib cannot read the
    // latter: NifStream(HeaderString&) (niflib src/NIF_IO.cpp:395-405) matches
    // the 20-character "Gamebryo File Format" prefix, hard-codes ver_start = 30,
    // then calls header.substr(30) on a 20-character string -- which throws
    // std::out_of_range("invalid string position").
    //
    // The version dword after the string is intact, so we rebuild a well-formed
    // ", Version x.x.x.x" suffix from it.
    //
    // The marker is strict: the header string must be EXACTLY the 20-character
    // prefix with nothing after it. A file whose header is merely unusual but
    // long enough for substr(30) parses fine in niflib and is left alone.
    const bool truncated =
        nl == kGamebryoPrefixLen && bytes.compare(0, kGamebryoPrefixLen, kGamebryoPrefix) == 0;
    if (!truncated) {
        return;
    }

    // --- Fix 2: version stamped 20.3.0.9 over a 20.2.0.8 layout -------------
    //
    // The truncated file additionally claims 20.3.0.9 while its header is
    // physically laid out as 20.2.0.8. Per nifxml, two extra header fields
    // appear above 20.2.0.7 -- a per-block size array (version >= 0x14020007,
    // niflib Header.cpp:104) and a string table (version >= 0x14010003,
    // Header.cpp:110). Neither is present, so niflib would consume the
    // block-type table as if it were those fields.
    //
    // This restamp is GATED on the truncation marker above and must stay that
    // way: the corpus also contains genuine 20.3.0.9 .kf files that really do
    // carry the string table and parse correctly untouched. An ungated version
    // of this fix-up corrupted all six of them during Phase 1.
    unsigned int version = report.originalVersion;
    if (version == kVer20_3_0_9) {
        version = kVer20_2_0_8;
        report.restampedVersion = true;
    }

    const std::string headerString =
        std::string(kGamebryoPrefix) + ", Version " + FormatNifVersion(version);
    std::string rebuilt = headerString + "\n" + bytes.substr(nl + 1);
    if (report.restampedVersion) {
        // Keep the dword consistent with the string we just wrote. It sits
        // immediately after the newline that terminates the header string.
        WriteDword(rebuilt, headerString.size() + 1, version);
    }
    bytes.swap(rebuilt);
    report.repairedTruncatedHeaderString = true;
}

bool ReadBlockTypeNames(const std::string& bytes, std::vector<std::string>& outTypes) {
    outTypes.clear();

    const size_t nl = bytes.find('\n');
    if (nl == std::string::npos) {
        return false;
    }

    // Header layout for the 20.x versions this tool targets, following the
    // nifxml field order:
    //   version dword | endian byte | userVersion dword | numBlocks dword
    //   | numBlockTypes ushort | numBlockTypes x (length dword + chars)
    size_t o = nl + 1;
    auto need = [&](size_t n) { return o + n <= bytes.size(); };
    auto u32 = [&]() {
        const unsigned int v = static_cast<unsigned char>(bytes[o]) |
                               (static_cast<unsigned char>(bytes[o + 1]) << 8) |
                               (static_cast<unsigned char>(bytes[o + 2]) << 16) |
                               (static_cast<unsigned char>(bytes[o + 3]) << 24);
        o += 4;
        return v;
    };

    if (!need(4)) return false;
    const unsigned int version = u32();
    // Only the 20.1.0.3+ layout is understood here; older files place these
    // fields differently and are not part of this corpus.
    if (version < 0x14010003) {
        return false;
    }
    if (!need(1)) return false;
    o += 1; // endian
    if (!need(4)) return false;
    u32(); // userVersion
    if (!need(4)) return false;
    u32(); // numBlocks
    if (!need(2)) return false;
    const unsigned int numTypes = static_cast<unsigned char>(bytes[o]) |
                                  (static_cast<unsigned char>(bytes[o + 1]) << 8);
    o += 2;

    outTypes.reserve(numTypes);
    for (unsigned int i = 0; i < numTypes; ++i) {
        if (!need(4)) return false;
        const unsigned int len = u32();
        // A sane upper bound: type names are short identifiers. Anything larger
        // means the layout assumption is wrong, so bail rather than allocate.
        if (len > 256 || !need(len)) {
            return false;
        }
        outTypes.emplace_back(bytes, o, len);
        o += len;
    }
    return true;
}

std::string FindUnsupportedBlockType(const std::string& bytes) {
    std::vector<std::string> types;
    if (!ReadBlockTypeNames(bytes, types)) {
        // Layout not understood; let niflib have its say rather than guessing.
        return std::string();
    }

    // The registry is normally filled on the first ReadNifList call. We query it
    // before that happens, so it is forced now. Re-registering later is
    // harmless: RegisterObject just overwrites identical map entries.
    EnsureNiflibObjectsRegistered();

    for (const std::string& t : types) {
        // Ask niflib itself rather than keeping a hardcoded blocklist, so this
        // stays correct if the submodule ever gains the missing classes.
        Niflib::NiObject* probe = Niflib::ObjectRegistry::CreateObject(t);
        if (probe == nullptr) {
            return t;
        }
        // CreateObject hands back a raw object with a refcount of 0; wrapping it
        // in a Ref and letting that go out of scope frees it.
        Niflib::Ref<Niflib::NiObject> owner(probe);
    }
    return std::string();
}

} // namespace gfnif
