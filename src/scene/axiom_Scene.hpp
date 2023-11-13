#pragma once

#include <axiom_Core.hpp>

namespace axiom
{
    namespace scene_ir
    {
        enum class BufferFormat
        {
            RGBA8,
        };

        struct ImageFileURI
        {
            std::string uri;
        };

        struct ImageFileBuffer
        {
            std::vector<u8> data;
        };

        struct ImageBuffer
        {
            std::vector<u8> data;
            Vec2U           size;
            BufferFormat  format;
        };

        using ImageDataSource = std::variant<ImageBuffer, ImageFileBuffer, ImageFileURI>;

        struct Texture
        {
            ImageDataSource data;
        };

        constexpr u32 InvalidIndex = UINT_MAX;

        struct TextureSwizzle
        {
            u32            texture_idx = InvalidIndex;
            std::array<i8, 4> channels{ -1, -1, -1, -1 };
        };

        namespace property {
            constexpr std::string_view BaseColor     = "base_color"sv;
            constexpr std::string_view Alpha         = "alpha"sv;
            constexpr std::string_view Normal        = "normal"sv;
            constexpr std::string_view Emissive      = "emissive"sv;
            constexpr std::string_view Metallic      = "metallic"sv;
            constexpr std::string_view Roughness     = "roughness"sv;
            constexpr std::string_view AlphaCutoff   = "alpha_cutoff"sv;
            constexpr std::string_view AlphaMask     = "alpha_blend"sv;
            constexpr std::string_view SpecularColor = "specular_color"sv;
            constexpr std::string_view Specular      = "specular"sv;
        }

        using PropertyValue = std::variant<
            TextureSwizzle,
            bool,
            i32,
            f32,
            Vec2,
            Vec3,
            Vec4>;

        struct Property
        {
            std::string_view name;
            PropertyValue   value;
        };

        struct Material
        {
            std::vector<Property> properties;

            template<class ValueT>
            ValueT* GetProperty(std::string_view type)
            {
                for (auto& property : properties) {
                    if (property.name == type
                            && std::holds_alternative<ValueT>(property.value)) {
                        return &std::get<ValueT>(property.value);
                    }
                }

                return nullptr;
            }
        };

        struct Mesh
        {
            std::vector<Vec3>  positions;
            std::vector<Vec3>    normals;
            std::vector<Vec2> tex_coords;
            std::vector<u32>     indices;
            u32             material_idx = InvalidIndex;
        };

        struct Instance
        {
            u32   mesh_idx = InvalidIndex;
            Mat4 transform;
        };

        struct Scene
        {
            std::vector<Texture>   textures;
            std::vector<Material> materials;
            std::vector<Mesh>        meshes;
            std::vector<Instance> instances;

            void Clear()
            {
                textures.clear();
                materials.clear();
                meshes.clear();
                instances.clear();
            }

            void Debug();
        };
    }
}