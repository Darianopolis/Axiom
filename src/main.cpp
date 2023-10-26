#include "axiom_Renderer.hpp"

#include <scene/axiom_Scene.hpp>
#include <scene/runtime/axiom_SceneCompiler.hpp>
#include <scene/import/axiom_GltfImporter.hpp>
#include <scene/import/axiom_FbxImporter.hpp>
#include <scene/import/axiom_AssimpImporter.hpp>

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
    "  --path-trace  : Path tracing renderer\n"
    "  --flip-uvs    : Flip UVs vertically\n"
    "  --flip-nmap-z : Flip normal map Z axis\n"
    "  --assimp      : Use assimp importer (experimental)\n"
    "  --raster      : Raster renderer";

int main(int argc, char* argv[])
{
    axiom::SceneCompiler compiler;
    axiom::GltfImporter gltfImporter;
    axiom::FbxImporter fbxImporter;
    axiom::AssimpImporter assimpImporter;

    bool pathTrace = false;
    bool raster = false;
    bool useAssimp = false;
    std::vector<std::filesystem::path> paths;

    for (i32 i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];

        if (arg == "--path-trace") {
            pathTrace = true;
        } else if (arg == "--raster") {
            raster = true;
        } else if (arg == "--flip-uvs") {
            compiler.flipUVs = true;
        } else if (arg == "--flip-nmap-z") {
            compiler.flipNormalMapZ = true;
        } else if (arg == "--assimp") {
            useAssimp = true;
        } else {
            try {
                auto path = std::filesystem::path(arg);
                if (!std::filesystem::exists(path)) {
                    NOVA_LOG("Argument: [{}] not a valid option or file does not exist", arg);
                    return 1;
                }
                paths.emplace_back(std::move(path));
            } catch (...) {
                NOVA_LOG("Argument: [{}] not a valid option", arg);
                return 1;
            }
        }
    }

    if (paths.empty()) {
        NOVA_LOG("No file path provided");
        return 1;
    }

    if (!(pathTrace | raster)) {
        NOVA_LOG("No render mode selected, defaulting to path tracing");
        pathTrace = true;
    }

// -----------------------------------------------------------------------------
    NOVA_LOG("Loading models:");
    for (auto& path : paths) {
        NOVA_LOG(" - {}", path.string());
    }
    NOVA_TIMEIT_RESET();
// -----------------------------------------------------------------------------

    axiom::CompiledScene compiledScene;

    for (auto& path : paths) {
        auto ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](char c) { return char(std::tolower(c)); });

        axiom::Scene scene;

        if (useAssimp) {
            scene = assimpImporter.Import(path);
        } else if (ext == ".gltf" || ext == ".glb") {
            scene = gltfImporter.Import(path);
        } else if (ext == ".fbx") {
            scene = fbxImporter.Import(path);
        } else {
            scene = assimpImporter.Import(path);
        }

        scene.Debug();
        compiler.Compile(scene, compiledScene);
    }

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
    auto cmdPool = nova::CommandPool::Create(context, queue);
    auto sampler = nova::Sampler::Create(context, nova::Filter::Linear,
        nova::AddressMode::Repeat, nova::BorderColor::TransparentBlack, 0.f);
    NOVA_CLEANUP(&) {
        fence.Wait();
        cmdPool.Destroy();
        sampler.Destroy();
        fence.Destroy();
        context.Destroy();
    };

// -----------------------------------------------------------------------------
    NOVA_TIMEIT("init-vulkan");
    NOVA_LOG("Compiling scene...");
// -----------------------------------------------------------------------------

    nova::Ref<axiom::Renderer> renderer;
    if (pathTrace) {
        renderer = axiom::CreatePathTraceRenderer(context);
    } else if (raster) {
        renderer = axiom::CreateRasterRenderer(context);
    }
    renderer->CompileScene(compiledScene, cmdPool, fence);

// -----------------------------------------------------------------------------
    NOVA_TIMEIT("compile-scene");
    NOVA_LOG("Setting up window...");
// -----------------------------------------------------------------------------

    glfwInit();
    NOVA_CLEANUP(&) { glfwTerminate(); };

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    auto window = glfwCreateWindow(1920, 1080, "Axiom", nullptr, nullptr);

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

    static f32 moveSpeed = 1.f;
    glfwSetScrollCallback(window, [](auto, f64, f64 dy) {
        if (!ImGui::GetIO().WantCaptureMouse) {
            if (dy > 0) moveSpeed *= 1.5f;
            if (dy < 0) moveSpeed /= 1.5f;
        }
    });

    auto imgui = nova::ImGuiLayer({
        .window = window,
        .context = context,
        .sampler = sampler,
    });

// -----------------------------------------------------------------------------
    NOVA_TIMEIT("create-window");
    NOVA_LOG("Rendering scene...");
// -----------------------------------------------------------------------------

    Quat rotation;

    // Bistro main
    Vec3 position{ -4.84f, 5.64f, 12.8f };
    rotation.x = -0.14f;
    rotation.y =  0.16f;
    rotation.z =  0.02f;
    rotation.w =  0.98f;

    // Bistro bookshelf
    // Vec3 position{ 50.61f, 2.58f, 21.04f };
    // rotation.x =  0.05f;
    // rotation.y =  0.81f;
    // rotation.z =  0.07f;
    // rotation.w = -0.58f;

    // Sponza main
    // Vec3 position{ 4.86f, 7.74f, 1.1f };
    // rotation.x = -0.01f;
    // rotation.y =  0.64f;
    // rotation.z =  0.01f;
    // rotation.w =  0.77f;

    rotation = glm::normalize(rotation);

    auto lastUpdateTime = std::chrono::steady_clock::now();
    auto lastReportTime = lastUpdateTime;
    u64 frames = 0;
    f32 fps = 0.f;
    i64 allocatedMem = 0;
    i64 allocationCountActive = 0;
    i64 allocationCountRate = 0;

    POINT savedPos{ 0, 0 };
    bool lastMouseDrag = false;
    f32 mouseSpeed = 0.0025f;

    /*

    Controls

    [[Control]]

    - Save location
    - Load location

    [[Profiling]]

    - Start capture for N seconds with fixed seed

    [[Debug views]]

    [[geometric attributes]]

    - Geometric Normals
    - Barycentric weights

    [[vertex attributes]]

    - Texture Coordinates
    - Vertex Normals
    - Vertex Tangents
    - Vertex Bitangents

    [[material attributes]]

    - Material ID
    - Alpha tested
    - Thin/Volume
    - Decal
    - Ior
    - Subsurface

    [[texture maps]]

    - Normal Map  (original)
    - Normals Map (projected)
    - Base color
    - Alpha
    - Metalness / Roughness
    - Emissive

    */

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

            allocatedMem = nova::rhi::stats::MemoryAllocated.load();
            allocationCountActive = nova::rhi::stats::AllocationCount.load();
            allocationCountRate = nova::rhi::stats::NewAllocationCount.exchange(0);
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
                } else {
                    GetCursorPos(&savedPos);
                    ShowCursor(false);
                    lastMouseDrag = true;
                }
                SetCursorPos(savedPos.x, savedPos.y);
            } else if (lastMouseDrag) {
                ShowCursor(true);
                lastMouseDrag = false;
            }

            if ((delta.x || delta.y) && glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_2)) {
                rotation = glm::angleAxis(delta.x * mouseSpeed, Vec3(0.f, -1.f, 0.f)) * rotation;
                auto pitchedRot = rotation * glm::angleAxis(delta.y * mouseSpeed, Vec3(-1.f, 0.f, 0.f));
                if (glm::dot(pitchedRot * Vec3(0.f, 1.f,  0.f), Vec3(0.f, 1.f, 0.f)) >= 0.f) {
                    rotation = pitchedRot;
                }
                rotation = glm::normalize(rotation);
            }
        }

        // Draw

        cmdPool.Reset();
        auto cmd = cmdPool.Begin();

        queue.Acquire({swapchain}, {fence});

        renderer->SetCamera(position, rotation,
            f32(swapchain.GetExtent().x) / f32(swapchain.GetExtent().y), glm::radians(90.f));

        renderer->Record(cmd, swapchain.GetCurrent());

        // UI

        {
            ImGui::Begin("Settings");
            NOVA_CLEANUP(&) { ImGui::End(); };

            ImGui::Text("Allocations: Mem = %s, Active = %i (%i / s)", nova::ByteSizeToString(allocatedMem).c_str(), allocationCountActive, allocationCountRate);
            ImGui::Text("Frametime: %s (%.2f fps)", nova::DurationToString(1s / fps).c_str(), fps);
            ImGui::Text("Position: (%.2f, %.2f, %.2f)", position.x, position.y, position.z);
            ImGui::Text("Rotation: (%.2f, %.2f, %.2f, %.2f)", rotation.x, rotation.y, rotation.z, rotation.w);

            ImGui::Separator();
            if (ImGui::SliderInt("Sample Radius", reinterpret_cast<i32*>(&renderer->sampleRadius), 1, 10)) {
                renderer->ResetSamples();
            }
            ImGui::Separator();

            ImGui::DragFloat("Exposure", &renderer->exposure, 0.01f, 0.f, 10.f);
            ImGui::Combo("Tonemapping", reinterpret_cast<i32*>(&renderer->mode),
                "None\0Aces\0Filmic\0Lottes\0Reinhard\0Reinhard2\0Uchimura\0Uncharted2\0Unreal\0AgX");
        }

        imgui.EndFrame();
        imgui.DrawFrame(cmd, swapchain.GetCurrent(), fence);

        cmd.Present(swapchain);
        queue.Submit({cmd}, {fence}, {fence});
        queue.Present({swapchain}, {fence});
    }
}