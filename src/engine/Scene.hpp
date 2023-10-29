#pragma once

#include "Engine.hpp"

#include <scene/runtime/axiom_Attributes.hpp>

namespace axiom
{
    struct ShadingAttribute
    {
        GPU_TangentSpace tangent_space;
        GPU_TexCoords    tex_coords;
    };

    struct SkinningAttribute
    {
        GPU_BoneWeights bone_weights;
    };

    template<class T>
    struct Index {
        u32 value = UINT_MAX;

        Index() noexcept
        {}

        Index(usz i) noexcept
            : value(u32(i))
        {}

        Index& operator=(usz i) noexcept
        {
            value = u32(i);
            return *this;
        }

        bool IsValid() const noexcept
        {
            return value != UINT_MAX;
        }

        template<class C>
        requires std::same_as<typename C::value_type, T>
        T& into(C& c) const noexcept
        {
            return c[value];
        }
    };

    struct Geometry
    {
        std::vector<u32>               indices;
        std::vector<Vec3>              position_attributes;
        std::vector<ShadingAttribute>  shading_attributes;
        std::vector<SkinningAttribute> skinning_attributes;
    };

    struct GeometryRange
    {
        Index<Geometry> geometry;
        u32             vertex_offset;
        u32             max_vertex;
        u32             first_index;
        u32             triangle_count;
    };

    struct Texture
    {
        Vec2U           size;
        u32             mips;
        nova::Format    format;
        std::vector<b8> data;
    };

    struct Material
    {
        Index<Texture> albedo_alpha;
        Index<Texture> metalness_roughness;
        Index<Texture> normal;
        Index<Texture> emission;
        Index<Texture> transmission;

        f32 ior;
        f32 alpha_cutoff;
    };

    struct TransformNode
    {
        glm::mat4x3          transform;
        Index<TransformNode> parent;
    };

    struct Mesh
    {
        Index<Material>      material;
        Index<GeometryRange> geometry_range;
        Index<TransformNode> transform;
    };

    struct MeshGroup
    {
        Index<TransformNode>     base_transform;
        std::vector<Index<Mesh>> meshes;

        bool opaque;
    };

    struct Scene
    {
        std::vector<Geometry>      geometries;
        std::vector<GeometryRange> geometry_ranges;

        std::vector<Texture>       textures;
        std::vector<Material>      materials;

        std::vector<TransformNode> transform_nodes;

        std::vector<Mesh>          meshes;
        std::vector<MeshGroup>     mesh_groups;
    };
}