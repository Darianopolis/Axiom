#include "axiom_Renderer.hpp"

#ifndef VK_NO_PROTOTYPES
#  define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

namespace axiom
{
    static Mat4 ProjInfReversedZRH(f32 fov_y, f32 aspect_wbh, f32 z_near)
    {
        // https://nlguillemot.wordpress.com/2016/12/07/reversed-z-in-opengl/

        f32 f = 1.f / glm::tan(fov_y / 2.f);
        Mat4 proj{};
        proj[0][0] = f / aspect_wbh;
        proj[1][1] = f;
        proj[3][2] = z_near; // Right, middle-bottom
        proj[2][3] = -1.f;  // Bottom, middle-right

        /*

        post-multiply

        f/a  0  0  0
          0  f  0  0
          0  0  0 zn
          0  0 -1  0

        pre-multiply

        f/a  0  0  0
          0  f  0  0
          0  0  0 -1
          0  0 zn  0

        */

        return proj;
    }

// -----------------------------------------------------------------------------

    struct RasterRenderer : Renderer
    {
        CompiledScene* scene = nullptr;

        nova::Context context;

        nova::Buffer position_attribute_buffer;
        nova::Buffer  shading_attribute_buffer;
        nova::Buffer              index_buffer;

        nova::HashMap<void*, std::pair<i32, u32>> mesh_offsets;

        nova::Buffer transform_buffer;

        nova::Buffer indirect_buffer;
        u32           indirect_count;

        nova::Shader   vertex_shader;
        nova::Shader fragment_shader;

        nova::Image depth_image;

        Mat4 view_proj;

        RasterRenderer();
        ~RasterRenderer();

        virtual void CompileScene(CompiledScene& scene, nova::CommandPool cmd_pool, nova::Fence fence);

        virtual void SetCamera(Vec3 position, Quat rotation, f32 aspect, f32 fov);
        virtual void Record(nova::CommandList cmd, nova::Image target);

        virtual void ResetSamples() {}
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
        position_attribute_buffer.Destroy();
        shading_attribute_buffer.Destroy();
        index_buffer.Destroy();
        transform_buffer.Destroy();
        indirect_buffer.Destroy();

        vertex_shader.Destroy();
        fragment_shader.Destroy();

        depth_image.Destroy();
    }

    void RasterRenderer::CompileScene(CompiledScene& _scene, nova::CommandPool cmd_pool, nova::Fence fence)
    {
        (void)cmd_pool;
        (void)fence;

        scene = &_scene;

        u64 vertex_count = 0;
        u64 index_count = 0;
        for (auto& mesh : scene->meshes) {
            vertex_count += mesh->position_attributes.size();
            index_count += mesh->indices.size();
        }

#ifdef AXIOM_TRACE_COMPILE // --------------------------------------------------
        NOVA_LOG("Compiling, unique vertices = {}, unique indices = {}", vertex_count, index_count);
#endif // ----------------------------------------------------------------------

        position_attribute_buffer = nova::Buffer::Create(context,
            vertex_count * sizeof(Vec3),
            nova::BufferUsage::Storage,
            nova::BufferFlags::DeviceLocal | nova::BufferFlags::Mapped);

        shading_attribute_buffer = nova::Buffer::Create(context,
            vertex_count * sizeof(ShadingAttributes),
            nova::BufferUsage::Storage,
            nova::BufferFlags::DeviceLocal | nova::BufferFlags::Mapped);

        index_buffer = nova::Buffer::Create(context,
            index_count * sizeof(u32),
            nova::BufferUsage::Index,
            nova::BufferFlags::DeviceLocal | nova::BufferFlags::Mapped);

        u64 vertex_offset = 0;
        u64 index_offset = 0;
        for (auto& mesh : scene->meshes) {
            mesh_offsets[mesh.Raw()] = { i32(vertex_offset), u32(index_offset) };

            position_attribute_buffer.Set<Vec3>(mesh->position_attributes, vertex_offset);
            shading_attribute_buffer.Set<ShadingAttributes>(mesh->shading_attributes, vertex_offset);
            vertex_offset += mesh->position_attributes.size();

            index_buffer.Set<u32>(mesh->indices, index_offset);
            index_offset += mesh->indices.size();
        }

        transform_buffer = nova::Buffer::Create(context, scene->instances.size() * sizeof Mat4,
            nova::BufferUsage::Storage,
            nova::BufferFlags::DeviceLocal | nova::BufferFlags::Mapped);

        indirect_buffer = nova::Buffer::Create(context, scene->instances.size() * sizeof VkDrawIndexedIndirectCommand,
            nova::BufferUsage::Indirect,
            nova::BufferFlags::DeviceLocal | nova::BufferFlags::Mapped);

        transform_buffer.Set<Mat4>({Mat4(1.f)});

        indirect_count = u32(scene->instances.size());
        for (u32 i = 0; i < scene->instances.size(); ++i) {
            auto& instance = scene->instances[i];
            auto& offsets = mesh_offsets.at(instance->mesh.Raw());
            indirect_buffer.Set<VkDrawIndexedIndirectCommand>({{
                .indexCount = u32(instance->mesh->indices.size()),
                .instanceCount = 1,
                .firstIndex = offsets.second,
                .vertexOffset = offsets.first,
                .firstInstance = i,
            }}, i);

            transform_buffer.Set<Mat4>({instance->transform}, i);
        }

        vertex_shader = nova::Shader::Create(context, nova::ShaderLang::Glsl,
            nova::ShaderStage::Vertex, "main", "src/renderers/rasterizer/axiom_Vertex.glsl", {});

        fragment_shader = nova::Shader::Create(context, nova::ShaderLang::Glsl,
            nova::ShaderStage::Fragment, "main", "src/renderers/rasterizer/axiom_Fragment.glsl", {});
    }

    void RasterRenderer::SetCamera(Vec3 position, Quat rotation, f32 aspect, f32 fov)
    {
        auto proj = ProjInfReversedZRH(fov, aspect, 0.01f);
        auto pos_tform = glm::translate(glm::mat4(1.f), position);
        auto rot_tform = glm::mat4_cast(rotation);
        auto view = glm::affineInverse(pos_tform * rot_tform);
        view_proj = proj * view;
    }

    void RasterRenderer::Record(nova::CommandList cmd, nova::Image target)
    {
        if (!depth_image || depth_image.GetExtent() != target.GetExtent()) {
            depth_image.Destroy();

            depth_image = nova::Image::Create(context, { Vec2U(target.GetExtent()), 0 },
                nova::ImageUsage::DepthStencilAttach,
                nova::Format::D32_SFloat,
                {});
        }

        auto size = target.GetExtent();

        cmd.ResetGraphicsState();
        cmd.SetBlendState({ true, false });
        cmd.SetViewports({{{0, size.y}, Vec2I(size.x, -i32(size.y))}}, true);
        cmd.SetDepthState(true, true, nova::CompareOp::Greater);
        cmd.SetCullState(nova::CullMode::None, nova::FrontFace::CounterClockwise);
        cmd.BindShaders({ vertex_shader, fragment_shader });

        struct PushConstants
        {
            u64 position_attributes;
            u64  shading_attributes;
            u64           instances;
            Mat4          view_proj;
        };

        cmd.BeginRendering({{}, size}, {target}, depth_image);
        cmd.ClearColor(0, Vec4(Vec3(0.2f), 1.f), Vec2(size));
        cmd.ClearDepth(0.f, Vec2(size));
        cmd.BindIndexBuffer(index_buffer, nova::IndexType::U32);
        cmd.PushConstants(PushConstants {
            .position_attributes = position_attribute_buffer.GetAddress(),
            .shading_attributes = shading_attribute_buffer.GetAddress(),
            .instances = transform_buffer.GetAddress(),
            .view_proj = view_proj,
        });
        cmd.DrawIndexedIndirect(indirect_buffer, 0, indirect_count, sizeof(VkDrawIndexedIndirectCommand));
        cmd.EndRendering();
    }
}