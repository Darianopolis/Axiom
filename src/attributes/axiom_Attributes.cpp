#include "axiom_Attributes.hpp"

#include <nova/core/nova_Files.hpp>

#include <stb_image.h>

#include <base64.h>

namespace axiom
{
    namespace
    {
        inline
        Vec3 Reorthogonalize(Vec3 v, Vec3 other)
        {
            return glm::normalize(v - glm::dot(v, other) * other);
        }

// -----------------------------------------------------------------------------
//                               Encode Normals
// -----------------------------------------------------------------------------

        inline
        Vec3 SignedOctEncode(Vec3 n)
        {
            Vec3 outN;

            n /= (glm::abs(n.x) + glm::abs(n.y) + glm::abs(n.z));

            outN.y = n.y *  0.5f + 0.5f;
            outN.x = n.x *  0.5f + outN.y;
            outN.y = n.x * -0.5f + outN.y;

            outN.z = glm::clamp(n.z * FLT_MAX, 0.f, 1.f);
            return outN;
        }

// -----------------------------------------------------------------------------
//                              Decode Normals
// -----------------------------------------------------------------------------

        inline
        Vec3 SignedOctDecode(Vec3 n)
        {
            Vec3 outN;

            outN.x = (n.x - n.y);
            outN.y = (n.x + n.y) - 1.f;
            outN.z = n.z * 2.f - 1.f;
            outN.z = outN.z * (1.f - glm::abs(outN.x) - glm::abs(outN.y));

            outN = glm::normalize(outN);
            return outN;
        }

// -----------------------------------------------------------------------------
//                              Encode Tangents
// -----------------------------------------------------------------------------

        inline
        f32 EncodeDiamond(Vec2 p)
        {
            // Project to the unit diamond, then to the x-axis.
            f32 x = p.x / (glm::abs(p.x) + glm::abs(p.y));

            // Contract the x coordinate by a factor of 4 to represent all 4 quadrants in
            // the unit range and remap
            f32 pySign = glm::sign(p.y);
            return -pySign * 0.25f * x + 0.5f + pySign * 0.25f;
        }

        // Given a normal and tangent vector, encode the tangent as a single float that can be
        // subsequently quantized.
        inline
        f32 EncodeTangent(Vec3 normal, Vec3 tangent)
        {
            // First, find a canonical direction in the tangent plane
            Vec3 t1;
            if (glm::abs(normal.y) > glm::abs(normal.z))
            {
                // Pick a canonical direction orthogonal to n with z = 0
                t1 = Vec3(normal.y, -normal.x, 0.f);
            }
            else
            {
                // Pick a canonical direction orthogonal to n with y = 0
                t1 = Vec3(normal.z, 0.f, -normal.x);
            }
            t1 = glm::normalize(t1);

            // Construct t2 such that t1 and t2 span the plane
            Vec3 t2 = glm::cross(t1, normal);

            // Decompose the tangent into two coordinates in the canonical basis
            Vec2 packedTangent = Vec2(glm::dot(tangent, t1), glm::dot(tangent, t2));

            // Apply our diamond encoding to our two coordinates
            return EncodeDiamond(packedTangent);
        }

// -----------------------------------------------------------------------------
//                              Decode Tangents
// -----------------------------------------------------------------------------

        inline
        Vec2 DecodeDiamond(f32 p)
        {
            Vec2 v;

            // Remap p to the appropriate segment on the diamond
            f32 pSign = glm::sign(p - 0.5f);
            v.x = -pSign * 4.f * p + 1.f + pSign * 2.f;
            v.y = pSign * (1.f - glm::abs(v.x));

            // Normalization extends the point on the diamond back to the unit circle
            return glm::normalize(v);
        }

        inline
        Vec3 DecodeTangent(Vec3 normal, f32 diamondTangent)
        {
            // As in the encode step, find our canonical tangent basis span(t1, t2)
            Vec3 t1;
            if (glm::abs(normal.y) > glm::abs(normal.z)) {
                t1 = Vec3(normal.y, -normal.x, 0.f);
            } else {
                t1 = Vec3(normal.z, 0.f, -normal.x);
            }
            t1 = glm::normalize(t1);

            Vec3 t2 = glm::cross(t1, normal);

            // Recover the coordinates used with t1 and t2
            Vec2 packedTangent = DecodeDiamond(diamondTangent);

            return packedTangent.x * t1 + packedTangent.y * t2;
        }
    }

// -----------------------------------------------------------------------------
//                              Mesh Processing
// -----------------------------------------------------------------------------

    void MeshProcessor::ProcessMesh(
        InStridedRegion         positions,
        InStridedRegion           normals,
        InStridedRegion         texCoords,
        InStridedRegion           indices,
        OutStridedRegion outTangentSpaces,
        OutStridedRegion     outTexCoords)
    {
        bool hasNormals = normals.count;
        bool hasTexCoords = texCoords.count;

        // Update and clear scratch space

        vertexTangentSpaces.resize(positions.count);
        if (hasNormals) {
            for (u32 i = 0; i < normals.count; ++i) {
                vertexTangentSpaces[i].normal = normals.Get<Vec3>(i);
                vertexTangentSpaces[i].tangent = Vec3(0.f);
                vertexTangentSpaces[i].bitangent = Vec3(0.f);
            }
        } else {
            std::ranges::fill(vertexTangentSpaces, TangentSpace{});
        }

        // Update normal, tangent, bitangent, and area for vertex

        auto updateNormalTangent = [&](u32 i, Vec3 normal, Vec3 tangent, Vec3 bitangent, f32 area) {
            if (!hasNormals) {
                vertexTangentSpaces[i].normal += area * normal;
            }
            vertexTangentSpaces[i].tangent += area * tangent;
            vertexTangentSpaces[i].bitangent += area * bitangent;
        };

        // Accumulate triangle tangent spaces

        for (u32 i = 0; i < indices.count; i += 3) {
            u32 v1i = indices.Get<u32>(i + 0);
            u32 v2i = indices.Get<u32>(i + 1);
            u32 v3i = indices.Get<u32>(i + 2);

            auto& v1 = positions.Get<Vec3>(v1i);
            auto& v2 = positions.Get<Vec3>(v2i);
            auto& v3 = positions.Get<Vec3>(v3i);

            auto v12 = v2 - v1;
            auto v13 = v3 - v1;

            Vec3 tangent = {};
            Vec3 bitangent = {};
            // TODO: If no tex coords, pick suitable stable tangents

            if (hasTexCoords) {
                auto tc1 = texCoords.Get<Vec2>(v1i);
                auto tc2 = texCoords.Get<Vec2>(v2i);
                auto tc3 = texCoords.Get<Vec2>(v3i);

                auto u12 = tc2 - tc1;
                auto u13 = tc3 - tc1;

                f32 f = 1.f / (u12.x * u13.y - u13.x * u12.y);
                tangent = f * Vec3 {
                    u13.y * v12.x - u12.y * v13.x,
                    u13.y * v12.y - u12.y * v13.y,
                    u13.y * v12.z - u12.y * v13.z,
                };

                bitangent = f * Vec3 {
                    u13.x * v12.x - u12.x * v13.x,
                    u13.x * v12.y - u12.x * v13.y,
                    u13.x * v12.z - u12.x * v13.z,
                };
            }

            auto cross = glm::cross(v12, v13);
            auto area = glm::length(0.5f * cross);
            auto normal = glm::normalize(cross);

            if (area) {
                updateNormalTangent(v1i, normal, tangent, bitangent, area);
                updateNormalTangent(v2i, normal, tangent, bitangent, area);
                updateNormalTangent(v3i, normal, tangent, bitangent, area);
            }
        }

        for (u32 i = 0; i < positions.count; ++i) {

            auto& tbn = vertexTangentSpaces[i];
            tbn.bitangent = glm::normalize(tbn.bitangent);
            tbn.tangent = Reorthogonalize(glm::normalize(tbn.tangent), tbn.normal);
            tbn.normal = glm::normalize(tbn.normal);

            // Quantize and output tangent space

            GPU_TangentSpace ts;

            auto encNormal = SignedOctEncode(tbn.normal);
            ts.octX = u32(encNormal.x * 1023.0);
            ts.octY = u32(encNormal.y * 1023.0);
            ts.octS = u32(encNormal.z);

            auto encTangent = EncodeTangent(tbn.normal, tbn.tangent);
            ts.tgtA = u32(encTangent * 1023.0);

            auto encBitangent = glm::dot(glm::cross(tbn.normal, tbn.tangent), tbn.bitangent) > 0.f;
            ts.tgtS = u32(encBitangent);

            outTangentSpaces.Get<GPU_TangentSpace>(i) = ts;

            // Quantize and output texture coordinates

            outTexCoords.Get<GPU_TexCoords>(i)
                = GPU_TexCoords(glm::packHalf2x16(hasTexCoords ? texCoords.Get<Vec2>(i) : Vec2(0.f)));
        }
    }

// -----------------------------------------------------------------------------
//                              Image Processing
// -----------------------------------------------------------------------------

    void ImageProcessor::ProcessImage(
        const char*       path,
        usz       embeddedSize,
        ImageType         type,
        i32             maxDim,
        ImageProcess processes)
    {
        (void)type;

        if (embeddedSize) {
            NOVA_THROW("In-memory caching not currently supported!");
        }

        auto cachedName = base64_encode(std::string_view(path), true);
        auto cachedPath = std::filesystem::path("cache") / cachedName;

        std::unique_lock lock{ mutex };

        if (std::filesystem::exists(cachedPath)) {
            lock.unlock();

            nova::File file{ cachedPath.string().c_str() };
            ImageHeader header;
            file.Read(header);

            size = { header.width, header.height };
            minAlpha = header.minAlpha;
            maxAlpha = header.maxAlpha;

            data.resize(header.size);
            file.Read(data.data(), header.size);

            return;
        }

        NOVA_LOG("Image[{}] not cached, generating...", path);

        i32 width, height, channels;
        stbi_uc* pData = nullptr;
        if (embeddedSize) {
            pData = stbi_load_from_memory(
                reinterpret_cast<const uc8*>(path), i32(embeddedSize),
                &width, &height, &channels,
                STBI_rgb_alpha);
        } else {
            pData = stbi_load(path, &width, &height, &channels, STBI_rgb_alpha);
        }

        if (!pData) {
            NOVA_THROW("File not loaded!");
        }

        auto getIndex = [](i32 x, i32 y, i32 pitch) {
            return x + y * pitch;
        };

        if (width > maxDim || height > maxDim) {
            i32 uWidth = width;
            i32 uHeight = height;
            i32 factor = std::max(width / maxDim, height / maxDim);
            i32 sWidth = uWidth / factor;
            i32 sHeight = uHeight / factor;
            i32 factor2 = factor * factor;

            image.init(sWidth, sHeight);
            for (i32 x = 0; x < sWidth; ++x) {
                for (i32 y = 0; y < sHeight; ++y) {

                    Vec4 acc = {};

                    for (i32 dx = 0; dx < factor; ++dx) {
                        for (i32 dy = 0; dy < factor; ++dy) {
                            auto* pixel = pData + getIndex(x * factor + dx, y * factor + dy, uWidth) * 4;
                            acc.r += pixel[0];
                            acc.g += pixel[1];
                            acc.b += pixel[2];
                            acc.a += pixel[3];
                        }
                    }

                    acc /= f32(factor2);
                    image.get_pixels()[getIndex(x, y, sWidth)]
                        = { u8(acc.r), u8(acc.g), u8(acc.b), u8(acc.a) };
                }
            }

            width = sWidth;
            height = sHeight;
        } else {

            image.init(width, height);
            std::memcpy(image.get_pixels().data(), pData, width * height * 4);
        }

        stbi_image_free(pData);

        minAlpha = 1.f;
        maxAlpha = 0.f;

        if (type == ImageType::ColorAlpha) {
            // Find alpha values
            for (i32 x = 0; x < width; ++x) {
                for (i32 y = 0; y < height; ++y) {
                    auto& pixel = image.get_pixels()[getIndex(x, y, width)];
                    f32 alpha = f32(pixel[3]) / 255.f;
                    minAlpha = std::min(alpha, minAlpha);
                    maxAlpha = std::max(alpha, maxAlpha);
                }
            }
        }

        if (processes >= ImageProcess::FlipNrmZ) {
            for (i32 x = 0; x < width; ++x) {
                for (i32 y = 0; y < height; ++y) {
                    auto& pixel = image.get_pixels()[getIndex(x, y, width)];
                    pixel[2] = u8(255 - pixel[2]);
                }
            }
        }

        size = { width, height };

        constexpr bool UseBC7 = true;

        if (UseBC7) {
            rdo_bc::rdo_bc_params params;
            params.m_bc7enc_reduce_entropy = true;
            params.m_rdo_multithreading    = true;

            encoder.init(image, params);
            encoder.encode();

            data.resize(encoder.get_total_blocks_size_in_bytes());
            std::memcpy(data.data(), encoder.get_blocks(), encoder.get_total_blocks_size_in_bytes());
        } else {
            auto byteSize = width * height * 4;
            data.resize(byteSize);
            std::memcpy(data.data(), image.get_pixels().data(), byteSize);
        }

        {
            nova::File file{ cachedPath.string().c_str(), true };

            ImageHeader header{};
            header.width = size.x;
            header.height = size.y;
            header.minAlpha = minAlpha;
            header.maxAlpha = maxAlpha;
            header.size = u32(data.size());

            file.Write(header);
            file.Write(data.data(), header.size);
        }
    }

    const void* ImageProcessor::GetImageData()
    {
        return data.data();
    }

    usz ImageProcessor::GetImageDataSize()
    {
        return data.size();
    }

    Vec2U ImageProcessor::GetImageDimensions()
    {
        return size;
    }

    nova::Format ImageProcessor::GetImageFormat()
    {
        return nova::Format::BC7_Unorm;
    }
}