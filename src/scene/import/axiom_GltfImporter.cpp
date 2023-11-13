#include "axiom_GltfImporter.hpp"

#include <nova/core/nova_Containers.hpp>

#include <fastgltf/parser.hpp>
#include <fastgltf/glm_element_traits.hpp>

namespace axiom
{
    void GltfImporter::Reset()
    {
        gltf_mesh_offsets.clear();
        scene.Clear();
    }

    scene_ir::Scene GltfImporter::Import(const std::filesystem::path& path)
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

        {
            NOVA_LOG("Validating...");
            auto error = fastgltf::validate(*asset);
            if (error != fastgltf::Error::None) {
                NOVA_THROW("Validation error loading [{}] - {}", path.string(), fastgltf::getErrorMessage(error));
            }
            NOVA_LOG("passed validation...");
        }

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

        gltf_mesh_offsets.resize(asset->meshes.size());
        for (u32 i = 0; i < asset->meshes.size(); ++i) {
            gltf_mesh_offsets[i].first = u32(scene.meshes.size());
            gltf_mesh_offsets[i].second = u32(asset->meshes[i].primitives.size());
            for (u32 j = 0; j < asset->meshes[i].primitives.size(); ++j) {
                ProcessMesh(i, j);
            }
        }

        // Instances

        for (auto root_node_index : asset->scenes[asset->defaultScene.value()].nodeIndices) {
            ProcessNode(root_node_index, Mat4(1.f));
        }

        return std::move(scene);
    }

    void GltfImporter::ProcessTexture(u32 tex_idx)
    {
        auto& in_texture = asset->textures[tex_idx];
        auto& out_texture = scene.textures[tex_idx];

        if (in_texture.imageIndex) {
            auto& image = asset->images[in_texture.imageIndex.value()];

            std::visit(nova::Overloads {
                [&](fastgltf::sources::URI& uri) {
                    out_texture.data = scene_ir::ImageFileURI(std::format("{}/{}", dir.string(), uri.uri.path()));
                },
                [&](fastgltf::sources::Vector& vec) {
                    scene_ir::ImageFileBuffer source;
                    source.data.resize(vec.bytes.size());
                    std::memcpy(source.data.data(), vec.bytes.data(), vec.bytes.size());
                    out_texture.data = std::move(source);
                },
                [&](fastgltf::sources::ByteView& byte_view) {
                    scene_ir::ImageFileBuffer source;
                    source.data.resize(byte_view.bytes.size());
                    std::memcpy(source.data.data(), byte_view.bytes.data(), byte_view.bytes.size());
                    out_texture.data = std::move(source);
                },
                [&](fastgltf::sources::BufferView& buffer_view_idx) {
                    auto& view = asset->bufferViews[buffer_view_idx.bufferViewIndex];
                    auto& buffer = asset->buffers[view.bufferIndex];
                    auto* bytes = fastgltf::DefaultBufferDataAdapter{}(buffer) + view.byteOffset;
                    scene_ir::ImageFileBuffer source;
                    source.data.resize(view.byteLength);
                    std::memcpy(source.data.data(), bytes, view.byteLength);
                    out_texture.data = std::move(source);
                },
                [&](auto&) {
                    NOVA_THROW("Unknown image source: {}", image.data.index());
                },
            }, asset->images[in_texture.imageIndex.value()].data);
        } else {
            out_texture.data = scene_ir::ImageBuffer{
                .data{ 255, 0, 255, 255 },
                .size{ 1, 1 },
                .format = scene_ir::BufferFormat::RGBA8,
            };
        }
    }

    void GltfImporter::ProcessMaterial(u32 mat_idx)
    {
        auto& in_material = asset->materials[mat_idx];
        auto& out_material = scene.materials[mat_idx];

        auto AddProperty = nova::Overloads {
            [&](std::string_view name, fastgltf::Optional<fastgltf::TextureInfo>& texture) {
                if (texture) out_material.properties.emplace_back(name, scene_ir::TextureSwizzle{ .texture_idx = u32(texture->textureIndex) }); },
            [&](std::string_view name, fastgltf::Optional<fastgltf::NormalTextureInfo>& texture) {
                if (texture) out_material.properties.emplace_back(name, scene_ir::TextureSwizzle{ .texture_idx = u32(texture->textureIndex) }); },
            [&](std::string_view name, nova::Span<f32> values) {
                switch (values.size()) {
                    break;case 1: out_material.properties.emplace_back(name, values[0]);
                    break;case 2: out_material.properties.emplace_back(name, Vec2(values[0], values[1]));
                    break;case 3: out_material.properties.emplace_back(name, Vec3(values[0], values[1], values[2]));
                    break;case 4: out_material.properties.emplace_back(name, Vec4(values[0], values[1], values[2], values[3]));
                    break;default: NOVA_THROW("Invalid number of values: {}", values.size());
                }
            },
            [&](std::string_view name, f32  scalar) { out_material.properties.emplace_back(name, scalar); },
            [&](std::string_view name, i32  scalar) { out_material.properties.emplace_back(name, scalar); },
            [&](std::string_view name, bool scalar) { out_material.properties.emplace_back(name, scalar); },
            [&](std::string_view name, fastgltf::Optional<f32> scalar) {
                if (scalar) out_material.properties.emplace_back(name, scalar.value()); },
            [&](std::string_view name, fastgltf::Optional<i32> scalar) {
                if (scalar) out_material.properties.emplace_back(name, scalar.value()); },
            [&](std::string_view name, fastgltf::Optional<bool> scalar) {
                if (scalar) out_material.properties.emplace_back(name, scalar.value()); },
        };

        AddProperty(scene_ir::property::BaseColor, in_material.pbrData.baseColorTexture);
        AddProperty(scene_ir::property::BaseColor, in_material.pbrData.baseColorFactor);

        AddProperty(scene_ir::property::Normal, in_material.normalTexture);

        AddProperty(scene_ir::property::Emissive, in_material.emissiveTexture);
        AddProperty(scene_ir::property::Emissive, in_material.emissiveFactor);
        AddProperty(scene_ir::property::Emissive, in_material.emissiveStrength);

        AddProperty(scene_ir::property::Metallic , in_material.pbrData.metallicRoughnessTexture);
        AddProperty(scene_ir::property::Metallic , in_material.pbrData.metallicFactor);
        AddProperty(scene_ir::property::Roughness, in_material.pbrData.metallicRoughnessTexture);
        AddProperty(scene_ir::property::Roughness, in_material.pbrData.roughnessFactor);

        AddProperty(scene_ir::property::AlphaCutoff, in_material.alphaCutoff);
        AddProperty(scene_ir::property::AlphaMask  , in_material.alphaMode == fastgltf::AlphaMode::Mask);
    }

    void GltfImporter::ProcessMesh(u32 gltf_mesh_idx, u32 primitive_index)
    {
        auto& primitive = asset->meshes[gltf_mesh_idx].primitives[primitive_index];

        if (asset->meshes[gltf_mesh_idx].name.contains("decal")) {
            return;
        }

        if (!primitive.indicesAccessor.has_value()
                || primitive.findAttribute("POSITION") == primitive.attributes.end())
            return;

        auto& out_mesh = scene.meshes.emplace_back();
        out_mesh.material_idx = u32(primitive.materialIndex.value_or(scene_ir::InvalidIndex));

        // Indices
        auto& indices = asset->accessors[primitive.indicesAccessor.value()];
        out_mesh.indices.resize(indices.count);
        fastgltf::copyFromAccessor<u32>(*asset, indices, out_mesh.indices.data());

        // Positions
        auto& positions = asset->accessors[primitive.findAttribute("POSITION")->second];
        out_mesh.positions.resize(positions.count);
        fastgltf::copyFromAccessor<Vec3>(*asset, positions, out_mesh.positions.data());

        // Normals
        if (auto normals = primitive.findAttribute("NORMAL"); normals != primitive.attributes.end()) {
            auto& accessor = asset->accessors[normals->second];
            out_mesh.normals.resize(accessor.count);
            fastgltf::copyFromAccessor<Vec3>(*asset, accessor, out_mesh.normals.data());
        }

        // TexCoords (0)
        if (auto tex_coords = primitive.findAttribute("TEXCOORD_0"); tex_coords != primitive.attributes.end()) {
            auto& accessor = asset->accessors[tex_coords->second];
            out_mesh.tex_coords.resize(accessor.count);
            fastgltf::copyFromAccessor<Vec2>(*asset, accessor, out_mesh.tex_coords.data());
        }
    }

    void GltfImporter::ProcessNode(usz node_idx, Mat4 parent_transform)
    {
        auto& node = asset->nodes[node_idx];

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

        transform = parent_transform * transform;

        if (node.meshIndex.has_value()) {
            auto[mesh_idx, mesh_count] = gltf_mesh_offsets[node.meshIndex.value()];
            for (u32 i = 0; i < mesh_count; ++i) {
                scene.instances.emplace_back(scene_ir::Instance {
                    .mesh_idx = mesh_idx + i,
                    .transform = transform,
                });
            }
        }

        for (auto child_idx : node.children) {
            ProcessNode(child_idx, transform);
        }
    }
}