#pragma once

#include "axiom_CompiledScene.hpp"

#include <scene/axiom_Scene.hpp>

namespace axiom
{
    struct SceneCompiler
    {
        bool          flip_uvs = false;
        bool flip_normal_map_z = false;

        void Compile(scene_ir::Scene& in_scene, CompiledScene& out_scene);
    };
}