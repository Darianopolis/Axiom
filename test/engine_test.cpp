#include <engine/Engine.hpp>

#include <engine/Renderer.hpp>

#include <engine/SceneCompiler.hpp>

#include <scene/axiom_Scene.hpp>
#include <scene/import/axiom_GltfImporter.hpp>
#include <scene/import/axiom_FbxImporter.hpp>
#include <scene/import/axiom_AssimpImporter.hpp>

#include <glfw/glfw3.h>
#include <GLFW/glfw3native.h>

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
            if (glfwGetKey(engine.window, GLFW_KEY_W))          translate += Vec3( 0.f,  0.f, -1.f);
            if (glfwGetKey(engine.window, GLFW_KEY_A))          translate += Vec3(-1.f,  0.f,  0.f);
            if (glfwGetKey(engine.window, GLFW_KEY_S))          translate += Vec3( 0.f,  0.f,  1.f);
            if (glfwGetKey(engine.window, GLFW_KEY_D))          translate += Vec3( 1.f,  0.f,  0.f);
            if (glfwGetKey(engine.window, GLFW_KEY_LEFT_SHIFT)) translate += Vec3( 0.f, -1.f,  0.f);
            if (glfwGetKey(engine.window, GLFW_KEY_SPACE))      translate += Vec3( 0.f,  1.f,  0.f);
            if (translate.x || translate.y || translate.z) {
                renderer.position += renderer.rotation * (glm::normalize(translate) * move_speed * time_step);
            }
        }

        {
            Vec2 delta = {};
            if (GetFocus() == glfwGetWin32Window(engine.window) && glfwGetMouseButton(engine.window, GLFW_MOUSE_BUTTON_2)) {
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

            if ((delta.x || delta.y) && glfwGetMouseButton(engine.window, GLFW_MOUSE_BUTTON_2)) {
                renderer.rotation = glm::angleAxis(delta.x * mouse_speed, Vec3(0.f, -1.f, 0.f)) * renderer.rotation;
                auto pitchedRot = renderer.rotation * glm::angleAxis(delta.y * mouse_speed, Vec3(-1.f, 0.f, 0.f));
                if (glm::dot(pitchedRot * Vec3(0.f, 1.f,  0.f), Vec3(0.f, 1.f, 0.f)) >= 0.f) {
                    renderer.rotation = pitchedRot;
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

int main()
{
    try {
        axiom::Engine engine;

        NOVA_CLEANUP(&) { engine.Shutdown(); };
        engine.Init();

        auto step = new DemoStep;
        step->renderer.engine = &engine;
        step->renderer.Init();

        // axiom::Scene scene;
        // scene.geometries.emplace_back(axiom::Geometry {
        //     .indices = { 0, 1, 2 },
        //     .position_attributes = { Vec3(-0.6f, 0.6f, 0.f), Vec3(0.6f, 0.6f, 0.f), Vec3(0.f, -0.6f, 0.f) },
        // });
        // scene.geometry_ranges.emplace_back(axiom::GeometryRange {
        //     .geometry = 0,
        //     .vertex_offset = 0,
        //     .max_vertex = 2,
        //     .first_index = 0,
        //     .triangle_count = 1,
        // });
        // scene.transform_nodes.emplace_back(axiom::TransformNode {
        //     .transform = glm::mat4x3(glm::translate(Mat4(1.f), Vec3(0.f, 0.f, -10.f))),
        // });
        // scene.meshes.emplace_back(axiom::Mesh {
        //     .geometry_range = 0,
        //     .transform = 0,
        // });

        axiom::GltfImporter gltfImporter;
        auto scene_ir = gltfImporter.Import("D:/Dev/Cloned/glTF-Sample-Models/2.0/Sponza/glTF/Sponza.gltf");
        auto scene = axiom::Compile(scene_ir);

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