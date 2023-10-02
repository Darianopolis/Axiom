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

    struct RasterRenderer : Renderer
    {
        Scene* scene = nullptr;

        nova::Context context;

        nova::Buffer     posAttribBuffer;
        nova::Buffer shadingAttribBuffer;
        nova::Buffer         indexBuffer;
        nova::HashMap<void*, std::pair<i32, u32>> meshOffsets;

        nova::Buffer transformBuffer;

        nova::Buffer indirectBuffer;
        u32           indirectCount;

        nova::Shader   vertexShader;
        nova::Shader fragmentShader;

        nova::Texture depthImage;

        Mat4 viewProj;

        RasterRenderer();
        ~RasterRenderer();

        virtual void CompileScene(Scene& scene, nova::CommandPool cmdPool, nova::Fence fence);

        virtual void SetCamera(Vec3 position, Quat rotation, f32 aspect, f32 fov);
        virtual void Record(nova::CommandList cmd, nova::Texture target, u32 targetIdx);
    };

    nova::Ref<Renderer> CreateRasterRenderer(nova::Context context)
    {
        auto renderer = nova::Ref<RasterRenderer>::Create();
        renderer->context = context;
        return renderer;
    }

    RasterRenderer::RasterRenderer()
    {

    }

    RasterRenderer::~RasterRenderer()
    {
        posAttribBuffer.Destroy();
        shadingAttribBuffer.Destroy();
        indexBuffer.Destroy();
        transformBuffer.Destroy();
        indirectBuffer.Destroy();

        vertexShader.Destroy();
        fragmentShader.Destroy();

        depthImage.Destroy();
    }

    void RasterRenderer::CompileScene(Scene& _scene, nova::CommandPool cmdPool, nova::Fence fence)
    {
        (void)cmdPool;
        (void)fence;

        scene = &_scene;

        u64 vertexCount = 0;
        u64 indexCount = 0;
        for (auto& mesh : scene->meshes) {
            vertexCount += mesh->positionAttribs.size();
            indexCount += mesh->indices.size();
        }

#ifdef AXIOM_TRACE_COMPILE // --------------------------------------------------
        NOVA_LOG("Compiling, unique vertices = {}, unique indices = {}", vertexCount, indexCount);
#endif // ----------------------------------------------------------------------

        posAttribBuffer = nova::Buffer::Create(context,
            vertexCount * sizeof(Vec3),
            nova::BufferUsage::Storage,
            nova::BufferFlags::DeviceLocal | nova::BufferFlags::Mapped);

        shadingAttribBuffer = nova::Buffer::Create(context,
            vertexCount * sizeof(ShadingAttrib),
            nova::BufferUsage::Storage,
            nova::BufferFlags::DeviceLocal | nova::BufferFlags::Mapped);

        indexBuffer = nova::Buffer::Create(context,
            indexCount * sizeof(u32),
            nova::BufferUsage::Index,
            nova::BufferFlags::DeviceLocal | nova::BufferFlags::Mapped);

        u64 vertexOffset = 0;
        u64 indexOffset = 0;
        for (auto& mesh : scene->meshes) {
            meshOffsets[mesh.Raw()] = { i32(vertexOffset), u32(indexOffset) };

            posAttribBuffer.Set<Vec3>(mesh->positionAttribs, vertexOffset);
            shadingAttribBuffer.Set<ShadingAttrib>(mesh->shadingAttribs, vertexOffset);
            vertexOffset += mesh->positionAttribs.size();

            indexBuffer.Set<u32>(mesh->indices, indexOffset);
            indexOffset += mesh->indices.size();
        }

        transformBuffer = nova::Buffer::Create(context, scene->instances.size() * sizeof Mat4,
            nova::BufferUsage::Storage,
            nova::BufferFlags::DeviceLocal | nova::BufferFlags::Mapped);

        indirectBuffer = nova::Buffer::Create(context, scene->instances.size() * sizeof VkDrawIndexedIndirectCommand,
            nova::BufferUsage::Indirect,
            nova::BufferFlags::DeviceLocal | nova::BufferFlags::Mapped);

        transformBuffer.Set<Mat4>({Mat4(1.f)});

        indirectCount = u32(scene->instances.size());
        for (u32 i = 0; i < scene->instances.size(); ++i) {
            auto& instance = scene->instances[i];
            auto& offsets = meshOffsets.at(instance->mesh.Raw());
            indirectBuffer.Set<VkDrawIndexedIndirectCommand>({{
                .indexCount = u32(instance->mesh->indices.size()),
                .instanceCount = 1,
                .firstIndex = offsets.second,
                .vertexOffset = offsets.first,
                .firstInstance = i,
            }}, i);

            transformBuffer.Set<Mat4>({instance->transform}, i);
        }

        vertexShader = nova::Shader::Create(context, nova::ShaderStage::Vertex, "main",
            nova::glsl::Compile( nova::ShaderStage::Vertex, "", {R"glsl(
                #version 460
                #extension GL_EXT_scalar_block_layout  : require
                #extension GL_EXT_buffer_reference2    : require
                #extension GL_EXT_nonuniform_qualifier : require

                layout(buffer_reference, scalar, buffer_reference_align = 4) readonly buffer PosAttrib {
                    vec3 position;
                };

                layout(buffer_reference, scalar, buffer_reference_align = 4) readonly buffer ShadingAttrib {
                    uint tangentSpace_matIndex;
                    uint texCoord;
                };

                layout(buffer_reference, scalar, buffer_reference_align = 4) readonly buffer Instance {
                    mat4 transform;
                };

                layout(push_constant, scalar) readonly uniform pc_ {
                    PosAttrib         posAttribs;
                    ShadingAttrib shadingAttribs;
                    Instance           instances;
                    mat4                viewProj;
                } pc;

                layout(location = 0) out vec3 outPosition;

                void main()
                {
                    PosAttrib p = pc.posAttribs[gl_VertexIndex];
                    Instance instance = pc.instances[gl_InstanceIndex];

                    vec4 worldPos = instance.transform * vec4(p.position, 1);
                    outPosition = vec3(worldPos);
                    gl_Position = pc.viewProj * worldPos;
                }
            )glsl"}));

        fragmentShader = nova::Shader::Create(context, nova::ShaderStage::Fragment, "main",
            nova::glsl::Compile(nova::ShaderStage::Fragment, "", {R"glsl(
                #version 460
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
            )glsl"}));
    }

    void RasterRenderer::SetCamera(Vec3 position, Quat rotation, f32 aspect, f32 fov)
    {
        auto proj = ProjInfReversedZRH(fov, aspect, 0.01f);
        auto mTranslation = glm::translate(glm::mat4(1.f), position);
        auto mRotation = glm::mat4_cast(rotation);
        auto view = glm::affineInverse(mTranslation * mRotation);
        viewProj = proj * view;
    }

    void RasterRenderer::Record(nova::CommandList cmd, nova::Texture target, u32 targetIdx)
    {
        (void)targetIdx;

        if (!depthImage || depthImage.GetExtent() != target.GetExtent()) {
            depthImage.Destroy();

            depthImage = nova::Texture::Create(context, { Vec2U(target.GetExtent()), 0 },
                nova::TextureUsage::DepthStencilAttach,
                nova::Format::D32_SFloat,
                {});
        }

        auto size = target.GetExtent();

        cmd.ResetGraphicsState();
        cmd.SetBlendState({ true, false });
        cmd.SetViewports({{{0, size.y}, Vec2I(size.x, -i32(size.y))}}, true);
        cmd.SetDepthState(true, true, nova::CompareOp::Greater);
        cmd.SetPolygonState(
            nova::Topology::Triangles,
            nova::PolygonMode::Fill,
            nova::CullMode::None,
            nova::FrontFace::CounterClockwise,
            1.f);
        cmd.BindShaders({ vertexShader, fragmentShader });

        struct PushConstants
        {
            u64     posAttribs;
            u64 shadingAttribs;
            u64      instances;
            Mat4      viewProj;
        };

        cmd.BeginRendering({{}, size}, {target}, depthImage);
        cmd.ClearColor(0, Vec4(Vec3(0.2f), 1.f), Vec2(size));
        cmd.ClearDepth(0.f, Vec2(size));
        cmd.BindIndexBuffer(indexBuffer, nova::IndexType::U32);
        cmd.PushConstants(PushConstants {
            .posAttribs = posAttribBuffer.GetAddress(),
            .shadingAttribs = shadingAttribBuffer.GetAddress(),
            .instances = transformBuffer.GetAddress(),
            .viewProj = viewProj,
        });
        cmd.DrawIndexedIndirect(indirectBuffer, 0, indirectCount, sizeof(VkDrawIndexedIndirectCommand));
        cmd.EndRendering();
    }
}