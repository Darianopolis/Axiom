#include "axiom_Importer.hpp"

#include <nova/core/nova_Math.hpp>

#include <fastgltf/parser.hpp>
#include <fastgltf/glm_element_traits.hpp>

#include <stb_image.h>

#include <meshoptimizer.h>

namespace axiom
{
    struct GltfImporter : Importer
    {
        Scene* scene;

        GltfImporter(Scene& scene);

        virtual void Import(std::filesystem::path gltf, const ImportSettings& settings, std::optional<std::string_view> scene = {});
    };

    nova::Ref<Importer> CreateGltfImporter(Scene& scene)
    {
        return nova::Ref<GltfImporter>::Create(scene);
    }

    GltfImporter::GltfImporter(Scene& _scene)
        : scene(&_scene)
    {}

    struct GltfImporterImpl
    {
        GltfImporter& importer;

        ImportSettings settings;

        std::filesystem::path          baseDir;
        std::unique_ptr<fastgltf::Asset> asset;

        std::vector<nova::Ref<Mesh>>         meshes;
        std::vector<nova::Ref<TextureMap>> textures;
        std::vector<nova::Ref<Material>>  materials;

        struct ShadingAttribUnpacked
        {
            Vec3    normal;
            Vec2 texCoords;
            Vec4   tangent;
        };

        std::vector<ShadingAttribUnpacked> shadingAttribs;
        std::vector<f32>                      summedAreas;

        nova::HashMap<u32, u32> singlePixelTextures;

#ifdef AXIOM_TRACE_IMPORT // ---------------------------------------------------
        u32 debugLongestNodeName;
#endif // ----------------------------------------------------------------------

        u32 debug_maxTriangleVertexIndexRange = 0;
        u32 debug_numTrianglesToDuplicate = 0;
        u64 debug_oldSize = 0;
        u64 debug_newSize1 = 0;
        u64 debug_newSize2 = 0;

        void ProcessMeshes();
        void ProcessMesh(const fastgltf::Mesh& mesh);

        void ProcessTextures();
        void ProcessTexture(u32 index, fastgltf::Texture& texture);

        void ProcessMaterials();
        void ProcessMaterial(u32 index, fastgltf::Material& material);

        void ProcessScene(const fastgltf::Scene& scene);
        void ProcessNode(const fastgltf::Node& node, Mat4 parentTransform);
    };

    void GltfImporter::Import(std::filesystem::path gltf, const ImportSettings& settings, std::optional<std::string_view> sceneName)
    {
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
        data.loadFromFile(gltf);

        auto baseDir = gltf.parent_path();

        constexpr auto GltfOptions =
              fastgltf::Options::DontRequireValidAssetMember
            | fastgltf::Options::AllowDouble
            | fastgltf::Options::LoadGLBBuffers
            | fastgltf::Options::LoadExternalBuffers;

        auto type = fastgltf::determineGltfFileType(&data);

        auto res = type == fastgltf::GltfType::glTF
            ? parser.loadGLTF(&data, baseDir, GltfOptions)
            : parser.loadBinaryGLTF(&data, baseDir, GltfOptions);

        if (!res) {
            NOVA_THROW("Error loading [{}] Message: {}", gltf.string(), fastgltf::getErrorMessage(res.error()));
        }

        GltfImporterImpl impl{ *this };

        impl.settings = settings;

        impl.baseDir = std::move(baseDir);
        impl.asset = std::make_unique<fastgltf::Asset>(std::move(res.get()));

        impl.ProcessTextures();
        impl.ProcessMaterials();
        impl.ProcessMeshes();
        NOVA_LOGEXPR(impl.debug_maxTriangleVertexIndexRange);
        NOVA_LOGEXPR(impl.debug_numTrianglesToDuplicate);
        NOVA_LOG("Old Size: {}", nova::ByteSizeToString(impl.debug_oldSize));
        NOVA_LOG("New Size 1: {}", nova::ByteSizeToString(impl.debug_newSize1));
        NOVA_LOG("New Size 2: {}", nova::ByteSizeToString(impl.debug_newSize2));

        if (sceneName) {
            for (auto& gltfScene : impl.asset->scenes) {
                if (gltfScene.name == *sceneName) {
                    impl.ProcessScene(gltfScene);
                    return;
                }
            }
            NOVA_THROW("Could not find scene: {}", *sceneName);
        } else {
            if (!impl.asset->defaultScene) {
                NOVA_THROW("No scene specified and scene has no default scene!");
            }

            impl.ProcessScene(impl.asset->scenes[impl.asset->defaultScene.value()]);
        }
    }

    void GltfImporterImpl::ProcessMeshes()
    {
#ifdef AXIOM_TRACE_IMPORT // ---------------------------------------------------
        NOVA_LOG("Loading {} meshes...", asset->meshes.size());
#endif // ----------------------------------------------------------------------

        for (auto& mesh : asset->meshes) {

#ifdef AXIOM_TRACE_IMPORT // ---------------------------------------------------
            NOVA_LOG("  - Mesh: {}", mesh.name);
#endif // ----------------------------------------------------------------------

            ProcessMesh(mesh);
        }
    }

    void GltfImporterImpl::ProcessMesh(const fastgltf::Mesh& mesh)
    {
        usz vertexCount = 0;
        usz indexCount = 0;

        std::vector<const fastgltf::Primitive*> primitives;
        primitives.reserve(mesh.primitives.size());

        for (auto& prim : mesh.primitives) {
            if (!prim.indicesAccessor.has_value())
                continue;

            auto positions = prim.findAttribute("POSITION");
            if (positions == prim.attributes.end())
                continue;

            // TODO: Skip decals for now, need to implement decal masking
            if (prim.materialIndex && materials[prim.materialIndex.value()]->decal)
                continue;

            vertexCount += asset->accessors[positions->second].count;
            indexCount  += asset->accessors[prim.indicesAccessor.value()].count;

            primitives.push_back(&prim);
        }

        usz vertexOffset = 0;
        usz indexOffset = 0;

        auto outMesh = nova::Ref<Mesh>::Create();
        importer.scene->meshes.emplace_back(outMesh);
        meshes.emplace_back(outMesh);
        outMesh->positionAttribs.resize(vertexCount);
        outMesh->shadingAttribs.resize(vertexCount);
        outMesh->indices.resize(indexCount);
        outMesh->subMeshes.reserve(primitives.size());

        for (auto& prim : primitives) {

            auto& subMesh = outMesh->subMeshes.emplace_back();
            subMesh.vertexOffset = u32(vertexOffset);
            subMesh.firstIndex = u32(indexOffset);
            if (prim->materialIndex) {
                subMesh.material = materials[prim->materialIndex.value()];
            }

            // Indices
            auto& indices = asset->accessors[prim->indicesAccessor.value()];
            fastgltf::iterateAccessorWithIndex<u32>(*asset,
                indices, [&](u32 vIndex, usz iIndex) {
                    outMesh->indices[indexOffset + iIndex] = u32(vIndex);
                });

            // Positions
            auto& positions = asset->accessors[prim->findAttribute("POSITION")->second];
            fastgltf::iterateAccessorWithIndex<Vec3>(*asset,
                positions, [&](Vec3 pos, usz index) {
                    outMesh->positionAttribs[vertexOffset + index] = pos;
                });

            subMesh.maxVertex = u32(positions.count - 1);
            subMesh.indexCount = u32(indices.count);

            shadingAttribs.resize(positions.count);

            bool missingNormalsOrTangents = false;

            // Normals
            if (auto normals = prim->findAttribute("NORMAL"); normals != prim->attributes.end()) {
                auto& accessor = asset->accessors[normals->second];
                fastgltf::iterateAccessorWithIndex<Vec3>(*asset,
                    accessor, [&](Vec3 normal, usz index) {
                        shadingAttribs[index].normal = normal;
                    });
            } else {
                missingNormalsOrTangents = true;
            }

            // Tangents
            if (auto tangents = prim->findAttribute("TANGENT"); tangents != prim->attributes.end()) {
                fastgltf::iterateAccessorWithIndex<Vec4>(*asset,
                    asset->accessors[tangents->second], [&](Vec4 tangent, usz index) {
                        shadingAttribs[index].tangent = tangent;
                    });
            } else {
                missingNormalsOrTangents = true;
            }

            // TexCoords (1)
            if (auto texCoords = prim->findAttribute("TEXCOORD_0"); texCoords != prim->attributes.end()) {
                fastgltf::iterateAccessorWithIndex<Vec2>(*asset,
                    asset->accessors[texCoords->second], [&](Vec2 texCoords, usz index) {
                        shadingAttribs[index].texCoords = texCoords;
                        outMesh->shadingAttribs[vertexOffset + index].texCoords
                            = glm::packHalf2x16(texCoords);
                    });
            }

            if (missingNormalsOrTangents || settings.genTBN)
            {
                summedAreas.resize(shadingAttribs.size());

                for (auto& ts : shadingAttribs) {
                    ts.normal  = Vec3(0.f);
                    ts.tangent = Vec4(0.f);
                }

                auto updateNormalTangent = [&](u32 i, Vec3 normal, Vec4 tangent, f32 area) {
                    f32 lastArea = summedAreas[i];
                    summedAreas[i] += area;

                    f32 lastWeight = lastArea / (lastArea + area);
                    f32 newWeight = 1.f - lastWeight;

                    auto& v = shadingAttribs[i];
                    v.normal = (lastWeight * v.normal) + (newWeight * normal);

                    // Signed tangents

                    f32 tl = glm::length(tangent);
                    if (tl == 0.f || glm::isnan(tl) || glm::isinf(tl))
                    {
                        auto T = glm::normalize(Vec3(1.f, 2.f, 3.f));
                        tangent = Vec4(glm::normalize(T - glm::dot(T, normal) * normal), tangent.w);
                    }

                    v.tangent = Vec4((lastWeight * Vec3(v.tangent)) + (newWeight * Vec3(tangent)), tangent.w);
                };

                // std::vector<bool> keepPrim(indices.count / 3);

                for (u32 i = u32(indexOffset); i < indexOffset + indices.count; i += 3)
                {
                    u32 v1i = outMesh->indices[i + 0];
                    u32 v2i = outMesh->indices[i + 1];
                    u32 v3i = outMesh->indices[i + 2];

                    auto& v1 = outMesh->positionAttribs[v1i + vertexOffset];
                    auto& v2 = outMesh->positionAttribs[v2i + vertexOffset];
                    auto& v3 = outMesh->positionAttribs[v3i + vertexOffset];

                    auto v12 = v2 - v1;
                    auto v13 = v3 - v1;

                    auto& sa1 = shadingAttribs[v1i];
                    auto& sa2 = shadingAttribs[v2i];
                    auto& sa3 = shadingAttribs[v3i];

                    auto u12 = sa2.texCoords - sa1.texCoords;
                    auto u13 = sa3.texCoords - sa1.texCoords;

                    f32 f = 1.f / (u12.x * u13.y - u13.x * u12.y);
                    Vec3 T = f * Vec3 {
                        u13.y * v12.x - u12.y * v13.x,
                        u13.y * v12.y - u12.y * v13.y,
                        u13.y * v12.z - u12.y * v13.z,
                    };

                    Vec3 bitangent = f * Vec3 {
                        u13.x * v12.x - u12.x * v13.x,
                        u13.x * v12.y - u12.x * v13.y,
                        u13.x * v12.z - u12.x * v13.z,
                    };

                    auto cross = glm::cross(v12, v13);
                    auto area = glm::length(0.5f * cross);
                    auto normal = glm::normalize(cross);

                    Vec4 tangent = glm::dot(glm::cross(normal, T), bitangent) >= 0.f
                        ? Vec4(T, 1.f)
                        : Vec4(T, 0.f);

                    if (area)
                    {
                        // keepPrim[(i - indexOffset) / 3] = true;

                        updateNormalTangent(v1i, normal, tangent, area);
                        updateNormalTangent(v2i, normal, tangent, area);
                        updateNormalTangent(v3i, normal, tangent, area);
                    }
                }
            }

            {
                // TODO: Filter primitives

                for (u32 i = 0; i < shadingAttribs.size(); ++i) {
                    auto& saUnpacked = shadingAttribs[i];
                    saUnpacked.normal = glm::normalize(saUnpacked.normal);

                    auto& saPacked = outMesh->shadingAttribs[vertexOffset + i];

                    auto encNormal = math::SignedOctEncode(saUnpacked.normal);
                    saPacked.octX = u32(encNormal.x * 1023.0);
                    saPacked.octY = u32(encNormal.y * 1023.0);
                    saPacked.octS = u32(encNormal.z);

                    auto encTangent = math::EncodeTangent(saUnpacked.normal, saUnpacked.tangent);
                    saPacked.tgtA = u32(encTangent * 1023.0);
                    saPacked.tgtS = u32(saUnpacked.tangent.w);
                }
            }

            vertexOffset += positions.count;
            indexOffset += indices.count;
        }
    }

    void GltfImporterImpl::ProcessTextures()
    {
#ifdef AXIOM_TRACE_IMPORT // ---------------------------------------------------
        NOVA_LOG("Loading {} textures...", asset->textures.size());
#endif // ----------------------------------------------------------------------
        textures.resize(asset->textures.size());
#pragma omp parallel for
        for (u32 i = 0; i < asset->textures.size(); ++i) {
            ProcessTexture(i, asset->textures[i]);
        }
        for (auto& tex : textures) {
            importer.scene->textures.emplace_back(tex);
        }
    }

    void GltfImporterImpl::ProcessTexture(u32 index, fastgltf::Texture& texture)
    {
        auto outTexture = nova::Ref<TextureMap>::Create();
        textures[index] = outTexture;

        if (texture.imageIndex) {
            int width, height, channels;
            stbi_uc* imageData = nullptr;

            auto& gltfImage = asset->images[texture.imageIndex.value()];

            std::visit(nova::Overloads {
                [&](fastgltf::sources::URI& uri) {
                    auto path = std::format("{}/{}", baseDir.string(), uri.uri.path());
#ifdef AXIOM_TRACE_IMPORT // ---------------------------------------------------
                    NOVA_LOG("  Texture[{}] = File[{}]", index, path);
#endif // ----------------------------------------------------------------------
                    imageData = stbi_load(
                        path.c_str(),
                        &width, &height, &channels, STBI_rgb_alpha);
                    if (!imageData)
                        NOVA_LOG("STB Failed to load image: {}", path);
                },
                [&](fastgltf::sources::Vector& vec) {
                    imageData = stbi_load_from_memory(
                        vec.bytes.data(), u32(vec.bytes.size()),
                        &width, &height, &channels, STBI_rgb_alpha);
                },
                [&](fastgltf::sources::ByteView& byteView) {
                    imageData = stbi_load_from_memory(
                        reinterpret_cast<const unsigned char*>(byteView.bytes.data()), u32(byteView.bytes.size()),
                        &width, &height, &channels, STBI_rgb_alpha);
                },
                [&](fastgltf::sources::BufferView& bufferViewIdx) {
                    auto& view = asset->bufferViews[bufferViewIdx.bufferViewIndex];
                    auto& buffer = asset->buffers[view.bufferIndex];
                    auto* bytes = fastgltf::DefaultBufferDataAdapter{}(buffer) + view.byteOffset;

                    imageData = stbi_load_from_memory(
                        reinterpret_cast<const unsigned char*>(bytes), i32(view.byteLength),
                        &width, &height, &channels, STBI_rgb_alpha);
                },
                [&](auto&) {
                    NOVA_THROW("Unknown image source: {}", gltfImage.data.index());
                },
            }, gltfImage.data);

            if (imageData) {
                // constexpr u32 MaxSize = 1024;
                constexpr u32 MaxSize = 4096;

                if (width > MaxSize || height > MaxSize) {
                    u32 uWidth = u32(width);
                    u32 uHeight = u32(height);
                    u32 factor = std::max(width / MaxSize, height / MaxSize);
                    u32 sWidth = uWidth / factor;
                    u32 sHeight = uHeight / factor;
                    u32 factor2 = factor * factor;
                    auto getIndex = [](u32 x, u32 y, u32 pitch) {
                        return x + y * pitch;
                    };

                    outTexture->data.resize(sWidth * sHeight * 4);
                    outTexture->size = Vec2U(sWidth, sHeight);
                    for (u32 x = 0; x < sWidth; ++x) {
                        for (u32 y = 0; y < sHeight; ++y) {
                            u32 r = 0, g = 0, b = 0, a = 0;
                            for (u32 dx = 0; dx < factor; ++dx) {
                                for (u32 dy = 0; dy < factor; ++dy) {
                                    auto* pixel = imageData + getIndex(x * factor + dx, y * factor + dy, uWidth) * 4;
                                    r += pixel[0];
                                    g += pixel[1];
                                    b += pixel[2];
                                    a += pixel[3];
                                }
                            }

                            auto* pixel = outTexture->data.data() + getIndex(x, y, sWidth) * 4;
                            pixel[0] = b8(r / factor2);
                            pixel[1] = b8(g / factor2);
                            pixel[2] = b8(b / factor2);
                            pixel[3] = b8(a / factor2);
                        }
                    }
                } else {
                    auto pData = reinterpret_cast<const std::byte*>(imageData);
                    outTexture->data.assign(pData, pData + (width * height * 4));
                    outTexture->size = Vec2U(u32(width), u32(height));
                }

                stbi_image_free(imageData);
            }
        }

        if (outTexture->data.empty()) {
            outTexture->data.resize(4);
            outTexture->data[0] = b8(255);
            outTexture->data[1] = b8(255);
            outTexture->data[2] = b8(255);
            outTexture->data[3] = b8(255);
            outTexture->size = Vec2U(1, 1);
        }
    }

    void GltfImporterImpl::ProcessMaterials()
    {
        for (u32 i = 0; i < asset->materials.size(); ++i) {
            ProcessMaterial(i, asset->materials[i]);
        }
    }

    void GltfImporterImpl::ProcessMaterial(u32 index, fastgltf::Material& material)
    {
        (void)index;

        auto outMaterial = nova::Ref<Material>::Create();
        materials.emplace_back(outMaterial);
        importer.scene->materials.emplace_back(outMaterial);

        auto getImage = [&](
                const std::optional<fastgltf::TextureInfo>& texture,
                Span<f32> factor) {

            if (texture) {
                return textures[texture->textureIndex];
            }

            std::array<u8, 4> data { 0, 0, 0, 255 };
            for (u32 j = 0; j < factor.size(); ++j)
                data[j] = u8(factor[j] * 255.f);

            u32 encoded = std::bit_cast<u32>(data);

            if (singlePixelTextures.contains(encoded)) {
                return textures[singlePixelTextures.at(encoded)];
            }

            auto image = Ref<TextureMap>::Create();
            image->size = Vec2(1);
            image->data = { b8(data[0]), b8(data[1]), b8(data[2]), b8(data[3]) };

            textures.push_back(image);
            importer.scene->textures.emplace_back(image);
            singlePixelTextures.insert({ encoded, u32(textures.size() - 1) });

            return image;
        };

        // Material Texture maps

        auto& pbr = material.pbrData;
        outMaterial->baseColor_alpha = getImage(pbr.baseColorTexture, pbr.baseColorFactor);
        outMaterial->normals = getImage(material.normalTexture, { 0.5f, 0.5f, 1.f });
        outMaterial->emissivity = getImage(material.emissiveTexture, material.emissiveFactor);
        if (material.transmission) {
            outMaterial->transmission = getImage(material.transmission->transmissionTexture, { material.transmission->transmissionFactor });
        } else {
            outMaterial->transmission = getImage(std::nullopt, { 0.f });
        }
        outMaterial->metalness_roughness = getImage(pbr.metallicRoughnessTexture, { 0.f, pbr.roughnessFactor, pbr.metallicFactor });

        // Material Attribute

        outMaterial->alphaCutoff = material.alphaCutoff;
        switch (material.alphaMode) {
            using enum fastgltf::AlphaMode;
            break;case Blend: outMaterial->alphaBlend = true;
            break;case  Mask: outMaterial->alphaMask  = true;
        }

        // TODO: Sponza temporary hack DELETEME
        if (material.name == "dirt_decal") {
            outMaterial->decal = true;
        }
    }

    void GltfImporterImpl::ProcessScene(const fastgltf::Scene& scene)
    {
#ifdef AXIOM_TRACE_IMPORT // ---------------------------------------------------
        NOVA_LOG("Processing scene [{}]...", scene.name);

        debugLongestNodeName = 0;
        for (auto nodeIdx : scene.nodeIndices) {
            [&](this auto&& self, const fastgltf::Node& node) -> void {
                if (node.meshIndex) {
                    debugLongestNodeName = std::max(debugLongestNodeName, u32(node.name.size()));
                }
                for (auto childIdx : node.children) {
                    self(asset->nodes[childIdx]);
                }
            }(asset->nodes[nodeIdx]);
        }
#endif // ----------------------------------------------------------------------

        for (auto nodeIdx : scene.nodeIndices) {
            ProcessNode(asset->nodes[nodeIdx], Mat4(1.f));
        }
    }

    void GltfImporterImpl::ProcessNode(const fastgltf::Node& node, Mat4 parentTransform)
    {
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
#ifdef AXIOM_TRACE_IMPORT // ---------------------------------------------------
            NOVA_LOG("  - Mesh Instance: {:>{}} -> {}", node.name, debugLongestNodeName, asset->meshes[node.meshIndex.value()].name);
#endif // ----------------------------------------------------------------------
            importer.scene->instances.emplace_back(
                new MeshInstance{ {}, meshes[node.meshIndex.value()], transform });
        }

        for (auto& childIndex : node.children) {
            ProcessNode(asset->nodes[childIndex], transform);
        }
    }
}