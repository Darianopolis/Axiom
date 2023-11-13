#include "Engine.hpp"

#define GLFW_EXPOSE_NATIVE_WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

namespace axiom
{
    void Engine::Init()
    {
        context = nova::Context::Create({
            .debug = true,
        });

        queue = context.GetQueue(nova::QueueFlags::Graphics, 0);
        fence = nova::Fence::Create(context);
        cmdPool = nova::CommandPool::Create(context, queue);

        sampler = nova::Sampler::Create(context, nova::Filter::Linear,
            nova::AddressMode::Repeat, nova::BorderColor::TransparentBlack, 0.f);

        glfwInit();
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        window = glfwCreateWindow(1920, 1080, "Axiom", nullptr, nullptr);
        swapchain = nova::Swapchain::Create(context, glfwGetWin32Window(window),
            nova::TextureUsage::Storage | nova::TextureUsage::ColorAttach,
            nova::PresentMode::Mailbox);

        glfwSetWindowUserPointer(window, this);
        glfwSetScrollCallback(window, [](auto* w, f64, f64 y) {
            auto* self = static_cast<Engine*>(glfwGetWindowUserPointer(w));
            self->scroll_offset += y;
        });

        imgui = new nova::imgui::ImGuiLayer({
            // .flags = ImGuiConfigFlags_DockingEnable,
            .window = window,
            .context = context,
            .sampler = sampler,
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
        glfwTerminate();
        sampler.Destroy();
        cmdPool.Destroy();
        fence.Destroy();
        context.Destroy();
    }

    bool Engine::Update()
    {
        if (glfwWindowShouldClose(window)) {
            return false;
        }

        glfwPollEvents();

        fence.Wait();
        queue.Acquire({swapchain}, {fence});
        cmdPool.Reset();
        cmd = cmdPool.Begin();

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