#pragma once

#include <cstdint>

namespace axiom
{
    struct StridedRegion
    {
        void*      begin = nullptr;
        size_t    stride = 0;
        size_t     count = 0;
    };

    enum class ImageType
    {
        ColorAlpha,
        ColorHDR,
        Normal,
        Scalar2,
        Scalar1,
    };

    struct ShadingAttrib
    {
        uint32_t octX : 10;
        uint32_t octY : 10;
        uint32_t octS :  1;
        uint32_t tgtA : 10;
        uint32_t tgtS :  1;
        uint16_t    u;
        uint16_t    v;
    };

    struct AssetProcessor
    {
        size_t ProcessMesh(
            StridedRegion positions,
            StridedRegion   normals,
            StridedRegion texCoords,
            StridedRegion   indices,
            void*              pOut = nullptr,
            size_t          outSize = 0);

        size_t LoadImage(
            const char* path,
            ImageType   type,
            bool         mip,
            void*       pOut = nullptr,
            size_t   outSize = 0);
    };
}