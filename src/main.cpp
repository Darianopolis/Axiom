#include "axiom_Scene.hpp"
#include "axiom_GltfImporter.hpp"
#include "axiom_DebugRenderer.hpp"

#include <nova/rhi/nova_RHI.hpp>
#include <nova/rhi/vulkan/nova_VulkanRHI.hpp>

#ifndef GLFW_EXPOSE_NATIVE_WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#endif
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

int main(int argc, char* argv[])
{
    if (argc < 2) {
        std::cout << "Usage: \"path/to/scene.gltf\" \"scene name\"";
        return 1;
    }

    std::filesystem::path path{ argv[1] };

// -----------------------------------------------------------------------------
    NOVA_LOG("Loading scene: {}", path.string());
// -----------------------------------------------------------------------------

    axiom::Scene scene;

    axiom::GltfImporter importer{ scene };
    importer.Import(path);

// -----------------------------------------------------------------------------
    NOVA_LOG("Initializing nova::rhi");
// -----------------------------------------------------------------------------

    auto context = nova::Context::Create({
        .debug = true,
    });
    auto queue = context.GetQueue(nova::QueueFlags::Graphics, 0);
    auto fence = nova::Fence::Create(context);
    auto heap = nova::DescriptorHeap::Create(context, 1024 * 1024);
    auto cmdPool = nova::CommandPool::Create(context, queue);
    NOVA_CLEANUP(&) {
        fence.Wait();
        cmdPool.Destroy();
        heap.Destroy();
        fence.Destroy();
        context.Destroy();
    };

// -----------------------------------------------------------------------------
    NOVA_LOG("Compiling scene...");
// -----------------------------------------------------------------------------

    axiom::DebugRenderer renderer;
    renderer.CompileScene(scene);

// -----------------------------------------------------------------------------
    NOVA_LOG("Setting up window...");
// -----------------------------------------------------------------------------

    glfwInit();
    NOVA_CLEANUP(&) { glfwTerminate(); };

    auto title = std::format("Axiom - {}", path.string());
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    auto window = glfwCreateWindow(1920, 1080, title.c_str(), nullptr, nullptr);

    auto swapchain = nova::Swapchain::Create(context,
        glfwGetWin32Window(window),
        nova::TextureUsage::Storage
        | nova::TextureUsage::ColorAttach
        | nova::TextureUsage::TransferDst,
        nova::PresentMode::Mailbox);
    NOVA_CLEANUP(&) {
        fence.Wait();
        swapchain.Destroy();
    };

// -----------------------------------------------------------------------------
    NOVA_LOG("Rendering scene...");
// -----------------------------------------------------------------------------

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        fence.Wait();

        cmdPool.Reset();
        auto cmd = cmdPool.Begin();

        queue.Acquire({swapchain}, {fence});

        renderer.Record(cmd, swapchain.GetCurrent());

        cmd.Present(swapchain);

        queue.Submit({cmd}, {fence}, {fence});
        queue.Present({swapchain}, {fence});
    }
}