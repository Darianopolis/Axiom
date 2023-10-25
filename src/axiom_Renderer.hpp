#pragma once

#include <scene/runtime/axiom_CompiledScene.hpp>

#include <nova/core/nova_SubAllocation.hpp>
#include <nova/rhi/nova_RHI.hpp>

namespace axiom
{
    enum class ToneMappingMode : i32
    {
        None       = 0,
        Aces       = 1,
        Filmic     = 2,
        Lottes     = 3,
        Reinhard   = 4,
        Reinhard2  = 5,
        Uchimura   = 6,
        Uncharted2 = 7,
        Unreal     = 8,
        AgX        = 9,
    };

    struct Renderer : nova::RefCounted
    {
        f32         exposure = 1.f;
        u32     sampleRadius = 1;
        ToneMappingMode mode = ToneMappingMode::None;

    public:
        virtual ~Renderer() = 0;

        virtual void CompileScene(CompiledScene& scene, nova::CommandPool cmdPool, nova::Fence fence) = 0;

        virtual void ResetSamples() = 0;
        virtual void SetCamera(Vec3 position, Quat rotation, f32 aspect, f32 fov) = 0;
        virtual void Record(nova::CommandList cmd, nova::Texture target) = 0;
    };

    inline
    Renderer::~Renderer() {}

    nova::Ref<Renderer> CreateRasterRenderer(nova::Context context);
    nova::Ref<Renderer> CreatePathTraceRenderer(nova::Context context);
}