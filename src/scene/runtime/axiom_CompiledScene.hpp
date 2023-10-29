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

        f32 minAlpha = 1.f;
        f32 maxAlpha = 0.f;
    };

    struct UVMaterial : nova::RefCounted
    {
        nova::Ref<UVTexture>     baseColor_alpha;
        nova::Ref<UVTexture>             normals;
        nova::Ref<UVTexture>          emissivity;
        nova::Ref<UVTexture>        transmission;
        nova::Ref<UVTexture> metalness_roughness;

        f32  alphaCutoff = 0.5f;
        bool   alphaMask = false;
        bool  alphaBlend = false;
        bool        thin = false;
        bool  subsurface = false;
        bool       decal = false;
    };

    struct TriSubMesh
    {
        u32               vertexOffset;
        u32                  maxVertex;
        u32                 firstIndex;
        u32                 indexCount;
        nova::Ref<UVMaterial> material;
    };

    struct ShadingAttributes
    {
        GPU_TangentSpace tangentSpace;
        GPU_TexCoords       texCoords;
    };

    struct TriMesh : nova::RefCounted
    {
        std::vector<Vec3>             positionAttributes;
        std::vector<ShadingAttributes> shadingAttributes;
        std::vector<u32>                         indices;

        std::vector<TriSubMesh> subMeshes;
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
            for (auto[meshIdx, mesh] : meshes | std::views::enumerate) {
                NOVA_LOG("Mesh[{}]", meshIdx);
                NOVA_LOGEXPR(mesh->indices.size());
                NOVA_LOGEXPR(mesh->shadingAttributes.size());
                NOVA_LOGEXPR(mesh->positionAttributes.size());
                NOVA_LOGEXPR(mesh->subMeshes.size());
                for (auto[subMeshIdx, subMesh] : mesh->subMeshes | std::views::enumerate) {
                    NOVA_LOG("Submesh[{}]", subMeshIdx);
                    NOVA_LOGEXPR(subMesh.vertexOffset);
                    NOVA_LOGEXPR(subMesh.maxVertex);
                    NOVA_LOGEXPR(subMesh.firstIndex);
                    NOVA_LOGEXPR(subMesh.indexCount);
                }
            }
        }

        void Compile(imp::Scene& scene);
    };
}