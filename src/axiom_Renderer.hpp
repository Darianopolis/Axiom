#pragma once

#include "axiom_Scene.hpp"

#include <nova/core/nova_SubAllocation.hpp>
#include <nova/rhi/nova_RHI.hpp>

namespace axiom
{
    struct Renderer : nova::RefCounted
    {
        virtual ~Renderer() = 0;

        virtual void CompileScene(Scene& scene, nova::CommandPool cmdPool, nova::Fence fence) = 0;

        virtual void SetCamera(Vec3 position, Quat rotation, f32 aspect, f32 fov) = 0;
        virtual void Record(nova::CommandList cmd, nova::Texture target, u32 targetIdx) = 0;
    };

    inline
    Renderer::~Renderer() {}

    nova::Ref<Renderer> CreateRasterRenderer(nova::Context context, nova::DescriptorHeap heap, nova::IndexFreeList* heapSlots);
    nova::Ref<Renderer> CreatePathTraceRenderer(nova::Context context, nova::DescriptorHeap heap, nova::IndexFreeList* heapSlots);
}