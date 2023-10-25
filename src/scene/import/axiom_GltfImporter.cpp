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

        auto addProperty = nova::Overloads {
            [&](std::string_view name, fastgltf::Optional<fastgltf::TextureInfo>& texture) {
                if (texture) outMaterial.properties.emplace_back(name, TextureSwizzle{ .textureIdx = u32(texture->textureIndex) }); },
            [&](std::string_view name, fastgltf::Optional<fastgltf::NormalTextureInfo>& texture) {
                if (texture) outMaterial.properties.emplace_back(name, TextureSwizzle{ .textureIdx = u32(texture->textureIndex) }); },
            [&](std::string_view name, Span<f32> values) {
                switch (values.size()) {
                    break;case 1: outMaterial.properties.emplace_back(name, values[0]);
                    break;case 2: outMaterial.properties.emplace_back(name, Vec2(values[0], values[1]));
                    break;case 3: outMaterial.properties.emplace_back(name, Vec3(values[0], values[1], values[2]));
                    break;case 4: outMaterial.properties.emplace_back(name, Vec4(values[0], values[1], values[2], values[3]));
                    break;default: NOVA_THROW("Invalid number of values: {}", values.size());
                }
            },
            [&](std::string_view name, f32  scalar) { outMaterial.properties.emplace_back(name, scalar); },
            [&](std::string_view name, i32  scalar) { outMaterial.properties.emplace_back(name, scalar); },
            [&](std::string_view name, bool scalar) { outMaterial.properties.emplace_back(name, scalar); },
            [&](std::string_view name, fastgltf::Optional<f32> scalar) {
                if (scalar) outMaterial.properties.emplace_back(name, scalar.value()); },
            [&](std::string_view name, fastgltf::Optional<i32> scalar) {
                if (scalar) outMaterial.properties.emplace_back(name, scalar.value()); },
            [&](std::string_view name, fastgltf::Optional<bool> scalar) {
                if (scalar) outMaterial.properties.emplace_back(name, scalar.value()); },
        };

        addProperty(property::BaseColor, inMaterial.pbrData.baseColorTexture);
        addProperty(property::BaseColor, inMaterial.pbrData.baseColorFactor);

        addProperty(property::Normal, inMaterial.normalTexture);

        addProperty(property::Emissive, inMaterial.emissiveTexture);
        addProperty(property::Emissive, inMaterial.emissiveFactor);
        addProperty(property::Emissive, inMaterial.emissiveStrength);

        addProperty(property::Metallic , inMaterial.pbrData.metallicRoughnessTexture);
        addProperty(property::Metallic , inMaterial.pbrData.metallicFactor);
        addProperty(property::Roughness, inMaterial.pbrData.metallicRoughnessTexture);
        addProperty(property::Roughness, inMaterial.pbrData.roughnessFactor);

        addProperty(property::AlphaCutoff, inMaterial.alphaCutoff);
        addProperty(property::AlphaMask  , inMaterial.alphaMode == fastgltf::AlphaMode::Mask);
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
        fastgltf::copyFromAccessor<u32>(*asset, indices, outMesh.indices.data());

        // Positions
        auto& positions = asset->accessors[primitive.findAttribute("POSITION")->second];
        outMesh.positions.resize(positions.count);
        fastgltf::copyFromAccessor<Vec3>(*asset, positions, outMesh.positions.data());

        // Normals
        if (auto normals = primitive.findAttribute("NORMAL"); normals != primitive.attributes.end()) {
            auto& accessor = asset->accessors[normals->second];
            outMesh.normals.resize(accessor.count);
            fastgltf::copyFromAccessor<Vec3>(*asset, accessor, outMesh.normals.data());
        }

        // TexCoords (0)
        if (auto texCoords = primitive.findAttribute("TEXCOORD_0"); texCoords != primitive.attributes.end()) {
            auto& accessor = asset->accessors[texCoords->second];
            outMesh.texCoords.resize(accessor.count);
            fastgltf::copyFromAccessor<Vec2>(*asset, accessor, outMesh.texCoords.data());
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