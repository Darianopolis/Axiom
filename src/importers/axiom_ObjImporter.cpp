#include <axiom_Importer.hpp>

#include <fast_obj.h>

namespace axiom
{
    void ImportObj(LoadableScene& scene, std::filesystem::path path)
    {
        (void)scene;

        std::string pathStr = path.string();
        const char* pPath = pathStr.c_str();

        NOVA_LOG("Loading: [{}]", pPath);

        {
            NOVA_LOG("Loading with: fast_obj");
            NOVA_TIMEIT_RESET();
            auto* mesh = fast_obj_read(pPath);
            NOVA_TIMEIT("fast_obj");
            if (mesh) {
                NOVA_LOGEXPR(mesh->position_count);
                NOVA_LOGEXPR(mesh->texcoord_count);
                NOVA_LOGEXPR(mesh->normal_count);
                NOVA_LOGEXPR(mesh->color_count);
                NOVA_LOGEXPR(mesh->face_count);
                NOVA_LOGEXPR(mesh->index_count);
                NOVA_LOGEXPR(mesh->material_count);
                NOVA_LOGEXPR(mesh->object_count);
                NOVA_LOGEXPR(mesh->group_count);
            } else {
                NOVA_LOGEXPR("Error loading from fast_obj");
            }
            fast_obj_destroy(mesh);
        }
    }
}