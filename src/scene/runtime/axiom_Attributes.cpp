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
        Vec3 SignedOctEncode(Vec3 normal)
        {
            Vec3 encoded;

            normal /= (glm::abs(normal.x) + glm::abs(normal.y) + glm::abs(normal.z));

            encoded.y = normal.y *  0.5f + 0.5f;
            encoded.x = normal.x *  0.5f + encoded.y;
            encoded.y = normal.x * -0.5f + encoded.y;

            encoded.z = glm::clamp(normal.z * FLT_MAX, 0.f, 1.f);
            return encoded;
        }

// -----------------------------------------------------------------------------
//                              Decode Normals
// -----------------------------------------------------------------------------

        inline
        Vec3 SignedOctDecode(Vec3 encoded)
        {
            Vec3 normal;

            normal.x = (encoded.x - encoded.y);
            normal.y = (encoded.x + encoded.y) - 1.f;
            normal.z = encoded.z * 2.f - 1.f;
            normal.z = normal.z * (1.f - glm::abs(normal.x) - glm::abs(normal.y));

            normal = glm::normalize(normal);
            return normal;
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
            f32 py_sign = glm::sign(p.y);
            return -py_sign * 0.25f * x + 0.5f + py_sign * 0.25f;
        }

        // Given a normal and tangent vector, encode the tangent as a single float that can be
        // subsequently quantized.
        inline
        f32 EncodeTangent(Vec3 normal, Vec3 tangent, bool& choice)
        {
            // First, find a canonical direction in the tangent plane
            Vec3 t1;
            if (glm::abs(normal.y) > glm::abs(normal.z))
            {
                // Pick a canonical direction orthogonal to n with z = 0
                t1 = Vec3(normal.y, -normal.x, 0.f);
                choice = true;
            }
            else
            {
                // Pick a canonical direction orthogonal to n with y = 0
                t1 = Vec3(normal.z, 0.f, -normal.x);
                choice = false;
            }
            t1 = glm::normalize(t1);

            // Construct t2 such that t1 and t2 span the plane
            Vec3 t2 = glm::cross(t1, normal);

            // Decompose the tangent into two coordinates in the canonical basis
            Vec2 packed_tangent = Vec2(glm::dot(tangent, t1), glm::dot(tangent, t2));

            // Apply our diamond encoding to our two coordinates
            return EncodeDiamond(packed_tangent);
        }

// -----------------------------------------------------------------------------
//                              Decode Tangents
// -----------------------------------------------------------------------------

        inline
        Vec2 DecodeDiamond(f32 p)
        {
            Vec2 v;

            // Remap p to the appropriate segment on the diamond
            f32 p_sign = glm::sign(p - 0.5f);
            v.x = -p_sign * 4.f * p + 1.f + p_sign * 2.f;
            v.y = p_sign * (1.f - glm::abs(v.x));

            // Normalization extends the point on the diamond back to the unit circle
            return glm::normalize(v);
        }

        inline
        Vec3 DecodeTangent(Vec3 normal, f32 diamond_tangent)
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
            Vec2 packed_tangent = DecodeDiamond(diamond_tangent);

            return packed_tangent.x * t1 + packed_tangent.y * t2;
        }
    }

// -----------------------------------------------------------------------------
//                              Mesh Processing
// -----------------------------------------------------------------------------

    void MeshProcessor::ProcessMesh(
        InStridedRegion           positions,
        InStridedRegion             normals,
        InStridedRegion          tex_coords,
        InStridedRegion             indices,
        OutStridedRegion out_tangent_spaces,
        OutStridedRegion     out_tex_coords)
    {
        bool has_normals = normals.count;
        bool has_tex_coords = tex_coords.count;

        // Update and clear scratch space

        vertex_tangent_spaces.resize(positions.count);
        if (has_normals) {
            for (u32 i = 0; i < normals.count; ++i) {
                vertex_tangent_spaces[i].normal = normals.Get<Vec3>(i);
                vertex_tangent_spaces[i].tangent = Vec3(0.f);
                vertex_tangent_spaces[i].bitangent = Vec3(0.f);
            }
        } else {
            std::ranges::fill(vertex_tangent_spaces, TangentSpace{});
        }

        // Update normal, tangent, bitangent, and area for vertex

        auto update_normal_tangent = [&](u32 i, Vec3 normal, Vec3 tangent, Vec3 bitangent, f32 area) {
            if (!has_normals) {
                vertex_tangent_spaces[i].normal += area * normal;
            }
            vertex_tangent_spaces[i].tangent += area * tangent;
            vertex_tangent_spaces[i].bitangent += area * bitangent;
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

            if (has_tex_coords) {
                auto tc1 = tex_coords.Get<Vec2>(v1i);
                auto tc2 = tex_coords.Get<Vec2>(v2i);
                auto tc3 = tex_coords.Get<Vec2>(v3i);

                if (flip_uvs) {
                    tc1.y = 1.f - tc1.y;
                    tc2.y = 1.f - tc2.y;
                    tc3.y = 1.f - tc3.y;
                }

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
                update_normal_tangent(v1i, normal, tangent, bitangent, area);
                update_normal_tangent(v2i, normal, tangent, bitangent, area);
                update_normal_tangent(v3i, normal, tangent, bitangent, area);
            }
        }

        for (u32 i = 0; i < positions.count; ++i) {

            auto& tbn = vertex_tangent_spaces[i];
            tbn.bitangent = glm::normalize(tbn.bitangent);
            tbn.tangent = Reorthogonalize(glm::normalize(tbn.tangent), tbn.normal);
            tbn.normal = glm::normalize(tbn.normal);

            // Quantize and output tangent space

            GPU_TangentSpace ts;

            auto enc_normal = SignedOctEncode(tbn.normal);
            ts.oct_x = u32(enc_normal.x * 1023.0);
            ts.oct_y = u32(enc_normal.y * 1023.0);
            ts.oct_s = u32(enc_normal.z);

            auto decode_normal = SignedOctDecode(Vec3(
                f32(ts.oct_x) / 1023.f,
                f32(ts.oct_y) / 1023.f,
                f32(ts.oct_s)
            ));

            bool tgt_choice;
            auto enc_tangent = EncodeTangent(decode_normal, tbn.tangent, tgt_choice);
            ts.tgt_a = u32(enc_tangent * 1023.0);
            ts.tgt_s = u32(tgt_choice);
            // auto enc_bitangent = glm::dot(glm::cross(tbn.normal, tbn.tangent), tbn.bitangent) > 0.f;
            // ts.tgt_s = u32(enc_bitangent);

            out_tangent_spaces.Get<GPU_TangentSpace>(i) = ts;

            // Quantize and output texture coordinates

            Vec2 uv = has_tex_coords
                ? tex_coords.Get<Vec2>(i)
                : Vec2(0.f);
            if (flip_uvs) {
                uv.y = 1.f - uv.y;
            }
            out_tex_coords.Get<GPU_TexCoords>(i) = GPU_TexCoords(glm::packHalf2x16(uv));
        }
    }

// -----------------------------------------------------------------------------
//                              Image Processing
// -----------------------------------------------------------------------------

    void ImageProcessor::ProcessImage(
        const char*       path,
        usz      embedded_size,
        ImageType         type,
        i32            max_dim,
        ImageProcess processes)
    {
        (void)type;

        constexpr bool UseBC7 = true;

        std::string cached_name;
        std::filesystem::path cached_path;

        if (!embedded_size) {
            cached_name = base64_encode(std::string_view(path), true);
            cached_name += std::format("${}${}${}", u32(processes), max_dim, u32(UseBC7));
            cached_path = std::filesystem::path("cache") / cached_name;
        }

        std::unique_lock lock{ mutex };

        if (!embedded_size && std::filesystem::exists(cached_path)) {
            lock.unlock();

            nova::File file{ cached_path.string().c_str() };
            ImageHeader header;
            file.Read(header);

            size = { header.width, header.height };
            min_alpha = header.min_alpha;
            max_alpha = header.max_alpha;

            format = header.format;

            data.resize(header.size);
            file.Read(data.data(), header.size);

            return;
        }

        NOVA_LOG("Image[{}] not cached, generating...", embedded_size ? "$embedded" : path);

        i32 width, height, channels;
        stbi_uc* raw_data = nullptr;
        if (embedded_size) {
            raw_data = stbi_load_from_memory(
                reinterpret_cast<const uc8*>(path), i32(embedded_size),
                &width, &height, &channels,
                STBI_rgb_alpha);
        } else {
            raw_data = stbi_load(path, &width, &height, &channels, STBI_rgb_alpha);
        }

        if (!raw_data) {
            NOVA_THROW("File not loaded!");
        }

        auto GetIndex = [](i32 x, i32 y, i32 pitch) {
            return x + y * pitch;
        };

        if (width > max_dim || height > max_dim) {
            i32 factor = std::max(width / max_dim, height / max_dim);
            i32 new_width = width / factor;
            i32 new_height = height / factor;
            i32 factor2 = factor * factor;

            image.init(new_width, new_height);
            for (i32 x = 0; x < new_width; ++x) {
                for (i32 y = 0; y < new_height; ++y) {

                    Vec4 acc = {};

                    for (i32 dx = 0; dx < factor; ++dx) {
                        for (i32 dy = 0; dy < factor; ++dy) {
                            auto* pixel = raw_data + GetIndex(x * factor + dx, y * factor + dy, width) * 4;
                            acc.r += pixel[0];
                            acc.g += pixel[1];
                            acc.b += pixel[2];
                            acc.a += pixel[3];
                        }
                    }

                    acc /= f32(factor2);
                    image.get_pixels()[GetIndex(x, y, new_width)]
                        = { u8(acc.r), u8(acc.g), u8(acc.b), u8(acc.a) };
                }
            }

            width = new_width;
            height = new_height;
        } else {

            image.init(width, height);
            std::memcpy(image.get_pixels().data(), raw_data, width * height * 4);
        }

        stbi_image_free(raw_data);

        min_alpha = 1.f;
        max_alpha = 0.f;

        if (type == ImageType::ColorAlpha) {
            // Find alpha values
            for (i32 x = 0; x < width; ++x) {
                for (i32 y = 0; y < height; ++y) {
                    auto& pixel = image.get_pixels()[GetIndex(x, y, width)];
                    f32 alpha = f32(pixel[3]) / 255.f;
                    min_alpha = std::min(alpha, min_alpha);
                    max_alpha = std::max(alpha, max_alpha);
                }
            }
        }

        if (processes >= ImageProcess::FlipNrmZ) {
            for (i32 x = 0; x < width; ++x) {
                for (i32 y = 0; y < height; ++y) {
                    auto& pixel = image.get_pixels()[GetIndex(x, y, width)];
                    pixel[2] = u8(255 - pixel[2]);
                }
            }
        }

        size = { width, height };

        if (UseBC7) {
            rdo_bc::rdo_bc_params params;
            params.m_bc7enc_reduce_entropy = true;
            params.m_rdo_multithreading    = true;

            format = nova::Format::BC7_Unorm;

            encoder.init(image, params);
            encoder.encode();

            data.resize(encoder.get_total_blocks_size_in_bytes());
            std::memcpy(data.data(), encoder.get_blocks(), encoder.get_total_blocks_size_in_bytes());
        } else {
            auto byte_size = width * height * 4;
            data.resize(byte_size);
            std::memcpy(data.data(), image.get_pixels().data(), byte_size);

            format = nova::Format::RGBA8_UNorm;
        }

        if (!embedded_size) {
            nova::File file{ cached_path.string().c_str(), true };

            ImageHeader header{};
            header.width = size.x;
            header.height = size.y;
            header.min_alpha = min_alpha;
            header.max_alpha = max_alpha;
            header.size = u32(data.size());
            header.format = format;

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
        return format;
    }
}