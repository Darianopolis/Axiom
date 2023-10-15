#pragma once

#include <scene/axiom_Scene.hpp>

#include <fastgltf/parser.hpp>

namespace axiom
{
    struct GltfImporter
    {
        std::unique_ptr<fastgltf::Asset> asset;

        std::filesystem::path dir;

        Scene scene;

        std::vector<std::pair<u32, u32>> gltfMeshOffsets;

        void Reset();
        void ProcessTexture(u32 texIdx);
        void ProcessMaterial(u32 matIdx);
        void ProcessMesh(u32 gltfMeshIdx, u32 primIdx);
        void ProcessNode(usz nodeIdx, Mat4 parentTransform);

        Scene Import(const std::filesystem::path& path);
    };
}