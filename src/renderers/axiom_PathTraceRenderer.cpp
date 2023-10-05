#include "axiom_Renderer.hpp"

#include <nova/rhi/vulkan/glsl/nova_VulkanGlsl.hpp>

namespace axiom
{
    struct CompiledMesh
    {
        i32 vertexOffset;
        u32 firstIndex;
        nova::AccelerationStructure blas;
    };

    struct GPU_MeshInstance
    {
        u64 shadingAttribs;
        u64        indices;
    };

    struct PathTraceRenderer : Renderer
    {
        Scene* scene = nullptr;

        nova::Context context;

        nova::AccelerationStructure tlas;

        nova::Buffer            shadingAttribBuffer;
        nova::Buffer                    indexBuffer;
        nova::Buffer             meshInstanceBuffer;
        nova::HashMap<void*, CompiledMesh> meshData;

        nova::Buffer tlasInstanceBuffer;

        nova::RayTracingPipeline pipeline;
        nova::Shader     closestHitShader;
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
        shadingAttribBuffer.Destroy();
        indexBuffer.Destroy();
        tlasInstanceBuffer.Destroy();
        meshInstanceBuffer.Destroy();

        for (auto&[p, data] : meshData) {
            data.blas.Destroy();
        }
        tlas.Destroy();

        closestHitShader.Destroy();
        rayGenShader.Destroy();
        pipeline.Destroy();
    }

    void PathTraceRenderer::CompileScene(Scene& _scene, nova::CommandPool cmdPool, nova::Fence fence)
    {
        scene = &_scene;

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

        u64 vertexOffset = 0;
        u64 indexOffset = 0;
        NOVA_LOGEXPR(scene->meshes.size());
        for (auto& mesh : scene->meshes) {
            meshData[mesh.Raw()] = CompiledMesh{ i32(vertexOffset), u32(indexOffset) };

            shadingAttribBuffer.Set<ShadingAttrib>(mesh->shadingAttribs, vertexOffset);
            vertexOffset += mesh->positionAttribs.size();

            indexBuffer.Set<u32>(mesh->indices, indexOffset);
            indexOffset += mesh->indices.size();
        }

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

            builder.Prepare(
                nova::AccelerationStructureType::BottomLevel,
                nova::AccelerationStructureFlags::AllowDataAccess
                | nova::AccelerationStructureFlags::AllowCompaction
                | nova::AccelerationStructureFlags::PreferFastTrace, 1);

            // Find maximal scratch and build BLAS sizes for all meshes

            u64 scratchSize = 0;
            u64 buildBlasSize = 0;
            for (u32 i = 0; i < scene->meshes.size(); ++i) {
                auto& mesh = scene->meshes[i];
                auto& data = meshData.at(mesh.Raw());

                builder.SetTriangles(0,
                    posAttribBuffer.GetAddress(), nova::Format::RGB32_SFloat, u32(sizeof(Vec3)), u32(mesh->positionAttribs.size()),
                    indexBuffer.GetAddress() + data.firstIndex * sizeof(u32), nova::IndexType::U32, u32(mesh->indices.size()) / 3);

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
                builder.SetTriangles(0,
                    posAttribBuffer.GetAddress(), nova::Format::RGB32_SFloat, u32(sizeof(Vec3)), u32(mesh->positionAttribs.size()),
                    indexBuffer.GetAddress() + data.firstIndex * sizeof(u32), nova::IndexType::U32, u32(mesh->indices.size()) / 3);

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

        meshInstanceBuffer = nova::Buffer::Create(context,
            scene->instances.size() * sizeof(GPU_MeshInstance),
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

            meshInstanceBuffer.Set<GPU_MeshInstance>({{
                .shadingAttribs = shadingAttribBuffer.GetAddress()
                    + data.vertexOffset * sizeof(ShadingAttrib),
                .indices = indexBuffer.GetAddress()
                    + data.firstIndex * sizeof(u32),
            }}, i);

            builder.WriteInstance(tlasInstanceBuffer.GetMapped(), selectedInstanceCount,
                data.blas, instance->transform, selectedInstanceCount, 0xFF, 0, {});
            selectedInstanceCount++;

#ifdef AXIOM_TRACE_COMPILE // --------------------------------------------------
            instancedVertexCount += instance->mesh->positionAttribs.size();
            instancedIndexCount += instance->mesh->indices.size();
#endif // ----------------------------------------------------------------------
        }

#ifdef AXIOM_TRACE_COMPILE // --------------------------------------------------
        NOVA_LOG("Compiling, instanced vertices = {}, instanced triangles = {}", instancedVertexCount, instancedIndexCount / 3);
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
            nova::glsl::Compile(nova::ShaderStage::ClosestHit, "", {R"glsl(
                #version 460
                #extension GL_EXT_ray_tracing                : require
                #extension GL_EXT_ray_tracing_position_fetch : require

                struct RayPayload {
                    vec3 position[3];
                };
                layout(location = 0) rayPayloadInEXT RayPayload rayPayload;

                void main()
                {
                    rayPayload.position[0] = gl_HitTriangleVertexPositionsEXT[0];
                    rayPayload.position[1] = gl_HitTriangleVertexPositionsEXT[1];
                    rayPayload.position[2] = gl_HitTriangleVertexPositionsEXT[2];
                }
            )glsl"}));

        rayGenShader = nova::Shader::Create(context, nova::ShaderStage::RayGen, "main",
            nova::glsl::Compile(nova::ShaderStage::RayGen, "", {R"glsl(
                #version 460
                #extension GL_EXT_scalar_block_layout                    : require
                #extension GL_EXT_buffer_reference2                      : require
                #extension GL_EXT_nonuniform_qualifier                   : require
                #extension GL_EXT_ray_tracing                            : require
                #extension GL_EXT_ray_tracing_position_fetch             : require
                #extension GL_NV_shader_invocation_reorder               : require
                #extension GL_EXT_shader_image_load_formatted            : require
                #extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

                layout(set = 0, binding = 0) uniform image2D RWImage2D[];

                struct RayPayload {
                    vec3 position[3];
                };
                layout(location = 0) rayPayloadEXT RayPayload rayPayload;

                layout(location = 0) hitObjectAttributeNV vec3 bary;

                layout(buffer_reference, scalar, buffer_reference_align = 4) readonly buffer ShadingAttrib {
                    uint  tgtSpace;
                    uint texCoords;
                };

                layout(buffer_reference, scalar, buffer_reference_align = 4) readonly buffer Index {
                    uint value;
                };

                layout(buffer_reference, scalar, buffer_reference_align = 8) readonly buffer MeshInstance {
                    ShadingAttrib shadingAttribs;
                    Index                indices;
                };

                layout(push_constant, scalar) uniform pc_ {
                    uint64_t          tlas;
                    MeshInstance instances;
                    uint            target;
                    vec3               pos;
                    vec3              camX;
                    vec3              camY;
                    float       camZOffset;
                } pc;

                float PI = 3.14159265358979323846264338327950288419716939937510;

                vec3 SignedOctDecode_(vec3 n)
                {
                    vec3 OutN;

                    OutN.x = (n.x - n.y);
                    OutN.y = (n.x + n.y) - 1.0;
                    OutN.z = n.z * 2.0 - 1.0;
                    OutN.z = OutN.z * (1.0 - abs(OutN.x) - abs(OutN.y));

                    OutN = normalize(OutN);
                    return OutN;
                }

                vec3 SignedOctDecode(uint tgtSpace)
                {
                    float x = float(bitfieldExtract(tgtSpace, 0, 10)) / 1023.0;
                    float y = float(bitfieldExtract(tgtSpace, 10, 10)) / 1023.0;
                    float s = float(bitfieldExtract(tgtSpace, 20, 1));

                    return SignedOctDecode_(vec3(x, y, s));
                }

                vec2 DecodeDiamond(float p)
                {
                    vec2 v;

                    // Remap p to the appropriate segment on the diamond
                    float p_sign = sign(p - 0.5);
                    v.x = -p_sign * 4.0 * p + 1.0 + p_sign * 2.0;
                    v.y = p_sign * (1.0 - abs(v.x));

                    // Normalization extends the point on the diamond back to the unit circle
                    return normalize(v);
                }

                vec3 DecodeTangent_(vec3 normal, float diamondTangent)
                {
                    // As in the encode step, find our canonical tangent basis span(t1, t2)
                    vec3 t1;
                    if (abs(normal.y) > abs(normal.z)) {
                        t1 = vec3(normal.y, -normal.x, 0.f);
                    } else {
                        t1 = vec3(normal.z, 0.f, -normal.x);
                    }
                    t1 = normalize(t1);

                    vec3 t2 = cross(t1, normal);

                    // Recover the coordinates used with t1 and t2
                    vec2 packedTangent = DecodeDiamond(diamondTangent);

                    return packedTangent.x * t1 + packedTangent.y * t2;
                }

                vec3 DecodeTangent(vec3 normal, uint tgtSpace)
                {
                    float tgtAngle = float(bitfieldExtract(tgtSpace, 21, 10)) / 1023.0;
                    return DecodeTangent_(normal, tgtAngle);
                }

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

                    // // Equirectangular
                    // vec2 uv = d;
                    // float yaw = uv.x * PI;
                    // float pitch = uv.y * PI * 0.5f;
                    // float x = sin(yaw) * cos(pitch);
                    // float y = -sin(pitch);
                    // float z = -cos(yaw) * cos(pitch);
                    // mat3 tbn = mat3(pc.camX, pc.camY, focalPoint);
                    // vec3 dir = tbn * vec3(x, y, z);

                    // // Fisheye
                    // float fov = radians(180);
                    // float aspect = float(gl_LaunchSizeEXT.x) / float(gl_LaunchSizeEXT.y);
                    // vec2 uv = d;
                    // vec2 xy = uv * vec2(1, -aspect);
                    // float r = sqrt(dot(xy, xy));
                    // vec2 cs = vec2 (cos(r * fov), sin(r * fov));
                    // mat3 tbn = mat3(pc.camX, pc.camY, -focalPoint);
                    // vec3 dir = tbn * vec3 (cs.y * xy / r, cs.x);

                    hitObjectNV hit;
                    hitObjectTraceRayNV(hit,
                        accelerationStructureEXT(pc.tlas),
                        0,       // Flags
                        0xFF,    // Hit Mask
                        0,       // sbtOffset
                        0,       // sbtStride
                        0,       // missOffset
                        pc.pos,  // rayOrigin
                        0.0,     // tMin
                        dir,     // rayDir
                        8000000, // tMax
                        0);      // payload

                    // TODO: Only reorder on bounces
                    reorderThreadNV(0, 0);

                    vec3 color = vec3(0.1);
                    if (hitObjectIsHitNV(hit)) {
                        int instanceID = hitObjectGetInstanceCustomIndexNV(hit);
                        int primitiveID = hitObjectGetInstanceCustomIndexNV(hit);
                        int geometryIndex = hitObjectGetGeometryIndexNV(hit);
                        uint hitKind = hitObjectGetHitKindNV(hit);

                        mat4x3 objToWorld = hitObjectGetObjectToWorldNV(hit);
                        mat3x3 tgtSpaceToWorld = transpose(inverse(mat3(objToWorld)));

                        hitObjectGetAttributesNV(hit, 0);
                        hitObjectExecuteShaderNV(hit, 0);

                        // Barycentric weights
                        vec3 w = vec3(1.0 - bary.x - bary.y, bary.x, bary.y);

                        // Instance
                        MeshInstance instance = pc.instances[instanceID];

                        // Indices
                        uint primID = hitObjectGetPrimitiveIndexNV(hit);
                        uint i0 = instance.indices[primID * 3 + 0].value;
                        uint i1 = instance.indices[primID * 3 + 1].value;
                        uint i2 = instance.indices[primID * 3 + 2].value;

                        // Shading attributes
                        ShadingAttrib sa0 = instance.shadingAttribs[i0];
                        ShadingAttrib sa1 = instance.shadingAttribs[i1];
                        ShadingAttrib sa2 = instance.shadingAttribs[i2];

                        // Normals
                        vec3 nrm0 = SignedOctDecode(sa0.tgtSpace);
                        vec3 nrm1 = SignedOctDecode(sa1.tgtSpace);
                        vec3 nrm2 = SignedOctDecode(sa2.tgtSpace);
                        vec3 nrm = nrm0 * w.x + nrm1 * w.y + nrm2 * w.z;
                        nrm = normalize(tgtSpaceToWorld * nrm);

                        // Tangents
                        vec3 tgt0 = DecodeTangent(nrm0, sa0.tgtSpace);
                        vec3 tgt1 = DecodeTangent(nrm1, sa1.tgtSpace);
                        vec3 tgt2 = DecodeTangent(nrm2, sa2.tgtSpace);
                        vec3 tgt = tgt0 * w.x + tgt1 * w.y + tgt2 * w.z;
                        tgt = normalize(tgtSpaceToWorld * tgt);

                        // Tex Coords
                        vec2 uv0 = unpackHalf2x16(sa0.texCoords);
                        vec2 uv1 = unpackHalf2x16(sa1.texCoords);
                        vec2 uv2 = unpackHalf2x16(sa2.texCoords);
                        vec2 uv = uv0 * w.x + uv1 * w.y + uv2 * w.z;

                        // Positions (local)
                        vec3 v0 = rayPayload.position[0];
                        vec3 v1 = rayPayload.position[1];
                        vec3 v2 = rayPayload.position[2];

                        // Positions (transformed)
                        vec3 v0w = objToWorld * vec4(v0, 1);
                        vec3 v1w = objToWorld * vec4(v1, 1);
                        vec3 v2w = objToWorld * vec4(v2, 1);

                        // Flat normal
                        vec3 v01 = v1w - v0w;
                        vec3 v02 = v2w - v0w;
                        vec3 flatNrm = normalize(cross(v01, v02));

                        // Side corrected normals
                        if (hitKind != gl_HitKindFrontFacingTriangleEXT) {
                            nrm = -nrm;
                            flatNrm = -flatNrm;
                        }

// -----------------------------------------------------------------------------
// #define DEBUG_UV
// #define DEBUG_FLAT_NRM
#define DEBUG_NRM
// #define DEBUG_TGT
// #define DEBUG_BARY
// -----------------------------------------------------------------------------

#if   defined(DEBUG_UV)
                        color = vec3(uv, 0);
#elif defined(DEBUG_FLAT_NRM)
                        color = (flatNrm * 0.5 + 0.5) * 0.75;
#elif defined(DEBUG_NRM)
                        color = (nrm * 0.5 + 0.5) * 0.75;
#elif defined(DEBUG_TGT)
                        color = (tgt * 0.5 + 0.5) * 0.75;
#elif defined(DEBUG_BARY)
                        color = vec3(1.0 - bary.x - bary.y, bary.x, bary.y);
#else
                        color = vec3(1, 0, 0);
#endif




                    }
                    imageStore(RWImage2D[pc.target], ivec2(gl_LaunchIDEXT.xy), vec4(color, 1));
                }
            )glsl"}));

        pipeline = nova::RayTracingPipeline::Create(context);
        pipeline.Update({ rayGenShader }, {}, { { closestHitShader } }, {});
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
            u64  instances;
            u32     target;
            Vec3       pos;
            Vec3      camX;
            Vec3      camY;
            f32 camZOffset;
        };

        cmd.PushConstants(PushConstants {
            .tlas = tlas.GetAddress(),
            .instances = meshInstanceBuffer.GetAddress(),
            .target = targetIdx,
            .pos = viewPos,
            .camX = viewRot * Vec3(1.f, 0.f, 0.f),
            .camY = viewRot * Vec3(0.f, 1.f, 0.f),
            .camZOffset = 1.f / glm::tan(0.5f * viewFov),
        });

        cmd.TraceRays(pipeline, target.GetExtent(), 0);
    }
}