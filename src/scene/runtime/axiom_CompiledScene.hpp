#pragma once

#include "axiom_Core.hpp"

#include "axiom_Attributes.hpp"

#include <imp/imp_Importer.hpp>

namespace axiom
{
    struct UVTexture : nova::RefCounted
    {
        Vec2U           size;
        std::vector<b8> data;

        nova::Format format = nova::Format::RGBA8_UNorm;

        f32 min_alpha = 1.f;
        f32 max_alpha = 0.f;
    };

    struct UVMaterial : nova::RefCounted
    {
        nova::Ref<UVTexture>     basecolor_alpha;
        nova::Ref<UVTexture>             normals;
        nova::Ref<UVTexture>          emissivity;
        nova::Ref<UVTexture>        transmission;
        nova::Ref<UVTexture> metalness_roughness;

        f32  alpha_cutoff = 0.5f;
        bool   alpha_mask = false;
        bool  alpha_blend = false;
        bool         thin = false;
        bool   subsurface = false;
        bool        decal = false;
    };

    struct TriSubMesh
    {
        u32              vertex_offset;
        u32                 max_vertex;
        u32                first_index;
        u32                index_count;
        nova::Ref<UVMaterial> material;
    };

    struct ShadingAttributes
    {
        GPU_TangentSpace tangent_space;
        GPU_TexCoords       tex_coords;
    };

    struct TriMesh : nova::RefCounted
    {
        std::vector<Vec3>             position_attributes;
        std::vector<ShadingAttributes> shading_attributes;
        std::vector<u32>                          indices;

        std::vector<TriSubMesh> sub_meshes;
    };

    struct TriMeshInstance : nova::RefCounted
    {
        nova::Ref<TriMesh> mesh;
        nova::Mat4    transform;
    };

    struct CompiledScene
    {
        std::vector<nova::Ref<UVTexture>>        textures;
        std::vector<nova::Ref<UVMaterial>>      materials;
        std::vector<nova::Ref<TriMesh>>            meshes;
        std::vector<nova::Ref<TriMeshInstance>> instances;

        inline
        void DebugDump()
        {
            for (auto[mesh_idx, mesh] : meshes | std::views::enumerate) {
                NOVA_LOG("Mesh[{}]", mesh_idx);
                NOVA_LOGEXPR(mesh->indices.size());
                NOVA_LOGEXPR(mesh->shading_attributes.size());
                NOVA_LOGEXPR(mesh->position_attributes.size());
                NOVA_LOGEXPR(mesh->sub_meshes.size());
                for (auto[sub_mesh_idx, sub_mesh] : mesh->sub_meshes | std::views::enumerate) {
                    NOVA_LOG("Submesh[{}]", sub_mesh_idx);
                    NOVA_LOGEXPR(sub_mesh.vertex_offset);
                    NOVA_LOGEXPR(sub_mesh.max_vertex);
                    NOVA_LOGEXPR(sub_mesh.first_index);
                    NOVA_LOGEXPR(sub_mesh.index_count);
                }
            }
        }

        void Compile(imp::Scene& scene);
    };
}