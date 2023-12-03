#include "Engine.hpp"

namespace axiom
{
    void Engine::Init()
    {
        context = nova::Context::Create({
            .debug = true,
        });

        queue = context.GetQueue(nova::QueueFlags::Graphics, 0);
        fence = nova::Fence::Create(context);
        cmd_pool = nova::CommandPool::Create(context, queue);

        sampler = nova::Sampler::Create(context, nova::Filter::Linear,
            nova::AddressMode::Repeat, nova::BorderColor::TransparentBlack, 0.f);

        app = nova::Application::Create();

        window = nova::Window::Create(app, {
            .title = "Axiom",
            .size = { 1920, 1080 },
        });

        swapchain = nova::Swapchain::Create(context, window.GetNativeHandle(),
            nova::ImageUsage::Storage | nova::ImageUsage::ColorAttach,
            nova::PresentMode::Mailbox);

        imgui = new nova::imgui::ImGuiLayer({
            // .flags = ImGuiConfigFlags_DockingEnable,
            .window = window,
            .context = context,
            .sampler = sampler,
        });

        app.AddCallback([&](const nova::AppEvent& event) {
            if (event.type == nova::EventType::MouseScroll) {
                scroll_offset += event.scroll.scrolled.y;
            }
        });

        imgui->no_dock_bg = true;
    }

    void Engine::Shutdown()
    {
        fence.Wait();

        for (auto* step : steps) {
            delete step;
        }

        delete imgui;
        swapchain.Destroy();
        app.Destroy();
        sampler.Destroy();
        cmd_pool.Destroy();
        fence.Destroy();
        context.Destroy();
    }

    bool Engine::Update()
    {
        if (!app.IsRunning()) {
            return false;
        }

        app.PollEvents();

        fence.Wait();
        queue.Acquire({swapchain}, {fence});
        cmd_pool.Reset();
        cmd = cmd_pool.Begin();

        cmd.ClearColor(swapchain.GetCurrent(), Vec4(0.f));

        imgui->BeginFrame();

        for (auto* step : steps) {
            step->Execute(*this);
        }

        imgui->DrawFrame(cmd, swapchain.GetCurrent(), fence);
        cmd.Present(swapchain);

        queue.Submit({cmd}, {fence}, {fence});
        queue.Present({swapchain}, {fence});

        return true;
    }

    void Engine::Run()
    {
        while (Update());
    }
}