#include "scanner/EntityScanner.hpp"

#include "util/PathUtils.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace gfnif {
namespace {

std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string Trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t");
    if (b == std::string::npos) {
        return std::string();
    }
    size_t e = s.find_last_not_of(" \t");
    return s.substr(b, e - b + 1);
}

/*! First path component of `relative`, which is the entity type directory when
 *  the input root is a corpus root. "" for a file sitting directly in the root. */
std::string EntityTypeOf(const fs::path& relative, const std::string& rootType) {
    if (!rootType.empty()) {
        // The scan root is itself an entity directory (--input input/monster),
        // so everything under it belongs to that type. Without this, the type
        // would come out empty and --entities monster would exclude every file.
        return rootType;
    }
    if (relative.empty()) {
        return std::string();
    }
    const fs::path first = *relative.begin();
    if (first == relative) {
        return std::string(); // file directly in the root, no type directory
    }
    return ToLower(first.string());
}

/*! The entity type named by the scan root's own directory name, or "". */
std::string RootEntityType(const fs::path& root) {
    // filename() is empty for a path with a trailing separator; go up in that case.
    fs::path name = root.filename();
    if (name.empty()) {
        name = root.parent_path().filename();
    }
    const std::string lowered = ToLower(name.string());
    const std::vector<std::string>& known = KnownEntityTypes();
    if (std::find(known.begin(), known.end(), lowered) != known.end()) {
        return lowered;
    }
    return std::string();
}

} // namespace

const std::vector<std::string>& KnownEntityTypes() {
    static const std::vector<std::string> kTypes = {"chair", "char",    "effect", "elf",
                                                    "item",  "monster", "npc",    "ride"};
    return kTypes;
}

std::vector<std::string> ParseEntityList(const std::string& csv) {
    std::vector<std::string> out;
    std::stringstream ss(csv);
    std::string item;
    while (std::getline(ss, item, ',')) {
        const std::string t = Trim(ToLower(item));
        if (!t.empty()) {
            out.push_back(t);
        }
    }
    return out;
}

std::string FirstUnknownEntity(const std::vector<std::string>& filter) {
    const std::vector<std::string>& known = KnownEntityTypes();
    for (const std::string& e : filter) {
        if (std::find(known.begin(), known.end(), e) == known.end()) {
            return e;
        }
    }
    return std::string();
}

ScanResult ScanEntities(const fs::path& inputRoot, const std::vector<std::string>& entityFilter) {
    ScanResult result;

    std::error_code ec;
    if (!fs::exists(inputRoot, ec) || ec) {
        result.error = "input path not found: " + ToPosix(inputRoot);
        return result;
    }
    if (!fs::is_directory(inputRoot, ec) || ec) {
        result.error = "input path is not a directory: " + ToPosix(inputRoot);
        return result;
    }

    // A scan rooted at an entity directory (--input input/monster) still has a
    // type; it just is not part of the relative paths below.
    const std::string rootType = RootEntityType(inputRoot);

    // Collect .nif and .kf in one walk: pairing needs both, and walking the
    // tree twice on a 4300-file corpus is wasted I/O.
    std::vector<fs::path> nifs;
    // key: "<entityType>/<lowercased stem>" -> .kf path, matching the
    // same-type/same-basename rule from docs/NAMING_CONVENTIONS.md §2.
    std::map<std::string, fs::path> kfByKey;

    for (auto it = fs::recursive_directory_iterator(
             inputRoot, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) {
            // A single unreadable subtree must not abort the scan; skip and
            // carry on, matching the run-never-stops discipline elsewhere.
            ec.clear();
            continue;
        }
        std::error_code fec;
        if (!it->is_regular_file(fec) || fec) {
            continue;
        }
        const fs::path& p = it->path();
        if (IsNifFile(p)) {
            nifs.push_back(p);
        } else if (IsKfFile(p)) {
            const fs::path rel = RelativeUnder(p, inputRoot);
            kfByKey[EntityTypeOf(rel, rootType) + "/" + ToLower(p.stem().string())] = p;
        }
    }

    std::sort(nifs.begin(), nifs.end());

    std::set<std::string> typesSeen;
    std::set<std::string> pairedKfKeys;

    for (const fs::path& nif : nifs) {
        ScannedModel m;
        m.nifPath = nif;
        m.relativePath = RelativeUnder(nif, inputRoot);
        m.entityType = EntityTypeOf(m.relativePath, rootType);

        if (!entityFilter.empty()) {
            if (std::find(entityFilter.begin(), entityFilter.end(), m.entityType) ==
                entityFilter.end()) {
                ++result.filteredOut;
                continue;
            }
        }

        if (!m.entityType.empty()) {
            typesSeen.insert(m.entityType);
        }

        const std::string key = m.entityType + "/" + ToLower(nif.stem().string());
        auto kf = kfByKey.find(key);
        if (kf != kfByKey.end()) {
            m.kfPath = kf->second;
            pairedKfKeys.insert(key);
            ++result.withKf;
        }

        std::error_code sec;
        const auto size = fs::file_size(nif, sec);
        m.sizeBytes = sec ? 0 : size;

        result.models.push_back(std::move(m));
    }

    // Only meaningful on an unfiltered scan: with --entities, the .kf files of
    // the excluded types are unpaired by construction, not orphaned.
    if (entityFilter.empty()) {
        for (const auto& kv : kfByKey) {
            if (pairedKfKeys.count(kv.first) == 0) {
                ++result.orphanKf;
            }
        }
    }

    result.entityTypesFound.assign(typesSeen.begin(), typesSeen.end());
    result.success = true;
    return result;
}

} // namespace gfnif
