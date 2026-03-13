#include <axiom_Renderer.hpp>

#include <scene/axiom_Scene.hpp>
#include <scene/runtime/axiom_SceneCompiler.hpp>
#include <scene/import/axiom_GltfImporter.hpp>
#include <scene/import/axiom_FbxImporter.hpp>
#include <scene/import/axiom_AssimpImporter.hpp>

#include <nova/rhi/nova_RHI.hpp>

#include <nova/ui/nova_ImGui.hpp>

#include <nova/window/nova_Window.hpp>
#include <nova/core/win32/nova_Win32.hpp>

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
    axiom::GltfImporter gltf_importer;
    axiom::FbxImporter fbx_importer;
    axiom::AssimpImporter assimp_importer;

    bool path_trace = false;
    bool raster = false;
    bool use_assimp = false;
    std::vector<std::filesystem::path> paths;

    for (i32 i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];

        if (arg == "--path-trace") {
            path_trace = true;
        } else if (arg == "--raster") {
            raster = true;
        } else if (arg == "--flip-uvs") {
            compiler.flip_uvs = true;
        } else if (arg == "--flip-nmap-z") {
            compiler.flip_normal_map_z = true;
        } else if (arg == "--assimp") {
            use_assimp = true;
        } else {
            try {
                auto path = std::filesystem::path(arg);
                if (!std::filesystem::exists(path)) {
                    nova::Log("Argument: [{}] not a valid option or file does not exist", arg);
                    return 1;
                }
                paths.emplace_back(std::move(path));
            } catch (...) {
                nova::Log("Argument: [{}] not a valid option", arg);
                return 1;
            }
        }
    }

    if (paths.empty()) {
        nova::Log("No file path provided");
        return 1;
    }

    if (!(path_trace | raster)) {
        nova::Log("No render mode selected, defaulting to path tracing");
        path_trace = true;
    }

// -----------------------------------------------------------------------------
    nova::Log("Loading models:");
    for (auto& path : paths) {
        nova::Log(" - {}", path.string());
    }
    NOVA_TIMEIT_RESET();
// -----------------------------------------------------------------------------

    axiom::CompiledScene compiled_scene;

    for (auto& path : paths) {
        auto ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](char c) { return char(std::tolower(c)); });

        axiom::scene_ir::Scene scene;

        nova::Log("Loading: {}", path.string());

        if (use_assimp) {
            nova::Log("Forcing assimp!");
            scene = assimp_importer.Import(path);
        } else if (ext == ".gltf" || ext == ".glb") {
            nova::Log("Detected gltf!");
            scene = gltf_importer.Import(path);
        } else if (ext == ".fbx") {
            nova::Log("Detected fbx");
            scene = fbx_importer.Import(path);
        } else {
            nova::Log("Unknown format, using assimp");
            scene = assimp_importer.Import(path);
        }

        // scene.Debug();
        compiler.Compile(scene, compiled_scene);
        // compiled_scene.DebugDump();
    }

    // {
    //     auto& path = paths[0];
    //     imp::Importer importer;
    //     importer.SetBaseDir(path.parent_path());
    //     importer.LoadFile(path);
    //     importer.ReportStatistics();
    //     auto scene = importer.GenerateScene();
    //     compiled_scene.Compile(scene);
    // }

    // {
    //     axiom::scene_ir::Scene scene;
    //     struct Vertex {
    //         Vec3 position;
    //         Vec3 normal;
    //         Vec2 texCoord;
    //     };

    //     std::vector<Vertex> vertices;
    //     std::vector<u32> indices;

    //     {
    //         // Base case

    //         constexpr f32 X = 0.525731112119133606f;
    //         constexpr f32 Z = 0.850650808352039932f;
    //         constexpr f32 N = 0.f;

    //         vertices = {
    //             {{-X,N,Z}}, {{ X,N, Z}}, {{-X, N,-Z}}, {{X, N,-Z}},
    //             {{ N,Z,X}}, {{ N,Z,-X}}, {{ N,-Z,X}}, {{ N,-Z,-X}},
    //             {{ Z,X,N}}, {{-Z,X, N}}, {{ Z,-X,N}}, {{-Z,-X, N}}
    //         };

    //         for (auto& v : vertices)
    //         {
    //             v.position = glm::normalize(v.position);
    //             v.normal = v.position;
    //         }

    //         indices = {
    //             0,  4,  1, 0, 9,  4, 9,  5, 4,  4, 5, 8, 4, 8,  1,
    //             8, 10,  1, 8, 3, 10, 5,  3, 8,  5, 2, 3, 2, 7,  3,
    //             7, 10,  3, 7, 6, 10, 7, 11, 6, 11, 0, 6, 0, 1,  6,
    //             6,  1, 10, 9, 0, 11, 9, 11, 2,  9, 2, 5, 7, 2, 11,
    //         };
    //     }

    //     using Lookup = ankerl::unordered_dense::map<u64, u32>;
    //     auto vertexForEdge = [&](Lookup& lookup, std::vector<Vertex>& vertices,  u32 first, u32 second) {
    //         u64 key = first < second
    //             ? u64(first) << 32 | second
    //             : u64(second) << 32 | first;

    //         auto inserted = lookup.insert({ key, u32(vertices.size()) });
    //         if (inserted.second)
    //         {
    //             auto& edge0 = vertices[first].position;
    //             auto& edge1 = vertices[second].position;
    //             auto point = glm::normalize(edge0 + edge1);
    //             vertices.push_back(Vertex{.position = point, .normal = point});
    //         }

    //         return inserted.first->second;
    //     };

    //     constexpr u32 SubDivisions = 7;
    //     for (u32 i = 0; i < SubDivisions; ++i)
    //     {
    //         Lookup lookup;
    //         std::vector<u32> result;

    //         for (u32 j = 0; j < indices.size(); j += 3)
    //         {
    //             std::array<u32, 3> vi = { indices[j + 0], indices[j + 1], indices[j + 2] };

    //             std::array<u32, 3> mid;
    //             for (u32 edge = 0; edge < 3; ++edge)
    //             {
    //                 mid[edge] = vertexForEdge(lookup, vertices, vi[edge], vi[(edge + 1)%3]);
    //             }

    //             result.push_back(vi[0]);
    //             result.push_back(mid[0]);
    //             result.push_back(mid[2]);

    //             result.push_back(vi[1]);
    //             result.push_back(mid[1]);
    //             result.push_back(mid[0]);

    //             result.push_back(vi[2]);
    //             result.push_back(mid[2]);
    //             result.push_back(mid[1]);

    //             result.push_back(mid[0]);
    //             result.push_back(mid[1]);
    //             result.push_back(mid[2]);
    //         }

    //         indices = std::move(result);
    //         nova::Log("Subdivision level: {}, triangles = {}", i + 1, indices.size() / 3);
    //     }

    //     for (auto& v : vertices)
    //     {
    //         // v.texCoord = Vec2(
    //         //     (glm::atan(v.position.z, v.position.x) / (2.f * glm::pi<f32>())) + 0.5f,
    //         //     (glm::asin(v.position.y) / glm::pi<f32>())) + 0.5f;

    //         v.texCoord = Vec2(
    //             1.f - ((glm::atan(v.position.z, v.position.x) / (2.f * glm::pi<f32>())) + 0.5f),
    //             1.f - ((glm::asin(v.position.y) / glm::pi<f32>()) + 0.5f));
    //     }

    //     for (u32 idx = 0; idx < indices.size(); idx += 3) {
    //         auto v0 = vertices[indices[idx + 0]];
    //         auto v1 = vertices[indices[idx + 1]];
    //         auto v2 = vertices[indices[idx + 2]];

    //         auto wrapVertex = [&](u32 local_idx) {
    //             auto new_idx = u32(vertices.size());
    //             auto& v = vertices.emplace_back(vertices[indices[idx + local_idx]]);
    //             v.texCoord.x += 1.f;
    //             switch (local_idx) {
    //                 break;case 0: v0 = v;
    //                 break;case 1: v1 = v;
    //                 break;case 2: v2 = v;
    //             }
    //             indices[idx + local_idx] = new_idx;
    //         };

    //         float tolerance = 0.9f;
    //         if (std::abs(v1.texCoord.x - v0.texCoord.x) > tolerance) wrapVertex(v1.texCoord.x > v0.texCoord.x ? 0 : 1);
    //         if (std::abs(v2.texCoord.x - v0.texCoord.x) > tolerance) wrapVertex(v2.texCoord.x > v0.texCoord.x ? 0 : 2);
    //         if (std::abs(v2.texCoord.x - v1.texCoord.x) > tolerance) wrapVertex(v2.texCoord.x > v1.texCoord.x ? 1 : 2);

    //         // auto fixVertex = [&](u32 local_idx, f32 new_u) {
    //         //     auto new_idx = u32(vertices.size());
    //         //     auto& v = vertices.emplace_back();
    //         //     auto& old_v = vertices[indices[idx + local_idx]];
    //         //     v.position = old_v.position;
    //         //     v.normal = old_v.normal;
    //         //     v.texCoord = old_v.texCoord;
    //         //     v.texCoord.x = new_u;
    //         //     switch (local_idx) {
    //         //         break;case 0: v0 = v;
    //         //         break;case 1: v1 = v;
    //         //         break;case 2: v2 = v;
    //         //     }
    //         //     indices[idx + local_idx] = new_idx;
    //         // };

    //         // float ax = v0.texCoord.x;
    //         // float bx = v1.texCoord.x;
    //         // float cx = v2.texCoord.x;
    //         // float ay = v0.texCoord.y;
    //         // float by = v1.texCoord.y;
    //         // float cy = v2.texCoord.y;

    //         // u32 a = 0, b = 1, c = 2;

    //         // auto Eq = [](float a, float b) -> bool { return std::abs(a - b) < 0.001f; };

    //         // if (bx - ax >= 0.5f && !Eq(ay, 1.f)) fixVertex(b, bx - 1.f);
    //         // if (cx - bx > 0.5f) fixVertex(c, cx - 1.f);
    //         // if ((ax > 0.5f && ax - cx > 0.5f) || (Eq(ax, 1.f) && Eq(cy, 0.f))) fixVertex(a, ax - 1.f);
    //         // if (bx > 0.5f && bx - ax > 0.5f) fixVertex(b, bx - 1.f);
    //         // if (Eq(ay, 0.f) || Eq(ay, 1.f)) fixVertex(a, (bx + cx) / 2.f);
    //         // if (Eq(by, 0.f) || Eq(by, 1.f)) fixVertex(b, (ax + cx) / 2.f);
    //         // if (Eq(cy, 0.f) || Eq(cy, 1.f)) fixVertex(c, (ax + bx) / 2.f);
    //     }

    //     auto& mesh = scene.meshes.emplace_back();
    //     for (auto& v : vertices) {
    //         mesh.positions.push_back(v.position);
    //         mesh.normals.push_back(v.normal);
    //         mesh.tex_coords.push_back(v.texCoord);
    //     }
    //     mesh.indices = std::move(indices);
    //     mesh.material_idx = 0;

    //     auto& texture = scene.textures.emplace_back();
    //     // texture.data = axiom::scene_ir::ImageFileURI("C:/Users/Darian/Downloads/equirectangular_projection.jpg");
    //     // texture.data = axiom::scene_ir::ImageFileURI("C:/Users/Darian/Downloads/testimage.jpg");
    //     texture.data = axiom::scene_ir::ImageFileURI("C:/Users/Darian/Downloads/HDR_041_Path/HDR_041_Path.hdr");
    //     // texture.data = axiom::scene_ir::ImageBuffer{ {255, 0, 0, 255}, {1, 1}, axiom::scene_ir::BufferFormat::RGBA8 };

    //     auto& material = scene.materials.emplace_back();
    //     // material.properties.emplace_back(axiom::scene_ir::property::BaseColor, Vec4(1.f, 0.f, 0.f, 1.f));
    //     material.properties.emplace_back(axiom::scene_ir::property::BaseColor, axiom::scene_ir::TextureSwizzle{0, { 0, 1, 2, 3 }});

    //     auto& instance = scene.instances.emplace_back();
    //     instance.mesh_idx = 0;
    //     instance.transform = Mat4(1.f);

    //     compiler.Compile(scene, compiled_scene);
    // }

// -----------------------------------------------------------------------------
    NOVA_TIMEIT("load-scene");
    nova::Log("Initializing nova::rhi");
// -----------------------------------------------------------------------------

    auto context = nova::Context::Create({
        .debug = false,
        .ray_tracing = true,
    });
    auto queue = context.Queue(nova::QueueFlags::Graphics, 0);
    auto sampler = nova::Sampler::Create(context, nova::Filter::Linear,
        nova::AddressMode::Repeat, nova::BorderColor::TransparentBlack, 0.f);
    NOVA_DEFER(&) {
        queue.WaitIdle();
        sampler.Destroy();
        context.Destroy();
    };

// -----------------------------------------------------------------------------
    NOVA_TIMEIT("init-vulkan");
    nova::Log("Compiling scene...");
// -----------------------------------------------------------------------------

    nova::Ref<axiom::Renderer> renderer;
    if (path_trace) {
        renderer = axiom::CreatePathTraceRenderer(context);
    } else if (raster) {
        renderer = axiom::CreateRasterRenderer(context);
    }
    renderer->CompileScene(compiled_scene);

// -----------------------------------------------------------------------------
    NOVA_TIMEIT("compile-scene");
    nova::Log("Setting up window...");
// -----------------------------------------------------------------------------

    auto app = nova::Application::Create();
    NOVA_DEFER(&) { app.Destroy(); };

    auto window = nova::Window::Create(app)
        .SetTitle("Axiom")
        .SetSize({ 1920, 1080 }, nova::WindowPart::Client)
        .Show(true);

    auto swapchain = nova::Swapchain::Create(context,
        window,
        nova::ImageUsage::Storage
        | nova::ImageUsage::ColorAttach
        | nova::ImageUsage::TransferDst,
        nova::PresentMode::Immediate);
    NOVA_DEFER(&) {
        queue.WaitIdle();
        swapchain.Destroy();
    };

    // TODO: Callbacks

    f32 move_speed = 1.f;
    bool show_settings = true;
    bool fullscreen = false;

    app.AddCallback([&](const nova::AppEvent& event) {
        switch (event.type) {
            break;case nova::EventType::MouseScroll:
                if (!ImGui::GetIO().WantCaptureMouse) {
                    if (event.scroll.scrolled.y > 0.f) move_speed *= 1.5f;
                    if (event.scroll.scrolled.y < 0.f) move_speed /= 1.5f;
                }
            break;case nova::EventType::Input:
                if (event.input.pressed) {
                    switch (app.ToVirtualKey(event.input.channel)) {
                        break;case nova::VirtualKey::F1:
                            show_settings = !show_settings;
                        break;case nova::VirtualKey::F11:
                            fullscreen = !fullscreen;
                            window.SetFullscreen(fullscreen);
                    }
                }
        }
    });

    auto imgui = nova::imgui::ImGuiLayer({
        .window = window,
        .context = context,
        .sampler = sampler,
        .frames_in_flight = 1,
    });

// -----------------------------------------------------------------------------
    NOVA_TIMEIT("create-window");
    nova::Log("Rendering scene...");
// -----------------------------------------------------------------------------

    Vec3 position = {};
    Quat rotation = Quat(Vec3(0.f));

    // Bistro main
    // Vec3 position{ -4.84f, 5.64f, 12.8f };
    // rotation.x = -0.14f;
    // rotation.y =  0.16f;
    // rotation.z =  0.02f;
    // rotation.w =  0.98f;

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

    auto last_update_time = std::chrono::steady_clock::now();
    auto last_report_time = last_update_time;
    u64 frames = 0;
    f32 fps = 0.f;
    i64 allocated_mem = 0;
    i64 allocation_count_active = 0;
    i64 allocation_count_rate = 0;

    POINT saved_pos{ 0, 0 };
    bool last_mouse_drag = false;
    f32 mouse_speed = 0.0025f;


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

    NOVA_DEFER(&) { queue.WaitIdle(); };
    while (app.ProcessEvents()) {
        imgui.BeginFrame();

        queue.WaitIdle();

        // Time

        using namespace std::chrono;
        auto now = steady_clock::now();
        auto delta_time = duration_cast<duration<f32>>(now - last_update_time).count();
        last_update_time = now;

        // FPS

        frames++;
        if (now - last_report_time > 1s)
        {
            fps = frames / duration_cast<duration<f32>>(now - last_report_time).count();
            last_report_time = now;
            frames = 0;

            allocated_mem = nova::rhi::stats::MemoryAllocated.load();
            allocation_count_active = nova::rhi::stats::AllocationCount.load();
            allocation_count_rate = nova::rhi::stats::NewAllocationCount.exchange(0);
        }

        // Camera

        if (GetFocus() == (HWND)window.NativeHandle()) {
            Vec3 translate = {};
            if (app.IsVirtualKeyDown(nova::VirtualKey::W))         translate += Vec3( 0.f,  0.f, -1.f);
            if (app.IsVirtualKeyDown(nova::VirtualKey::A))         translate += Vec3(-1.f,  0.f,  0.f);
            if (app.IsVirtualKeyDown(nova::VirtualKey::S))         translate += Vec3( 0.f,  0.f,  1.f);
            if (app.IsVirtualKeyDown(nova::VirtualKey::D))         translate += Vec3( 1.f,  0.f,  0.f);
            if (app.IsVirtualKeyDown(nova::VirtualKey::LeftShift)) translate += Vec3( 0.f, -1.f,  0.f);
            if (app.IsVirtualKeyDown(nova::VirtualKey::Space))     translate += Vec3( 0.f,  1.f,  0.f);
            if (translate.x || translate.y || translate.z) {
                position += rotation * (glm::normalize(translate) * move_speed * delta_time);
            }
        }

        {
            Vec2 delta = {};
            if (GetFocus() == (HWND)window.NativeHandle() && app.IsVirtualKeyDown(nova::VirtualKey::MouseSecondary)) {
                POINT p;
                GetCursorPos(&p);
                LONG dx = p.x - saved_pos.x;
                LONG dy = p.y - saved_pos.y;
                if (last_mouse_drag) {
                    delta = { f32(dx), f32(dy) };
                } else {
                    GetCursorPos(&saved_pos);
                    ShowCursor(false);
                    last_mouse_drag = true;
                }
                SetCursorPos(saved_pos.x, saved_pos.y);
            } else if (last_mouse_drag) {
                ShowCursor(true);
                last_mouse_drag = false;
            }

            if ((delta.x || delta.y) && app.IsVirtualKeyDown(nova::VirtualKey::MouseSecondary)) {
                rotation = glm::angleAxis(delta.x * mouse_speed, Vec3(0.f, -1.f, 0.f)) * rotation;
                auto pitched_rot = rotation * glm::angleAxis(delta.y * mouse_speed, Vec3(-1.f, 0.f, 0.f));
                if (glm::dot(pitched_rot * Vec3(0.f, 1.f,  0.f), Vec3(0.f, 1.f, 0.f)) >= 0.f) {
                    rotation = pitched_rot;
                }
                rotation = glm::normalize(rotation);
            }
        }

        // Draw

        auto cmd = queue.Begin();

        queue.Acquire({swapchain});

        renderer->SetCamera(position, rotation,
            f32(swapchain.Extent().x) / f32(swapchain.Extent().y), glm::radians(90.f));

        renderer->Record(cmd, swapchain.Target());

        // UI

        if (show_settings) {
            ImGui::Begin("Settings (F1 to show/hide)");
            NOVA_DEFER(&) { ImGui::End(); };

            ImGui::Text("Allocations: Mem = %s, Active = %i (%i / s)", nova::ByteSizeToString(allocated_mem).c_str(), allocation_count_active, allocation_count_rate);
            ImGui::Text("Frametime: %s (%.2f fps)", nova::DurationToString(1s / fps).c_str(), fps);
            ImGui::Text("Position: (%.2f, %.2f, %.2f)", position.x, position.y, position.z);
            ImGui::Text("Rotation: (%.2f, %.2f, %.2f, %.2f)", rotation.x, rotation.y, rotation.z, rotation.w);

            ImGui::Separator();
            if (ImGui::SliderInt("Sample Radius", reinterpret_cast<i32*>(&renderer->sample_radius), 1, 10)) {
                renderer->ResetSamples();
            }
            ImGui::Separator();

            ImGui::DragFloat("Exposure", &renderer->exposure, 0.01f, 0.f, 10.f);
            ImGui::Combo("Tonemapping", reinterpret_cast<i32*>(&renderer->mode),
                "None\0Aces\0Filmic\0Lottes\0Reinhard\0Reinhard2\0Uchimura\0Uncharted2\0Unreal\0AgX");
        }

        imgui.EndFrame();
        imgui.DrawFrame(cmd, swapchain.Target());

        cmd.Present(swapchain);
        queue.Submit({cmd}, {});
        queue.Present({swapchain}, {});
    }
}