#include "axiom_GltfImporter.hpp"

namespace axiom
{
    GltfImporter::GltfImporter(Scene& _scene)
        : scene(&_scene)
    {}

    void GltfImporter::Import(std::filesystem::path gltf)
    {
        (void)gltf;
    }
}