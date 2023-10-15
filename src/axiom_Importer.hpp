#pragma once

#include "axiom_Scene.hpp"

namespace axiom
{
    struct ImportSettings
    {
        bool         genTBN = false;
        bool        flipUVs = false;
        bool flipNormalMapZ = false;
    };

    struct Importer : nova::RefCounted
    {
        virtual ~Importer() = 0;

        virtual void Import(std::filesystem::path gltf, const ImportSettings& settings, std::optional<std::string_view> scene = {}) = 0;
    };

    inline
    Importer::~Importer() = default;

    nova::Ref<Importer> CreateGltfImporter(LoadableScene& scene);
    nova::Ref<Importer> CreateAssimpImporter(LoadableScene& scene);
}