#pragma once

#include "Engine.hpp"
#include <imp/imp_Scene.hpp>

namespace axiom
{
    struct GPU_Geometry
    {
        u64 indices_va;
        u64 positions_va;
        u64 tangent_spaces_va;
        u64 tex_coords_va;
    };

    struct GPU_PushConstants
    {
        u64 geometries_va;
        u64 geometry_ranges_va;
        u64 materials_va;
        // u64 transform_nodes_va;
        // u64 transform_cache_va;
        u64 meshes_va;

        Mat4 view_proj;
    };

    struct Renderer
    {
        static constexpr u32 MaxGeometries     = 1ull << 20;
        static constexpr u32 MaxGeometryRanges = 1ull << 21;
        static constexpr u32 MaxTextures       = 1ull << 20;
        static constexpr u32 MaxMaterials      = 1ull << 20;
        static constexpr u32 MaxTransformNodes = 1ull << 21;
        static constexpr u32 MaxMeshes         = 1ull << 21;

    public:
        Engine* engine;
        imp::Scene*  scene;

        std::vector<nova::Buffer>  geometry_buffers;
        nova::Buffer               geometries;
        nova::Buffer               geometry_ranges;

        std::vector<nova::Image> textures;
        nova::Buffer               materials;

        nova::Buffer               transform_nodes;
        nova::Buffer               transform_cache;

        nova::Buffer meshes;
        std::vector<nova::AccelerationStructure> mesh_groups;

    public:
        nova::Image depth_buffer;

    public:
        Vec3 position = Vec3(0.f, 0.f, 1.f);
        Quat rotation = Vec3();
        f32  fov = glm::radians(90.f);

    public:
        nova::Shader vertex_shader;
        nova::Shader fragment_shader;

    public:
        void Init();
        void Destroy();
        void Update();
        void Draw();
    };
}