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

    enum class ImageProcess
    {
        None     = 0,
        FlipNrmZ = 1 << 0,
        GenMips  = 1 << 1,
    };
    NOVA_DECORATE_FLAG_ENUM(ImageProcess)

    struct ImageHeader
    {
        u32           width;
        u32          height;
        nova::Format format;
        f32        min_alpha;
        f32        max_alpha;
        u32            size;
    };

    struct GPU_TangentSpace
    {
        // https://johnwhite3d.blogspot.com/2017/10/signed-octahedron-normal-encoding.html?view=classic
        u32 oct_x : 10;
        u32 oct_y : 10;
        u32 oct_s :  1;

        // https://www.jeremyong.com/graphics/2023/01/09/tangent-spaces-and-diamond-encoding/
        u32 tgt_a : 10;
        u32 tgt_s :  1;
    };

    struct GPU_TexCoords
    {
        u32 packed;
    };

    struct GPU_BoneWeights
    {
        u32 bone_indices[2];
        u32 bone_weights[2];
    };

    class MeshProcessor
    {
        struct TangentSpace
        {
            Vec3    normal;
            Vec3   tangent;
            Vec3 bitangent;
        };

        std::vector<TangentSpace> vertex_tangent_spaces;

    public:
        void ProcessMesh(
             InStridedRegion       in_positions,
             InStridedRegion         in_normals,
             InStridedRegion      in_tex_coords,
             InStridedRegion         in_indices,
            OutStridedRegion out_tangent_spaces,
            OutStridedRegion     out_tex_coords);

        bool flip_uvs = false;
    };

    inline thread_local MeshProcessor S_MeshProcessor;

    class ImageProcessor
    {
        utils::image_u8          image;
        rdo_bc::rdo_bc_encoder encoder;

        std::mutex mutex;

        Vec2U             size;
        std::vector<char> data;
        nova::Format    format;

        f32 min_alpha = 0.f;
        f32 max_alpha = 1.f;

    public:
        void ProcessImage(
            const char*       path,
            usz      embedded_size,
            ImageType         type,
            i32            max_dim,
            ImageProcess processes);

        const void* GetImageData();
        usz GetImageDataSize();
        Vec2U GetImageDimensions();
        nova::Format GetImageFormat();

        f32 GetMinAlpha() { return min_alpha; }
        f32 GetMaxAlpha() { return max_alpha; }
    };

    inline thread_local ImageProcessor S_ImageProcessor;
}