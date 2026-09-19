#pragma once

#include "export/SceneModel.hpp"
#include "texture/TextureResolver.hpp"

#include <string>
#include <vector>

namespace Niflib {
class NiAVObject;
}

namespace gfnif {

/*! Builds MaterialData from the NiProperty list attached to a geometry block,
 *  de-duplicating identical materials across the meshes of one file.
 *
 *  Grand Fantasia models attach properties per NiTriShape/NiTriStrips, and the
 *  same NiMaterialProperty + NiSourceTexture combination is routinely shared by
 *  several geometries. Emitting one MaterialData per distinct combination keeps
 *  the .gfmodel small and matches how a renderer wants to batch. */
class MaterialExtractor {
public:
    /*! \param resolver used to turn .dds names into on-disk .png paths.
     *  \param warnings appended to when a texture cannot be resolved. */
    MaterialExtractor(const TextureResolver& resolver, std::vector<std::string>* warnings);

    /*! Extracts the material for `geometry` and returns its index in
     *  `materials`, reusing an existing entry when identical. Returns -1 when
     *  the geometry carries no material-bearing property at all. */
    int ExtractFor(Niflib::NiAVObject* geometry, std::vector<MaterialData>& materials);

private:
    const TextureResolver& resolver_;
    std::vector<std::string>* warnings_;
};

} // namespace gfnif
