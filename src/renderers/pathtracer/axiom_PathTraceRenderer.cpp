#include "axiom_Renderer.hpp"

#include <nova/rhi/vulkan/glsl/nova_VulkanGlsl.hpp>
#include <nova/core/nova_SubAllocation.hpp>

#include <nova/rhi/vulkan/nova_VulkanRHI.hpp>

namespace axiom
{
    struct CompiledMesh
    {
        i32                 vertexOffset;
        u32                   firstIndex;
        u32               geometryOffset;
        nova::AccelerationStructure blas;
    };

    struct LoadedTexture
    {
        nova::Texture         texture;
        nova::DescriptorHandle handle;
    };

    struct GPU_Material
    {
        nova::DescriptorHandle baseColor_alpha;
    };

    struct GPU_InstanceData
    {
        u32 geometryOffset;
    };

    struct GPU_GeometryInfo
    {
        u64 shadingAttribs;
        u64        indices;
        u64       material;
    };

    struct PathTraceRenderer : Renderer
    {
        Scene* scene = nullptr;

        nova::Context          context;
        nova::DescriptorHeap      heap;
        nova::IndexFreeList* heapSlots;

        nova::AccelerationStructure tlas;

        nova::Sampler             linearSampler;
        nova::DescriptorHandle linearSamplerIdx;

        nova::Buffer                        materialBuffer;
        nova::HashMap<void*, u64>        materialAddresses;
        nova::HashMap<void*, LoadedTexture> loadedTextures;

        nova::Buffer            shadingAttribBuffer;
        nova::Buffer                    indexBuffer;
        nova::Buffer             geometryInfoBuffer;
        nova::Buffer             instanceDataBuffer;
        nova::HashMap<void*, CompiledMesh> meshData;

        nova::Buffer tlasInstanceBuffer;

        nova::RayTracingPipeline pipeline;
        nova::Buffer            hitGroups;
        nova::Shader     closestHitShader;
        nova::Shader         rayGenShader;

        Vec3 viewPos;
        Quat viewRot;
        f32  viewFov;

        PathTraceRenderer();
        ~PathTraceRenderer();

        void CompileMaterials(nova::CommandPool cmdPool, nova::Fence fence);

        virtual void CompileScene(Scene& scene, nova::CommandPool cmdPool, nova::Fence fence);

        virtual void SetCamera(Vec3 position, Quat rotation, f32 aspect, f32 fov);
        virtual void Record(nova::CommandList cmd, nova::Texture target, u32 targetIdx);
    };

    nova::Ref<Renderer> CreatePathTraceRenderer(nova::Context context, nova::DescriptorHeap heap, nova::IndexFreeList* heapSlots)
    {
        auto renderer = nova::Ref<PathTraceRenderer>::Create();
        renderer->context = context;
        renderer->heap = heap;
        renderer->heapSlots = heapSlots;

        renderer->linearSampler = nova::Sampler::Create(context, nova::Filter::Linear, nova::AddressMode::Repeat, nova::BorderColor::TransparentBlack, 16.f);
        renderer->linearSamplerIdx = heapSlots->Acquire();

        return renderer;
    }

    PathTraceRenderer::PathTraceRenderer()
    {

    }

    PathTraceRenderer::~PathTraceRenderer()
    {
        shadingAttribBuffer.Destroy();
        indexBuffer.Destroy();
        tlasInstanceBuffer.Destroy();
        geometryInfoBuffer.Destroy();
        instanceDataBuffer.Destroy();
        hitGroups.Destroy();

        for (auto&[p, data] : meshData) {
            data.blas.Destroy();
        }
        tlas.Destroy();

        materialBuffer.Destroy();
        for (auto&[p, texture] : loadedTextures) {
            texture.texture.Destroy();
            heapSlots->Release(texture.handle.id);
        }

        closestHitShader.Destroy();
        rayGenShader.Destroy();
        pipeline.Destroy();
    }

    void PathTraceRenderer::CompileMaterials(nova::CommandPool cmdPool, nova::Fence fence)
    {
        (void)cmdPool, (void)fence;

        for (auto& texture : scene->textures) {
            auto& loadedTexture = loadedTextures[texture.Raw()];
            loadedTexture.texture = nova::Texture::Create(context,
                Vec3U(texture->size, 0),
                nova::TextureUsage::Sampled,
                nova::Format::RGBA8_UNorm,
                {});

            loadedTexture.texture.Set({}, loadedTexture.texture.GetExtent(),
                texture->data.data());

            loadedTexture.handle = heapSlots->Acquire();
            heap.WriteSampledTexture(loadedTexture.handle, loadedTexture.texture);

            NOVA_LOG("Compiled texture, obj = {}, idx = {}",
                std::bit_cast<void*>(loadedTexture.texture),
                loadedTexture.handle.ToShaderUInt());
        }

        materialBuffer = nova::Buffer::Create(context,
            scene->materials.size() * sizeof(GPU_Material),
            nova::BufferUsage::Storage,
            nova::BufferFlags::DeviceLocal | nova::BufferFlags::Mapped);

        for (u32 i = 0; i < scene->materials.size(); ++i) {
            auto& material = scene->materials[i];

            u64 address = materialBuffer.GetAddress() + (i * sizeof(GPU_Material));
            materialAddresses[material.Raw()] = address;

            materialBuffer.Set<GPU_Material>({{
                .baseColor_alpha = loadedTextures.at(material->baseColor_alpha.Raw()).handle,
            }}, i);

            NOVA_LOG("Compiled material, address = {}", address);
        }
    }

    void PathTraceRenderer::CompileScene(Scene& _scene, nova::CommandPool cmdPool, nova::Fence fence)
    {
        scene = &_scene;

        CompileMaterials(cmdPool, fence);

        u64 vertexCount = 0;
        u64 maxPerBlasVertexCount = 0;
        u64 indexCount = 0;
        for (auto& mesh : scene->meshes) {
            maxPerBlasVertexCount = std::max(maxPerBlasVertexCount, mesh->positionAttribs.size());
            vertexCount += mesh->positionAttribs.size();
            indexCount += mesh->indices.size();
        }

#ifdef AXIOM_TRACE_COMPILE // --------------------------------------------------
        NOVA_LOG("Compiling, unique vertices = {}, unique indices = {}", vertexCount, indexCount);
#endif // ----------------------------------------------------------------------

        shadingAttribBuffer = nova::Buffer::Create(context,
            vertexCount * sizeof(ShadingAttrib),
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

            shadingAttribBuffer.Set<ShadingAttrib>(mesh->shadingAttribs, vertexOffset);
            vertexOffset += mesh->positionAttribs.size();

            indexBuffer.Set<u32>(mesh->indices, indexOffset);
            indexOffset += mesh->indices.size();

            geometryCount += u32(mesh->subMeshes.size());
        }

        geometryInfoBuffer = nova::Buffer::Create(context,
            geometryCount * sizeof(GPU_GeometryInfo),
            nova::BufferUsage::Storage,
            nova::BufferFlags::DeviceLocal | nova::BufferFlags::Mapped);

        auto builder = nova::AccelerationStructureBuilder::Create(context);
        auto scratch = nova::Buffer::Create(context, 0, nova::BufferUsage::Storage, nova::BufferFlags::DeviceLocal);
        NOVA_CLEANUP(&) {
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
            NOVA_CLEANUP(&) { posAttribBuffer.Destroy(); };

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
            NOVA_CLEANUP(&) { buildBlas.Destroy(); };

            for (u32 i = 0; i < scene->meshes.size(); ++i) {
                auto& mesh = scene->meshes[i];
                auto& data = meshData.at(mesh.Raw());

                // Load position data

                posAttribBuffer.Set<Vec3>(mesh->positionAttribs);
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

                    geometryInfoBuffer.Set<GPU_GeometryInfo>({{
                        .shadingAttribs = shadingAttribBuffer.GetAddress() + (data.vertexOffset + subMesh.vertexOffset) * sizeof(ShadingAttrib),
                        .indices =                indexBuffer.GetAddress() + (data.firstIndex   + subMesh.firstIndex  ) * sizeof(u32),
                        .material = materialAddresses.at(subMesh.material.Raw()),
                    }}, data.geometryOffset + j);
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
            scene->instances.size() * sizeof(VkAccelerationStructureInstanceKHR),
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
            instancedVertexCount += instance->mesh->positionAttribs.size();
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

        closestHitShader = nova::Shader::Create(context, nova::ShaderStage::ClosestHit, "main",
            nova::glsl::Compile(nova::ShaderStage::ClosestHit, "src/renderers/pathtracer/axiom_ClosestHit.glsl", {}));

        rayGenShader = nova::Shader::Create(context, nova::ShaderStage::RayGen, "main",
            nova::glsl::Compile(nova::ShaderStage::RayGen, "src/renderers/pathtracer/axiom_RayGen.glsl", {}));

        pipeline = nova::RayTracingPipeline::Create(context);
        pipeline.Update(rayGenShader, {}, { { closestHitShader } }, {});

        hitGroups = nova::Buffer::Create(context,
            pipeline.GetTableSize(geometryCount),
            nova::BufferUsage::ShaderBindingTable,
            nova::BufferFlags::DeviceLocal | nova::BufferFlags::Mapped);

        for (u32 i = 0; i < geometryCount; ++i) {
            pipeline.WriteHandle(hitGroups.GetMapped(), i, 0);
        }
    }

    void PathTraceRenderer::SetCamera(Vec3 position, Quat rotation, f32 aspect, f32 fov)
    {
        (void)aspect;

        viewPos = position;
        viewRot = rotation;
        viewFov = fov;
    }

    void PathTraceRenderer::Record(nova::CommandList cmd, nova::Texture target, u32 targetIdx)
    {
        auto size = target.GetExtent();

        struct PushConstants
        {
            u64       tlas;
            u64 geometries;
            u64  instances;

            u32 target;

            Vec3       pos;
            Vec3      camX;
            Vec3      camY;
            f32 camZOffset;

            nova::DescriptorHandle linearSampler;
        };

        cmd.PushConstants(PushConstants {
            .tlas = tlas.GetAddress(),
            .geometries = geometryInfoBuffer.GetAddress(),
            .instances = instanceDataBuffer.GetAddress(),
            .target = targetIdx,
            .pos = viewPos,
            .camX = viewRot * Vec3(1.f, 0.f, 0.f),
            .camY = viewRot * Vec3(0.f, 1.f, 0.f),
            .camZOffset = 1.f / glm::tan(0.5f * viewFov),
            .linearSampler = linearSamplerIdx,
        });

        cmd.TraceRays(pipeline, target.GetExtent(), hitGroups.GetAddress(), 1);
    }
}