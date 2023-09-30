#pragma once

#include "axiom_Scene.hpp"

#include <nova/rhi/nova_RHI.hpp>

namespace axiom
{
    struct DebugRenderer
    {
        Scene* scene = nullptr;

        void CompileScene(Scene& scene);

        void SetCamera(Vec3 position, Quat rotation, f32 fov);
        void Record(nova::CommandList cmd, nova::Texture target);
    };
}