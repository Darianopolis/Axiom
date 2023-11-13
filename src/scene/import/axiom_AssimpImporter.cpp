#include "axiom_AssimpImporter.hpp"

#include <nova/core/nova_Containers.hpp>

namespace axiom
{
    void AssimpImporter::Reset()
    {
        scene.Clear();
        assimp.FreeScene();
    }

    scene_ir::Scene AssimpImporter::Import(const std::filesystem::path& path)
    {
        u32 aiFlags = 0;
        aiFlags |= aiProcess_JoinIdenticalVertices;
        aiFlags |= aiProcess_Triangulate;
        aiFlags |= aiProcess_SortByPType;

        aiFlags |= aiProcess_FindInvalidData;

        aiFlags |= aiProcess_GenUVCoords;
        aiFlags |= aiProcess_TransformUVCoords;

        dir = path.parent_path();
        asset = assimp.ReadFile(path.string(), aiFlags);

        if (!asset) {
            NOVA_THROW("ASSIMP: Error loading [{}]: {}", path.string(), assimp.GetErrorString());
        }

        // Textures

        scene.textures.resize(asset->mNumTextures);
        for (u32 i = 0; i < asset->mNumTextures; ++i) {
            ProcessTexture(i);
        }

        // Materials

        scene.materials.resize(asset->mNumMaterials);
        for (u32 i = 0; i < asset->mNumMaterials; ++i) {
            ProcessMaterial(i);
        }

        // Meshes

        scene.meshes.resize(asset->mNumMeshes);
        for (u32 i = 0; i < asset->mNumMeshes; ++i) {
            ProcessMesh(i);
        }

        // Nodes

        ProcessNode(asset->mRootNode, Mat4(1.f));

        // ----

        return std::move(scene);
    }

    void AssimpImporter::ProcessTexture(u32 textureIndex)
    {
        auto& inTexture = asset->mTextures[textureIndex];
        auto& outTexture = scene.textures[textureIndex];

        NOVA_LOG("Texture[{}]: {}", textureIndex, inTexture->mFilename.C_Str());
        NOVA_LOG("  size = ({}, {})", inTexture->mWidth, inTexture->mHeight);
        NOVA_LOG("  format hint: {:.9s}",  inTexture->achFormatHint);
        NOVA_LOG("  pTexels: {}", (void*)inTexture->pcData);
        NOVA_LOG("  magic: {:.4s}", (char*)inTexture->pcData);

        if (inTexture->pcData) {
            if (inTexture->mHeight == 0) {
                // Compressed file contents stored inline

                scene_ir::ImageFileBuffer buffer;

                buffer.data.resize(inTexture->mWidth);
                std::memcpy(buffer.data.data(), inTexture->pcData, inTexture->mWidth);

                outTexture.data = std::move(buffer);

            } else {
                // Texel data

                scene_ir::ImageBuffer buffer;
                buffer.size = { inTexture->mWidth, inTexture->mHeight };
                buffer.format = scene_ir::BufferFormat::RGBA8;
                buffer.data.resize(inTexture->mWidth);
                std::memcpy(buffer.data.data(), inTexture->pcData, inTexture->mWidth);

                outTexture.data = std::move(buffer);
            }
        } else {
            outTexture.data = scene_ir::ImageFileURI(std::format("{}/{}", dir.string(), inTexture->mFilename.C_Str()));
        }
    }

    void AssimpImporter::ProcessMaterial(u32 materialIndex)
    {
        auto& inMaterial = asset->mMaterials[materialIndex];
        auto& outMaterial = scene.materials[materialIndex];

        auto findTexture = [&](nova::Span<aiTextureType> texTypes) -> std::optional<u32> {
            for (auto type : texTypes) {
                if (inMaterial->GetTextureCount(type) > 0) {
                    aiString str;
                    auto res = inMaterial->GetTexture(type, 0, &str);
                    if (res == aiReturn_SUCCESS) {
                        if (str.data[0] == '*') {
                            return std::atoi(str.C_Str() + 1);
                        } else {
                            auto path = std::format("{}/{}", dir.string(), str.C_Str());
                            auto& textureIndex = textureIndices[path];
                            if (textureIndex.value == scene_ir::InvalidIndex) {
                                textureIndex.value = u32(scene.textures.size());
                                scene.textures.emplace_back(scene_ir::ImageFileURI(path));
                            }
                            return textureIndex.value;
                        }
                    }
                }
            }
            return std::nullopt;
        };

        {
            NOVA_LOG("Material[{}]: {}", materialIndex, inMaterial->GetName().C_Str());

            auto debugTexture = [&](aiTextureType type, const char* name) {
                auto index = findTexture({ type });
                if (index) {
                    NOVA_LOG("    {}: {}", name, index.value());
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
            for (uint32_t i = 0; i < inMaterial->mNumProperties; ++i) {
                aiMaterialProperty* property = inMaterial->mProperties[i];
                std::string data;
                if (property->mType == aiPropertyTypeInfo::aiPTI_String) {
                    aiString str;
                    inMaterial->Get(property->mKey.C_Str(), property->mType, property->mIndex, str);
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
        }

        (void)outMaterial;

        // auto addChannel = [&](
        //         ChannelType type,
        //         Span<std::pair<aiTextureType, Span<i8>>> texture,
        //         Span<std::pair<const char*, Span<i8>>> properties) -> bool {

        //     Property channel;
        //     channel.type = type;

        //     bool foundTexture = false;
        //     for (auto[texType, swizzle] : texture) {
        //         auto textureIndex = findTexture({ texType });
        //         if (!textureIndex) continue;

        //         channel.texture.textureIdx = textureIndex.value();
        //         for (u32 i = 0; i < swizzle.size(); ++i) {
        //             channel.texture.channels[i] = swizzle[i];
        //         }

        //         foundTexture = true;
        //         break;
        //     }

        //     bool foundProperty = false;
        //     for (auto[propName, swizzle] : properties) {

        //         Vec4 value{ 0.f, 0.f, 0.f, 1.f };

        //         for (u32 i = 0; i < inMaterial->mNumProperties; ++i) {
        //             auto* property = inMaterial->mProperties[i];
        //             if (strcmp(property->mKey.C_Str(), propName))
        //                 continue;

        //             if (property->mType == aiPropertyTypeInfo::aiPTI_Double) {
        //                 for (u32 j = 0; j < property->mDataLength / 8; ++j) {
        //                     value[j] = f32(((f64*)property->mData)[j]);
        //                 }
        //             } else if (property->mType == aiPropertyTypeInfo::aiPTI_Float) {
        //                 for (u32 j = 0; j < property->mDataLength / 4; ++j) {
        //                     value[j] = ((f32*)property->mData)[j];
        //                 }
        //             } else {
        //                 NOVA_THROW("ASSIMP: Property[{}] has invalid type: {}",
        //                     property->mKey.C_Str(), u32(property->mType));
        //             }

        //             foundProperty = true;
        //             break;
        //         }

        //         if (!foundProperty)
        //             continue;

        //         for (u32 i = 0; i < swizzle.size(); ++i) {
        //             channel.value[i] = value[swizzle[i]];
        //         }

        //         break;
        //     }

        //     if (foundTexture || foundProperty) {

        //         outMaterial.properties.push_back(channel);

        //         return true;
        //     } else {
        //         return false;
        //     }
        // };

        // addChannel(ChannelType::BaseColor, {
        //         { aiTextureType_BASE_COLOR, { 0, 1, 2, 3 } },
        //         { aiTextureType_DIFFUSE,    { 0, 1, 2, 3 } },
        //     }, {
        //         { "$clr.base",    { 0, 1, 2, 3 } },
        //         { "$clr.diffuse", { 0, 1, 2, 3 } },
        //     });

        // addChannel(ChannelType::Normal,
        //     {{ aiTextureType_NORMALS, { 0, 1, 2 } }},
        //     {});

        // addChannel(ChannelType::Metalness,
        //     {{ aiTextureType_METALNESS, { 0 } }},
        //     {{ "$mat.metallicFactor",   { 0 } }});

        // addChannel(ChannelType::Roughness,
        //     {{ aiTextureType_DIFFUSE_ROUGHNESS, { 0 } }},
        //     {{ "$mat.roughnessFactor",          { 0 } }});

        // addChannel(ChannelType::Emissive,
        //     {{ aiTextureType_EMISSIVE, { 0, 1, 2 } }},
        //     {{ "$clr.emissive",        { 0, 1, 2 } }});
    }

    void AssimpImporter::ProcessMesh(u32 meshIndex)
    {
        auto& inMesh = asset->mMeshes[meshIndex];
        auto& outMesh = scene.meshes[meshIndex];

        if (!inMesh->HasPositions()) {
            NOVA_LOG("Mesh [{}] has no positions, skipping...", inMesh->mName.C_Str());
            return;
        }

        NOVA_LOG("Mesh[{}]: {}", meshIndex, inMesh->mName.C_Str());
        NOVA_LOG("  vertices = {}", inMesh->mNumVertices);
        NOVA_LOG("  faces: {}",  inMesh->mNumFaces);

        outMesh.materialIdx = inMesh->mMaterialIndex;

        // Indices

        if (inMesh->mNumFaces > 0) {
            outMesh.indices.resize(inMesh->mNumFaces * 3);
            for (u32 i = 0; i < inMesh->mNumFaces; ++i) {
                if (inMesh->mFaces[i].mNumIndices != 3) {
                    NOVA_THROW("Invalid face, num indices = {}", inMesh->mFaces[i].mNumIndices);
                }
                outMesh.indices[i * 3 + 0] = inMesh->mFaces[i].mIndices[0];
                outMesh.indices[i * 3 + 1] = inMesh->mFaces[i].mIndices[1];
                outMesh.indices[i * 3 + 2] = inMesh->mFaces[i].mIndices[2];
            }
        } else {
            u32 safeIndices = inMesh->mNumVertices - (inMesh->mNumVertices % 3);
            outMesh.indices.resize(safeIndices);
            for (u32 i = 0; i < safeIndices; ++i) {
                outMesh.indices[i] = i;
                NOVA_LOGEXPR(i);
            }
        }

        // Positions

        outMesh.positions.resize(inMesh->mNumVertices);
        for (u32 i = 0; i < inMesh->mNumVertices; ++i) {
            auto pos = inMesh->mVertices[i];
            outMesh.positions[i] = Vec3(pos.x, pos.y, pos.z);
        }

        // Normals

        if (inMesh->HasNormals()) {
            outMesh.normals.resize(inMesh->mNumVertices);
            for (u32 i = 0; i < inMesh->mNumVertices; ++i) {
                auto nrm = inMesh->mNormals[i];
                outMesh.normals[i] = Vec3(nrm.x, nrm.y, nrm.z);
            }
        }

        // Tex Coords

        if (inMesh->HasTextureCoords(0)) {
            outMesh.texCoords.resize(inMesh->mNumVertices);
            for (u32 i = 0; i < inMesh->mNumVertices; ++i) {
                auto uv = inMesh->mTextureCoords[0][i];
                outMesh.texCoords[i] = Vec2(uv.x, uv.y);
            }
        }
    }

    void AssimpImporter::ProcessNode(aiNode* node, Mat4 parentTransform)
    {
        auto transform = std::bit_cast<Mat4>(node->mTransformation);
        transform = glm::transpose(transform);
        transform = parentTransform * transform;

        for (u32 i = 0; i < node->mNumMeshes; ++i) {
            if (scene.meshes[node->mMeshes[i]].indices.empty())
                continue;

            scene.instances.push_back(scene_ir::Instance {
                .meshIdx = node->mMeshes[i],
                .transform = transform,
            });
        }

        for (u32 i = 0; i < node->mNumChildren; ++i) {
            ProcessNode(node->mChildren[i], transform);
        }
    }
}