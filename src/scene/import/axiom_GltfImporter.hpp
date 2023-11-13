#pragma once

#include <scene/axiom_Scene.hpp>

#include <fastgltf/parser.hpp>

namespace axiom
{
    struct GltfImporter
    {
        std::unique_ptr<fastgltf::Asset> asset;

        std::filesystem::path dir;

        scene_ir::Scene scene;

        std::vector<std::pair<u32, u32>> gltf_mesh_offsets;

        void Reset();
        void ProcessTexture(u32 tex_idx);
        void ProcessMaterial(u32 mat_idx);
        void ProcessMesh(u32 gltf_mesh_idx, u32 prim_idx);
        void ProcessNode(usz node_idx, Mat4 parent_transform);

        scene_ir::Scene Import(const std::filesystem::path& path);
    };
}