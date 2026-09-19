#pragma once

#include <string>
#include <vector>

namespace gfnif {

/*! Resolves the .dds texture names stored in NIFs to the .png files actually
 *  shipped alongside them.
 *
 *  Phase 1 established the rule: a NiSourceTexture stores a bare filename with
 *  a .dds extension and no directory, and the corpus ships the same basename
 *  with a .png extension under <entity type>/texture/. There is no mapping
 *  table and no renaming -- only the extension differs.
 *
 *  The resolver is constructed with the model file's own directory and works
 *  outward from there, so it does not need to know the entity type by name. */
class TextureResolver {
public:
    /*! \param nifPath path of the .nif whose textures are being resolved. The
     *                 search roots are derived from it (see Resolve). */
    explicit TextureResolver(const std::string& nifPath);

    /*! Result of one lookup. */
    struct Result {
        bool found = false;
        /*! Path to the .png on disk, forward slashes. Relative to `relativeTo`
         *  when that was given to the constructor, absolute otherwise. Empty
         *  when not found. */
        std::string path;
        /*! The .png basename that was looked for, for diagnostics. */
        std::string searchedFor;
    };

    /*! Looks up `ddsName` (as stored in the NIF, e.g. "M01101.dds" or
     *  "textures\\foo.dds") and returns where its .png lives.
     *
     *  A miss is a normal outcome: Phase 1 found real gaps in the corpus
     *  (char/ ships no texture/ directory at all). The caller is expected to
     *  warn and carry on with textureFound = false. */
    Result Resolve(const std::string& ddsName) const;

    /*! Directories searched, in order, for diagnostics. */
    const std::vector<std::string>& SearchRoots() const { return searchRoots_; }

private:
    std::vector<std::string> searchRoots_;
};

/*! Replaces any extension on `name` with ".png", keeping the basename. Also
 *  strips any directory component, since NIF texture names occasionally carry
 *  a stale authoring path that does not match the shipped layout. */
std::string ToPngBasename(const std::string& name);

} // namespace gfnif
