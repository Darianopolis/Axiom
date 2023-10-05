#pragma once

#include "axiom_Core.hpp"

namespace axiom
{
    namespace math
    {
// -----------------------------------------------------------------------------
//                               Encode Normals
// -----------------------------------------------------------------------------

        inline
        Vec3 SignedOctEncode(Vec3 n)
        {
            Vec3 outN;

            n /= (glm::abs(n.x) + glm::abs(n.y) + glm::abs(n.z));

            outN.y = n.y *  0.5f + 0.5f;
            outN.x = n.x *  0.5f + outN.y;
            outN.y = n.x * -0.5f + outN.y;

            outN.z = glm::clamp(n.z * FLT_MAX, 0.f, 1.f);
            return outN;
        }

// -----------------------------------------------------------------------------
//                              Decode Normals
// -----------------------------------------------------------------------------

        inline
        Vec3 SignedOctDecode(Vec3 n)
        {
            Vec3 outN;

            outN.x = (n.x - n.y);
            outN.y = (n.x + n.y) - 1.f;
            outN.z = n.z * 2.f - 1.f;
            outN.z = outN.z * (1.f - glm::abs(outN.x) - glm::abs(outN.y));

            outN = glm::normalize(outN);
            return outN;
        }

// -----------------------------------------------------------------------------
//                              Encode Tangents
// -----------------------------------------------------------------------------

        inline
        f32 EncodeDiamond(Vec2 p)
        {
            // Project to the unit diamond, then to the x-axis.
            f32 x = p.x / (glm::abs(p.x) + glm::abs(p.y));

            // Contract the x coordinate by a factor of 4 to represent all 4 quadrants in
            // the unit range and remap
            f32 pySign = glm::sign(p.y);
            return -pySign * 0.25f * x + 0.5f + pySign * 0.25f;
        }

        // Given a normal and tangent vector, encode the tangent as a single float that can be
        // subsequently quantized.
        inline
        f32 EncodeTangent(Vec3 normal, Vec3 tangent)
        {
            // First, find a canonical direction in the tangent plane
            Vec3 t1;
            if (glm::abs(normal.y) > glm::abs(normal.z))
            {
                // Pick a canonical direction orthogonal to n with z = 0
                t1 = Vec3(normal.y, -normal.x, 0.f);
            }
            else
            {
                // Pick a canonical direction orthogonal to n with y = 0
                t1 = Vec3(normal.z, 0.f, -normal.x);
            }
            t1 = glm::normalize(t1);

            // Construct t2 such that t1 and t2 span the plane
            Vec3 t2 = glm::cross(t1, normal);

            // Decompose the tangent into two coordinates in the canonical basis
            Vec2 packedTangent = Vec2(glm::dot(tangent, t1), glm::dot(tangent, t2));

            // Apply our diamond encoding to our two coordinates
            return EncodeDiamond(packedTangent);
        }

// -----------------------------------------------------------------------------
//                              Decode Tangents
// -----------------------------------------------------------------------------

        inline
        Vec2 DecodeDiamond(f32 p)
        {
            Vec2 v;

            // Remap p to the appropriate segment on the diamond
            f32 pSign = glm::sign(p - 0.5f);
            v.x = -pSign * 4.f * p + 1.f + pSign * 2.f;
            v.y = pSign * (1.f - glm::abs(v.x));

            // Normalization extends the point on the diamond back to the unit circle
            return glm::normalize(v);
        }

        inline
        Vec3 DecodeTangent(Vec3 normal, f32 diamondTangent)
        {
            // As in the encode step, find our canonical tangent basis span(t1, t2)
            Vec3 t1;
            if (glm::abs(normal.y) > glm::abs(normal.z)) {
                t1 = Vec3(normal.y, -normal.x, 0.f);
            } else {
                t1 = Vec3(normal.z, 0.f, -normal.x);
            }
            t1 = glm::normalize(t1);

            Vec3 t2 = glm::cross(t1, normal);

            // Recover the coordinates used with t1 and t2
            Vec2 packedTangent = DecodeDiamond(diamondTangent);

            return packedTangent.x * t1 + packedTangent.y * t2;
        }
    }

    struct ShadingAttrib
    {
        // // https://johnwhite3d.blogspot.com/2017/10/signed-octahedron-normal-encoding.html?view=classic
        u32 octX : 10;
        u32 octY : 10;
        u32 octS :  1;

        // // https://www.jeremyong.com/graphics/2023/01/09/tangent-spaces-and-diamond-encoding/
        u32 tgtA : 10;
        u32 tgtS :  1;

        // Just boring half float quantization for this
        u32 texCoords;
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