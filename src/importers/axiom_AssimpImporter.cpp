#include "axiom_Importer.hpp"

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
        Scene* scene;

        AssimpImporter(Scene& scene);

        virtual void Import(std::filesystem::path gltf, const ImportSettings& settings, std::optional<std::string_view> scene = {});
    };

    nova::Ref<Importer> CreateAssimpImporter(Scene& scene)
    {
        return nova::Ref<AssimpImporter>::Create(scene);
    }

    AssimpImporter::AssimpImporter(Scene& _scene)
        : scene(&_scene)
    {}

    enum class TextureOperations
    {
        None        = 0,
        FlipNormalZ = 1 << 0,
        ScanAlpha   = 1 << 1,
    };
    NOVA_DECORATE_FLAG_ENUM(TextureOperations)

    struct AssimpImporterImpl
    {
        AssimpImporter& importer;

        ImportSettings settings;

        std::filesystem::path   baseDir;
        Assimp::Importer assimpImporter;
        const aiScene*            asset = nullptr;

        std::vector<nova::Ref<Mesh>>         meshes;
        std::vector<nova::Ref<Material>>  materials;

        nova::HashMap<std::filesystem::path, nova::Ref<TextureMap>> textures;

        nova::HashMap<TextureMap*, TextureOperations> textureOperations;

        struct ShadingAttribUnpacked
        {
            Vec3    normal;
            Vec2 texCoords;
            Vec4   tangent;
        };

        std::vector<ShadingAttribUnpacked> shadingAttribs;
        std::vector<f32>                      summedAreas;

        nova::HashMap<u32, nova::Ref<TextureMap>> singlePixelTextures;

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
        void ProcessTexture(TextureMap* outTexture, const std::filesystem::path& texture);

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

        auto outMesh = nova::Ref<Mesh>::Create();
        meshes.emplace_back(outMesh);

        if (!mesh->HasPositions()) {
            NOVA_LOG("Mesh [{}] has no positions, skipping", mesh->mName.C_Str());
            return;
        }

        if (mesh->mPrimitiveTypes != aiPrimitiveType_TRIANGLE) {
            NOVA_LOG("Mesh [{}] contains non-triangle primitives, skipping", mesh->mName.C_Str());
            return;
        }

        importer.scene->meshes.emplace_back(outMesh);
        outMesh->positionAttribs.resize(vertexCount);
        outMesh->shadingAttribs.resize(vertexCount);
        outMesh->indices.resize(indexCount);

        shadingAttribs.resize(vertexCount);

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
            outMesh->positionAttribs[i] = Vec3(pos.x, pos.y, pos.z);
        }

        // Tangent space
        bool missingNormalsOrTangents = false;
        if (mesh->HasNormals() && mesh->HasTangentsAndBitangents()) {
            for (u32 i = 0; i < mesh->mNumVertices; ++i) {
                auto& _nrm = mesh->mNormals[i];
                Vec3 nrm{ _nrm.x, _nrm.y, _nrm.z };
                shadingAttribs[i].normal = Vec3(nrm.x, nrm.y, nrm.x);

                auto& _tgt = mesh->mTangents[i];
                Vec3 tgt{ _tgt.x, _tgt.y, _tgt.z };

                auto& _btgt = mesh->mBitangents[i];
                Vec3 btgt{ _btgt.x, _btgt.y, _btgt.z };

                auto sign = glm::dot(btgt, glm::cross(tgt, nrm)) > 0.f ? 1.f : 0.f;

                shadingAttribs[i].tangent = Vec4(tgt.x, tgt.y, tgt.z, sign);
            }
        } else {
            missingNormalsOrTangents = true;
        }

        // TexCoords (1)
        if (mesh->HasTextureCoords(0)) {
            for (u32 i = 0; i < mesh->mNumVertices; ++i) {
                auto& uv = mesh->mTextureCoords[0][i];
                outMesh->shadingAttribs[i].texCoords = glm::packHalf2x16(Vec2(uv.x, uv.y));
                shadingAttribs[i].texCoords = Vec2(uv.x, uv.y);
            }
        }

        if (missingNormalsOrTangents || settings.genTBN)
        {
            NOVA_LOG("      Regenerating tangent space: {}", missingNormalsOrTangents ? "missing" : "forced");

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

            for (u32 i = 0; i < indexCount; i += 3)
            {
                u32 v1i = outMesh->indices[i + 0];
                u32 v2i = outMesh->indices[i + 1];
                u32 v3i = outMesh->indices[i + 2];


                auto& v1 = outMesh->positionAttribs[v1i];
                auto& v2 = outMesh->positionAttribs[v2i];
                auto& v3 = outMesh->positionAttribs[v3i];

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

            {
                // TODO: Filter primitives

                for (u32 i = 0; i < shadingAttribs.size(); ++i) {
                    auto& saUnpacked = shadingAttribs[i];
                    saUnpacked.normal = glm::normalize(saUnpacked.normal);

                    auto& saPacked = outMesh->shadingAttribs[i];

                    auto encNormal = math::SignedOctEncode(saUnpacked.normal);
                    // auto encNormal = math::SignedOctEncode(Vec3(saUnpacked.tangent));
                    saPacked.octX = u32(encNormal.x * 1023.0);
                    saPacked.octY = u32(encNormal.y * 1023.0);
                    saPacked.octS = u32(encNormal.z);

                    auto encTangent = math::EncodeTangent(saUnpacked.normal, saUnpacked.tangent);
                    saPacked.tgtA = u32(encTangent * 1023.0);
                    saPacked.tgtS = u32(saUnpacked.tangent.w);
                }
            }
        }
    }

    void AssimpImporterImpl::ProcessTextures()
    {
#ifdef AXIOM_TRACE_IMPORT // ---------------------------------------------------
        NOVA_LOG("Loading {} textures...", asset->mNumTextures);
#endif // ----------------------------------------------------------------------

        std::vector<std::pair<const std::filesystem::path*, TextureMap*>> flatTextures;
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

    void AssimpImporterImpl::ProcessTexture(TextureMap* outTexture, const std::filesystem::path& texture)
    {
        {
            int width, height, channels;
            stbi_uc* imageData = nullptr;

            auto path = std::format("{}/{}", baseDir.string(), texture.string());
#ifdef AXIOM_TRACE_IMPORT // ---------------------------------------------------
            NOVA_LOG("  Texture[{}]", path);
#endif // ----------------------------------------------------------------------
            imageData = stbi_load(
                path.c_str(),
                &width, &height, &channels, STBI_rgb_alpha);
            if (!imageData)
                NOVA_LOG("    STB Failed to load image: {}", path);

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

                auto _ops = textureOperations.find(outTexture);
                auto ops = _ops == textureOperations.end()
                    ? TextureOperations::None
                    : _ops->second;

                if (ops >= TextureOperations::ScanAlpha) {
                    // Find alpha values
                    for (i32 x = 0; x < width; ++x) {
                        for (i32 y = 0; y < height; ++y) {
                            u32 index = ((x * width) + y) * 4;
                            auto* pixel = outTexture->data.data() + index;
                            f32 alpha = f32(pixel[3]) / 255.f;
                            outTexture->minAlpha = std::min(alpha, outTexture->minAlpha);
                            outTexture->maxAlpha = std::max(alpha, outTexture->maxAlpha);
                        }
                    }
                }

                if (ops >= TextureOperations::FlipNormalZ) {
                    for (i32 x = 0; x < width; ++x) {
                        for (i32 y = 0; y < height; ++y) {
                            u32 index = ((x * width) + y) * 4;
                            auto* pixel = outTexture->data.data() + index;
                            pixel[2] = b8(255 - u8(pixel[2]));
                        }
                    }
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
        auto outMaterial = nova::Ref<Material>::Create();
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

        auto findImage = [&](Span<aiTextureType> texTypes) -> std::optional<Ref<TextureMap>> {
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
                texture = Ref<TextureMap>::Create();
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

            auto image = Ref<TextureMap>::Create();
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
        textureOperations[outMaterial->baseColor_alpha] |= TextureOperations::ScanAlpha;

        if (auto tex = findImage({ aiTextureType_NORMALS })) {
            outMaterial->normals = tex.value();
            if (settings.flipNormalMapZ) {
                textureOperations[outMaterial->normals] |= TextureOperations::FlipNormalZ;
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
#ifdef AXIOM_TRACE_IMPORT // ---------------------------------------------------
            NOVA_LOG("  - Mesh Instance: {:>{}} -> {}",
                node->mName.C_Str(),
                debugLongestNodeName,
                asset->mMeshes[node->mMeshes[i]]->mName.C_Str());
#endif // ----------------------------------------------------------------------
            importer.scene->instances.emplace_back(
                new MeshInstance{ {}, meshes[node->mMeshes[i]], transform });
        }

        for (u32 i = 0; i < node->mNumChildren; ++i) {
            ProcessNode(node->mChildren[i], transform);
        }
    }
}