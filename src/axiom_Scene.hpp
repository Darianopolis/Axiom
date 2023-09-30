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

    struct TriangleMesh : nova::RefCounted
    {
        std::vector<Vertex> vertices;
        std::vector<u32>     indices;
    };

    struct TextureMap : nova::RefCounted
    {
        Vec2U           size;
        std::vector<b8> data;
    };

    struct PbrMaterial : nova::RefCounted
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

    struct MeshInstance : nova::RefCounted
    {
        nova::Ref<TriangleMesh>    mesh;
        nova::Ref<PbrMaterial> material;
        nova::Trs             transform;
    };

    struct Scene
    {
        std::vector<nova::Ref<MeshInstance>> instances;
    };
}