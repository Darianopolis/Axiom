#pragma once

#include <scene/axiom_Scene.hpp>

// TODO:
// #define UFBX_REAL_IS_FLOAT
#include <ufbx.h>

namespace axiom
{
    struct FbxIndex
    {
        u32 value = InvalidIndex;
    };

    struct FbxVertex
    {
        Vec3 pos = {};
        Vec2  uv = {};
        Vec3 nrm = {};

        bool operator==(const FbxVertex& other) const noexcept {
            return pos == other.pos
                && uv == other.uv
                && nrm == other.nrm;
        }
    };

}
NOVA_MEMORY_HASH(axiom::FbxVertex);
namespace axiom
{

    struct FbxImporter
    {
        ufbx_scene* fbx = nullptr;

        std::filesystem::path dir;

        Scene scene;

        std::vector<std::pair<u32, u32>> fbxMeshOffsets;

        HashMap<void*, u32>  textureIndices;
        HashMap<void*, u32> materialIndices;

        std::vector<u32>                 triIndices;
        HashMap<FbxVertex, FbxIndex> uniqueVertices;
        std::vector<FbxVertex>        vertexIndices;

        ~FbxImporter();

        void Reset();
        Scene Import(const std::filesystem::path& path);

        void ProcessTexture(u32 texIdx);
        void ProcessMaterial(u32 matIdx);
        void ProcessMesh(u32 fbxMeshIdx, u32 primIdx);
        void ProcessNode(ufbx_node* node, Mat4 parentTransform);
    };
}