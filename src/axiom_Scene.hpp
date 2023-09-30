#pragma once

#include "axiom_Core.hpp"

namespace axiom
{
    struct Vertex
    {
        Vec3 position;
        Vec3 normal;
        Vec4 tangent;
        Vec2 uv;
    };

    struct TriMesh : nova::RefCounted
    {
        std::vector<Vertex> vertices;
        std::vector<u32>     indices;
    };

    struct TextureMap : nova::RefCounted
    {
        Vec2U           size;
        std::vector<b8> data;
    };

    struct UVMaterial : nova::RefCounted
    {
        nova::Ref<TextureMap>    albedo;
        nova::Ref<TextureMap>     alpha;
        nova::Ref<TextureMap>   normals;
        nova::Ref<TextureMap> roughness;
        nova::Ref<TextureMap> metalness;

        f32 alphaCutoff = -1.f;
        bool       thin = false;
        bool subsurface = false;
    };

    struct TriMeshInstance : nova::RefCounted
    {
        nova::Ref<TriMesh>    mesh;
        nova::Ref<UVMaterial> material;
        nova::Mat4            transform;
    };

    struct Scene
    {
        std::vector<nova::Ref<TriMesh>>            meshes;
        std::vector<nova::Ref<TriMeshInstance>> instances;
    };
}