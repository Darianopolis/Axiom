#pragma once

#include "axiom_Scene.hpp"

namespace axiom
{
    struct GltfImporter
    {
        Scene* scene;

        GltfImporter(Scene& scene);

        void Import(std::filesystem::path gltf, std::optional<std::string_view> scene = {});
    };
}