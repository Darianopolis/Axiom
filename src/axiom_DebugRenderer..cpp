#include "axiom_DebugRenderer.hpp"

namespace axiom
{
    void DebugRenderer::CompileScene(Scene& _scene)
    {
        scene = &_scene;
    }

    void DebugRenderer::SetCamera(Vec3 position, Quat rotation, f32 fov)
    {
        (void)position;
        (void)rotation;
        (void)fov;
    }

    void DebugRenderer::Record(nova::CommandList cmd, nova::Texture target)
    {
        (void)cmd;
        (void)target;
    }
}