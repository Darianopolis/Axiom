#include "axiom_Renderer.hpp"

#include <nova/core/nova_ToString.hpp>
#include <nova/core/nova_Guards.hpp>

#include <nova/rhi/vulkan/glsl/nova_VulkanGlsl.hpp>

#include <rdo_bc_encoder.h>

namespace axiom
{
    struct CompiledMesh
    {
        i32                vertex_offset;
        u32                  first_index;
        u32              geometry_offset;
        nova::AccelerationStructure blas;
    };

    struct GPU_Material
    {
        u32     basecolor_alpha;
        u32             normals;
        u32          emissivity;
        u32        transmission;
        u32 metalness_roughness;

        f32 alpha_cutoff = 0.5f;
        bool  alpha_mask = false;
        bool alpha_blend = false;
        bool        thin = false;
        bool  subsurface = false;
    };

    struct GPU_InstanceData
    {
        u32 geometry_offset;
    };

    struct GPU_GeometryInfo
    {
        u64 shading_attributes;
        u64            indices;
        u64           material;
    };

    struct PathTraceRenderer : Renderer
    {
        CompiledScene* scene = nullptr;

        nova::Context context;

        nova::AccelerationStructure tlas;

        nova::Sampler linear_sampler;

        nova::Image accumulation_target;
        u32                  sample_count;

        nova::Buffer                        material_buffer;
        nova::HashMap<void*, u64>        material_addresses;
        nova::HashMap<void*, nova::Image> loaded_textures;

        nova::Buffer       shading_attributes_buffer;
        nova::Buffer                    index_buffer;
        nova::Buffer            geometry_info_buffer;
        nova::Buffer            instance_data_buffer;
        nova::HashMap<void*, CompiledMesh> mesh_data;

        nova::Buffer noise_buffer;

        nova::Buffer tlas_instance_buffer;

        nova::RayTracingPipeline pipeline;
        nova::Buffer           hit_groups;
        nova::Shader        anyhit_shader;
        nova::Shader    closesthit_shader;
        nova::Shader        raygen_shader;

        nova::Shader postprocess_shader;

        std::mt19937 rng;
        std::uniform_real_distribution<f32> dist{ 0.f, 1.f };

        Vec3 view_pos;
        Quat view_rot;
        f32  view_fov;

        PathTraceRenderer();
        ~PathTraceRenderer();

        void Init();
        void CompileMaterials(nova::CommandPool cmd_pool, nova::Fence fence);

        virtual void CompileScene(CompiledScene& scene, nova::CommandPool cmd_pool, nova::Fence fence);

        virtual void SetCamera(Vec3 position, Quat rotation, f32 aspect, f32 fov);
        virtual void Record(nova::CommandList cmd, nova::Image target);
        virtual void ResetSamples();
    };

    nova::Ref<Renderer> CreatePathTraceRenderer(nova::Context context)
    {
        auto renderer = nova::Ref<PathTraceRenderer>::Create();
        renderer->context = context;

        renderer->linear_sampler = nova::Sampler::Create(context, nova::Filter::Linear, nova::AddressMode::Repeat, nova::BorderColor::TransparentBlack, 16.f);

        renderer->noise_buffer = nova::Buffer::Create(context, 0, nova::BufferUsage::Storage,
            nova::BufferFlags::DeviceLocal | nova::BufferFlags::Mapped);

        return renderer;
    }

    PathTraceRenderer::PathTraceRenderer()
    {

    }

    void PathTraceRenderer::Init()
    {

    }

    PathTraceRenderer::~PathTraceRenderer()
    {
        shading_attributes_buffer.Destroy();
        index_buffer.Destroy();
        tlas_instance_buffer.Destroy();
        geometry_info_buffer.Destroy();
        instance_data_buffer.Destroy();
        noise_buffer.Destroy();
        hit_groups.Destroy();

        for (auto&[p, data] : mesh_data) {
            data.blas.Destroy();
        }
        tlas.Destroy();

        material_buffer.Destroy();
        for (auto&[p, texture] : loaded_textures) {
            texture.Destroy();
        }

        anyhit_shader.Destroy();
        closesthit_shader.Destroy();
        raygen_shader.Destroy();
        postprocess_shader.Destroy();
        pipeline.Destroy();

        linear_sampler.Destroy();

        accumulation_target.Destroy();
    }

    void PathTraceRenderer::CompileMaterials(nova::CommandPool cmd_pool, nova::Fence fence)
    {
        (void)cmd_pool, (void)fence;

        for (auto& texture : scene->textures) {
            loaded_textures.insert({ texture.Raw(), {} });
        }

        std::atomic_uint64_t total_resident_textures = 0;

#pragma omp parallel for
        for (u32 i = 0; i < scene->textures.size(); ++i) {
            auto& texture = scene->textures[i];
            auto& loaded_texture = loaded_textures.at(texture.Raw());

            if (texture->data.size()) {
                loaded_texture = nova::Image::Create(context,
                    Vec3U(texture->size, 0),
                    nova::ImageUsage::Sampled,
                    texture->format,
                    {});

                loaded_texture.Set({}, loaded_texture.GetExtent(),
                    texture->data.data());

                total_resident_textures += texture->data.size();
            }
        }

        NOVA_LOG("Total image memory resident: {}", nova::ByteSizeToString(total_resident_textures));

        material_buffer = nova::Buffer::Create(context,
            scene->materials.size() * sizeof(GPU_Material),
            nova::BufferUsage::Storage,
            nova::BufferFlags::DeviceLocal | nova::BufferFlags::Mapped);

        for (u32 i = 0; i < scene->materials.size(); ++i) {
            auto& material = scene->materials[i];

            u64 address = material_buffer.GetAddress() + (i * sizeof(GPU_Material));
            material_addresses[material.Raw()] = address;

            material_buffer.Set<GPU_Material>({{
                .basecolor_alpha     = loaded_textures.at(material->basecolor_alpha.Raw()    ).GetDescriptor(),
                .normals             = loaded_textures.at(material->normals.Raw()            ).GetDescriptor(),
                .emissivity          = loaded_textures.at(material->emissivity.Raw()         ).GetDescriptor(),
                .transmission        = loaded_textures.at(material->transmission.Raw()       ).GetDescriptor(),
                .metalness_roughness = loaded_textures.at(material->metalness_roughness.Raw()).GetDescriptor(),

                .alpha_cutoff = material->alpha_cutoff,
                .alpha_mask   = material->alpha_mask,
                .alpha_blend  = material->alpha_blend,
                .thin        = material->thin,
                .subsurface  = material->subsurface,
            }}, i);
        }
    }

    void PathTraceRenderer::CompileScene(CompiledScene& _scene, nova::CommandPool cmd_pool, nova::Fence fence)
    {
        scene = &_scene;

        // Shaders

        postprocess_shader = nova::Shader::Create(context, nova::ShaderStage::Compute, "main",
            nova::glsl::Compile(nova::ShaderStage::Compute, "main", "src/renderers/pathtracer/axiom_PostProcess.glsl", {}));

        anyhit_shader = nova::Shader::Create(context, nova::ShaderStage::AnyHit, "main",
            nova::glsl::Compile(nova::ShaderStage::AnyHit, "main", "src/renderers/pathtracer/axiom_AnyHit.glsl", {}));

        closesthit_shader = nova::Shader::Create(context, nova::ShaderStage::ClosestHit, "main",
            nova::glsl::Compile(nova::ShaderStage::ClosestHit, "main", "src/renderers/pathtracer/axiom_ClosestHit.glsl", {}));

        raygen_shader = nova::Shader::Create(context, nova::ShaderStage::RayGen, "main",
            nova::glsl::Compile(nova::ShaderStage::RayGen, "main", "src/renderers/pathtracer/axiom_RayGen.glsl", {}));
            // nova::glsl::Compile(nova::ShaderStage::RayGen, "main", "src/renderers/pathtracer/axiom_RayDebug.glsl", {}));

        constexpr u32 SBT_Opaque      = 0;
        constexpr u32 SBT_AlphaMasked = 1;

        pipeline = nova::RayTracingPipeline::Create(context);
        pipeline.Update(raygen_shader, {}, {
            { closesthit_shader               }, // Opaque
            { closesthit_shader, anyhit_shader }, // Alpha-tested
        }, {});

        // Materials

        CompileMaterials(cmd_pool, fence);

        // Geometry

        u64 vertex_count = 0;
        u64 max_per_blas_vertex_count = 0;
        u64 index_count = 0;
        for (auto& mesh : scene->meshes) {
            max_per_blas_vertex_count = std::max(max_per_blas_vertex_count, mesh->position_attributes.size());
            vertex_count += mesh->position_attributes.size();
            index_count += mesh->indices.size();
        }

#ifdef AXIOM_TRACE_COMPILE // --------------------------------------------------
        NOVA_LOG("Compiling, unique vertices = {}, unique indices = {}", vertex_count, index_count);
#endif // ----------------------------------------------------------------------

        shading_attributes_buffer = nova::Buffer::Create(context,
            vertex_count * sizeof(ShadingAttributes),
            nova::BufferUsage::Storage | nova::BufferUsage::AccelBuild,
            nova::BufferFlags::DeviceLocal | nova::BufferFlags::Mapped);

        index_buffer = nova::Buffer::Create(context,
            index_count * sizeof(u32),
            nova::BufferUsage::Index | nova::BufferUsage::AccelBuild,
            nova::BufferFlags::DeviceLocal | nova::BufferFlags::Mapped);

        u32 geometry_count = 0;

        u64 vertex_offset = 0;
        u64 index_offset = 0;
        NOVA_LOGEXPR(scene->meshes.size());
        for (auto& mesh : scene->meshes) {
            mesh_data[mesh.Raw()] = CompiledMesh{ i32(vertex_offset), u32(index_offset), geometry_count };

            shading_attributes_buffer.Set<ShadingAttributes>(mesh->shading_attributes, vertex_offset);
            vertex_offset += mesh->position_attributes.size();

            index_buffer.Set<u32>(mesh->indices, index_offset);
            index_offset += mesh->indices.size();

            geometry_count += u32(mesh->sub_meshes.size());
        }

        geometry_info_buffer = nova::Buffer::Create(context,
            geometry_count * sizeof(GPU_GeometryInfo),
            nova::BufferUsage::Storage,
            nova::BufferFlags::DeviceLocal | nova::BufferFlags::Mapped);

        hit_groups = nova::Buffer::Create(context,
            pipeline.GetTableSize(geometry_count),
            nova::BufferUsage::ShaderBindingTable,
            nova::BufferFlags::DeviceLocal | nova::BufferFlags::Mapped);

        auto builder = nova::AccelerationStructureBuilder::Create(context);
        auto scratch = nova::Buffer::Create(context, 0, nova::BufferUsage::Storage, nova::BufferFlags::DeviceLocal);
        NOVA_DEFER(&) {
            builder.Destroy();
            scratch.Destroy();
        };

        {
            // Create temporary buffer to hold vertex positions

            constexpr usz MinAccelInputBufferSize = 64ull * 1024 * 1024;
            auto pos_attrib_buffer = nova::Buffer::Create(context,
                std::max(MinAccelInputBufferSize, max_per_blas_vertex_count * sizeof(Vec3)),
                nova::BufferUsage::Storage | nova::BufferUsage::AccelBuild,
                nova::BufferFlags::DeviceLocal | nova::BufferFlags::Mapped);
            NOVA_DEFER(&) { pos_attrib_buffer.Destroy(); };

            // Find maximal scratch and build BLAS sizes for all meshes

            u64 scratch_size = 0;
            u64 build_blas_size = 0;
            for (u32 i = 0; i < scene->meshes.size(); ++i) {
                auto& mesh = scene->meshes[i];
                auto& data = mesh_data.at(mesh.Raw());

                builder.Prepare(
                    nova::AccelerationStructureType::BottomLevel,
                    nova::AccelerationStructureFlags::AllowDataAccess
                    | nova::AccelerationStructureFlags::AllowCompaction
                    | nova::AccelerationStructureFlags::PreferFastTrace, u32(mesh->sub_meshes.size()));

                for (u32 j = 0; j < mesh->sub_meshes.size(); ++j) {
                    auto& sub_mesh = mesh->sub_meshes[j];

                    builder.SetTriangles(j,
                        pos_attrib_buffer.GetAddress() +  sub_mesh.vertex_offset                  * sizeof(Vec3), nova::Format::RGBA32_SFloat, u32(sizeof(Vec3)), sub_mesh.max_vertex,
                            index_buffer.GetAddress() + (sub_mesh.first_index + data.first_index) * sizeof(u32),  nova::IndexType::U32,                           sub_mesh.index_count / 3);
                }

                scratch_size = std::max(scratch_size, builder.GetBuildScratchSize());
                build_blas_size = std::max(build_blas_size, builder.GetBuildSize());
            }

            // Create temporary scratch and build BLAS

            scratch.Resize(scratch_size);
            auto build_blas = nova::AccelerationStructure::Create(context, build_blas_size,
                nova::AccelerationStructureType::BottomLevel);
            NOVA_DEFER(&) { build_blas.Destroy(); };

            for (u32 i = 0; i < scene->meshes.size(); ++i) {
                auto& mesh = scene->meshes[i];
                auto& data = mesh_data.at(mesh.Raw());

                // Load position data

                pos_attrib_buffer.Set<Vec3>(mesh->position_attributes);
                builder.Prepare(
                    nova::AccelerationStructureType::BottomLevel,
                    nova::AccelerationStructureFlags::AllowDataAccess
                    | nova::AccelerationStructureFlags::AllowCompaction
                    | nova::AccelerationStructureFlags::PreferFastTrace, u32(mesh->sub_meshes.size()));

                for (u32 j = 0; j < mesh->sub_meshes.size(); ++j) {
                    auto& sub_mesh = mesh->sub_meshes[j];
                    auto geometry_index = data.geometry_offset + j;

                    // Add geometry to build

                    builder.SetTriangles(j,
                        pos_attrib_buffer.GetAddress() +  sub_mesh.vertex_offset                  * sizeof(Vec3), nova::Format::RGBA32_SFloat, u32(sizeof(Vec3)), sub_mesh.max_vertex,
                            index_buffer.GetAddress() + (sub_mesh.first_index + data.first_index) * sizeof(u32),  nova::IndexType::U32,                           sub_mesh.index_count / 3);

                    // Store geometry offsets and material

                    geometry_info_buffer.Set<GPU_GeometryInfo>({{
                        .shading_attributes = shading_attributes_buffer.GetAddress() + (data.vertex_offset + sub_mesh.vertex_offset) * sizeof(ShadingAttributes),
                        .indices =                index_buffer.GetAddress() + (data.first_index   + sub_mesh.first_index  ) * sizeof(u32),
                        .material = material_addresses.at(sub_mesh.material.Raw()),
                    }}, geometry_index);

                    // Bind shaders

                    u32 sbt_index = SBT_Opaque;
                    if (sub_mesh.material->alpha_mask
                            || sub_mesh.material->alpha_blend) {
                        sbt_index = SBT_AlphaMasked;
                    }
                    pipeline.WriteHandle(hit_groups.GetMapped(), geometry_index, sbt_index);
                }

                // Build

                auto cmd = cmd_pool.Begin();
                cmd.BuildAccelerationStructure(builder, build_blas, scratch);
                context.GetQueue(nova::QueueFlags::Graphics, 0).Submit({cmd}, {}, {fence});
                fence.Wait();

                // Create final BLAS and compact

                data.blas = nova::AccelerationStructure::Create(context, builder.GetCompactSize(),
                    nova::AccelerationStructureType::BottomLevel);
                cmd = cmd_pool.Begin();
                cmd.CompactAccelerationStructure(data.blas, build_blas);
                context.GetQueue(nova::QueueFlags::Graphics, 0).Submit({cmd}, {}, {fence});
                fence.Wait();
            }
        }

        instance_data_buffer = nova::Buffer::Create(context,
            scene->instances.size() * sizeof(GPU_InstanceData),
            nova::BufferUsage::Storage,
            nova::BufferFlags::DeviceLocal | nova::BufferFlags::Mapped);

        tlas_instance_buffer = nova::Buffer::Create(context,
            scene->instances.size() * builder.GetInstanceSize(),
            nova::BufferUsage::AccelBuild,
            nova::BufferFlags::DeviceLocal | nova::BufferFlags::Mapped);

#ifdef AXIOM_TRACE_COMPILE // --------------------------------------------------
        u64 instanced_vertex_count = 0;
        u64 instanced_index_count = 0;
#endif // ----------------------------------------------------------------------

        u32 selected_instance_count = 0;
        for (u32 i = 0; i < scene->instances.size(); ++i) {
            auto& instance = scene->instances[i];
            auto& data = mesh_data.at(instance->mesh.Raw());
            if (!data.blas)
                continue;

            instance_data_buffer.Set<GPU_InstanceData>({{
                .geometry_offset = data.geometry_offset,
            }}, selected_instance_count);

            builder.WriteInstance(
                tlas_instance_buffer.GetMapped(),
                selected_instance_count,
                data.blas,
                instance->transform,
                data.geometry_offset,
                0xFF,
                data.geometry_offset,
                {});
            selected_instance_count++;

#ifdef AXIOM_TRACE_COMPILE // --------------------------------------------------
            instanced_vertex_count += instance->mesh->position_attributes.size();
            instanced_index_count += instance->mesh->indices.size();
#endif // ----------------------------------------------------------------------
        }

#ifdef AXIOM_TRACE_COMPILE // --------------------------------------------------
        NOVA_LOG("Compiling scene:");
        NOVA_LOG("  vertices   = {}", vertex_count);
        NOVA_LOG("  indices    = {}", index_count);
        NOVA_LOG("  meshes     = {}", scene->meshes.size());
        NOVA_LOG("  geometries = {}", geometry_count);
        NOVA_LOG("  instances  = {}", scene->instances.size());
        NOVA_LOG("  triangles  = {}", instanced_index_count / 3);
#endif // ----------------------------------------------------------------------

        {
            builder.SetInstances(0, tlas_instance_buffer.GetAddress(), selected_instance_count);
            builder.Prepare(
                nova::AccelerationStructureType::TopLevel,
                nova::AccelerationStructureFlags::AllowDataAccess
                | nova::AccelerationStructureFlags::PreferFastTrace, 1);

            scratch.Resize(builder.GetBuildScratchSize());

            auto cmd = cmd_pool.Begin();
            tlas = nova::AccelerationStructure::Create(context, builder.GetBuildSize(),
                nova::AccelerationStructureType::TopLevel);
            cmd.BuildAccelerationStructure(builder, tlas, scratch);
            context.GetQueue(nova::QueueFlags::Graphics, 0).Submit({cmd}, {}, {fence});
            fence.Wait();
        }
    }

    void PathTraceRenderer::SetCamera(Vec3 position, Quat rotation, f32 aspect, f32 fov)
    {
        (void)aspect;

        if (view_pos != position || view_rot != rotation || view_fov != fov) {
            view_pos = position;
            view_rot = rotation;
            view_fov = fov;

            sample_count = 0;
        }
    }

    void PathTraceRenderer::Record(nova::CommandList cmd, nova::Image target)
    {
        auto size = target.GetExtent();

        // Resize backing buffers

        if (!accumulation_target || accumulation_target.GetExtent() != size) {
            accumulation_target.Destroy();

            accumulation_target = nova::Image::Create(context, Vec3U(Vec2U(size), 0),
                nova::ImageUsage::Storage,
                nova::Format::RGBA32_SFloat,
                {});

            accumulation_target.Transition(nova::ImageLayout::GeneralImage);

            sample_count = 0;
        }

        // Randomness

        {
            u32 noise_len = (size.x + size.y) * 2;
            noise_buffer.Resize(noise_len * sizeof(u32));

            u32* noise = reinterpret_cast<u32*>(noise_buffer.GetMapped());
            for (u32 i = 0; i < noise_len; ++i) {
                noise[i] = rng();
            }
        }

        // Trace rays

        struct PC_RayTrace
        {
            u64        tlas;
            u64  geometries;
            u64   instances;
            u64  noise_seed;

            u32 target;

            Vec3         pos;
            Vec3       cam_x;
            Vec3       cam_y;
            f32 cam_z_offset;

            u32 linear_sampler;

            u32 sample_count;

            Vec2 jitter;

            u32 sample_radius;
        };

        // Apply pixel jitter for all non-initial samples
        auto jitter = sample_count == 0
            ? Vec2(0.5f)
            : Vec2(dist(rng), dist(rng));

        cmd.PushConstants(PC_RayTrace {
            .tlas = tlas.GetAddress(),
            .geometries = geometry_info_buffer.GetAddress(),
            .instances = instance_data_buffer.GetAddress(),
            .noise_seed = noise_buffer.GetAddress(),
            .target = accumulation_target.GetDescriptor(),
            // .target = target.GetDescriptor(),
            .pos = view_pos,
            .cam_x = view_rot * Vec3(1.f, 0.f, 0.f),
            .cam_y = view_rot * Vec3(0.f, 1.f, 0.f),
            .cam_z_offset = 1.f / glm::tan(0.5f * view_fov),
            .linear_sampler = linear_sampler.GetDescriptor(),
            .sample_count = sample_count,
            .jitter = jitter,
            .sample_radius = sample_radius,
        });

        sample_count++;

        cmd.TraceRays(pipeline, Vec3U(Vec2U(target.GetExtent()) / Vec2U(sample_radius), 1), hit_groups.GetAddress(), 1);

        // Post process

        struct PC_PostProcess
        {
            Vec2U   size;
            u32   source;
            u32   target;
            f32 exposure;
            u32     mode;
        };

        cmd.PushConstants(PC_PostProcess {
            .size = Vec2U(target.GetExtent()),
            .source = accumulation_target.GetDescriptor(),
            .target = target.GetDescriptor(),
            .exposure = exposure,
            .mode = u32(mode),
        });

        Quat q = Quat(Vec3(0.f));
        Quat q2 = glm::angleAxis(0.f, Vec3(1.f));

        Vec3 v = q * Vec3(1.f);

        cmd.BindShaders({ postprocess_shader });
        cmd.Barrier(nova::PipelineStage::RayTracing, nova::PipelineStage::Compute);
        cmd.Dispatch(Vec3U((Vec2U(size) + Vec2U(15)) / Vec2U(16), 1));
    }

    void PathTraceRenderer::ResetSamples()
    {
        sample_count = 0;
    }
}