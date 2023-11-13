#pragma once

#include <scene/axiom_Scene.hpp>

#include <nova/core/nova_Containers.hpp>

// TODO:
#include <ufbx.h>

namespace axiom
{
    struct FbxIndex
    {
        u32 value = scene_ir::InvalidIndex;
    };

    struct FbxVertex
    {
        Vec3 pos = {};
        Vec2  uv = {};
        Vec3 nrm = {};

        bool operator==(const FbxVertex& other) const noexcept {
            return pos == other.pos
                && uv == other.uv
                && nrm == other.nrm;
        }
    };

}
NOVA_MEMORY_HASH(axiom::FbxVertex);
namespace axiom
{
    struct FbxImporter
    {
        ufbx_scene* fbx = nullptr;

        std::filesystem::path dir;

        scene_ir::Scene scene;

        std::vector<std::pair<u32, u32>> fbx_mesh_offsets;

        nova::HashMap<void*, u32>  texture_indices;
        nova::HashMap<void*, u32> material_indices;

        std::vector<u32>                       tri_indices;
        nova::HashMap<FbxVertex, FbxIndex> unique_vertices;
        std::vector<FbxVertex>              vertex_indices;

        ~FbxImporter();

        void Reset();
        scene_ir::Scene Import(const std::filesystem::path& path);

        void ProcessTexture(u32 tex_idx);
        void ProcessMaterial(u32 mat_idx);
        void ProcessMesh(u32 fbx_mesh_idx, u32 prim_idx);
        void ProcessNode(ufbx_node* node, Mat4 parent_transform);
    };
}