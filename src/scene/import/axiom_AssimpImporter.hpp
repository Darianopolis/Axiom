#pragma once

#include <scene/axiom_Scene.hpp>

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

        HashMap<std::string, AssimpIndex> textureIndices;

        void Reset();

        void ProcessTexture(u32 textureIndex);
        void ProcessMaterial(u32 materialIndex);
        void ProcessMesh(u32 meshIndex);
        void ProcessNode(aiNode* node, Mat4 parentTransform);

        // void ProcessTextures();
        // void ProcessTexture(UVTexture* outTexture, const std::filesystem::path& texture);

        // void ProcessMaterials();
        // void ProcessMaterial(u32 index, aiMaterial* material);

        // void ProcessScene();
        // void ProcessNode(aiNode* node, Mat4 parentTransform);

        scene_ir::Scene Import(const std::filesystem::path& path);
    };
}