#include "axiom_Importer.hpp"

#include <attributes/axiom_Attributes.hpp>

#include <nova/core/nova_Math.hpp>

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include <stb_image.h>

#include <meshoptimizer.h>

namespace axiom
{
    struct AssimpImporter : Importer
    {
        LoadableScene* scene;

        AssimpImporter(LoadableScene& scene);

        virtual void Import(std::filesystem::path gltf, const ImportSettings& settings, std::optional<std::string_view> scene = {});
    };

    nova::Ref<Importer> CreateAssimpImporter(LoadableScene& scene)
    {
        return nova::Ref<AssimpImporter>::Create(scene);
    }

    AssimpImporter::AssimpImporter(LoadableScene& _scene)
        : scene(&_scene)
    {}

    struct AssimpImporterImpl
    {
        AssimpImporter&        importer;
        ImportSettings         settings;
        std::filesystem::path   baseDir;
        Assimp::Importer assimpImporter;
        const aiScene*            asset = nullptr;

        std::vector<nova::Ref<TriMesh>>        meshes;
        std::vector<nova::Ref<UVMaterial>>  materials;

        nova::HashMap<std::filesystem::path, nova::Ref<UVTexture>> textures;
        nova::HashMap<UVTexture*, ImageProcess>           textureOperations;
        nova::HashMap<u32, nova::Ref<UVTexture>>        singlePixelTextures;

#ifdef AXIOM_TRACE_IMPORT // ---------------------------------------------------
        u32 debugLongestNodeName;
#endif // ----------------------------------------------------------------------

        u32 debug_maxTriangleVertexIndexRange = 0;
        u32 debug_numTrianglesToDuplicate = 0;
        u64 debug_oldSize = 0;
        u64 debug_newSize1 = 0;
        u64 debug_newSize2 = 0;

        void ProcessMeshes();
        void ProcessMesh(aiMesh* mesh);

        void ProcessTextures();
        void ProcessTexture(UVTexture* outTexture, const std::filesystem::path& texture);

        void ProcessMaterials();
        void ProcessMaterial(u32 index, aiMaterial* material);

        void ProcessScene();
        void ProcessNode(aiNode* node, Mat4 parentTransform);
    };

    void AssimpImporter::Import(std::filesystem::path gltf, const ImportSettings& settings, std::optional<std::string_view> sceneName)
    {
        (void)sceneName;

        AssimpImporterImpl impl{ *this };

        u32 aiFlags = 0;
        aiFlags |= aiProcess_JoinIdenticalVertices;
        aiFlags |= aiProcess_Triangulate;
        aiFlags |= aiProcess_SortByPType;

        // aiFlags |= aiProcess_GenSmoothNormals;
        // aiFlags |= aiProcess_CalcTangentSpace;

        if (!settings.flipUVs) {
            aiFlags |= aiProcess_FlipUVs;
        }

        aiFlags |= aiProcess_FindInvalidData;

        aiFlags |= aiProcess_GenUVCoords;
        aiFlags |= aiProcess_TransformUVCoords;

        impl.settings = settings;
        impl.baseDir = std::move(gltf.parent_path());
        impl.asset = impl.assimpImporter.ReadFile(gltf.string(), aiFlags);

        if (!impl.asset) {
            NOVA_THROW("Error loading [{}] Message: {}", gltf.string(), impl.assimpImporter.GetErrorString());
        }

        impl.ProcessMaterials();
        impl.ProcessTextures();
        impl.ProcessMeshes();

        NOVA_LOG("Loading nodes");

        impl.ProcessScene();
    }

    void AssimpImporterImpl::ProcessMeshes()
    {
#ifdef AXIOM_TRACE_IMPORT // ---------------------------------------------------
        NOVA_LOG("Loading {} meshes...", asset->mNumMeshes);
#endif // ----------------------------------------------------------------------

        for (u32 i = 0; i < asset->mNumMeshes; ++i) {
            auto* mesh = asset->mMeshes[i];

#ifdef AXIOM_TRACE_IMPORT // ---------------------------------------------------
            NOVA_LOG("  - Mesh[{}]: {}", i, mesh->mName.C_Str());
#endif // ----------------------------------------------------------------------

            ProcessMesh(mesh);
        }
    }

    void AssimpImporterImpl::ProcessMesh(aiMesh* mesh)
    {
        u32 vertexCount = mesh->mNumVertices;
        u32 indexCount = mesh->mNumFaces * 3;

        // TODO: Identify decals

        auto outMesh = nova::Ref<TriMesh>::Create();
        meshes.emplace_back(outMesh);

        if (!mesh->HasPositions()) {
            NOVA_LOG("Mesh [{}] has no positions, skipping", mesh->mName.C_Str());
            return;
        }

        importer.scene->meshes.emplace_back(outMesh);
        outMesh->positionAttributes.resize(vertexCount);
        outMesh->shadingAttributes.resize(vertexCount);
        outMesh->indices.resize(indexCount);

        auto& subMesh = outMesh->subMeshes.emplace_back();
        subMesh.vertexOffset = 0;
        subMesh.maxVertex = vertexCount - 1;
        subMesh.indexCount = indexCount;
        subMesh.firstIndex = 0;
        subMesh.material = materials[mesh->mMaterialIndex];

        // Indices
        for (u32 i = 0; i < mesh->mNumFaces; ++i) {
            if (mesh->mFaces[i].mNumIndices != 3) {
                NOVA_THROW("Invalid face, num indices = {}", mesh->mFaces[i].mNumIndices);
            }
            outMesh->indices[i * 3 + 0] = mesh->mFaces[i].mIndices[0];
            outMesh->indices[i * 3 + 1] = mesh->mFaces[i].mIndices[1];
            outMesh->indices[i * 3 + 2] = mesh->mFaces[i].mIndices[2];
        }

        // Positions
        for (u32 i = 0; i < mesh->mNumVertices; ++i) {
            auto& pos = mesh->mVertices[i];
            outMesh->positionAttributes[i] = Vec3(pos.x, pos.y, pos.z);
        }

        s_MeshProcessor.ProcessMesh(
            { &mesh->mVertices[0], sizeof(mesh->mVertices[0]), vertexCount },
            mesh->HasNormals()
                ? InStridedRegion{ &mesh->mNormals[0], sizeof(mesh->mNormals[0]), vertexCount }
                : InStridedRegion{},
            mesh->HasTextureCoords(0)
                ? InStridedRegion{ &mesh->mTextureCoords[0][0], sizeof(mesh->mTextureCoords[0][0]), vertexCount }
                : InStridedRegion{},
            { outMesh->indices.data(), sizeof(u32), outMesh->indices.size() },
            { &outMesh->shadingAttributes[0].tangentSpace, sizeof(outMesh->shadingAttributes[0]), vertexCount },
            { &outMesh->shadingAttributes[0].texCoords, sizeof(outMesh->shadingAttributes[0]), vertexCount });
    }

    void AssimpImporterImpl::ProcessTextures()
    {
#ifdef AXIOM_TRACE_IMPORT // ---------------------------------------------------
        NOVA_LOG("Loading {} textures...", asset->mNumTextures);
#endif // ----------------------------------------------------------------------

        std::vector<std::pair<const std::filesystem::path*, UVTexture*>> flatTextures;
        for (auto&[path, texture] : textures) {
            flatTextures.emplace_back(&path, texture.Raw());
        }

#pragma omp parallel for
        for (u32 i = 0; i < flatTextures.size(); ++i) {
            ProcessTexture(flatTextures[i].second, *flatTextures[i].first);
        }

        for (auto& mat : materials) {
            // TODO: Detect if this is already set by source
            if (mat->baseColor_alpha->minAlpha < 0.5f) {
                mat->alphaMask = true;
                mat->alphaCutoff = 0.5f;
            }
        }

        for (auto& tex : flatTextures) {
            importer.scene->textures.emplace_back(tex.second);
        }
    }

    void AssimpImporterImpl::ProcessTexture(UVTexture* outTexture, const std::filesystem::path& texture)
    {
        auto path = std::format("{}/{}", baseDir.string(), texture.string());

#ifdef AXIOM_TRACE_IMPORT // ---------------------------------------------------
            NOVA_LOG("  Texture[{}]", path);
#endif // ----------------------------------------------------------------------

        auto _ops = textureOperations.find(outTexture);
        auto ops = _ops == textureOperations.end()
            ? ImageProcess::None
            : _ops->second;

        s_ImageProcessor.ProcessImage(path.c_str(), 0, ImageType::ColorAlpha, 4096, ops);
        outTexture->data.resize(s_ImageProcessor.GetImageDataSize());
        std::memcpy(outTexture->data.data(), s_ImageProcessor.GetImageData(), outTexture->data.size());
        outTexture->size = s_ImageProcessor.GetImageDimensions();
        outTexture->minAlpha = s_ImageProcessor.GetMinAlpha();
        outTexture->maxAlpha = s_ImageProcessor.GetMaxAlpha();
    }

    void AssimpImporterImpl::ProcessMaterials()
    {
#ifdef AXIOM_TRACE_IMPORT // ---------------------------------------------------
        NOVA_LOG("Loading {} materials...", asset->mNumMaterials);
#endif // ----------------------------------------------------------------------
        for (u32 i = 0; i < asset->mNumMaterials; ++i) {
#ifdef AXIOM_TRACE_IMPORT // ---------------------------------------------------
            NOVA_LOG("  - Material: {}", asset->mMaterials[i]->GetName().C_Str());
#endif // ----------------------------------------------------------------------
            ProcessMaterial(i, asset->mMaterials[i]);
        }
    }

    void AssimpImporterImpl::ProcessMaterial(u32/* index*/, aiMaterial* material)
    {
        auto outMaterial = nova::Ref<UVMaterial>::Create();
        materials.emplace_back(outMaterial);
        importer.scene->materials.emplace_back(outMaterial);

        auto findImagePath = [&](Span<aiTextureType> texTypes) -> std::optional<std::string> {
            for (auto type : texTypes) {
                if (material->GetTextureCount(type) > 0) {
                    aiString str;
                    auto res = material->GetTexture(type, 0, &str);
                    if (res == aiReturn_SUCCESS) {
                        return std::string(str.C_Str(), str.length);
                    }
                }
            }
            return std::nullopt;
        };

#ifdef AXIOM_TRACE_IMPORT // ---------------------------------------------------
        auto debugTexture = [&](aiTextureType type, const char* name) {
            auto path = findImagePath({ type });
            if (path) {
                NOVA_LOG("    {} = {}", name, path.value());
            }
        };

        debugTexture(aiTextureType_NONE, "None");
        debugTexture(aiTextureType_DIFFUSE, "Diffuse");
        debugTexture(aiTextureType_SPECULAR, "Specular");
        debugTexture(aiTextureType_AMBIENT, "Ambient");
        debugTexture(aiTextureType_EMISSIVE, "Emissive");
        debugTexture(aiTextureType_HEIGHT, "Height");
        debugTexture(aiTextureType_NORMALS, "Normals");
        debugTexture(aiTextureType_SHININESS, "Shininess");
        debugTexture(aiTextureType_OPACITY, "Opacity");
        debugTexture(aiTextureType_DISPLACEMENT, "Displacement");
        debugTexture(aiTextureType_LIGHTMAP, "Lightmap");
        debugTexture(aiTextureType_REFLECTION, "Reflection");
        debugTexture(aiTextureType_BASE_COLOR, "Base Color");
        debugTexture(aiTextureType_NORMAL_CAMERA, "Normal Camera");
        debugTexture(aiTextureType_EMISSION_COLOR, "Emission Color");
        debugTexture(aiTextureType_METALNESS, "Metalness");
        debugTexture(aiTextureType_DIFFUSE_ROUGHNESS, "Diffuse Roughness");
        debugTexture(aiTextureType_AMBIENT_OCCLUSION, "Ambient occlusion");
        debugTexture(aiTextureType_SHEEN, "Sheen");
        debugTexture(aiTextureType_CLEARCOAT, "Clearcoat");
        debugTexture(aiTextureType_TRANSMISSION, "Tranmission");
        debugTexture(aiTextureType_UNKNOWN, "Unknown");

        std::unordered_map<std::string, int> propertyIndexes;
        for (uint32_t i = 0; i < material->mNumProperties; ++i) {
            aiMaterialProperty* property = material->mProperties[i];
            std::string data;
            if (property->mType == aiPropertyTypeInfo::aiPTI_String) {
                aiString str;
                material->Get(property->mKey.C_Str(), property->mType, property->mIndex, str);
                data = str.C_Str();
            } else if (property->mType == aiPropertyTypeInfo::aiPTI_Double) {
                for (uint32_t j = 0; j < property->mDataLength / 8; ++j)
                    data += std::to_string(((double*)(property->mData))[j]) + " ";
            } else if (property->mType == aiPropertyTypeInfo::aiPTI_Float) {
                for (uint32_t j = 0; j < property->mDataLength / 4; ++j)
                    data += std::to_string(((float*)(property->mData))[j]) + " ";
            } else if (property->mType == aiPropertyTypeInfo::aiPTI_Integer) {
                for (uint32_t j = 0; j < property->mDataLength / 4; ++j)
                    data += std::to_string(((int*)(property->mData))[j]) + " ";
            } else {
                if (property->mDataLength == 1) {
                    data += std::to_string((int)((char*)property->mData)[0]);
                } else if (property->mDataLength == 4) {
                    data += std::to_string(((int*)(property->mData))[0]);
                } else {
                    data += "|";
                    for (uint32_t j = 0; j < property->mDataLength; ++j)
                        data += std::format("{:02x}|", property->mData[j]);
                }
            }

            if (!data.empty()) {
                if (property->mSemantic == aiTextureType_NONE) {
                    std::cout << "    Property." << property->mKey.C_Str() << " = " << data << "\n";
                } else {
                    int index = propertyIndexes[property->mKey.C_Str()]++;
                    std::cout << "    Property[" << index << "]." << property->mKey.C_Str() << " = " << data << "\n";
                }
            }
        }
#endif // ----------------------------------------------------------------------

        auto findImage = [&](Span<aiTextureType> texTypes) -> std::optional<Ref<UVTexture>> {
            auto path = findImagePath(texTypes);
            if (!path) return std::nullopt;

            auto p = std::filesystem::path(path.value());
            NOVA_LOG("Extension: {}", p.extension().string());
            if (p.extension().string() == ".dds") {
                p.replace_extension(".png");
            }
            NOVA_LOG("  found file: {}", p.string());
            auto& texture = textures[std::move(p)];
            if (!texture) {
                texture = Ref<UVTexture>::Create();
            }
            return texture;
        };

        auto findColorProperty = [&](Span<const char*> properties) -> std::optional<std::array<f32, 4>> {
            for (auto property : properties) {
                aiColor4D color4;
                if (material->Get(property, 0, 0, color4) == aiReturn_SUCCESS) {
                    return std::array<f32, 4>{ color4.r, color4.g, color4.b, color4.a };
                }

                aiColor3D color3;
                if (material->Get(property, 0, 0, color3) == aiReturn_SUCCESS) {
                    return std::array<f32, 4>{ color3.r, color3.g,  color3.b, 1.f };
                }
            }

            return std::nullopt;
        };

        auto findScalarProperty = [&](Span<const char*> properties) -> std::optional<f32> {
            for (auto property : properties) {
                float scalar;
                if (material->Get(property, 0, 0, scalar) == aiReturn_SUCCESS) {
                    return scalar;
                }
            }

            return std::nullopt;
        };

        auto makePixelImage = [&](Span<f32> factor) {
            std::array<u8, 4> data { 0, 0, 0, 255 };

            for (u32 j = 0; j < factor.size(); ++j)
                data[j] = u8(factor[j] * 255.f);

            u32 encoded = std::bit_cast<u32>(data);

            if (singlePixelTextures.contains(encoded)) {
                return singlePixelTextures.at(encoded);
            }

            auto image = Ref<UVTexture>::Create();
            image->size = Vec2(1);
            image->data = { b8(data[0]), b8(data[1]), b8(data[2]), b8(data[3]) };

            importer.scene->textures.emplace_back(image);
            singlePixelTextures.insert({ encoded, image });

            return image;
        };

        // Material Texture maps

        if (auto tex = findImage({ aiTextureType_BASE_COLOR, aiTextureType_DIFFUSE })) {
            outMaterial->baseColor_alpha = tex.value();
        } else if (auto color = findColorProperty({ "$clr.base", "$clr.diffuse" })) {
            outMaterial->baseColor_alpha = makePixelImage(color.value());
        } else {
            outMaterial->baseColor_alpha = makePixelImage({ 1.f, 1.f, 1.f });
        }

        if (auto tex = findImage({ aiTextureType_NORMALS })) {
            outMaterial->normals = tex.value();
            if (settings.flipNormalMapZ) {
                textureOperations[outMaterial->normals] |= ImageProcess::FlipNrmZ;
            }
        } else {
            outMaterial->normals = makePixelImage({ 0.5f, 0.5f, 1.f });
        }

        if (auto tex = findImage({ aiTextureType_EMISSION_COLOR, aiTextureType_EMISSIVE })) {
            outMaterial->emissivity = tex.value();
        } else if (auto emissive = findColorProperty({ "$clr.emissive" })) {
            outMaterial->emissivity = makePixelImage(emissive.value());
        } else {
            outMaterial->emissivity = makePixelImage({});
        }

        // TODO
        {
            outMaterial->transmission = makePixelImage({ 0.f });
        }

        {
            auto metalness = findImage({ aiTextureType_METALNESS, aiTextureType_SPECULAR });
            auto roughness = findImage({ aiTextureType_DIFFUSE_ROUGHNESS, aiTextureType_SPECULAR });

            if (metalness && roughness && metalness.value() == roughness.value()) {
                outMaterial->metalness_roughness = metalness.value();
            } else {
                auto mFactor = findScalarProperty({ "$mat.metallicFactor" });
                auto rFactor = findScalarProperty({ "$mat.roughnessFactor" });

                outMaterial->metalness_roughness = makePixelImage({
                    0.f,
                    rFactor.value_or(0.5f),
                    mFactor.value_or(0.f),
                });
            }
        }

        // Material Attribute

        // TODO:
        // outMaterial->alphaCutoff = material.alphaCutoff;
        // switch (material.alphaMode) {
        //     using enum fastgltf::AlphaMode;
        //     break;case Blend: outMaterial->alphaBlend = true;
        //     break;case  Mask: outMaterial->alphaMask  = true;
        // }

        // TODO: Sponza temporary hack DELETEME
        // if (material.name == "dirt_decal") {
        //     outMaterial->decal = true;
        // }
    }

    void AssimpImporterImpl::ProcessScene()
    {
#ifdef AXIOM_TRACE_IMPORT // ---------------------------------------------------
        NOVA_LOG("Processing scene...");

        debugLongestNodeName = 0;
        [&](this auto&& self, aiNode* node) -> void {
            if (node->mNumMeshes > 0) {
                debugLongestNodeName = std::max(debugLongestNodeName, u32(node->mName.length));
            }
            for (u32 i = 0; i < node->mNumChildren; ++i) {
                self(node->mChildren[i]);
            }
        }(asset->mRootNode);

#endif // ----------------------------------------------------------------------

        {
            ProcessNode(asset->mRootNode, Mat4(1.f));
        }
    }

    void AssimpImporterImpl::ProcessNode(aiNode* node, Mat4 parentTransform)
    {
        Mat4 transform = std::bit_cast<Mat4>(node->mTransformation);
        transform = glm::transpose(transform);
        transform = parentTransform * transform;

        for (u32 i = 0; i < node->mNumMeshes; ++i) {
            if (meshes[node->mMeshes[i]]->indices.empty())
                continue;

#ifdef AXIOM_TRACE_IMPORT // ---------------------------------------------------
            NOVA_LOG("  - Mesh Instance: {:>{}} -> {}",
                node->mName.C_Str(),
                debugLongestNodeName,
                asset->mMeshes[node->mMeshes[i]]->mName.C_Str());
#endif // ----------------------------------------------------------------------
            importer.scene->instances.emplace_back(
                new TriMeshInstance{ {}, meshes[node->mMeshes[i]], transform });
        }

        for (u32 i = 0; i < node->mNumChildren; ++i) {
            ProcessNode(node->mChildren[i], transform);
        }
    }
}