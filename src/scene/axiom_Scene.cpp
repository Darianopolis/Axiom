#include "axiom_Scene.hpp"

#include <unordered_set>

namespace axiom
{
    void scene_ir::Scene::Debug()
    {
        auto WriteHeader = [&](std::string_view header) {
            nova::Log("\n{:=^80}\n", std::format(" {} ", header));
        };

        WriteHeader("Overview");

        nova::Log("Textures = {}", textures.size());
        {
            std::unordered_set<std::string_view> unique_paths;
            uint32_t raw_data_count = 0;
            uint32_t duplicated_ids = 0;
            for (auto& tex : textures) {
                if (auto uri = std::get_if<ImageFileURI>(&tex.data)) {
                    if (unique_paths.contains(uri->uri)) {
                        duplicated_ids++;
                    } else {
                        unique_paths.insert(uri->uri);
                    }
                } else {
                    raw_data_count++;
                }
            }
            nova::Log("  Unique Files: {} ({} duplicates)", unique_paths.size(), duplicated_ids);
            nova::Log("  Buffers: {}", raw_data_count);
        }
        nova::Log("Materials: {}", materials.size());
        nova::Log("Meshes: {}", meshes.size());
        nova::Log("Instances: {}", instances.size());

        WriteHeader("Textures");

        for (auto& texture : textures) {
            std::cout << "Texture[" << (&texture - textures.data()) << "]";
            if (auto uri = std::get_if<ImageFileURI>(&texture.data)) {
                nova::Log(": File[{}]", uri->uri);
            } else if (auto file = std::get_if<ImageFileBuffer>(&texture.data)) {
                nova::Log(": InlineFile[magic = {}|{:#x}, size = {}]",
                    std::string_view(reinterpret_cast<char*>(file->data.data())).substr(0, 4),
                    *reinterpret_cast<uint32_t*>(file->data.data()),
                    file->data.size());
            } else if (auto buffer = std::get_if<ImageBuffer>(&texture.data)) {
                const char* format_name = "Unknown";
                switch (buffer->format) {
                        using enum BufferFormat;
                    break;case RGBA8: format_name = "RGBA8";
                }
                nova::Log(": Raw[size = ({}, {}), format = {}]", buffer->size.x, buffer->size.y, format_name);
            }
        }

        WriteHeader("Materials");

        for (auto& material: materials) {
            nova::Log("Material[{}]", &material - materials.data());
            for (auto& property : material.properties) {
                nova::Log("  {}:", property.name);
                std::visit(nova::Overloads {
                    [&](const TextureSwizzle& value) {
                        nova::Log("    Texture: {}", value.texture_idx);
                    },
                    [&](const bool& value) {
                        nova::Log("    Bool: {}", value);
                    },
                    [&](const i32& value) {
                        nova::Log("    Int: {}", value);
                    },
                    [&](const f32& value) {
                        nova::Log("    Float: {}", value);
                    },
                    [&](const Vec2& value) {
                        nova::Log("    Vec2: {}", glm::to_string(value));
                    },
                    [&](const Vec3& value) {
                        nova::Log("    Vec3: {}", glm::to_string(value));
                    },
                    [&](const Vec4& value) {
                        nova::Log("    Vec4: {}", glm::to_string(value));
                    },
                }, property.value);
            }
        }

        WriteHeader("Instances");

        for (auto& instance : instances) {
            nova::Log("Instance[{}]", &instance - instances.data());
            nova::Log("  Mesh[{}]", instance.mesh_idx);
            nova::Log("  Transform:");
            auto& M = instance.transform;
            nova::Log("    {:12.5f} {:12.5f} {:12.5f} {:12.5f}", M[0][0], M[1][0], M[2][0], M[3][0]);
            nova::Log("    {:12.5f} {:12.5f} {:12.5f} {:12.5f}", M[0][1], M[1][1], M[2][1], M[3][1]);
            nova::Log("    {:12.5f} {:12.5f} {:12.5f} {:12.5f}", M[0][2], M[1][2], M[2][2], M[3][2]);
        }
    }
}
