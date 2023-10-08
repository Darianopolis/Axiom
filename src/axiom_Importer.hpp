#pragma once

#include "axiom_Scene.hpp"

namespace axiom
{
    struct ImportSettings
    {
        bool genTBN;
        bool flipUVs;
        bool flipNormalMapZ;
    };

    struct Importer : nova::RefCounted
    {
        virtual ~Importer() = 0;

        virtual void Import(std::filesystem::path gltf, const ImportSettings& settings, std::optional<std::string_view> scene = {}) = 0;
    };

    inline
    Importer::~Importer() = default;

    nova::Ref<Importer> CreateGltfImporter(Scene& scene);
    nova::Ref<Importer> CreateAssimpImporter(Scene& scene);
}