#include "axiom_Scene.hpp"
#include "axiom_Importer.hpp"
#include "axiom_Renderer.hpp"

#include <nova/rhi/nova_RHI.hpp>
#include <nova/rhi/vulkan/nova_VulkanRHI.hpp>

#include <nova/ui/nova_ImGui.hpp>

#ifndef GLFW_EXPOSE_NATIVE_WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#endif
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

using namespace nova::types;

constexpr std::string_view UsageString =
    "Usage: [options] \"path/to/scene.gltf\" \"scene name\"\n"
    "options:\n"
    "  --path-trace : Path tracing renderer\n"
    "  --raster     : Raster renderer";

int main(int argc, char* argv[])
{
    if (argc < 3) {
        std::cout << UsageString;
        return 1;
    }

    auto type = std::string_view(argv[1]);
    if (type != "--path-trace" && type != "--raster") {
        std::cout << UsageString;
        std::cout << "\nUnknown option [" << type << "]";
        return 1;
    }

    std::filesystem::path path{ argv[2] };
    if (!std::filesystem::exists(path)) {
        NOVA_LOG("File not found: {}", path.string());
        return -1;
    }

// -----------------------------------------------------------------------------
    NOVA_LOG("Loading scene: {}", path.string());
    NOVA_TIMEIT_RESET();
// -----------------------------------------------------------------------------

    axiom::Scene scene;

    auto importer = axiom::CreateGltfImporter(scene);
    importer->Import(path);

// -----------------------------------------------------------------------------
    NOVA_TIMEIT("load-scene");
    NOVA_LOG("Initializing nova::rhi");
// -----------------------------------------------------------------------------

    auto context = nova::Context::Create({
        .debug = false,
        .rayTracing = true,
    });
    auto queue = context.GetQueue(nova::QueueFlags::Graphics, 0);
    auto fence = nova::Fence::Create(context);
    auto heap = nova::DescriptorHeap::Create(context, 1024 * 1024);
    auto cmdPool = nova::CommandPool::Create(context, queue);
    auto sampler = nova::Sampler::Create(context, nova::Filter::Linear,
        nova::AddressMode::Repeat, nova::BorderColor::TransparentBlack, 0.f);
    NOVA_CLEANUP(&) {
        fence.Wait();
        cmdPool.Destroy();
        sampler.Destroy();
        heap.Destroy();
        fence.Destroy();
        context.Destroy();
    };

    heap.WriteSampler(1, sampler);

// -----------------------------------------------------------------------------
    NOVA_TIMEIT("init-vulkan");
    NOVA_LOG("Compiling scene...");
// -----------------------------------------------------------------------------

    nova::Ref<axiom::Renderer> renderer;
    if (type == "--path-trace") {
        renderer = axiom::CreatePathTraceRenderer(context);
    } else if (type == "--raster") {
        renderer = axiom::CreateRasterRenderer(context);
    }
    renderer->CompileScene(scene, cmdPool, fence);

// -----------------------------------------------------------------------------
    NOVA_TIMEIT("compile-scene");
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

    auto imgui = nova::ImGuiLayer({
        .window = window,
        .context = context,
        .heap = heap,
        .sampler = 1,
        .fontTextureID = 2,
    });

// -----------------------------------------------------------------------------
    NOVA_TIMEIT("create-window");
    NOVA_LOG("Rendering scene...");
// -----------------------------------------------------------------------------

    Vec3 position{ 0.f, 0.f, 2.f };
    Quat rotation{ Vec3(0.f) };
    static f32 moveSpeed = 1.f;

    auto lastUpdateTime = std::chrono::steady_clock::now();
    auto lastReportTime = lastUpdateTime;
    u64 frames = 0;
    f32 fps = 0.f;

    POINT savedPos{ 0, 0 };
    bool lastMouseDrag = false;
    f32 mouseSpeed = 0.0025f;

    glfwSetScrollCallback(window, [](auto, f64, f64 dy) {
        if (dy > 0) moveSpeed *= 1.5f;
        if (dy < 0) moveSpeed /= 1.5f;
    });

    NOVA_CLEANUP(&) { fence.Wait(); };
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        imgui.BeginFrame();

        fence.Wait();

        // Time

        using namespace std::chrono;
        auto now = steady_clock::now();
        auto timeStep = duration_cast<duration<f32>>(now - lastUpdateTime).count();
        lastUpdateTime = now;

        // FPS

        frames++;
        if (now - lastReportTime > 1s)
        {
            fps = frames / duration_cast<duration<f32>>(now - lastReportTime).count();
            lastReportTime = now;
            frames = 0;
        }

        // Camera

        {
            Vec3 translate = {};
            if (glfwGetKey(window, GLFW_KEY_W))          translate += Vec3( 0.f,  0.f, -1.f);
            if (glfwGetKey(window, GLFW_KEY_A))          translate += Vec3(-1.f,  0.f,  0.f);
            if (glfwGetKey(window, GLFW_KEY_S))          translate += Vec3( 0.f,  0.f,  1.f);
            if (glfwGetKey(window, GLFW_KEY_D))          translate += Vec3( 1.f,  0.f,  0.f);
            if (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT)) translate += Vec3( 0.f, -1.f,  0.f);
            if (glfwGetKey(window, GLFW_KEY_SPACE))      translate += Vec3( 0.f,  1.f,  0.f);
            if (translate.x || translate.y || translate.z) {
                position += rotation * (glm::normalize(translate) * moveSpeed * timeStep);
            }
        }

        {
            Vec2 delta = {};
            if (GetFocus() == glfwGetWin32Window(window) && glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_2)) {
                POINT p;
                GetCursorPos(&p);
                LONG dx = p.x - savedPos.x;
                LONG dy = p.y - savedPos.y;
                if (lastMouseDrag) {
                    delta = { f32(dx), f32(dy) };
                }
                else {
                    GetCursorPos(&savedPos);
                    ShowCursor(false);
                    lastMouseDrag = true;
                }
                SetCursorPos(savedPos.x, savedPos.y);
            }
            else if (lastMouseDrag) {
                ShowCursor(true);
                lastMouseDrag = false;
            }

            if ((delta.x || delta.y) && glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_2)) {
                rotation = glm::angleAxis(delta.x * mouseSpeed, Vec3(0.f, -1.f, 0.f)) * rotation;
                rotation = rotation * glm::angleAxis(delta.y * mouseSpeed, Vec3(-1.f, 0.f, 0.f));
                rotation = glm::normalize(rotation);
            }
        }

        // Draw

        cmdPool.Reset();
        auto cmd = cmdPool.Begin();

        queue.Acquire({swapchain}, {fence});

        renderer->SetCamera(position, rotation,
            f32(swapchain.GetExtent().x) / f32(swapchain.GetExtent().y), glm::radians(90.f));

        cmd.BindDescriptorHeap(nova::BindPoint::Graphics, heap);
        cmd.BindDescriptorHeap(nova::BindPoint::Compute, heap);
        cmd.BindDescriptorHeap(nova::BindPoint::RayTracing, heap);
        heap.WriteStorageTexture(0, swapchain.GetCurrent());

        renderer->Record(cmd, swapchain.GetCurrent(), 0);

        // UI

        {
            ImGui::Begin("Settings");
            NOVA_CLEANUP(&) { ImGui::End(); };

            ImGui::Text("Frametime: %s (%.2f fps)", nova::DurationToString(1s / fps).c_str(), fps);
        }

        imgui.EndFrame();
        imgui.DrawFrame(cmd, swapchain.GetCurrent(), fence);

        cmd.Present(swapchain);
        queue.Submit({cmd}, {fence}, {fence});
        queue.Present({swapchain}, {fence});
    }
}