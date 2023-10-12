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
        LoadableScene* scene;

        GltfImporter(LoadableScene& scene);

        virtual void Import(std::filesystem::path gltf, const ImportSettings& settings, std::optional<std::string_view> scene = {});
    };

    nova::Ref<Importer> CreateGltfImporter(LoadableScene& scene)
    {
        return nova::Ref<GltfImporter>::Create(scene);
    }

    GltfImporter::GltfImporter(LoadableScene& _scene)
        : scene(&_scene)
    {}

    struct GltfImporterImpl
    {
        GltfImporter& importer;

        ImportSettings settings;

        std::filesystem::path          baseDir;
        std::unique_ptr<fastgltf::Asset> asset;

        std::vector<nova::Ref<TriMesh>>       meshes;
        std::vector<nova::Ref<UVTexture>>   textures;
        std::vector<nova::Ref<UVMaterial>> materials;

        struct Vertex
        {
            Vec3 normal;
            Vec2 texCoords;
        };
        std::vector<Vertex> vertices;

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

        auto outMesh = nova::Ref<TriMesh>::Create();
        importer.scene->meshes.emplace_back(outMesh);
        meshes.emplace_back(outMesh);
        outMesh->positionAttributes.resize(vertexCount);
        outMesh->shadingAttributes.resize(vertexCount);
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
                    outMesh->positionAttributes[vertexOffset + index] = pos;
                });

            subMesh.maxVertex = u32(positions.count - 1);
            subMesh.indexCount = u32(indices.count);

            vertices.resize(vertexCount);

            bool hasNormals = false;
            if (auto normals = prim->findAttribute("NORMAL"); normals != prim->attributes.end()) {
                hasNormals = true;
                fastgltf::iterateAccessorWithIndex<Vec3>(*asset, asset->accessors[normals->second],
                    [&](Vec3 nrm, usz i) { vertices[i].normal = nrm; });
            }

            bool hasTexCoords = false;
            if (auto texCoords = prim->findAttribute("TEXCOORD_0"); texCoords != prim->attributes.end()) {
                hasTexCoords = true;
                fastgltf::iterateAccessorWithIndex<Vec2>(*asset, asset->accessors[texCoords->second],
                    [&](Vec2 uv, usz i) { vertices[i].texCoords = uv; });
            }

            s_MeshProcessor.ProcessMesh(
                { &outMesh->positionAttributes[vertexOffset], sizeof(outMesh->positionAttributes[0]), positions.count },
                hasNormals
                    ? InStridedRegion{ &vertices[0].normal, sizeof(vertices[0]), positions.count }
                    : InStridedRegion{},
                hasTexCoords
                    ? InStridedRegion{ &vertices[0].texCoords, sizeof(vertices[0]), positions.count }
                    : InStridedRegion{},
                { &outMesh->indices[indexOffset], sizeof(outMesh->indices[0]), indices.count },
                { &outMesh->shadingAttributes[vertexOffset].tangentSpace, sizeof(outMesh->shadingAttributes[0]), positions.count },
                { &outMesh->shadingAttributes[vertexOffset].texCoords, sizeof(outMesh->shadingAttributes[0]), positions.count });

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
        auto outTexture = nova::Ref<UVTexture>::Create();
        textures[index] = outTexture;

        constexpr u32 MaxDim = 4096;

        auto& gltfImage = asset->images[texture.imageIndex.value()];
        std::visit(nova::Overloads {
            [&](fastgltf::sources::URI& uri) {
                auto path = std::format("{}/{}", baseDir.string(), uri.uri.path());
#ifdef AXIOM_TRACE_IMPORT // ---------------------------------------------------
                NOVA_LOG("  Texture[{}] = File[{}]", index, path);
#endif // ----------------------------------------------------------------------
                s_ImageProcessor.ProcessImage(path.c_str(), 0, ImageType::ColorAlpha, MaxDim, {});
            },
            [&](fastgltf::sources::Vector& vec) {
                s_ImageProcessor.ProcessImage((const char*)vec.bytes.data(), vec.bytes.size(), ImageType::ColorAlpha, MaxDim, {});
            },
            [&](fastgltf::sources::ByteView& byteView) {
                s_ImageProcessor.ProcessImage((const char*)byteView.bytes.data(), byteView.bytes.size(), ImageType::ColorAlpha, MaxDim, {});
            },
            [&](fastgltf::sources::BufferView& bufferViewIdx) {
                auto& view = asset->bufferViews[bufferViewIdx.bufferViewIndex];
                auto& buffer = asset->buffers[view.bufferIndex];
                auto* bytes = fastgltf::DefaultBufferDataAdapter{}(buffer) + view.byteOffset;
                s_ImageProcessor.ProcessImage((const char*)bytes, view.byteLength, ImageType::ColorAlpha, MaxDim, {});
            },
            [&](auto&) {
                NOVA_THROW("Unknown image source: {}", gltfImage.data.index());
            },
        }, gltfImage.data);

        outTexture->data.resize(s_ImageProcessor.GetImageDataSize());
        std::memcpy(outTexture->data.data(), s_ImageProcessor.GetImageData(), outTexture->data.size());
        outTexture->size = s_ImageProcessor.GetImageDimensions();
        outTexture->minAlpha = s_ImageProcessor.GetMinAlpha();
        outTexture->maxAlpha = s_ImageProcessor.GetMaxAlpha();
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

        auto outMaterial = nova::Ref<UVMaterial>::Create();
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

            auto image = Ref<UVTexture>::Create();
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
                new TriMeshInstance{ {}, meshes[node.meshIndex.value()], transform });
        }

        for (auto& childIndex : node.children) {
            ProcessNode(asset->nodes[childIndex], transform);
        }
    }
}