#include "axiom_Scene.hpp"

namespace axiom
{
    void scene_ir::Scene::Debug()
    {
        auto writeHeader = [&](std::string_view header) {
            NOVA_LOG("\n{:=^80}\n", std::format(" {} ", header));
        };

        writeHeader("Overview");

        NOVA_LOG("Textures = {}", textures.size());
        {
            std::unordered_set<std::string_view> uniquePaths;
            uint32_t rawDataCount = 0;
            uint32_t duplicatedIds = 0;
            for (auto& tex : textures) {
                if (auto uri = std::get_if<ImageFileURI>(&tex.data)) {
                    if (uniquePaths.contains(uri->uri)) {
                        duplicatedIds++;
                    } else {
                        uniquePaths.insert(uri->uri);
                    }
                } else {
                    rawDataCount++;
                }
            }
            NOVA_LOG("  Unique Files: {} ({} duplicates)", uniquePaths.size(), duplicatedIds);
            NOVA_LOG("  Buffers: {}", rawDataCount);
        }
        NOVA_LOG("Materials: {}", materials.size());
        NOVA_LOG("Meshes: {}", meshes.size());
        NOVA_LOG("Instances: {}", instances.size());

        writeHeader("Textures");

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
                const char* formatName = "Unknown";
                switch (buffer->format) {
                        using enum BufferFormat;
                    break;case RGBA8: formatName = "RGBA8";
                }
                NOVA_LOG(": Raw[size = ({}, {}), format = {}]", buffer->size.x, buffer->size.y, formatName);
            }
        }

        writeHeader("Materials");

        // for (auto& material: materials) {
        //     NOVA_LOG("Material[{}]", &material - materials.data());
        //     if (material.alphaBlend || material.alphaMask) {
        //         NOVA_LOG("  Alpha[blend = {}, mask = {}, cutoff = {}]",
        //             material.alphaBlend, material.alphaMask, material.alphaCutoff);
        //     }
        //     if (material.decal) {
        //         NOVA_LOG("  Decal");
        //     }
        //     if (material.volume) {
        //         NOVA_LOG("  Volume");
        //     }
        //     for (auto& channel : material.properties) {
        //         std::cout << "  Channel[" << (&channel - material.properties.data()) << "]";
        //         const char* typeName = "Unknown";
        //         switch (channel.type) {
        //                 using enum ChannelType;
        //             break;case BaseColor:        typeName = "BaseColor";
        //             break;case Alpha:            typeName = "Alpha";
        //             break;case Normal:           typeName = "Normal";
        //             break;case Emissive:         typeName = "Emissive";
        //             break;case Metalness:        typeName = "Metalness";
        //             break;case Roughness:        typeName = "Roughness";
        //             break;case Transmission:     typeName = "Transmission";
        //             break;case Subsurface:       typeName = "Subsurface";
        //             break;case SpecularColor:    typeName = "SpecularColor";
        //             break;case SpecularStrength: typeName = "SpecularStrength";
        //             break;case Specular:         typeName = "Specular";
        //             break;case Glossiness:       typeName = "Glossiness";
        //             break;case Clearcoat:        typeName = "Clearcoat";
        //             break;case Diffuse:          typeName = "Diffuse";
        //             break;case Ior:              typeName = "Ior";
        //         }
        //         std::cout << ": " << typeName << '\n';
        //         if (channel.texture.textureIdx != InvalidIndex) {
        //             std::cout << std::format("    Texture[{}]: (", channel.texture.textureIdx);
        //             for (auto& swizzle : channel.texture.channels) {
        //                 if (swizzle == -1) break;
        //                 if (&swizzle != channel.texture.channels.data())
        //                     std::cout << ", ";
        //                 std::cout << i32(swizzle);
        //             }
        //             std::cout << ")\n";
        //         }
        //         auto v = channel.value;
        //         std::cout << std::format("    Value: ({}, {}, {}, {})\n", v.r, v.g, v.b, v.a);
        //     }
        // }

        writeHeader("Instances");

        for (auto& instance : instances) {
            NOVA_LOG("Instance[{}]", &instance - instances.data());
            NOVA_LOG("  Mesh[{}]", instance.meshIdx);
            NOVA_LOG("  Transform:");
            auto& M = instance.transform;
            NOVA_LOG("    {:12.5f} {:12.5f} {:12.5f} {:12.5f}", M[0][0], M[1][0], M[2][0], M[3][0]);
            NOVA_LOG("    {:12.5f} {:12.5f} {:12.5f} {:12.5f}", M[0][1], M[1][1], M[2][1], M[3][1]);
            NOVA_LOG("    {:12.5f} {:12.5f} {:12.5f} {:12.5f}", M[0][2], M[1][2], M[2][2], M[3][2]);
        }
    }
}