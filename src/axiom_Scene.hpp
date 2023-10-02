#pragma once

#include "axiom_Core.hpp"

namespace axiom
{
    struct ShadingAttrib
    {
        u32    normal : 16; // unorm8x2
        u32   tangent :  8; // snorm8
        u32  matIndex :  8; // uint8
        u32  texCoord;      // float16x2
    };

    struct TriMesh : nova::RefCounted
    {
        std::vector<Vec3>         positionAttribs;
        std::vector<ShadingAttrib> shadingAttribs;
        std::vector<u32>                  indices;
    };

    enum class TextureFormat
    {
        RGBA8_Srgb,
        RGBA8_Unorm,

        RG_Unorm,
    };

    struct TextureMap : nova::RefCounted
    {
        Vec2U           size;
        TextureFormat format;
        std::vector<b8> data;
    };

    struct TextureChannel
    {
        nova::Ref<TextureMap>   map;
        std::array<i32, 4> channels{ -1, -1, -1, -1 };

		TextureChannel() = default;

        TextureChannel(nova::Ref<TextureMap> _map, Span<i32> _channels)
            : map(std::move(_map))
        {
            std::copy(_channels.begin(), _channels.end(), channels.begin());
        }
    };

    struct UVMaterial : nova::RefCounted
    {
        TextureChannel       albedo;
        TextureChannel        alpha;
        TextureChannel      normals;
        TextureChannel   emissivity;
        TextureChannel transmission;
        TextureChannel    metalness;
        TextureChannel    roughness;

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