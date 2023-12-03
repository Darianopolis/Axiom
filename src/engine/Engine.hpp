#pragma once

#include <nova/core/nova_Math.hpp>
#include <nova/rhi/nova_RHI.hpp>
#include <nova/ui/nova_ImGui.hpp>

using namespace nova::types;

namespace axiom
{
    struct Engine;

    struct Step
    {
        virtual void Execute(Engine& engine) = 0;
        virtual ~Step() = 0;
    };

    inline
    Step::~Step() = default;

    struct Engine
    {
        nova::Context      context;
        nova::Queue          queue;
        nova::Fence          fence;
        nova::CommandPool cmd_pool;
        nova::CommandList      cmd;
        nova::Sampler      sampler;

        nova::Application          app;
        nova::Window            window;
        nova::Swapchain      swapchain;
        nova::imgui::ImGuiLayer* imgui;

        std::vector<Step*>  steps;

    public:
        f32 scroll_offset;

    public:
        void Init();
        void Shutdown();
        bool Update();
        void Run();
    };
}