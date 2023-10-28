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

            #define u32 uint
            #define u64 uint64_t
            #define f32 float

            #define BUFFER_REF(align) layout(buffer_reference, scalar, buffer_reference_align = align) buffer

            BUFFER_REF(4) readonly Index
            {
                u32 value;
            };

            BUFFER_REF(4) readonly PositionAttribute
            {
                vec3 position;
            };

            BUFFER_REF(4) readonly ShadingAttributes
            {
                u32 normal;
                u32 tangent;
                u32 tex_coords;
            };

            BUFFER_REF(4) readonly SkinningAttributes
            {
                u32 indices[2];
                u32 weights[2];
            };

            BUFFER_REF(8) readonly Geometry
            {
                Index              indices;
                PositionAttribute  position_attributes;
                ShadingAttributes  shading_attributes;
                SkinningAttributes skinning_attributes;
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
                u32 albedo_alpha;
                u32 metalness_roughness;
                u32 normal;
                u32 emission;
                u32 transmission;

                f32 ior;
                f32 alpha_cutoff;
            };

            BUFFER_REF(4) readonly TransformNode
            {
                mat4x3 transform;
                u32    parent;
            };

            BUFFER_REF(4) readonly TransformCache
            {
                mat4x3 transform;
            };

            BUFFER_REF(4) readonly Mesh
            {
                u32 material;
                u32 geometry_range;
                u32 transform;
            };

            layout(push_constant, scalar) readonly uniform PushConstants {
                Geometry       geometries;
                GeometryRange  geometry_ranges;
                Material       materials;
                TransformNode  transform_nodes;
                TransformCache transform_cache;
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
                        vec3 pos = geometry.position_attributes[gl_VertexIndex].position;
                        outPosition = pos;
                        gl_Position = pc.view_proj * vec4(pc.transform_cache[mesh.transform].transform * vec4(pos, 1), 1);
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
        geometry_ranges = nova::Buffer::Create(engine->context, nova::SizeOf<GeometryRange>(MaxGeometryRanges), usage, flags);
        textures.resize(MaxTextures);
        materials       = nova::Buffer::Create(engine->context, nova::SizeOf<Material>(MaxMaterials),           usage, flags);
        transform_nodes = nova::Buffer::Create(engine->context, nova::SizeOf<TransformNode>(MaxTransformNodes), usage, flags);
        transform_cache = nova::Buffer::Create(engine->context, nova::SizeOf<glm::mat4x3>(MaxTransformNodes),   usage, flags);
        meshes          = nova::Buffer::Create(engine->context, nova::SizeOf<Mesh>(MaxMeshes),                  usage, flags);
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
    }

    void Renderer::Update()
    {
        // Geometries

        for (u32 i = 0; i < scene->geometries.size(); ++i) {
            auto& geometry = scene->geometries[i];

            usz index_size    = geometry.indices.size()             * sizeof(u32);
            usz pos_size      = geometry.position_attributes.size() * sizeof(Vec3);
            usz shading_size  = geometry.shading_attributes.size()  * sizeof(ShadingAttribute);
            usz skinning_size = geometry.skinning_attributes.size() * sizeof(SkinningAttribute);

            GPU_Geometry gpu;
            gpu.indices_va             = 0;
            gpu.position_attributes_va = nova::AlignUpPower2(gpu.indices_va             + index_size,   4);
            gpu.shading_attributes_va  = nova::AlignUpPower2(gpu.position_attributes_va + pos_size,     4);
            gpu.skinning_attributes_va = nova::AlignUpPower2(gpu.shading_attributes_va  + shading_size, 4);

            auto buffer = nova::Buffer::Create(engine->context,
                gpu.skinning_attributes_va + skinning_size,
                nova::BufferUsage::Index       | nova::BufferUsage::Storage,
                nova::BufferFlags::DeviceLocal | nova::BufferFlags::Mapped);

            buffer.Set<u32>(              geometry.indices,             0, gpu.indices_va);
            buffer.Set<Vec3>(             geometry.position_attributes, 0, gpu.position_attributes_va);
            buffer.Set<ShadingAttribute>( geometry.shading_attributes,  0, gpu.shading_attributes_va);
            buffer.Set<SkinningAttribute>(geometry.skinning_attributes, 0, gpu.skinning_attributes_va);

            gpu.indices_va             += buffer.GetAddress();
            gpu.position_attributes_va += buffer.GetAddress();
            gpu.shading_attributes_va  += buffer.GetAddress();
            gpu.skinning_attributes_va += buffer.GetAddress();

            geometries.Set<GPU_Geometry>({ gpu }, i);

            // TODO: Scan through existing geometries and try to find best matches to avoid reallocations
            geometry_buffers[i].Destroy();
            geometry_buffers[i] = buffer;
        }

        geometry_ranges.Set<GeometryRange>(scene->geometry_ranges);

        // Textures

        for (u32 i = 0; i < scene->textures.size(); ++i) {
            auto& texture = scene->textures[i];

            textures[i] = nova::Texture::Create(engine->context,
                Vec3U(texture.size, 0),
                nova::TextureUsage::Sampled,
                texture.format,
                {});

            textures[i].Set({}, Vec3(texture.size, 1), texture.data.data());
        }

        // Materials

        for (u32 i = 0; i < scene->materials.size(); ++i) {
            auto material = scene->materials[i];

            material.albedo_alpha        = textures[material.albedo_alpha.value       ].GetDescriptor();
            material.metalness_roughness = textures[material.metalness_roughness.value].GetDescriptor();
            material.normal              = textures[material.normal.value             ].GetDescriptor();
            material.emission            = textures[material.emission.value           ].GetDescriptor();
            material.transmission        = textures[material.transmission.value       ].GetDescriptor();

            materials.Set<Material>({ material }, i);
        }

        // Transforms

        transform_nodes.Set<TransformNode>(scene->transform_nodes);

        for (u32 i = 0; i < scene->transform_nodes.size(); ++i) {
            auto* in_transform = &scene->transform_nodes[i];

            Mat4 tform = in_transform->transform;
            while (in_transform->parent.IsValid()) {
                in_transform = &in_transform->parent.into(scene->transform_nodes);
                tform = in_transform->transform * tform;
            }

            transform_cache.Set<glm::mat4x3>({ glm::mat4x3(tform) }, i);
        }
    }

    void Renderer::Draw()
    {
        auto cmd = engine->cmd;
        auto target = engine->swapchain.GetCurrent();
        auto extent = Vec2U(target.GetExtent());

        cmd.BeginRendering({{}, extent}, {target});
        cmd.ClearColor(0, Vec4(Vec3(0.1f), 1.f), extent);
        cmd.ResetGraphicsState();
        cmd.SetViewports({{Vec2I(0, extent.y), Vec2I(extent.x, -i32(extent.y))}}, true);
        cmd.SetBlendState({false});
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
            .transform_nodes_va = transform_nodes.GetAddress(),
            .transform_cache_va = transform_cache.GetAddress(),
            .meshes_va = meshes.GetAddress(),
            .view_proj = proj * view,
        });

        for (u32 i = 0; i < scene->meshes.size(); ++i) {
            auto& mesh = scene->meshes[i];
            auto& geom_range = mesh.geometry_range.into(scene->geometry_ranges);
            cmd.BindIndexBuffer(geometry_buffers[geom_range.geometry.value], nova::IndexType::U32);
            cmd.DrawIndexed(geom_range.triangle_count * 3, 1, geom_range.first_index, geom_range.vertex_offset, 0);
        }

        cmd.EndRendering();
    }
}