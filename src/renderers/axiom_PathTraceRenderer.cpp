#include "axiom_Renderer.hpp"

#include <nova/rhi/vulkan/glsl/nova_VulkanGlsl.hpp>

#include <nova/rhi/vulkan/nova_VulkanRHI.hpp>

namespace axiom
{
    struct CompiledMesh
    {
        i32 vertexOffset;
        u32 firstIndex;
        u32 geometryOffset;
        nova::AccelerationStructure blas;
    };

    // struct GPU_Material
    // {
    //     u32 albedo;
    // };

    struct GPU_InstanceData
    {
        u32 geometryOffset;
    };

    struct GPU_GeometryInfo
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
        geometryInfoBuffer.Destroy();
        instanceDataBuffer.Destroy();
        hitGroups.Destroy();

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
                        .indices        =         indexBuffer.GetAddress() + (data.firstIndex   + subMesh.firstIndex  ) * sizeof(u32),
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

                layout(buffer_reference, scalar, buffer_reference_align = 4) readonly buffer InstanceData {
                    uint geometryOffset;
                };

                layout(buffer_reference, scalar, buffer_reference_align = 8) readonly buffer GeometryInfo {
                    ShadingAttrib shadingAttribs;
                    Index                indices;
                };

                layout(push_constant, scalar) uniform pc_ {
                    uint64_t           tlas;
                    GeometryInfo geometries;
                    InstanceData  instances;
                    uint             target;
                    vec3                pos;
                    vec3               camX;
                    vec3               camY;
                    float        camZOffset;
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
                        1,       // sbtStride
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

                        // Hit Attributes
                        int instanceID = hitObjectGetInstanceIdNV(hit);
                        int customInstanceID = hitObjectGetInstanceCustomIndexNV(hit);
                        int geometryIndex = hitObjectGetGeometryIndexNV(hit);
                        uint sbtIndex = hitObjectGetShaderBindingTableRecordIndexNV(hit);
                        uint hitKind = hitObjectGetHitKindNV(hit);
                        // GeometryInfo geometry = pc.geometries[sbtIndex];
                        // GeometryInfo geometry = pc.geometries[pc.instances[instanceID].geometryOffset + geometryIndex];
                        GeometryInfo geometry = pc.geometries[customInstanceID + geometryIndex];

                        // Transforms
                        mat4x3 objToWorld = hitObjectGetObjectToWorldNV(hit);
                        mat3x3 tgtSpaceToWorld = transpose(inverse(mat3(objToWorld)));

                        // Barycentric weights
                        hitObjectGetAttributesNV(hit, 0);
                        vec3 w = vec3(1.0 - bary.x - bary.y, bary.x, bary.y);

                        // Indices
                        uint primID = hitObjectGetPrimitiveIndexNV(hit);
                        uint i0 = geometry.indices[primID * 3 + 0].value;
                        uint i1 = geometry.indices[primID * 3 + 1].value;
                        uint i2 = geometry.indices[primID * 3 + 2].value;

                        // Shading attributes
                        ShadingAttrib sa0 = geometry.shadingAttribs[i0];
                        ShadingAttrib sa1 = geometry.shadingAttribs[i1];
                        ShadingAttrib sa2 = geometry.shadingAttribs[i2];

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
                        hitObjectExecuteShaderNV(hit, 0);
                        vec3 v0 = rayPayload.position[0];
                        vec3 v1 = rayPayload.position[1];
                        vec3 v2 = rayPayload.position[2];

                        // Positions (transformed)
                        vec3 v0w = objToWorld * vec4(v0, 1);
                        vec3 v1w = objToWorld * vec4(v1, 1);
                        vec3 v2w = objToWorld * vec4(v2, 1);
                        vec3 pos = v0w * w.x + v1w * w.y + v2w * w.z;

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
            u32     target;
            Vec3       pos;
            Vec3      camX;
            Vec3      camY;
            f32 camZOffset;
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
        });

        cmd.TraceRays(pipeline, target.GetExtent(), hitGroups.GetAddress(), 1);
    }
}