#include "texture/TextureResolver.hpp"

#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

namespace gfnif {

std::string ToPngBasename(const std::string& name) {
    // Strip any directory component. Verified across the corpus: every
    // NiSourceTexture name is a bare filename, but a stale authoring path would
    // not match the shipped layout anyway, so only the basename is meaningful.
    size_t start = name.find_last_of("/\\");
    std::string base = (start == std::string::npos) ? name : name.substr(start + 1);

    const size_t dot = base.find_last_of('.');
    if (dot != std::string::npos) {
        base.erase(dot);
    }
    return base + ".png";
}

TextureResolver::TextureResolver(const std::string& nifPath) {
    std::error_code ec;
    fs::path modelDir = fs::path(nifPath).parent_path();

    // Layout established in Phase 1:
    //     <entityType>/model/NAME.nif
    //     <entityType>/texture/NAME.png
    // so the primary root is a sibling "texture" directory of the model's own
    // directory. The model directory itself and the entity-type directory are
    // searched as fallbacks, which costs nothing and tolerates flatter layouts.
    const fs::path entityDir = modelDir.parent_path();

    auto add = [&](const fs::path& p) {
        if (!p.empty() && fs::is_directory(p, ec)) {
            searchRoots_.push_back(p.string());
        }
    };

    add(entityDir / "texture");
    add(modelDir / "texture");
    add(modelDir);
}

TextureResolver::Result TextureResolver::Resolve(const std::string& ddsName) const {
    Result r;
    if (ddsName.empty()) {
        return r;
    }
    r.searchedFor = ToPngBasename(ddsName);

    std::error_code ec;
    for (const std::string& root : searchRoots_) {
        fs::path candidate = fs::path(root) / r.searchedFor;
        if (fs::is_regular_file(candidate, ec)) {
            r.found = true;
            r.path = candidate.generic_string();
            return r;
        }
    }
    return r;
}

} // namespace gfnif
