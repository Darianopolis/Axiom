#include "axiom_Renderer.hpp"

#include <nova/rhi/vulkan/glsl/nova_VulkanGlsl.hpp>

namespace axiom
{
    static Mat4 ProjInfReversedZRH(f32 fovY, f32 aspectWbyH, f32 zNear)
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

// -----------------------------------------------------------------------------

    struct CompiledMesh
    {
        i32 vertexOffset;
        u32 firstIndex;
        nova::AccelerationStructure blas;
    };

    struct PathTraceRenderer : Renderer
    {
        Scene* scene = nullptr;

        nova::Context context;

        nova::AccelerationStructure tlas;

        nova::Buffer vertexBuffer;
        nova::Buffer  indexBuffer;
        nova::HashMap<void*, CompiledMesh> meshData;

        nova::Buffer instanceBuffer;

        nova::RayTracingPipeline pipeline;
        nova::Shader         rayGenShader;

        Vec3 viewPos;
        Quat viewRot;
        f32  viewFov;

        PathTraceRenderer();
        ~PathTraceRenderer();

        virtual void CompileScene(Scene& scene, nova::CommandPool cmdPool, nova::Fence fence);

        virtual void SetCamera(Vec3 position, Quat rotation, f32 aspect, f32 fov);
        virtual void Record(nova::CommandList cmd, nova::Texture target, u32 targetIdx);
    };

    nova::Ref<Renderer> CreatePathTraceRenderer(nova::Context context)
    {
        auto renderer = nova::Ref<PathTraceRenderer>::Create();
        renderer->context = context;
        return renderer;
    }

    PathTraceRenderer::PathTraceRenderer()
    {

    }

    PathTraceRenderer::~PathTraceRenderer()
    {
        vertexBuffer.Destroy();
        indexBuffer.Destroy();
        instanceBuffer.Destroy();

        for (auto&[p, data] : meshData) {
            data.blas.Destroy();
        }
        tlas.Destroy();

        rayGenShader.Destroy();
        pipeline.Destroy();
    }

    void PathTraceRenderer::CompileScene(Scene& _scene, nova::CommandPool cmdPool, nova::Fence fence)
    {
        (void)cmdPool;
        (void)fence;

        scene = &_scene;

        u64 vertexCount = 0;
        u64 indexCount = 0;
        for (auto& mesh : scene->meshes) {
            vertexCount += mesh->vertices.size();
            indexCount += mesh->indices.size();
        }

#ifdef AXIOM_TRACE_COMPILE // --------------------------------------------------
        NOVA_LOG("Compiling, unique vertices = {}, unique indices = {}", vertexCount, indexCount);
#endif // ----------------------------------------------------------------------

        vertexBuffer = nova::Buffer::Create(context,
            vertexCount * sizeof(Vertex),
            nova::BufferUsage::Storage | nova::BufferUsage::AccelBuild,
            nova::BufferFlags::DeviceLocal | nova::BufferFlags::Mapped);

        indexBuffer = nova::Buffer::Create(context,
            indexCount * sizeof(u32),
            nova::BufferUsage::Index | nova::BufferUsage::AccelBuild,
            nova::BufferFlags::DeviceLocal | nova::BufferFlags::Mapped);

        u64 vertexOffset = 0;
        u64 indexOffset = 0;
        NOVA_LOGEXPR(scene->meshes.size());
        for (auto& mesh : scene->meshes) {
            meshData[mesh.Raw()] = CompiledMesh{ i32(vertexOffset), u32(indexOffset) };

            vertexBuffer.Set<Vertex>(mesh->vertices, vertexOffset);
            vertexOffset += mesh->vertices.size();

            indexBuffer.Set<u32>(mesh->indices, indexOffset);
            indexOffset += mesh->indices.size();
        }

        auto builder = nova::AccelerationStructureBuilder::Create(context);
        auto scratch = nova::Buffer::Create(context, 0, nova::BufferUsage::Storage, nova::BufferFlags::DeviceLocal);
        NOVA_CLEANUP(&) {
            builder.Destroy();
            scratch.Destroy();
        };

        for (u32 i = 0; i < scene->meshes.size(); ++i) {
            auto& mesh = scene->meshes[i];
            auto& data = meshData.at(mesh.Raw());

            builder.SetTriangles(0,
                vertexBuffer.GetAddress() + data.vertexOffset * sizeof(Vertex), nova::Format::RGB32_SFloat, u32(sizeof(Vertex)), u32(mesh->vertices.size()),
                indexBuffer.GetAddress() + data.firstIndex * sizeof(u32), nova::IndexType::U32, u32(mesh->indices.size()) / 3);

            builder.Prepare(
                nova::AccelerationStructureType::BottomLevel,
                nova::AccelerationStructureFlags::AllowDataAccess
                | nova::AccelerationStructureFlags::PreferFastTrace, 1);

            scratch.Resize(builder.GetBuildScratchSize());

            auto cmd = cmdPool.Begin();
            data.blas = nova::AccelerationStructure::Create(context, builder.GetBuildSize(),
                nova::AccelerationStructureType::BottomLevel);
            cmd.BuildAccelerationStructure(builder, data.blas, scratch);
            context.GetQueue(nova::QueueFlags::Graphics, 0).Submit({cmd}, {}, {fence});
            fence.Wait();
        }

        instanceBuffer = nova::Buffer::Create(context,
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

            builder.WriteInstance(instanceBuffer.GetMapped(), selectedInstanceCount,
                data.blas, instance->transform, selectedInstanceCount, 0xFF, 0, {});
            selectedInstanceCount++;

#ifdef AXIOM_TRACE_COMPILE // --------------------------------------------------
            instancedVertexCount += instance->mesh->vertices.size();
            instancedIndexCount += instance->mesh->indices.size();
#endif // ----------------------------------------------------------------------
        }

#ifdef AXIOM_TRACE_COMPILE // --------------------------------------------------
        NOVA_LOG("Compiling, instanced vertices = {}, instanced triangles = {}", instancedVertexCount, instancedIndexCount / 3);
#endif // ----------------------------------------------------------------------

        {
            builder.SetInstances(0, instanceBuffer.GetAddress(), selectedInstanceCount);
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

        rayGenShader = nova::Shader::Create(context, nova::ShaderStage::RayGen, "main",
            nova::glsl::Compile(nova::ShaderStage::RayGen, "", {R"glsl(
                #version 460
                #extension GL_EXT_scalar_block_layout         : require
                #extension GL_EXT_buffer_reference2           : require
                #extension GL_EXT_nonuniform_qualifier        : require
                #extension GL_EXT_ray_tracing                 : require
                #extension GL_EXT_ray_tracing_position_fetch  : require
                #extension GL_NV_shader_invocation_reorder    : require
                #extension GL_EXT_shader_image_load_formatted : require

                layout(set = 0, binding = 0) uniform image2D RWImage2D[];
                layout(set = 1, binding = 0) uniform accelerationStructureEXT TLAS;

                layout(location = 0) rayPayloadEXT uint     payload;
                layout(location = 0) hitObjectAttributeNV vec3 bary;

                layout(push_constant, scalar) uniform pc_ {
                    uint      target;
                    vec3         pos;
                    vec3        camX;
                    vec3        camY;
                    float camZOffset;
                } pc;

                void main()
                {
                    vec2 pixelCenter = vec2(gl_LaunchIDEXT.xy);
                    pixelCenter += vec2(0.5);
                    vec2 inUV = pixelCenter / vec2(gl_LaunchSizeEXT.xy);
                    vec2 d = inUV * 2.0 - 1.0;
                    vec3 focalPoint = pc.camZOffset * cross(pc.camX, pc.camY);

                    // Perspective
                    d.x *= float(gl_LaunchSizeEXT.x) / float(gl_LaunchSizeEXT.y);
                    d.y *= -1.0;
                    vec3 dir = normalize((pc.camY * d.y) + (pc.camX * d.x) - focalPoint);

                    hitObjectNV hit;
                    hitObjectTraceRayNV(hit, TLAS, 0, 0xFF, 0, 0, 0, pc.pos, 0, dir, 8000000, 0);

                    vec3 color = vec3(0.1);
                    if (hitObjectIsHitNV(hit)) {
                        hitObjectGetAttributesNV(hit, 0);
                        color = vec3(1.0 - bary.x - bary.y, bary.x, bary.y);
                    }
                    imageStore(RWImage2D[pc.target], ivec2(gl_LaunchIDEXT.xy), vec4(color, 1));
                }
            )glsl"}));

        pipeline = nova::RayTracingPipeline::Create(context);
        pipeline.Update({ rayGenShader }, {}, {}, {});
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
            u32     target;
            Vec3       pos;
            Vec3      camX;
            Vec3      camY;
            f32 camZOffset;
        };

        cmd.BindAccelerationStructure(nova::BindPoint::RayTracing, tlas);

        cmd.PushConstants(PushConstants {
            .target = targetIdx,
            .pos = viewPos,
            .camX = viewRot * Vec3(1.f, 0.f, 0.f),
            .camY = viewRot * Vec3(0.f, 1.f, 0.f),
            .camZOffset = 1.f / glm::tan(0.5f * viewFov),
        });

        cmd.TraceRays(pipeline, target.GetExtent(), 0);
    }
}