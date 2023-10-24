#include "axiom_SceneCompiler.hpp"

namespace axiom
{
    void SceneCompiler::Compile(Scene& inScene, CompiledScene& outScene)
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
            if (flipNormalMapZ) {
                for (auto& material : inScene.materials) {
                    // TODO: This should follow the mapping process
                    if (auto p = material.GetProperty<TextureSwizzle>(property::Normal); p && p->textureIdx == i) {
                        processes |= ImageProcess::FlipNrmZ;
                        break;
                    }
                }
            }

            // constexpr u32 MaxDim = 512;
            constexpr u32 MaxDim = 4096;

            if (auto uri = std::get_if<ImageFileURI>(&inTexture.data)) {
                auto path = std::filesystem::path(uri->uri);
                if (path.extension() == ".dds") {
                    path.replace_extension(".png");
                }
                if (!std::filesystem::exists(path)) {
                    NOVA_LOG("Cannot find file: {}", path.string());
                    continue;
                }
                path = std::filesystem::canonical(path);
                s_ImageProcessor.ProcessImage(path.string().c_str(), 0, ImageType::ColorAlpha, MaxDim, processes);
            } else if (auto file = std::get_if<ImageFileBuffer>(&inTexture.data)) {
                s_ImageProcessor.ProcessImage((const char*)file->data.data(), file->data.size(), ImageType::ColorAlpha, MaxDim, {});
            } else if (auto buffer = std::get_if<ImageBuffer>(&inTexture.data)) {
                NOVA_THROW("Buffer data source not currently supported");
            }

            outTexture->data.resize(s_ImageProcessor.GetImageDataSize());
            std::memcpy(outTexture->data.data(), s_ImageProcessor.GetImageData(), outTexture->data.size());
            outTexture->size = s_ImageProcessor.GetImageDimensions();
            outTexture->minAlpha = s_ImageProcessor.GetMinAlpha();
            outTexture->maxAlpha = s_ImageProcessor.GetMaxAlpha();
            outTexture->format = s_ImageProcessor.GetImageFormat();
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

        u32 materialOffset = u32(outScene.materials.size());
        for (auto& inMaterial : inScene.materials) {
            auto outMaterial = Ref<UVMaterial>::Create();
            outScene.materials.push_back(outMaterial);

            auto getImage = [&](std::string_view property, u32 fallback) {

                auto* texture = inMaterial.GetProperty<TextureSwizzle>(property);

                if (texture) {
                    auto tex = outScene.textures[textureOffset + texture->textureIdx];
                    if (tex->data.size()) {
                        return tex;
                    }
                }

                // NOVA_LOG("Using fallback!");

                std::array<u8, 4> data {
                    0, 0, 0, 255,
                    // u8(channel->value.r * 255.f),
                    // u8(channel->value.g * 255.f),
                    // u8(channel->value.b * 255.f),
                    // u8(channel->value.a * 255.f),
                };

                if (Vec4* v4 = inMaterial.GetProperty<Vec4>(property)) {
                    data[0] = u8(v4->r * 255.f);
                    data[1] = u8(v4->g * 255.f);
                    data[2] = u8(v4->b * 255.f);
                    data[3] = u8(v4->a * 255.f);
                } else if (Vec3* v3 = inMaterial.GetProperty<Vec3>(property)) {
                    data[0] = u8(v3->r * 255.f);
                    data[1] = u8(v3->g * 255.f);
                    data[2] = u8(v3->b * 255.f);
                } else if (Vec2* v2 = inMaterial.GetProperty<Vec2>(property)) {
                    data[0] = u8(v2->r * 255.f);
                    data[1] = u8(v2->g * 255.f);
                } else if (f32* v = inMaterial.GetProperty<f32>(property)) {
                    data[0] = u8(*v * 255.f);
                } else {
                    return outScene.textures[fallback];
                }

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

            // TODO: Channel remapping!

            outMaterial->baseColor_alpha = getImage(property::BaseColor, defaultBaseColorAlpha);
            outMaterial->normals = getImage(property::Normal, defaultNormal);
            outMaterial->metalness_roughness = getImage(property::Metallic, defaultMetalnessRoughness);
            outMaterial->emissivity = getImage(property::Emissive, defaultEmissivity);
            outMaterial->transmission = getImage("", defaultTransmission);

            outMaterial->alphaCutoff = [](f32*v){return v?*v:0.5f;}(inMaterial.GetProperty<f32>(property::AlphaCutoff));

            // outMaterial->alphaBlend = inMaterial.alphaBlend;
            outMaterial->alphaMask = inMaterial.GetProperty<bool>(property::AlphaMask) ||
                outMaterial->baseColor_alpha->minAlpha < outMaterial->alphaCutoff;
            // outMaterial->decal = inMaterial.decal;
        }

        std::to_chars(nullptr, nullptr, 1234);

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

            s_MeshProcessor.flipUVs = flipUVs;
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