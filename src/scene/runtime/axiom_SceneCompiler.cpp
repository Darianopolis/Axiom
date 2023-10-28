#include "axiom_SceneCompiler.hpp"

namespace axiom
{
    void SceneCompiler::Compile(scene_ir::Scene& inScene, CompiledScene& outScene)
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
            outScene.textures[textureOffset + i] = outTexture;

            ImageProcess processes = {};
            if (flipNormalMapZ) {
                for (auto& material : inScene.materials) {
                    // TODO: This should follow the mapping process
                    if (auto p = material.GetProperty<scene_ir::TextureSwizzle>(scene_ir::property::Normal); p && p->textureIdx == i) {
                        processes |= ImageProcess::FlipNrmZ;
                        break;
                    }
                }
            }

            // constexpr u32 MaxDim = 512;
            constexpr u32 MaxDim = 4096;

            if (auto uri = std::get_if<scene_ir::ImageFileURI>(&inTexture.data)) {
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
            } else if (auto file = std::get_if<scene_ir::ImageFileBuffer>(&inTexture.data)) {
                s_ImageProcessor.ProcessImage((const char*)file->data.data(), file->data.size(), ImageType::ColorAlpha, MaxDim, {});
            } else if (auto buffer = std::get_if<scene_ir::ImageBuffer>(&inTexture.data)) {
                NOVA_THROW("Buffer data source not currently supported");
            }

            outTexture->data.resize(s_ImageProcessor.GetImageDataSize());
            std::memcpy(outTexture->data.data(), s_ImageProcessor.GetImageData(), outTexture->data.size());
            outTexture->size = s_ImageProcessor.GetImageDimensions();
            outTexture->minAlpha = s_ImageProcessor.GetMinAlpha();
            outTexture->maxAlpha = s_ImageProcessor.GetMaxAlpha();
            outTexture->format = s_ImageProcessor.GetImageFormat();
        }

        nova::HashMap<u32, u32> singlePixelTextures;

        auto createPixelImage = [&](Vec4 value) {
            std::array<u8, 4> data {
                u8(value.r * 255.f),
                u8(value.g * 255.f),
                u8(value.b * 255.f),
                u8(value.a * 255.f),
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

        defaultMaterial->baseColor_alpha = createPixelImage({ 1.f, 0.f, 1.f, 1.f });
        defaultMaterial->normals = createPixelImage({ 0.5f, 0.5f, 1.f, 1.f });
        defaultMaterial->metalness_roughness = createPixelImage({ 0.f, 0.5f, 0.f, 1.f });
        defaultMaterial->emissivity = createPixelImage({ 0.f, 0.f, 0.f, 1.f });
        defaultMaterial->transmission = createPixelImage({ 0.f, 0.f, 0.f, 255.f });

        auto totalBaseColor = 0;

        u32 materialOffset = u32(outScene.materials.size());
        for (auto& inMaterial : inScene.materials) {
            auto outMaterial = Ref<UVMaterial>::Create();
            outScene.materials.push_back(outMaterial);

            auto getImage = [&](std::string_view property, nova::types::Ref<UVTexture> fallback) {

                auto* texture = inMaterial.GetProperty<scene_ir::TextureSwizzle>(property);

                if (texture) {
                    auto tex = outScene.textures[textureOffset + texture->textureIdx];
                    if (tex->data.size()) {
                        if (property == scene_ir::property::BaseColor) {
                            totalBaseColor++;
                        }
                        return tex;
                    }
                }

                // NOVA_LOG("Using fallback!");

                Vec4 data;

                if (Vec4* v4 = inMaterial.GetProperty<Vec4>(property)) {
                    data = *v4;
                } else if (Vec3* v3 = inMaterial.GetProperty<Vec3>(property)) {
                    data = Vec4(*v3, 1.f);
                } else if (Vec2* v2 = inMaterial.GetProperty<Vec2>(property)) {
                    data = Vec4(*v2, 0.f, 1.f);
                } else if (f32* v = inMaterial.GetProperty<f32>(property)) {
                    data = Vec4(*v, *v, *v, 1.f);
                } else {
                    return fallback;
                }

                return createPixelImage(data);
            };

            // TODO: Channel remapping!

            outMaterial->baseColor_alpha = getImage(scene_ir::property::BaseColor, defaultMaterial->baseColor_alpha);
            outMaterial->normals = getImage(scene_ir::property::Normal, defaultMaterial->normals);
            {
                if (auto* tex = inMaterial.GetProperty<scene_ir::TextureSwizzle>(scene_ir::property::Metallic);
                        tex && outScene.textures[textureOffset + tex->textureIdx]->data.size()) {
                    // TODO: Fixme
                    outMaterial->metalness_roughness = outScene.textures[textureOffset + tex->textureIdx];
                } else if (tex = inMaterial.GetProperty<scene_ir::TextureSwizzle>(scene_ir::property::SpecularColor);
                        tex && outScene.textures[textureOffset + tex->textureIdx]->data.size()) {
                    // TODO: Fixme
                    outMaterial->metalness_roughness = outScene.textures[textureOffset + tex->textureIdx];
                } else {
                    auto* _metalness = inMaterial.GetProperty<f32>(scene_ir::property::Metallic);
                    auto* _roughness = inMaterial.GetProperty<f32>(scene_ir::property::Roughness);

                    f32 metalness = _metalness ? *_metalness : 0.f;
                    f32 roughness = _roughness ? *_roughness : 0.5f;

                    outMaterial->metalness_roughness = createPixelImage({ 0.f, roughness, metalness, 1.f });
                }
            }
            outMaterial->emissivity = getImage(scene_ir::property::Emissive, defaultMaterial->emissivity);
            outMaterial->transmission = getImage("", defaultMaterial->transmission);

            outMaterial->alphaCutoff = [](f32*v){return v?*v:0.5f;}(inMaterial.GetProperty<f32>(scene_ir::property::AlphaCutoff));

            // outMaterial->alphaBlend = inMaterial.alphaBlend;
            outMaterial->alphaMask = inMaterial.GetProperty<bool>(scene_ir::property::AlphaMask) ||
                outMaterial->baseColor_alpha->minAlpha < outMaterial->alphaCutoff;
            // outMaterial->decal = inMaterial.decal;
        }

        NOVA_LOGEXPR(totalBaseColor);

        u64 totalTangentSpaces = 0;
        ankerl::unordered_dense::set<u64> uniqueTangentSpace;

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

            for (u32 i = 0; i < vertexCount; ++i) {
                totalTangentSpaces++;
                std::pair<u32, u32> ts;
                ts.first = std::bit_cast<u32>(outMesh->shadingAttributes[i].tangentSpace);
                ts.second = std::bit_cast<u32>(outMesh->shadingAttributes[i].texCoords);
                uniqueTangentSpace.insert(std::bit_cast<u64>(ts));
            }

            outMesh->subMeshes.push_back(TriSubMesh {
                .vertexOffset = 0,
                .maxVertex = u32(inMesh.positions.size() - 1),
                .firstIndex = 0,
                .indexCount = u32(inMesh.indices.size()),
                .material = inMesh.materialIdx == scene_ir::InvalidIndex
                    ? outScene.materials[materialOffset - 1]
                    : outScene.materials[materialOffset + inMesh.materialIdx],
            });
        }

        NOVA_LOG("Unique shading attributes: {} / {} ({:.2f}%)", uniqueTangentSpace.size(), totalTangentSpaces, (100.0 * uniqueTangentSpace.size()) / totalTangentSpaces);

        for (auto& inInstance : inScene.instances) {
            auto outInstance = Ref<TriMeshInstance>::Create();
            outScene.instances.push_back(outInstance);

            outInstance->mesh = outScene.meshes[meshOffset + inInstance.meshIdx];
            outInstance->transform = inInstance.transform;
        }
    }
}