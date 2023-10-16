#include "axiom_GltfImporter.hpp"

#include <fastgltf/parser.hpp>
#include <fastgltf/glm_element_traits.hpp>

namespace axiom
{
    void GltfImporter::Reset()
    {
        gltfMeshOffsets.clear();
        scene.Clear();
    }

    Scene GltfImporter::Import(const std::filesystem::path& path)
    {
        Reset();
        dir = path.parent_path();

        fastgltf::Parser parser {
              fastgltf::Extensions::KHR_texture_transform
            | fastgltf::Extensions::KHR_texture_basisu
            | fastgltf::Extensions::MSFT_texture_dds
            | fastgltf::Extensions::KHR_mesh_quantization
            | fastgltf::Extensions::EXT_meshopt_compression
            | fastgltf::Extensions::KHR_lights_punctual
            | fastgltf::Extensions::EXT_texture_webp
            | fastgltf::Extensions::KHR_materials_specular
            | fastgltf::Extensions::KHR_materials_ior
            | fastgltf::Extensions::KHR_materials_iridescence
            | fastgltf::Extensions::KHR_materials_volume
            | fastgltf::Extensions::KHR_materials_transmission
            | fastgltf::Extensions::KHR_materials_clearcoat
            | fastgltf::Extensions::KHR_materials_emissive_strength
            | fastgltf::Extensions::KHR_materials_sheen
            | fastgltf::Extensions::KHR_materials_unlit
        };

        fastgltf::GltfDataBuffer data;
        data.loadFromFile(path);

        constexpr auto GltfOptions =
              fastgltf::Options::DontRequireValidAssetMember
            | fastgltf::Options::AllowDouble
            | fastgltf::Options::LoadGLBBuffers
            | fastgltf::Options::LoadExternalBuffers;

        auto type = fastgltf::determineGltfFileType(&data);

        auto res = type == fastgltf::GltfType::glTF
            ? parser.loadGLTF(&data, dir, GltfOptions)
            : parser.loadBinaryGLTF(&data, dir, GltfOptions);

        if (!res) {
            NOVA_THROW("Error loading [{}] Message: {}", path.string(), fastgltf::getErrorMessage(res.error()));
        }

        asset = std::make_unique<fastgltf::Asset>(std::move(res.get()));

        // Textures

        scene.textures.resize(asset->textures.size());
        for (u32 i = 0; i < asset->textures.size(); ++i) {
            ProcessTexture(i);
        }

        // Materials

        scene.materials.resize(asset->materials.size());
        for (u32 i = 0; i < asset->materials.size(); ++i) {
            ProcessMaterial(i);
        }

        // Meshes

        gltfMeshOffsets.resize(asset->meshes.size());
        for (u32 i = 0; i < asset->meshes.size(); ++i) {
            gltfMeshOffsets[i].first = u32(scene.meshes.size());
            gltfMeshOffsets[i].second = u32(asset->meshes[i].primitives.size());
            for (u32 j = 0; j < asset->meshes[i].primitives.size(); ++j) {
                ProcessMesh(i, j);
            }
        }

        // Instances

        for (auto rootNodeIndex : asset->scenes[asset->defaultScene.value()].nodeIndices) {
            ProcessNode(rootNodeIndex, Mat4(1.f));
        }

        return std::move(scene);
    }

    void GltfImporter::ProcessTexture(u32 texIdx)
    {
        auto& inTexture = asset->textures[texIdx];
        auto& outTexture = scene.textures[texIdx];

        if (inTexture.imageIndex) {
            auto& image = asset->images[inTexture.imageIndex.value()];

            std::visit(nova::Overloads {
                [&](fastgltf::sources::URI& uri) {
                    outTexture.data = ImageFileURI(std::format("{}/{}", dir.string(), uri.uri.path()));
                },
                [&](fastgltf::sources::Vector& vec) {
                    ImageFileBuffer source;
                    source.data.resize(vec.bytes.size());
                    std::memcpy(source.data.data(), vec.bytes.data(), vec.bytes.size());
                    outTexture.data = std::move(source);
                },
                [&](fastgltf::sources::ByteView& byteView) {
                    ImageFileBuffer source;
                    source.data.resize(byteView.bytes.size());
                    std::memcpy(source.data.data(), byteView.bytes.data(), byteView.bytes.size());
                    outTexture.data = std::move(source);
                },
                [&](fastgltf::sources::BufferView& bufferViewIdx) {
                    auto& view = asset->bufferViews[bufferViewIdx.bufferViewIndex];
                    auto& buffer = asset->buffers[view.bufferIndex];
                    auto* bytes = fastgltf::DefaultBufferDataAdapter{}(buffer) + view.byteOffset;
                    ImageFileBuffer source;
                    source.data.resize(view.byteLength);
                    std::memcpy(source.data.data(), bytes, view.byteLength);
                    outTexture.data = std::move(source);
                },
                [&](auto&) {
                    NOVA_THROW("Unknown image source: {}", image.data.index());
                },
            }, asset->images[inTexture.imageIndex.value()].data);
        } else {
            outTexture.data = ImageBuffer{
                .data{ 255, 0, 255, 255 },
                .size{ 1, 1 },
                .format = BufferFormat::RGBA8,
            };
        }
    }

    void GltfImporter::ProcessMaterial(u32 matIdx)
    {
        auto& inMaterial = asset->materials[matIdx];
        auto& outMaterial = scene.materials[matIdx];

        auto addChannel = [&](
                ChannelType type,
                fastgltf::Optional<fastgltf::TextureInfo>& texture,
                Span<i8> channels,
                Span<f32> value) {
            Channel channel{ type };
            if (texture) {
                channel.texture.textureIdx = u32(texture->textureIndex);
                for (u32 i = 0; i < channels.size(); ++i)
                    channel.texture.channels[i] = channels[i];
            }

            for (u32 i = 0; i < value.size(); ++i)
                    channel.value[i] = value[i];

            outMaterial.channels.emplace_back(std::move(channel));
        };

        auto sum = [&](Span<f32> values) {
            f32 total = 0.f;
            for (f32 v : values) total += v;
            return total;
        };

        addChannel(ChannelType::BaseColor, inMaterial.pbrData.baseColorTexture, { 0, 1, 2 }, inMaterial.pbrData.baseColorFactor);
        if (inMaterial.normalTexture)
            addChannel(ChannelType::Normal, inMaterial.normalTexture, { 0, 1, 2 }, {});
        if (inMaterial.emissiveTexture || sum(inMaterial.emissiveFactor) > 0.f)
            addChannel(ChannelType::Emissive, inMaterial.emissiveTexture, { 0, 1, 2 }, inMaterial.emissiveFactor);
        addChannel(ChannelType::Metalness, inMaterial.pbrData.metallicRoughnessTexture, { 2 }, { inMaterial.pbrData.metallicFactor });
        addChannel(ChannelType::Roughness, inMaterial.pbrData.metallicRoughnessTexture, { 1 }, { inMaterial.pbrData.roughnessFactor });

        outMaterial.alphaCutoff = inMaterial.alphaCutoff;
        switch (inMaterial.alphaMode) {
            using enum fastgltf::AlphaMode;
            break;case Blend: outMaterial.alphaBlend = true;
            break;case  Mask: outMaterial.alphaMask  = true;
        }
    }

    void GltfImporter::ProcessMesh(u32 gltfMeshIdx, u32 primitiveIndex)
    {
        auto& primitive = asset->meshes[gltfMeshIdx].primitives[primitiveIndex];

        if (asset->meshes[gltfMeshIdx].name.contains("decal")) {
            return;
        }

        if (!primitive.indicesAccessor.has_value()
                || primitive.findAttribute("POSITION") == primitive.attributes.end())
            return;

        auto& outMesh = scene.meshes.emplace_back();
        outMesh.materialIdx = u32(primitive.materialIndex.value_or(InvalidIndex));

        // Indices
        auto& indices = asset->accessors[primitive.indicesAccessor.value()];
        outMesh.indices.resize(indices.count);
        fastgltf::iterateAccessorWithIndex<u32>(*asset, indices,
            [&](u32 vIndex, usz iIndex) { outMesh.indices[iIndex] = vIndex; });

        // Positions
        auto& positions = asset->accessors[primitive.findAttribute("POSITION")->second];
        outMesh.positions.resize(positions.count);
        fastgltf::iterateAccessorWithIndex<Vec3>(*asset, positions,
            [&](Vec3 pos, usz index) { outMesh.positions[index] = pos; });

        // Normals
        if (auto normals = primitive.findAttribute("NORMAL"); normals != primitive.attributes.end()) {
            auto& accessor = asset->accessors[normals->second];
            outMesh.normals.resize(accessor.count);
            fastgltf::iterateAccessorWithIndex<Vec3>(*asset, accessor,
                [&](Vec3 normal, usz i) { outMesh.normals[i] = normal; });
        }

        // TexCoords (0)
        if (auto texCoords = primitive.findAttribute("TEXCOORD_0"); texCoords != primitive.attributes.end()) {
            auto& accessor = asset->accessors[texCoords->second];
            outMesh.texCoords.resize(accessor.count);
            fastgltf::iterateAccessorWithIndex<Vec2>(*asset, accessor,
                [&](Vec2 texCoord, usz i) { outMesh.texCoords[i] = texCoord; });
        }
    }

    void GltfImporter::ProcessNode(usz nodeIdx, Mat4 parentTransform)
    {
        auto& node = asset->nodes[nodeIdx];

        Mat4 transform = Mat4(1.f);
        if (auto trs = std::get_if<fastgltf::Node::TRS>(&node.transform)) {
            auto translation = std::bit_cast<Vec3>(trs->translation);
            auto rotation = Quat(trs->rotation[3], trs->rotation[0], trs->rotation[1], trs->rotation[2]);
            auto scale = std::bit_cast<glm::vec3>(trs->scale);
            transform = glm::translate(Mat4(1.f), translation)
                * glm::mat4_cast(rotation)
                * glm::scale(Mat4(1.f), scale);
        } else if (auto m = std::get_if<fastgltf::Node::TransformMatrix>(&node.transform)) {
            transform = std::bit_cast<Mat4>(*m);
        }

        transform = parentTransform * transform;

        if (node.meshIndex.has_value()) {
            auto[meshIdx, meshCount] = gltfMeshOffsets[node.meshIndex.value()];
            for (u32 i = 0; i < meshCount; ++i) {
                scene.instances.emplace_back(Instance {
                    .meshIdx = meshIdx + i,
                    .transform = transform,
                });
            }
        }

        for (auto childIdx : node.children) {
            ProcessNode(childIdx, transform);
        }
    }
}