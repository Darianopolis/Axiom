#include "axiom_Scene.hpp"

namespace axiom
{
    void scene_ir::Scene::Debug()
    {
        auto WriteHeader = [&](std::string_view header) {
            NOVA_LOG("\n{:=^80}\n", std::format(" {} ", header));
        };

        WriteHeader("Overview");

        NOVA_LOG("Textures = {}", textures.size());
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
            NOVA_LOG("  Unique Files: {} ({} duplicates)", unique_paths.size(), duplicated_ids);
            NOVA_LOG("  Buffers: {}", raw_data_count);
        }
        NOVA_LOG("Materials: {}", materials.size());
        NOVA_LOG("Meshes: {}", meshes.size());
        NOVA_LOG("Instances: {}", instances.size());

        WriteHeader("Textures");

        for (auto& texture : textures) {
            std::cout << "Texture[" << (&texture - textures.data()) << "]";
            if (auto uri = std::get_if<ImageFileURI>(&texture.data)) {
                NOVA_LOG(": File[{}]", uri->uri);
            } else if (auto file = std::get_if<ImageFileBuffer>(&texture.data)) {
                NOVA_LOG(": InlineFile[magic = {}|{:#x}, size = {}]",
                    std::string_view(reinterpret_cast<char*>(file->data.data())).substr(0, 4),
                    *reinterpret_cast<uint32_t*>(file->data.data()),
                    file->data.size());
            } else if (auto buffer = std::get_if<ImageBuffer>(&texture.data)) {
                const char* format_name = "Unknown";
                switch (buffer->format) {
                        using enum BufferFormat;
                    break;case RGBA8: format_name = "RGBA8";
                }
                NOVA_LOG(": Raw[size = ({}, {}), format = {}]", buffer->size.x, buffer->size.y, format_name);
            }
        }

        WriteHeader("Materials");

        for (auto& material: materials) {
            NOVA_LOG("Material[{}]", &material - materials.data());
            for (auto& property : material.properties) {
                NOVA_LOG("  {}:", property.name);
                std::visit(nova::Overloads {
                    [&](const TextureSwizzle& value) {
                        NOVA_LOG("    Texture: {}", value.texture_idx);
                    },
                    [&](const bool& value) {
                        NOVA_LOG("    Bool: {}", value);
                    },
                    [&](const i32& value) {
                        NOVA_LOG("    Int: {}", value);
                    },
                    [&](const f32& value) {
                        NOVA_LOG("    Float: {}", value);
                    },
                    [&](const Vec2& value) {
                        NOVA_LOG("    Vec2: {}", glm::to_string(value));
                    },
                    [&](const Vec3& value) {
                        NOVA_LOG("    Vec3: {}", glm::to_string(value));
                    },
                    [&](const Vec4& value) {
                        NOVA_LOG("    Vec4: {}", glm::to_string(value));
                    },
                }, property.value);
            }
        }

        WriteHeader("Instances");

        for (auto& instance : instances) {
            NOVA_LOG("Instance[{}]", &instance - instances.data());
            NOVA_LOG("  Mesh[{}]", instance.mesh_idx);
            NOVA_LOG("  Transform:");
            auto& M = instance.transform;
            NOVA_LOG("    {:12.5f} {:12.5f} {:12.5f} {:12.5f}", M[0][0], M[1][0], M[2][0], M[3][0]);
            NOVA_LOG("    {:12.5f} {:12.5f} {:12.5f} {:12.5f}", M[0][1], M[1][1], M[2][1], M[3][1]);
            NOVA_LOG("    {:12.5f} {:12.5f} {:12.5f} {:12.5f}", M[0][2], M[1][2], M[2][2], M[3][2]);
        }
    }
}