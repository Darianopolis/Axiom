#include "axiom_SceneIR.hpp"

#include <axiom_Core.hpp>

namespace axiom
{
    void scene::DebugPrintScene(Scene& scene)
    {
        auto writeHeader = [&](std::string_view header) {
            NOVA_LOG("\n{:=^80}\n", std::format(" {} ", header));
        };

        writeHeader("Overview");

        NOVA_LOG("Textures = {}", scene.textures.size());
        {
            std::unordered_set<std::string_view> uniquePaths;
            uint32_t rawDataCount = 0;
            uint32_t duplicatedIds = 0;
            for (auto& tex : scene.textures) {
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
        NOVA_LOG("Materials: {}", scene.materials.size());
        NOVA_LOG("Meshes: {}", scene.meshes.size());
        NOVA_LOG("Instances: {}", scene.instances.size());

        writeHeader("Textures");

        for (auto& texture : scene.textures) {
            std::cout << "Texture[" << (&texture - scene.textures.data()) << "]";
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

        for (auto& material: scene.materials) {
            NOVA_LOG("Material[{}]", &material - scene.materials.data());
            if (material.alphaBlend || material.alphaMask) {
                NOVA_LOG("  Alpha[blend = {}, mask = {}, cutoff = {}]",
                    material.alphaBlend, material.alphaMask, material.alphaCutoff);
            }
            if (material.decal) {
                NOVA_LOG("  Decal");
            }
            if (material.volume) {
                NOVA_LOG("  Volume");
            }
            for (auto& channel : material.channels) {
                std::cout << "  Channel[" << (&channel - material.channels.data()) << "]";
                const char* typeName = "Unknown";
                switch (channel.type) {
                        using enum ChannelType;
                    break;case BaseColor:        typeName = "BaseColor";
                    break;case Alpha:            typeName = "Alpha";
                    break;case Normal:           typeName = "Normal";
                    break;case Emissive:         typeName = "Emissive";
                    break;case Metalness:        typeName = "Metalness";
                    break;case Roughness:        typeName = "Roughness";
                    break;case Transmission:     typeName = "Transmission";
                    break;case Subsurface:       typeName = "Subsurface";
                    break;case SpecularColor:    typeName = "SpecularColor";
                    break;case SpecularStrength: typeName = "SpecularStrength";
                    break;case Specular:         typeName = "Specular";
                    break;case Glossiness:       typeName = "Glossiness";
                    break;case Clearcoat:        typeName = "Clearcoat";
                    break;case Diffuse:          typeName = "Diffuse";
                    break;case Ior:              typeName = "Ior";
                }
                std::cout << ": " << typeName << '\n';
                if (channel.texture.textureIdx != InvalidIndex) {
                    std::cout << std::format("    Texture[{}]: (", channel.texture.textureIdx);
                    for (auto& swizzle : channel.texture.channels) {
                        if (swizzle == -1) break;
                        if (&swizzle != channel.texture.channels.data())
                            std::cout << ", ";
                        std::cout << i32(swizzle);
                    }
                    std::cout << ")\n";
                }
                auto v = channel.value;
                std::cout << std::format("    Value: ({}, {}, {}, {})\n", v.r, v.g, v.b, v.a);
            }
        }

        writeHeader("Materials");

        for (auto& instance : scene.instances) {
            NOVA_LOG("Instance[{}]", &instance - scene.instances.data());
            NOVA_LOG("  Mesh[{}]", instance.meshIdx);
            NOVA_LOG("  Transform:");
            auto& M = instance.transform;
            NOVA_LOG("    {:12.5f} {:12.5f} {:12.5f} {:12.5f}", M[0][0], M[1][0], M[2][0], M[3][0]);
            NOVA_LOG("    {:12.5f} {:12.5f} {:12.5f} {:12.5f}", M[0][1], M[1][1], M[2][1], M[3][1]);
            NOVA_LOG("    {:12.5f} {:12.5f} {:12.5f} {:12.5f}", M[0][2], M[1][2], M[2][2], M[3][2]);
        }
    }

    void scene::BuildScene(Scene& inScene, LoadableScene& outScene, const SceneProcessSettings& settings)
    {
        auto defaultMaterial = Ref<UVMaterial>::Create();
        outScene.materials.push_back(defaultMaterial);

        // BaseColor + Alpha = BC7
        // Normals           = BC5
        // Metal     + Rough = BC5
        // Emissivity        = BC6h
        // Transmission      = BC4

        u32 textureOffset = u32(outScene.textures.size());

        outScene.textures.resize(textureOffset + inScene.textures.size());
#pragma omp parallel for
        for (u32 i = 0; i < inScene.textures.size(); ++i) {
            auto& inTexture = inScene.textures[i];
            auto outTexture = Ref<UVTexture>::Create();
            outScene.textures[i] = outTexture;

            ImageProcess processes = {};
            if (settings.flipNormalMapZ) {
                for (auto& material : inScene.materials) {
                    if (material.GetChannel(ChannelType::Normal)->texture.textureIdx == i) {
                        processes |= ImageProcess::FlipNrmZ;
                        break;
                    }
                }
            }

            if (auto uri = std::get_if<ImageFileURI>(&inTexture.data)) {
                s_ImageProcessor.ProcessImage(uri->uri.c_str(), 0, ImageType::ColorAlpha, 4096, processes);
            } else if (auto file = std::get_if<ImageFileBuffer>(&inTexture.data)) {
                s_ImageProcessor.ProcessImage((const char*)file->data.data(), file->data.size(), ImageType::ColorAlpha, 4096, {});
            } else if (auto buffer = std::get_if<ImageBuffer>(&inTexture.data)) {
                NOVA_THROW("Buffer data source not currently supported");
            }

            outTexture->data.resize(s_ImageProcessor.GetImageDataSize());
            std::memcpy(outTexture->data.data(), s_ImageProcessor.GetImageData(), outTexture->data.size());
            outTexture->size = s_ImageProcessor.GetImageDimensions();
            outTexture->minAlpha = s_ImageProcessor.GetMinAlpha();
            outTexture->maxAlpha = s_ImageProcessor.GetMaxAlpha();
        }

        defaultMaterial->baseColor_alpha = Ref<UVTexture>::Create();
        defaultMaterial->baseColor_alpha->size = Vec2(1);
        defaultMaterial->baseColor_alpha->data = { b8(255), b8(0), b8(255), b8(255) };

        defaultMaterial->normals = Ref<UVTexture>::Create();
        defaultMaterial->normals->size = Vec2(1);
        defaultMaterial->normals->data = { b8(127), b8(127), b8(255), b8(255) };

        defaultMaterial->metalness_roughness = Ref<UVTexture>::Create();
        defaultMaterial->metalness_roughness->size = Vec2(1);
        defaultMaterial->metalness_roughness->data = { b8(0), b8(127), b8(0), b8(255) };

        defaultMaterial->emissivity = Ref<UVTexture>::Create();
        defaultMaterial->emissivity->size = Vec2(1);
        defaultMaterial->emissivity->data = { b8(0), b8(0), b8(0), b8(255) };

        defaultMaterial->transmission = Ref<UVTexture>::Create();
        defaultMaterial->transmission->size = Vec2(1);
        defaultMaterial->transmission->data = { b8(0), b8(0), b8(0), b8(255) };

        u32 defaultBaseColorAlpha = u32(outScene.textures.size());
        outScene.textures.emplace_back(defaultMaterial->baseColor_alpha);
        u32 defaultNormal = u32(outScene.textures.size());
        outScene.textures.emplace_back(defaultMaterial->normals);
        u32 defaultMetalnessRoughness = u32(outScene.textures.size());
        outScene.textures.emplace_back(defaultMaterial->metalness_roughness);
        u32 defaultEmissivity = u32(outScene.textures.size());
        outScene.textures.emplace_back(defaultMaterial->emissivity);
        u32 defaultTransmission = u32(outScene.textures.size());
        outScene.textures.emplace_back(defaultMaterial->transmission);

        nova::HashMap<u32, u32> singlePixelTextures;
        auto getImage = [&](Channel* channel, u32 fallback) {

            if (!channel) {
                // NOVA_LOG("MISSING CHANNEL - FALLBACK");
                return outScene.textures[fallback];
            }

            if (channel->texture.textureIdx != InvalidIndex) {
                // NOVA_LOG("Using texture");
                return outScene.textures[textureOffset + channel->texture.textureIdx];
            }

            // NOVA_LOG("Using fallback!");

            std::array<u8, 4> data {
                u8(channel->value.r * 255.f),
                u8(channel->value.g * 255.f),
                u8(channel->value.b * 255.f),
                u8(channel->value.a * 255.f),
            };

            u32 encoded = std::bit_cast<u32>(data);

            if (singlePixelTextures.contains(encoded)) {
                return outScene.textures[singlePixelTextures.at(encoded)];
            }

            auto image = Ref<UVTexture>::Create();
            image->size = Vec2(1);
            image->data = { b8(data[0]), b8(data[1]), b8(data[2]), b8(data[3]) };

            outScene.textures.push_back(image);
            singlePixelTextures.insert({ encoded, u32(outScene.textures.size() - 1) });

            return image;
        };

        u32 materialOffset = u32(outScene.materials.size());
        for (auto& inMaterial : inScene.materials) {
            auto outMaterial = Ref<UVMaterial>::Create();
            outScene.materials.push_back(outMaterial);

            auto* baseColor = inMaterial.GetChannel(ChannelType::BaseColor);
            [[maybe_unused]] auto* alpha = inMaterial.GetChannel(ChannelType::Alpha);
            outMaterial->baseColor_alpha = getImage(baseColor, defaultBaseColorAlpha);

            auto* normal = inMaterial.GetChannel(ChannelType::Normal);
            outMaterial->normals = getImage(normal, defaultNormal);

            auto* metalness = inMaterial.GetChannel(ChannelType::Metalness);
            [[maybe_unused]] auto* roughness = inMaterial.GetChannel(ChannelType::Roughness);
            outMaterial->metalness_roughness = getImage(metalness, defaultMetalnessRoughness);

            auto* emissivity = inMaterial.GetChannel(ChannelType::Emissive);
            outMaterial->emissivity = getImage(emissivity, defaultEmissivity);

            auto* transmission = inMaterial.GetChannel(ChannelType::Transmission);
            outMaterial->transmission = getImage(transmission, defaultTransmission);

            outMaterial->alphaBlend = inMaterial.alphaBlend;
            outMaterial->alphaMask = inMaterial.alphaMask || (
                baseColor->texture.textureIdx != InvalidIndex
                && outScene.textures[baseColor->texture.textureIdx]->minAlpha < inMaterial.alphaCutoff);
            outMaterial->alphaCutoff = inMaterial.alphaCutoff;
            outMaterial->decal = inMaterial.decal;
        }

        u32 meshOffset = u32(outScene.meshes.size());
        for (auto& inMesh : inScene.meshes) {
            auto outMesh = Ref<TriMesh>::Create();
            outScene.meshes.push_back(outMesh);

            outMesh->positionAttributes.resize(inMesh.positions.size());
            std::memcpy(outMesh->positionAttributes.data(),
                inMesh.positions.data(), inMesh.positions.size() * sizeof(Vec3));

            outMesh->shadingAttributes.resize(inMesh.positions.size());
            for (u32 i : inMesh.indices) outMesh->indices.push_back(i);

            usz vertexCount = outMesh->positionAttributes.size();
            usz indexCount = outMesh->indices.size();

            s_MeshProcessor.flipUVs = settings.flipUVs;
            s_MeshProcessor.ProcessMesh(
                { &outMesh->positionAttributes[0], sizeof(outMesh->positionAttributes[0]), vertexCount },
                !inMesh.normals.empty()
                    ? InStridedRegion{ &inMesh.normals[0], sizeof(inMesh.normals[0]), vertexCount }
                    : InStridedRegion{},
                !inMesh.texCoords.empty()
                    ? InStridedRegion{ &inMesh.texCoords[0], sizeof(inMesh.texCoords[0]), vertexCount }
                    : InStridedRegion{},
                { &outMesh->indices[0], sizeof(outMesh->indices[0]), indexCount },
                { &outMesh->shadingAttributes[0].tangentSpace, sizeof(outMesh->shadingAttributes[0]), vertexCount },
                { &outMesh->shadingAttributes[0].texCoords, sizeof(outMesh->shadingAttributes[0]), vertexCount });

            outMesh->subMeshes.push_back(TriSubMesh {
                .vertexOffset = 0,
                .maxVertex = u32(inMesh.positions.size() - 1),
                .firstIndex = 0,
                .indexCount = u32(inMesh.indices.size()),
                .material = inMesh.materialIdx == InvalidIndex
                    ? outScene.materials[materialOffset - 1]
                    : outScene.materials[materialOffset + inMesh.materialIdx],
            });
        }

        for (auto& inInstance : inScene.instances) {
            auto outInstance = Ref<TriMeshInstance>::Create();
            outScene.instances.push_back(outInstance);

            outInstance->mesh = outScene.meshes[meshOffset + inInstance.meshIdx];
            outInstance->transform = inInstance.transform;
        }
    }
}