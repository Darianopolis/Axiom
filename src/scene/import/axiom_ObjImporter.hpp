#pragma once

#include <scene/axiom_Scene.hpp>

#include <fast_obj.h>

namespace axiom
{
    struct ObjImporter
    {
        std::filesystem::path dir;
        scene_ir::Scene     scene;

        fastObjMesh* obj = nullptr;

        ~ObjImporter();

        void Reset();
        scene_ir::Scene Import(const std::filesystem::path& path);
    };
}