#include <axiom_Importer.hpp>

#include <ufbx.h>

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

namespace axiom
{
    void ImportFbx(LoadableScene&, std::filesystem::path path)
    {
        std::string pathStr = path.string();
        const char* pPath = pathStr.c_str();

        NOVA_LOG("Loading: [{}]", pPath);

        {
            NOVA_LOG("Loading with: ufbx");
            NOVA_TIMEIT_RESET();
            ufbx_load_opts opts{};
            ufbx_error error;
            ufbx_scene* scene = ufbx_load_file(pPath, &opts, &error);
            NOVA_TIMEIT("ufbx");
            if (scene) {
                NOVA_LOGEXPR(scene->meshes.count);
                NOVA_LOGEXPR(scene->textures.count);
                NOVA_LOGEXPR(scene->nodes.count);
            } else {
                NOVA_LOGEXPR(error.description.data);
                NOVA_LOGEXPR(error.info);
            }
        }

        {
            NOVA_LOG("Loading with: assimp");
            NOVA_TIMEIT_RESET();
            Assimp::Importer importer;
            auto asset = importer.ReadFile(pPath, 0);
            NOVA_TIMEIT("assimp");
            if (asset) {
                NOVA_LOGEXPR(asset->mNumMeshes);
                NOVA_LOGEXPR(asset->mNumTextures);
                NOVA_LOGEXPR(asset->mNumMaterials);
            } else {
                NOVA_LOGEXPR(importer.GetErrorString());
            }
        }
    }
}