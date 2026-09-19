#include "nif/NameClassifier.hpp"

#include <algorithm>
#include <cctype>

namespace gfnif {
namespace {

/*! Lowercases and trims, and collapses internal runs of whitespace to one
 *  space, so "Biped  Object" and "biped object" compare equal. */
std::string Normalize(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool pendingSpace = false;
    for (unsigned char c : s) {
        if (std::isspace(c)) {
            pendingSpace = !out.empty();
            continue;
        }
        if (pendingSpace) {
            out.push_back(' ');
            pendingSpace = false;
        }
        out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

/*! True if `s` is exactly `word`, or `word` followed only by a numeric or
 *  duplicate-name suffix.
 *
 *  3ds Max numbers duplicates as "Box01", and niflib renders NIF's repeated
 *  names as "Bone@#0". Both are the same helper. Anything with further
 *  letters after the word -- "Boxer", "Bonefish" -- is left alone. */
bool IsWordWithSuffix(const std::string& s, const char* word) {
    const std::string w(word);
    if (s.compare(0, w.size(), w) != 0) {
        return false;
    }
    for (size_t i = w.size(); i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (!std::isdigit(c) && c != '@' && c != '#' && c != '.' && c != '_' && c != '-' &&
            c != ' ') {
            return false;
        }
    }
    return true;
}

/*! Editor primitives and bone/biped gizmos.
 *
 *  Deliberately does NOT include "editable poly", "editable mesh", "poly
 *  mesh" or "plane", although the brief's starting regex listed them.
 *  Measured on the corpus those names carry real, textured geometry in
 *  91-99% of cases -- they are simply the default names artists never
 *  changed. The 1339 untextured ones among them range up to 5979 vertices
 *  and 148 of them reference a texture we merely failed to resolve, so they
 *  are body parts, not gizmos. Dropping them would delete real models.
 *
 *  "sphere", "cylinder" and "dummy" were also on the brief's starting list
 *  and were tried, but pulled a real, textured mesh out of the export:
 *  elf/X1051.nif's entire visible geometry is one NiTriStrips named
 *  "Sphere06" whose texture happens not to resolve, which is indistinguishable
 *  from a gizmo by the name+texture test alone. Pre-filter, "sphere" and
 *  "cylinder" were 82% and 94% textured corpus-wide -- nowhere near "bone"
 *  (0%), "biped object" (0%) or "box" (3%) -- so they are left as ordinary
 *  geometry rather than risk another X1051. */
const char* const kCollisionWords[] = {
    "bone",
    "biped object",
    "bipedobject",
    "box",
};

/*! Named anchor points. Kept in the skeleton; their gizmo geometry is not
 *  exported. */
const char* const kAttachWords[] = {
    "right rope nub", "left rope nub", "shield nub", "nub",
};

} // namespace

NodeRole ClassifyNodeName(const std::string& name) {
    const std::string n = Normalize(name);
    if (n.empty()) {
        return NodeRole::Visual;
    }
    // Attach points are checked first: "shield nub" would otherwise never be
    // reached, and an anchor is more specific than a generic primitive.
    for (const char* w : kAttachWords) {
        if (IsWordWithSuffix(n, w)) {
            return NodeRole::AttachPoint;
        }
    }
    for (const char* w : kCollisionWords) {
        if (IsWordWithSuffix(n, w)) {
            return NodeRole::CollisionHelper;
        }
    }
    return NodeRole::Visual;
}

bool ShouldDropGeometry(const std::string& name, bool hasResolvedTexture) {
    if (hasResolvedTexture) {
        return false;
    }
    const NodeRole role = ClassifyNodeName(name);
    return role == NodeRole::CollisionHelper || role == NodeRole::AttachPoint;
}

} // namespace gfnif
