#include "axiom_Renderer.hpp"

#include <nova/core/nova_ToString.hpp>
#include <nova/core/nova_Guards.hpp>

#include <nova/rhi/vulkan/glsl/nova_VulkanGlsl.hpp>

#include <rdo_bc_encoder.h>

namespace axiom
{
    struct CompiledMesh
    {
        i32                 vertexOffset;
        u32                   firstIndex;
        u32               geometryOffset;
        nova::AccelerationStructure blas;
    };

    struct GPU_Material
    {
        u32     baseColor_alpha;
        u32             normals;
        u32          emissivity;
        u32        transmission;
        u32 metalness_roughness;

        f32 alphaCutoff = 0.5f;
        bool  alphaMask = false;
        bool alphaBlend = false;
        bool       thin = false;
        bool subsurface = false;
    };

    struct GPU_InstanceData
    {
        u32 geometryOffset;
    };

    struct GPU_GeometryInfo
    {
        u64 shadingAttributes;
        u64           indices;
        u64          material;
    };

    struct PathTraceRenderer : Renderer
    {
        CompiledScene* scene = nullptr;

        nova::Context          context;

        nova::AccelerationStructure tlas;

        nova::Sampler             linearSampler;

        nova::Texture             accumulationTarget;
        u32                              sampleCount;

        nova::Buffer                        materialBuffer;
        nova::HashMap<void*, u64>        materialAddresses;
        nova::HashMap<void*, nova::Texture> loadedTextures;

        nova::Buffer        shadingAttributesBuffer;
        nova::Buffer                    indexBuffer;
        nova::Buffer             geometryInfoBuffer;
        nova::Buffer             instanceDataBuffer;
        nova::HashMap<void*, CompiledMesh> meshData;

        nova::Buffer noiseBuffer;

        nova::Buffer tlasInstanceBuffer;

        nova::RayTracingPipeline pipeline;
        nova::Buffer            hitGroups;
        nova::Shader         anyHitShader;
        nova::Shader     closestHitShader;
        nova::Shader         rayGenShader;

        nova::Shader    postProcessShader;

        std::mt19937 rng;
        std::uniform_real_distribution<f32> dist{ 0.f, 1.f };

        Vec3 viewPos;
        Quat viewRot;
        f32  viewFov;

        PathTraceRenderer();
        ~PathTraceRenderer();

        void Init();
        void CompileMaterials(nova::CommandPool cmdPool, nova::Fence fence);

        virtual void CompileScene(CompiledScene& scene, nova::CommandPool cmdPool, nova::Fence fence);

        virtual void SetCamera(Vec3 position, Quat rotation, f32 aspect, f32 fov);
        virtual void Record(nova::CommandList cmd, nova::Texture target);
        virtual void ResetSamples();
    };

    nova::Ref<Renderer> CreatePathTraceRenderer(nova::Context context)
    {
        auto renderer = nova::Ref<PathTraceRenderer>::Create();
        renderer->context = context;

        renderer->linearSampler = nova::Sampler::Create(context, nova::Filter::Linear, nova::AddressMode::Repeat, nova::BorderColor::TransparentBlack, 16.f);

        renderer->noiseBuffer = nova::Buffer::Create(context, 0, nova::BufferUsage::Storage,
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
        shadingAttributesBuffer.Destroy();
        indexBuffer.Destroy();
        tlasInstanceBuffer.Destroy();
        geometryInfoBuffer.Destroy();
        instanceDataBuffer.Destroy();
        noiseBuffer.Destroy();
        hitGroups.Destroy();

        for (auto&[p, data] : meshData) {
            data.blas.Destroy();
        }
        tlas.Destroy();

        materialBuffer.Destroy();
        for (auto&[p, texture] : loadedTextures) {
            texture.Destroy();
        }

        anyHitShader.Destroy();
        closestHitShader.Destroy();
        rayGenShader.Destroy();
        postProcessShader.Destroy();
        pipeline.Destroy();

        linearSampler.Destroy();

        accumulationTarget.Destroy();
    }

    void PathTraceRenderer::CompileMaterials(nova::CommandPool cmdPool, nova::Fence fence)
    {
        (void)cmdPool, (void)fence;

        for (auto& texture : scene->textures) {
            loadedTextures.insert({ texture.Raw(), {} });
        }

        std::atomic_uint64_t totalResidentTextures = 0;

#pragma omp parallel for
        for (u32 i = 0; i < scene->textures.size(); ++i) {
            auto& texture = scene->textures[i];
            auto& loadedTexture = loadedTextures.at(texture.Raw());

            if (texture->data.size()) {
                loadedTexture = nova::Texture::Create(context,
                    Vec3U(texture->size, 0),
                    nova::TextureUsage::Sampled,
                    texture->format,
                    {});

                loadedTexture.Set({}, loadedTexture.GetExtent(),
                    texture->data.data());

                totalResidentTextures += texture->data.size();
            }
        }

        NOVA_LOG("Total image memory resident: {}", nova::ByteSizeToString(totalResidentTextures));

        materialBuffer = nova::Buffer::Create(context,
            scene->materials.size() * sizeof(GPU_Material),
            nova::BufferUsage::Storage,
            nova::BufferFlags::DeviceLocal | nova::BufferFlags::Mapped);

        for (u32 i = 0; i < scene->materials.size(); ++i) {
            auto& material = scene->materials[i];

            u64 address = materialBuffer.GetAddress() + (i * sizeof(GPU_Material));
            materialAddresses[material.Raw()] = address;

            materialBuffer.Set<GPU_Material>({{
                .baseColor_alpha     = loadedTextures.at(material->baseColor_alpha.Raw()    ).GetDescriptor(),
                .normals             = loadedTextures.at(material->normals.Raw()            ).GetDescriptor(),
                .emissivity          = loadedTextures.at(material->emissivity.Raw()         ).GetDescriptor(),
                .transmission        = loadedTextures.at(material->transmission.Raw()       ).GetDescriptor(),
                .metalness_roughness = loadedTextures.at(material->metalness_roughness.Raw()).GetDescriptor(),

                .alphaCutoff = material->alphaCutoff,
                .alphaMask   = material->alphaMask,
                .alphaBlend  = material->alphaBlend,
                .thin        = material->thin,
                .subsurface  = material->subsurface,
            }}, i);
        }
    }

    void PathTraceRenderer::CompileScene(CompiledScene& _scene, nova::CommandPool cmdPool, nova::Fence fence)
    {
        scene = &_scene;

        // Shaders

        postProcessShader = nova::Shader::Create(context, nova::ShaderStage::Compute, "main",
            nova::glsl::Compile(nova::ShaderStage::Compute, "main", "src/renderers/pathtracer/axiom_PostProcess.glsl", {}));

        anyHitShader = nova::Shader::Create(context, nova::ShaderStage::AnyHit, "main",
            nova::glsl::Compile(nova::ShaderStage::AnyHit, "main", "src/renderers/pathtracer/axiom_AnyHit.glsl", {}));

        closestHitShader = nova::Shader::Create(context, nova::ShaderStage::ClosestHit, "main",
            nova::glsl::Compile(nova::ShaderStage::ClosestHit, "main", "src/renderers/pathtracer/axiom_ClosestHit.glsl", {}));

        rayGenShader = nova::Shader::Create(context, nova::ShaderStage::RayGen, "main",
            nova::glsl::Compile(nova::ShaderStage::RayGen, "main", "src/renderers/pathtracer/axiom_RayGen.glsl", {}));

        constexpr u32 SBT_Opaque      = 0;
        constexpr u32 SBT_AlphaMasked = 1;

        pipeline = nova::RayTracingPipeline::Create(context);
        pipeline.Update(rayGenShader, {}, {
            { closestHitShader               }, // Opaque
            { closestHitShader, anyHitShader }, // Alpha-tested
        }, {});

        // Materials

        CompileMaterials(cmdPool, fence);

        // Geometry

        u64 vertexCount = 0;
        u64 maxPerBlasVertexCount = 0;
        u64 indexCount = 0;
        for (auto& mesh : scene->meshes) {
            maxPerBlasVertexCount = std::max(maxPerBlasVertexCount, mesh->positionAttributes.size());
            vertexCount += mesh->positionAttributes.size();
            indexCount += mesh->indices.size();
        }

#ifdef AXIOM_TRACE_COMPILE // --------------------------------------------------
        NOVA_LOG("Compiling, unique vertices = {}, unique indices = {}", vertexCount, indexCount);
#endif // ----------------------------------------------------------------------

        shadingAttributesBuffer = nova::Buffer::Create(context,
            vertexCount * sizeof(ShadingAttributes),
            nova::BufferUsage::Storage | nova::BufferUsage::AccelBuild,
            nova::BufferFlags::DeviceLocal | nova::BufferFlags::Mapped);

        indexBuffer = nova::Buffer::Create(context,
            indexCount * sizeof(u32),
            nova::BufferUsage::Index | nova::BufferUsage::AccelBuild,
            nova::BufferFlags::DeviceLocal | nova::BufferFlags::Mapped);

        u32 geometryCount = 0;

        u64 vertexOffset = 0;
        u64 indexOffset = 0;
        NOVA_LOGEXPR(scene->meshes.size());
        for (auto& mesh : scene->meshes) {
            meshData[mesh.Raw()] = CompiledMesh{ i32(vertexOffset), u32(indexOffset), geometryCount };

            shadingAttributesBuffer.Set<ShadingAttributes>(mesh->shadingAttributes, vertexOffset);
            vertexOffset += mesh->positionAttributes.size();

            indexBuffer.Set<u32>(mesh->indices, indexOffset);
            indexOffset += mesh->indices.size();

            geometryCount += u32(mesh->subMeshes.size());
        }

        geometryInfoBuffer = nova::Buffer::Create(context,
            geometryCount * sizeof(GPU_GeometryInfo),
            nova::BufferUsage::Storage,
            nova::BufferFlags::DeviceLocal | nova::BufferFlags::Mapped);

        hitGroups = nova::Buffer::Create(context,
            pipeline.GetTableSize(geometryCount),
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
            auto posAttribBuffer = nova::Buffer::Create(context,
                std::max(MinAccelInputBufferSize, maxPerBlasVertexCount * sizeof(Vec3)),
                nova::BufferUsage::Storage | nova::BufferUsage::AccelBuild,
                nova::BufferFlags::DeviceLocal | nova::BufferFlags::Mapped);
            NOVA_DEFER(&) { posAttribBuffer.Destroy(); };

            // Find maximal scratch and build BLAS sizes for all meshes

            u64 scratchSize = 0;
            u64 buildBlasSize = 0;
            for (u32 i = 0; i < scene->meshes.size(); ++i) {
                auto& mesh = scene->meshes[i];
                auto& data = meshData.at(mesh.Raw());

                builder.Prepare(
                    nova::AccelerationStructureType::BottomLevel,
                    nova::AccelerationStructureFlags::AllowDataAccess
                    | nova::AccelerationStructureFlags::AllowCompaction
                    | nova::AccelerationStructureFlags::PreferFastTrace, u32(mesh->subMeshes.size()));

                for (u32 j = 0; j < mesh->subMeshes.size(); ++j) {
                    auto& subMesh = mesh->subMeshes[j];

                    builder.SetTriangles(j,
                        posAttribBuffer.GetAddress() +  subMesh.vertexOffset                  * sizeof(Vec3), nova::Format::RGBA32_SFloat, u32(sizeof(Vec3)), subMesh.maxVertex,
                            indexBuffer.GetAddress() + (subMesh.firstIndex + data.firstIndex) * sizeof(u32),  nova::IndexType::U32,                           subMesh.indexCount / 3);
                }

                scratchSize = std::max(scratchSize, builder.GetBuildScratchSize());
                buildBlasSize = std::max(buildBlasSize, builder.GetBuildSize());
            }

            // Create temporary scratch and build BLAS

            scratch.Resize(scratchSize);
            auto buildBlas = nova::AccelerationStructure::Create(context, buildBlasSize,
                nova::AccelerationStructureType::BottomLevel);
            NOVA_DEFER(&) { buildBlas.Destroy(); };

            for (u32 i = 0; i < scene->meshes.size(); ++i) {
                auto& mesh = scene->meshes[i];
                auto& data = meshData.at(mesh.Raw());

                // Load position data

                posAttribBuffer.Set<Vec3>(mesh->positionAttributes);
                builder.Prepare(
                    nova::AccelerationStructureType::BottomLevel,
                    nova::AccelerationStructureFlags::AllowDataAccess
                    | nova::AccelerationStructureFlags::AllowCompaction
                    | nova::AccelerationStructureFlags::PreferFastTrace, u32(mesh->subMeshes.size()));

                for (u32 j = 0; j < mesh->subMeshes.size(); ++j) {
                    auto& subMesh = mesh->subMeshes[j];
                    auto geometryIndex = data.geometryOffset + j;

                    // Add geometry to build

                    builder.SetTriangles(j,
                        posAttribBuffer.GetAddress() +  subMesh.vertexOffset                  * sizeof(Vec3), nova::Format::RGBA32_SFloat, u32(sizeof(Vec3)), subMesh.maxVertex,
                            indexBuffer.GetAddress() + (subMesh.firstIndex + data.firstIndex) * sizeof(u32),  nova::IndexType::U32,                           subMesh.indexCount / 3);

                    // Store geometry offsets and material

                    geometryInfoBuffer.Set<GPU_GeometryInfo>({{
                        .shadingAttributes = shadingAttributesBuffer.GetAddress() + (data.vertexOffset + subMesh.vertexOffset) * sizeof(ShadingAttributes),
                        .indices =                indexBuffer.GetAddress() + (data.firstIndex   + subMesh.firstIndex  ) * sizeof(u32),
                        .material = materialAddresses.at(subMesh.material.Raw()),
                    }}, geometryIndex);

                    // Bind shaders

                    u32 sbtIndex = SBT_Opaque;
                    if (subMesh.material->alphaMask
                            || subMesh.material->alphaBlend) {
                        sbtIndex = SBT_AlphaMasked;
                    }
                    pipeline.WriteHandle(hitGroups.GetMapped(), geometryIndex, sbtIndex);
                }

                // Build

                auto cmd = cmdPool.Begin();
                cmd.BuildAccelerationStructure(builder, buildBlas, scratch);
                context.GetQueue(nova::QueueFlags::Graphics, 0).Submit({cmd}, {}, {fence});
                fence.Wait();

                // Create final BLAS and compact

                data.blas = nova::AccelerationStructure::Create(context, builder.GetCompactSize(),
                    nova::AccelerationStructureType::BottomLevel);
                cmd = cmdPool.Begin();
                cmd.CompactAccelerationStructure(data.blas, buildBlas);
                context.GetQueue(nova::QueueFlags::Graphics, 0).Submit({cmd}, {}, {fence});
                fence.Wait();
            }
        }

        instanceDataBuffer = nova::Buffer::Create(context,
            scene->instances.size() * sizeof(GPU_InstanceData),
            nova::BufferUsage::Storage,
            nova::BufferFlags::DeviceLocal | nova::BufferFlags::Mapped);

        tlasInstanceBuffer = nova::Buffer::Create(context,
            scene->instances.size() * builder.GetInstanceSize(),
            nova::BufferUsage::AccelBuild,
            nova::BufferFlags::DeviceLocal | nova::BufferFlags::Mapped);

#ifdef AXIOM_TRACE_COMPILE // --------------------------------------------------
        u64 instancedVertexCount = 0;
        u64 instancedIndexCount = 0;
#endif // ----------------------------------------------------------------------

        u32 selectedInstanceCount = 0;
        for (u32 i = 0; i < scene->instances.size(); ++i) {
            auto& instance = scene->instances[i];
            auto& data = meshData.at(instance->mesh.Raw());
            if (!data.blas)
                continue;

            instanceDataBuffer.Set<GPU_InstanceData>({{
                .geometryOffset = data.geometryOffset,
            }}, selectedInstanceCount);

            builder.WriteInstance(
                tlasInstanceBuffer.GetMapped(),
                selectedInstanceCount,
                data.blas,
                instance->transform,
                data.geometryOffset,
                0xFF,
                data.geometryOffset,
                {});
            selectedInstanceCount++;

#ifdef AXIOM_TRACE_COMPILE // --------------------------------------------------
            instancedVertexCount += instance->mesh->positionAttributes.size();
            instancedIndexCount += instance->mesh->indices.size();
#endif // ----------------------------------------------------------------------
        }

#ifdef AXIOM_TRACE_COMPILE // --------------------------------------------------
        NOVA_LOG("Compiling scene:");
        NOVA_LOG("  vertices   = {}", vertexCount);
        NOVA_LOG("  indices    = {}", indexCount);
        NOVA_LOG("  meshes     = {}", scene->meshes.size());
        NOVA_LOG("  geometries = {}", geometryCount);
        NOVA_LOG("  instances  = {}", scene->instances.size());
        NOVA_LOG("  triangles  = {}", instancedIndexCount / 3);
#endif // ----------------------------------------------------------------------

        {
            builder.SetInstances(0, tlasInstanceBuffer.GetAddress(), selectedInstanceCount);
            builder.Prepare(
                nova::AccelerationStructureType::TopLevel,
                nova::AccelerationStructureFlags::AllowDataAccess
                | nova::AccelerationStructureFlags::PreferFastTrace, 1);

            scratch.Resize(builder.GetBuildScratchSize());

            auto cmd = cmdPool.Begin();
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

        if (viewPos != position || viewRot != rotation || viewFov != fov) {
            viewPos = position;
            viewRot = rotation;
            viewFov = fov;

            sampleCount = 0;
        }
    }

    void PathTraceRenderer::Record(nova::CommandList cmd, nova::Texture target)
    {
        auto size = target.GetExtent();

        // Resize backing buffers

        if (!accumulationTarget || accumulationTarget.GetExtent() != size) {
            accumulationTarget.Destroy();

            accumulationTarget = nova::Texture::Create(context, Vec3U(Vec2U(size), 0),
                nova::TextureUsage::Storage,
                nova::Format::RGBA32_SFloat,
                {});

            accumulationTarget.Transition(nova::TextureLayout::GeneralImage);

            sampleCount = 0;
        }

        // Randomness

        {
            u32 noiseLen = (size.x + size.y) * 2;
            noiseBuffer.Resize(noiseLen * sizeof(u32));

            u32* noise = reinterpret_cast<u32*>(noiseBuffer.GetMapped());
            for (u32 i = 0; i < noiseLen; ++i) {
                noise[i] = rng();
            }
        }

        // Trace rays

        struct PC_RayTrace
        {
            u64       tlas;
            u64 geometries;
            u64  instances;
            u64  noiseSeed;

            u32 target;

            Vec3       pos;
            Vec3      camX;
            Vec3      camY;
            f32 camZOffset;

            u32 linearSampler;

            u32 sampleCount;

            Vec2 jitter;

            u32 sampleRadius;
        };

        // Apply pixel jitter for all non-initial samples
        auto jitter = sampleCount == 0
            ? Vec2(0.5f)
            : Vec2(dist(rng), dist(rng));

        cmd.PushConstants(PC_RayTrace {
            .tlas = tlas.GetAddress(),
            .geometries = geometryInfoBuffer.GetAddress(),
            .instances = instanceDataBuffer.GetAddress(),
            .noiseSeed = noiseBuffer.GetAddress(),
            .target = accumulationTarget.GetDescriptor(),
            .pos = viewPos,
            .camX = viewRot * Vec3(1.f, 0.f, 0.f),
            .camY = viewRot * Vec3(0.f, 1.f, 0.f),
            .camZOffset = 1.f / glm::tan(0.5f * viewFov),
            .linearSampler = linearSampler.GetDescriptor(),
            .sampleCount = sampleCount,
            .jitter = jitter,
            .sampleRadius = sampleRadius,
        });

        sampleCount++;

        cmd.TraceRays(pipeline, Vec3U(Vec2U(target.GetExtent()) / Vec2U(sampleRadius), 1), hitGroups.GetAddress(), 1);

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
            .source = accumulationTarget.GetDescriptor(),
            .target = target.GetDescriptor(),
            .exposure = exposure,
            .mode = u32(mode),
        });

        Quat q = Quat(Vec3(0.f));
        Quat q2 = glm::angleAxis(0.f, Vec3(1.f));

        Vec3 v = q * Vec3(1.f);

        cmd.BindShaders({ postProcessShader });
        cmd.Barrier(nova::PipelineStage::RayTracing, nova::PipelineStage::Compute);
        cmd.Dispatch(Vec3U((Vec2U(size) + Vec2U(15)) / Vec2U(16), 1));
    }

    void PathTraceRenderer::ResetSamples()
    {
        sampleCount = 0;
    }
}