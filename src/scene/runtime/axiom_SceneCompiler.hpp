#pragma once

#include "axiom_CompiledScene.hpp"

#include <scene/axiom_Scene.hpp>

namespace axiom
{
    struct SceneCompiler
    {
        bool        flipUVs = false;
        bool flipNormalMapZ = false;

        void Compile(Scene& inScene, CompiledScene& outScene);
    };
}