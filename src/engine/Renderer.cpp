#include "Renderer.hpp"

#include <nova/rhi/vulkan/glsl/nova_VulkanGlsl.hpp>

namespace axiom
{
    static
    Mat4 ProjInfReversedZRH(f32 fovY, f32 aspectWbyH, f32 zNear)
    {
        // https://nlguillemot.wordpress.com/2016/12/07/reversed-z-in-opengl/

        f32 f = 1.f / glm::tan(fovY / 2.f);
        Mat4 proj{};
        proj[0][0] = f / aspectWbyH;
        proj[1][1] = f;
        proj[3][2] = zNear; // Right, middle-bottom
        proj[2][3] = -1.f;  // Bottom, middle-right

        return proj;
    }

    void Renderer::Init()
    {
        auto preamble = R"glsl(
            #extension GL_EXT_scalar_block_layout                    : require
            #extension GL_EXT_buffer_reference2                      : require
            #extension GL_EXT_nonuniform_qualifier                   : require
            #extension GL_EXT_shader_image_load_formatted            : require
            #extension GL_EXT_shader_explicit_arithmetic_types_int8  : require
            #extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

            #define i32 int
            #define u32 uint
            #define u64 uint64_t
            #define f32 float

            #define BUFFER_REF(align) layout(buffer_reference, scalar, buffer_reference_align = align) buffer

            BUFFER_REF(4) readonly Index
            {
                u32 value;
            };

            BUFFER_REF(4) readonly Position
            {
                vec3 value;
            };

            BUFFER_REF(4) readonly TangentSpace
            {
                u32 packed;
            };

            BUFFER_REF(4) readonly TexCoord
            {
                u32 packed;
            };

            BUFFER_REF(8) readonly Geometry
            {
                Index        indices;
                Position     positions;
                TangentSpace tangent_spaces;
                TexCoord     tex_coords;
            };

            BUFFER_REF(4) readonly GeometryRange
            {
                u32 geometry;
                u32 vertex_offset;
                u32 max_vertex;
                u32 first_index;
                u32 triangle_count;
            };

            BUFFER_REF(4) readonly Material
            {
                i32 albedo_alpha_texture;
                u32 albedo_alpha;

                i32 metalness_texture;
                i32 roughness_texture;
                u32 metalness_roughness;

                i32 normal_texture;

                i32 emission_texture;
                u32 emission_factor;

                i32 transmission_texture;
                u32 transmission_factor;
            };

            // BUFFER_REF(4) readonly TransformNode
            // {
            //     mat4x3 transform;
            //     u32    parent;
            // };

            // BUFFER_REF(4) readonly TransformCache
            // {
            //     mat4x3 transform;
            // };

            BUFFER_REF(4) readonly Mesh
            {
                u32    geometry_range;
                mat4x3 transform;
            };

            layout(push_constant, scalar) readonly uniform PushConstants {
                Geometry       geometries;
                GeometryRange  geometry_ranges;
                Material       materials;
                // TransformNode  transform_nodes;
                // TransformCache transform_cache;
                Mesh           meshes;

                mat4 view_proj;
            } pc;
        )glsl";

        vertex_shader = nova::Shader::Create(engine->context, nova::ShaderStage::Vertex, "main", {
            nova::glsl::Compile(nova::ShaderStage::Vertex, "main", "", {
                preamble,
                R"glsl(
                    layout(location = 0) out vec3 outPosition;
                    void main() {
                        Mesh mesh = pc.meshes[gl_InstanceIndex];
                        GeometryRange geom_range = pc.geometry_ranges[mesh.geometry_range];
                        Geometry geometry = pc.geometries[geom_range.geometry];
                        vec3 pos = geometry.positions[gl_VertexIndex].value;
                        outPosition = pos;
                        gl_Position = pc.view_proj * vec4(mesh.transform * vec4(pos, 1), 1);
                    }
                )glsl"
            })
        });

        fragment_shader = nova::Shader::Create(engine->context, nova::ShaderStage::Fragment, "main", {
            nova::glsl::Compile(nova::ShaderStage::Fragment, "main", "", {
                R"glsl(
                    #extension GL_EXT_fragment_shader_barycentric : require

                    layout(location = 0) in pervertexEXT vec3 inPosition[3];
                    layout(location = 0) out vec4 outColor;

                    void main()
                    {
                        vec3 v01 = inPosition[1] - inPosition[0];
                        vec3 v02 = inPosition[2] - inPosition[0];
                        vec3 nrm = normalize(cross(v01, v02));
                        if (!gl_FrontFacing) {
                            nrm = -nrm;
                        }
                        outColor = vec4((nrm * 0.5 + 0.5) * 0.75, 1.0);
                    }
                )glsl"
            })
        });

        constexpr auto usage = nova::BufferUsage::Storage;
        constexpr auto flags = nova::BufferFlags::DeviceLocal | nova::BufferFlags::Mapped;

        geometry_buffers.resize(MaxGeometries);
        geometries      = nova::Buffer::Create(engine->context, nova::SizeOf<GPU_Geometry>(MaxGeometries),      usage, flags);
        geometry_ranges = nova::Buffer::Create(engine->context, nova::SizeOf<imp::GeometryRange>(MaxGeometryRanges), usage, flags);
        textures.resize(MaxTextures);
        materials       = nova::Buffer::Create(engine->context, nova::SizeOf<imp::Material>(MaxMaterials),           usage, flags);
        // transform_nodes = nova::Buffer::Create(engine->context, nova::SizeOf<TransformNode>(MaxTransformNodes), usage, flags);
        // transform_cache = nova::Buffer::Create(engine->context, nova::SizeOf<glm::mat4x3>(MaxTransformNodes),   usage, flags);
        meshes          = nova::Buffer::Create(engine->context, nova::SizeOf<imp::Mesh>(MaxMeshes),                  usage, flags);
    }

    void Renderer::Destroy()
    {
        vertex_shader.Destroy();
        fragment_shader.Destroy();
        meshes.Destroy();
        transform_cache.Destroy();
        transform_nodes.Destroy();
        materials.Destroy();
        geometry_ranges.Destroy();
        geometries.Destroy();
        for (auto& buffer : geometry_buffers) {
            buffer.Destroy();
        }
        depth_buffer.Destroy();
    }

    void Renderer::Update()
    {
        // Geometries

        for (u32 i = 0; i < scene->geometries.count; ++i) {
            auto& geometry = scene->geometries[i];

            usz index_size         = geometry.indices.count        * sizeof(u32);
            usz pos_size           = geometry.positions.count      * sizeof(Vec3);
            usz tangent_space_size = geometry.tangent_spaces.count * sizeof(imp::Basis);
            usz tex_coord_size     = geometry.tex_coords.count     * sizeof(imp::Vec2<imp::Float16>);

            GPU_Geometry gpu;
            gpu.indices_va        = 0;
            gpu.positions_va      = nova::AlignUpPower2(gpu.indices_va        + index_size,         4);
            gpu.tangent_spaces_va = nova::AlignUpPower2(gpu.positions_va      + pos_size,           4);
            gpu.tex_coords_va     = nova::AlignUpPower2(gpu.tangent_spaces_va + tangent_space_size, 4);

            auto buffer = nova::Buffer::Create(engine->context,
                gpu.tex_coords_va + tex_coord_size,
                nova::BufferUsage::Index       | nova::BufferUsage::Storage,
                nova::BufferFlags::DeviceLocal | nova::BufferFlags::Mapped);

            buffer.Set<u32>(       nova::Span(geometry.indices.begin,        geometry.indices.count),        0, gpu.indices_va);
            buffer.Set<Vec3>(      nova::Span(geometry.positions.begin,      geometry.positions.count),      0, gpu.positions_va);
            buffer.Set<imp::Basis>(nova::Span(geometry.tangent_spaces.begin, geometry.tangent_spaces.count), 0, gpu.tangent_spaces_va);
            buffer.Set<imp::Vec2<imp::Float16>>(
                                   nova::Span(geometry.tex_coords.begin,     geometry.tex_coords.count),     0, gpu.tex_coords_va);

            gpu.indices_va        += buffer.GetAddress();
            gpu.positions_va      += buffer.GetAddress();
            gpu.tangent_spaces_va += buffer.GetAddress();
            gpu.tex_coords_va     += buffer.GetAddress();

            geometries.Set<GPU_Geometry>({ gpu }, i);

            // TODO: Scan through existing geometries and try to find best matches to avoid reallocations
            geometry_buffers[i].Destroy();
            geometry_buffers[i] = buffer;
        }

        geometry_ranges.Set<imp::GeometryRange>(nova::Span(scene->geometry_ranges.begin, scene->geometry_ranges.count));

        // Textures

        // for (u32 i = 0; i < scene->textures.count; ++i) {
        //     auto& texture = scene->textures[i];

        //     nova::Format nova_format;
        //     switch (texture.format) {
        //             using enum imp::TextureFormat;
        //         break;case RGBA8_UNORM: nova_format = nova::Format::RGBA8_UNorm;
        //         break;case RGBA8_SRGB:  nova_format = nova::Format::RGBA8_SRGB;
        //         break;case RG8_UNORM:   nova_format = nova::Format::RG8_UNorm;
        //         break;case R8_UNORM:    nova_format = nova::Format::R8_UNorm;
        //         break;default: std::unreachable();
        //     }

        //     textures[i] = nova::Texture::Create(engine->context,
        //         Vec3U(texture.size, 0),
        //         nova::TextureUsage::Sampled,
        //         nova_format,
        //         {});

        //     textures[i].Set({}, Vec3(texture.size, 1), texture.data.begin);
        // }

        // Materials

        // for (u32 i = 0; i < scene->materials.count; ++i) {
        //     auto material = scene->materials[i];

        //     { if (auto& d = material.albedo_alpha_texture; d != -1) d = textures[d].GetDescriptor(); }
        //     { if (auto& d = material.metalness_texture;    d != -1) d = textures[d].GetDescriptor(); }
        //     { if (auto& d = material.roughness_texture;    d != -1) d = textures[d].GetDescriptor(); }
        //     { if (auto& d = material.normal_texture;       d != -1) d = textures[d].GetDescriptor(); }
        //     { if (auto& d = material.emission_texture;     d != -1) d = textures[d].GetDescriptor(); }
        //     { if (auto& d = material.transmission_texture; d != -1) d = textures[d].GetDescriptor(); }

        //     materials.Set<imp::Material>({ material }, i);
        // }

        // Meshes

        meshes.Set<imp::Mesh>(nova::Span(scene->meshes.begin, scene->meshes.count));

        // // Transforms

        // transform_nodes.Set<TransformNode>(scene->transform_nodes);

        // for (u32 i = 0; i < scene->transform_nodes.size(); ++i) {
        //     auto* in_transform = &scene->transform_nodes[i];

        //     Mat4 tform = in_transform->transform;
        //     while (in_transform->parent.IsValid()) {
        //         in_transform = &in_transform->parent.into(scene->transform_nodes);
        //         tform = in_transform->transform * tform;
        //     }

        //     transform_cache.Set<glm::mat4x3>({ glm::mat4x3(tform) }, i);
        // }
    }

    void Renderer::Draw()
    {
        auto cmd = engine->cmd;
        auto target = engine->swapchain.GetCurrent();
        auto extent = Vec2U(target.GetExtent());

        if (!depth_buffer || depth_buffer.GetExtent() != target.GetExtent()) {
            depth_buffer.Destroy();

            depth_buffer = nova::Texture::Create(engine->context, { extent, 0 },
                nova::TextureUsage::DepthStencilAttach,
                nova::Format::D32_SFloat,
                {});
        }

        cmd.BeginRendering({{}, extent}, {target}, depth_buffer);
        cmd.ClearColor(0, Vec4(Vec3(0.1f), 1.f), extent);
        cmd.ClearDepth(0.f, extent);
        cmd.ResetGraphicsState();
        cmd.SetViewports({{Vec2I(0, extent.y), Vec2I(extent.x, -i32(extent.y))}}, true);
        cmd.SetDepthState(true, true, nova::CompareOp::Greater);
        cmd.SetBlendState({ true, false });
        cmd.BindShaders({vertex_shader, fragment_shader});

        f32 aspect = f32(extent.x) / f32(extent.y);
        auto proj = ProjInfReversedZRH(fov, aspect, 0.01f);
        auto mTranslation = glm::translate(glm::mat4(1.f), position);
        auto mRotation = glm::mat4_cast(rotation);
        auto view = glm::affineInverse(mTranslation * mRotation);

        cmd.PushConstants(GPU_PushConstants {
            .geometries_va = geometries.GetAddress(),
            .geometry_ranges_va = geometry_ranges.GetAddress(),
            .materials_va = materials.GetAddress(),
            // .transform_nodes_va = transform_nodes.GetAddress(),
            // .transform_cache_va = transform_cache.GetAddress(),
            .meshes_va = meshes.GetAddress(),
            .view_proj = proj * view,
        });

        for (u32 i = 0; i < scene->meshes.count; ++i) {
            auto& mesh = scene->meshes[i];
            auto& geom_range = scene->geometry_ranges[mesh.geometry_range_idx];
            cmd.BindIndexBuffer(geometry_buffers[geom_range.geometry_idx], nova::IndexType::U32);
            cmd.DrawIndexed(geom_range.triangle_count * 3, 1, geom_range.first_index, geom_range.vertex_offset, 0);
        }

        cmd.EndRendering();
    }
}