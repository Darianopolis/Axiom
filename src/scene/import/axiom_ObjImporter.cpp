#include "axiom_ObjImporter.hpp"

namespace axiom
{
    ObjImporter::~ObjImporter()
    {
        Reset();
    }

    void ObjImporter::Reset()
    {
        scene.Clear();
        fast_obj_destroy(obj);
    }

    Scene ObjImporter::Import(const std::filesystem::path& path)
    {
        Reset();

        dir = path.parent_path();

        obj = fast_obj_read(path.string().c_str());

        if (!obj) {
            NOVA_THROW("Error loading obj file!");
        }

        return std::move(scene);
    }
}