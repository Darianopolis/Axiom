#pragma once

#include <axiom_Core.hpp>

#include <nova/rhi/nova_RHI.hpp>

#include <rdo_bc_encoder.h>

namespace axiom
{
    struct InStridedRegion
    {
        const void* begin = nullptr;
        usz        stride = 0;
        usz         count = 0;

        template<class T>
        const T& Get(usz i) {
            if ((i + 1) * stride > count * stride) {
                NOVA_THROW("Index[{}] out of bounds for count: {}", i, count);
            }
            return *reinterpret_cast<const T*>(
                static_cast<const uc8*>(begin) + i * stride);
        }
    };

    struct OutStridedRegion
    {
        void* begin = nullptr;
        usz  stride = 0;
        usz   count = 0;

        template<class T>
        T& Get(usz i) {
            if ((i + 1) * stride > count * stride) {
                NOVA_THROW("Index[{}] out of bounds for count: {}", i, count);
            }
            return *reinterpret_cast<T*>(
                static_cast<uc8*>(begin) + i * stride);
        }
    };

    enum class ImageType
    {
        ColorAlpha,
        ColorHDR,
        Normal,
        Scalar2,
        Scalar1,
    };

    struct GPU_TangentSpace
    {
        // https://johnwhite3d.blogspot.com/2017/10/signed-octahedron-normal-encoding.html?view=classic
        u32 octX : 10;
        u32 octY : 10;
        u32 octS :  1;

        // https://www.jeremyong.com/graphics/2023/01/09/tangent-spaces-and-diamond-encoding/
        u32 tgtA : 10;
        u32 tgtS :  1;
    };

    struct GPU_TexCoords
    {
        u32 packed;
    };

    class MeshProcessor
    {
        struct TangentSpace
        {
            Vec3    normal;
            Vec3   tangent;
            Vec3 bitangent;
        };

        std::vector<TangentSpace> vertexTangentSpaces;

    public:
        void ProcessMesh(
             InStridedRegion      inPositions,
             InStridedRegion        inNormals,
             InStridedRegion      inTexCoords,
             InStridedRegion        inIndices,
            OutStridedRegion outTangentSpaces,
            OutStridedRegion     outTexCoords);
    };

    inline thread_local MeshProcessor s_MeshProcessor;

    class ImageProcessor
    {
        utils::image_u8          image;
        rdo_bc::rdo_bc_encoder encoder;

    public:
        void ProcessImage(
            const char* path,
            usz embeddedSize,
            ImageType   type,
            i32       maxDim,
            bool         mip);

        const void* GetImageData();
        usz GetImageDataSize();
        nova::Format GetImageFormat();
    };

    inline thread_local ImageProcessor s_ImageProcessor;
}