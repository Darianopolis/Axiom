#pragma once

#include <scene/axiom_Scene.hpp>

#include <nova/core/nova_Containers.hpp>

#include <assimp/Importer.hpp>
#include <assimp/scene.h>

#include <assimp/postprocess.h>

namespace axiom
{
    struct AssimpIndex {
        u32 value = scene_ir::InvalidIndex;
    };

    struct AssimpImporter
    {
        scene_ir::Scene scene;

        std::filesystem::path dir;
        Assimp::Importer   assimp;
        const aiScene*      asset = nullptr;

        nova::HashMap<std::string, AssimpIndex> texture_indices;

        void Reset();

        void ProcessTexture(u32 texture_index);
        void ProcessMaterial(u32 material_index);
        void ProcessMesh(u32 mesh_index);
        void ProcessNode(aiNode* node, Mat4 parent_transform);

        scene_ir::Scene Import(const std::filesystem::path& path);
    };
}