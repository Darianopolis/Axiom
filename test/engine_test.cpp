#include <engine/Engine.hpp>

#include <engine/Renderer.hpp>

#include <nova/core/nova_Guards.hpp>
#include <nova/core/nova_ToString.hpp>
#include <nova/core/win32/nova_Win32Include.hpp>

#include <imp/imp_Importer.hpp>
#include <imp/imp_Scene.hpp>

struct DemoStep : axiom::Step
{
    axiom::Renderer renderer;

    f32 mouse_speed = 0.0025f;
    f32 move_speed = 0.5f;
    std::chrono::steady_clock::time_point last_time;
    POINT saved_pos;
    bool last_mouse_drag = false;

    void Execute(axiom::Engine& engine) override
    {
        auto time = std::chrono::steady_clock::now();
        auto time_step = std::chrono::duration_cast<std::chrono::duration<f32>>(time - last_time).count();
        last_time = time;

        // Camera

        i32 delta_scroll = i32(engine.scroll_offset);
        for (i32 i = 0; i < std::abs(delta_scroll); ++i) {
            if (delta_scroll > 0) move_speed *= 1.1f;
            if (delta_scroll < 0) move_speed *= 0.9f;
        }
        engine.scroll_offset = 0;

        {
            Vec3 translate = {};
            if (engine.app.IsVirtualKeyDown(nova::VirtualKey::W))         translate += Vec3( 0.f,  0.f, -1.f);
            if (engine.app.IsVirtualKeyDown(nova::VirtualKey::A))         translate += Vec3(-1.f,  0.f,  0.f);
            if (engine.app.IsVirtualKeyDown(nova::VirtualKey::S))         translate += Vec3( 0.f,  0.f,  1.f);
            if (engine.app.IsVirtualKeyDown(nova::VirtualKey::D))         translate += Vec3( 1.f,  0.f,  0.f);
            if (engine.app.IsVirtualKeyDown(nova::VirtualKey::LeftShift)) translate += Vec3( 0.f, -1.f,  0.f);
            if (engine.app.IsVirtualKeyDown(nova::VirtualKey::Space))     translate += Vec3( 0.f,  1.f,  0.f);
            if (translate.x || translate.y || translate.z) {
                renderer.position += renderer.rotation * (glm::normalize(translate) * move_speed * time_step);
            }
        }

        {
            Vec2 delta = {};
            if (GetFocus() == engine.window.GetNativeHandle() && engine.app.IsVirtualKeyDown(nova::VirtualKey::MouseSecondary)) {
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

            if ((delta.x || delta.y) && engine.app.IsVirtualKeyDown(nova::VirtualKey::MouseSecondary)) {
                renderer.rotation = glm::angleAxis(delta.x * mouse_speed, Vec3(0.f, -1.f, 0.f)) * renderer.rotation;
                auto pitched_rot = renderer.rotation * glm::angleAxis(delta.y * mouse_speed, Vec3(-1.f, 0.f, 0.f));
                if (glm::dot(pitched_rot * Vec3(0.f, 1.f,  0.f), Vec3(0.f, 1.f, 0.f)) >= 0.f) {
                    renderer.rotation = pitched_rot;
                }
                renderer.rotation = glm::normalize(renderer.rotation);
            }
        }

        renderer.Draw();

        ImGui::Begin("Statistics");
        ImGui::Text("Allocated: %s", nova::ByteSizeToString(nova::rhi::stats::MemoryAllocated).c_str());
        ImGui::End();
    }

    ~DemoStep()
    {
        renderer.Destroy();
    }
};

int main(int argc, char* argv[])
{
    try {
        axiom::Engine engine;

        NOVA_DEFER(&) { engine.Shutdown(); };
        engine.Init();

        auto step = new DemoStep;
        step->renderer.engine = &engine;
        step->renderer.Init();

        // imp::Scene scene;

        // std::array<uint32_t, 3> indices = { 0, 1, 2 };
        // std::array<glm::vec3, 3> positions = { Vec3(-0.6f, 0.6f, 0.f), Vec3(0.6f, 0.6f, 0.f), Vec3(0.f, -0.6f, 0.f) };
        // imp::Geometry geometry;
        // geometry.indices   = { indices.data(), indices.size() };
        // geometry.positions = { positions.data(), positions.size() };
        // scene.geometries = { &geometry, 1 };

        // imp::GeometryRange range;
        // range.geometry_idx = 0;
        // range.vertex_offset = 0;
        // range.max_vertex = 2;
        // range.first_index = 0;
        // range.triangle_count = 1;
        // scene.geometry_ranges = { &range, 1 };

        // imp::Mesh mesh;
        // mesh.geometry_range_idx = 0;
        // mesh.transform = glm::mat4x3(glm::translate(Mat4(1.f), Vec3(0.f, 0.f, -10.f)));
        // scene.meshes = { &mesh, 1 };

        // axiom::GltfImporter gltf_importer;
        // auto scene_ir = gltf_importer.Import("D:/Dev/Cloned/glTF-Sample-Models/2.0/Sponza/glTF/Sponza.gltf");
        // auto scene = axiom::Compile(scene_ir);

        std::filesystem::path path;

        for (int i = 1; i < argc; ++i) {
            auto _path = std::filesystem::path(argv[i]);
            if (std::filesystem::exists(_path)) {
                path = std::move(_path);
                break;
            }
        }

        if (path.empty()) {
            fmt::println("Must provide a valid existing path!");
            return 1;
        }

        imp::Importer importer;
        importer.SetBaseDir(path.parent_path());
        importer.LoadFile(path);
        importer.ReportStatistics();
        auto scene = importer.GenerateScene();

        step->renderer.scene = &scene;
        step->renderer.Update();

        engine.steps.emplace_back(step);

        engine.Run();
    } catch (std::exception& e) {
        NOVA_LOG("Error: {}", e.what());
    } catch (...) {
        NOVA_LOG("Error");
    }
}