#pragma once

#include "axiom_Scene.hpp"

#include <nova/rhi/nova_RHI.hpp>

namespace axiom
{
    struct Renderer : nova::RefCounted
    {
        virtual ~Renderer() = 0;

        virtual void CompileScene(Scene& scene) = 0;

        virtual void SetCamera(Vec3 position, Quat rotation, f32 aspect, f32 fov) = 0;
        virtual void Record(nova::CommandList cmd, nova::Texture target) = 0;
    };

    inline
    Renderer::~Renderer() {}

    nova::Ref<Renderer> CreateDebugRasterRenderer(nova::Context context);
    nova::Ref<Renderer> CreateDebugRTRenderer(nova::Context context);
}