#pragma once

#include <scene/axiom_Scene.hpp>

#include <fast_obj.h>

namespace axiom
{
    struct ObjImporter
    {
        std::filesystem::path dir;
        Scene               scene;

        fastObjMesh* obj = nullptr;

        ~ObjImporter();

        void Reset();
        Scene Import(const std::filesystem::path& path);
    };
}