#include "util/PathUtils.hpp"

#include <algorithm>
#include <cctype>

namespace fs = std::filesystem;

namespace gfnif {

std::string LowerExtension(const fs::path& p) {
    std::string e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return e;
}

bool IsNifFile(const fs::path& p) { return LowerExtension(p) == ".nif"; }

bool IsKfFile(const fs::path& p) { return LowerExtension(p) == ".kf"; }

fs::path RelativeUnder(const fs::path& file, const fs::path& root) {
    if (root.empty()) {
        return file.filename();
    }
    std::error_code ec;
    const fs::path rel = fs::relative(file, root, ec);
    // fs::relative yields "../.." style paths when the two diverge, which would
    // place output outside the requested root. Treat that as "no common root".
    if (ec || rel.empty() || *rel.begin() == "..") {
        return file.filename();
    }
    return rel;
}

fs::path OutputBaseFor(const fs::path& nifFile, const fs::path& inputRoot,
                       const fs::path& outRoot) {
    fs::path rel = RelativeUnder(nifFile, inputRoot);
    rel.replace_extension();
    return outRoot / rel;
}

bool EnsureParentDirectory(const fs::path& p, std::string& error) {
    const fs::path parent = p.parent_path();
    if (parent.empty()) {
        return true;
    }
    std::error_code ec;
    if (fs::exists(parent, ec)) {
        return true;
    }
    // create_directories returns false both when it created nothing and when it
    // failed, so the error_code is what actually distinguishes the two.
    fs::create_directories(parent, ec);
    if (ec) {
        error = "cannot create output directory '" + ToPosix(parent) + "': " + ec.message();
        return false;
    }
    return true;
}

bool OutputIsUpToDate(const fs::path& source, const fs::path& outBase) {
    std::error_code ec;
    const fs::path model = fs::path(outBase).concat(".gfmodel");
    const fs::path bin = fs::path(outBase).concat(".gfbin");

    if (!fs::exists(model, ec) || ec) {
        return false;
    }
    if (!fs::exists(bin, ec) || ec) {
        return false;
    }

    const auto srcTime = fs::last_write_time(source, ec);
    if (ec) {
        return false;
    }
    const auto modelTime = fs::last_write_time(model, ec);
    if (ec) {
        return false;
    }
    const auto binTime = fs::last_write_time(bin, ec);
    if (ec) {
        return false;
    }
    return modelTime >= srcTime && binTime >= srcTime;
}

std::string ToPosix(const fs::path& p) {
    std::string s = p.generic_string();
    return s;
}

} // namespace gfnif
