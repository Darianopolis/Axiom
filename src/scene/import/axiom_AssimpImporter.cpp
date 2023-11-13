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
        u32 ai_flags = 0;
        ai_flags |= aiProcess_JoinIdenticalVertices;
        ai_flags |= aiProcess_Triangulate;
        ai_flags |= aiProcess_SortByPType;

        ai_flags |= aiProcess_FindInvalidData;

        ai_flags |= aiProcess_GenUVCoords;
        ai_flags |= aiProcess_TransformUVCoords;

        dir = path.parent_path();
        asset = assimp.ReadFile(path.string(), ai_flags);

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

    void AssimpImporter::ProcessTexture(u32 texture_index)
    {
        auto& in_texture = asset->mTextures[texture_index];
        auto& out_texture = scene.textures[texture_index];

        NOVA_LOG("Texture[{}]: {}", texture_index, in_texture->mFilename.C_Str());
        NOVA_LOG("  size = ({}, {})", in_texture->mWidth, in_texture->mHeight);
        NOVA_LOG("  format hint: {:.9s}",  in_texture->achFormatHint);
        NOVA_LOG("  texels: {}", (void*)in_texture->pcData);
        NOVA_LOG("  magic: {:.4s}", (char*)in_texture->pcData);

        if (in_texture->pcData) {
            if (in_texture->mHeight == 0) {
                // Compressed file contents stored inline

                scene_ir::ImageFileBuffer buffer;

                buffer.data.resize(in_texture->mWidth);
                std::memcpy(buffer.data.data(), in_texture->pcData, in_texture->mWidth);

                out_texture.data = std::move(buffer);

            } else {
                // Texel data

                scene_ir::ImageBuffer buffer;
                buffer.size = { in_texture->mWidth, in_texture->mHeight };
                buffer.format = scene_ir::BufferFormat::RGBA8;
                buffer.data.resize(in_texture->mWidth);
                std::memcpy(buffer.data.data(), in_texture->pcData, in_texture->mWidth);

                out_texture.data = std::move(buffer);
            }
        } else {
            out_texture.data = scene_ir::ImageFileURI(std::format("{}/{}", dir.string(), in_texture->mFilename.C_Str()));
        }
    }

    void AssimpImporter::ProcessMaterial(u32 material_index)
    {
        auto& in_material = asset->mMaterials[material_index];
        auto& out_material = scene.materials[material_index];

        auto FindTexture = [&](nova::Span<aiTextureType> tex_types) -> std::optional<u32> {
            for (auto type : tex_types) {
                if (in_material->GetTextureCount(type) > 0) {
                    aiString str;
                    auto res = in_material->GetTexture(type, 0, &str);
                    if (res == aiReturn_SUCCESS) {
                        if (str.data[0] == '*') {
                            return std::atoi(str.C_Str() + 1);
                        } else {
                            auto path = std::format("{}/{}", dir.string(), str.C_Str());
                            auto& texture_index = texture_indices[path];
                            if (texture_index.value == scene_ir::InvalidIndex) {
                                texture_index.value = u32(scene.textures.size());
                                scene.textures.emplace_back(scene_ir::ImageFileURI(path));
                            }
                            return texture_index.value;
                        }
                    }
                }
            }
            return std::nullopt;
        };

        {
            NOVA_LOG("Material[{}]: {}", material_index, in_material->GetName().C_Str());

            auto DebugTexture = [&](aiTextureType type, const char* name) {
                auto index = FindTexture({ type });
                if (index) {
                    NOVA_LOG("    {}: {}", name, index.value());
                }
            };

            DebugTexture(aiTextureType_NONE, "None");
            DebugTexture(aiTextureType_DIFFUSE, "Diffuse");
            DebugTexture(aiTextureType_SPECULAR, "Specular");
            DebugTexture(aiTextureType_AMBIENT, "Ambient");
            DebugTexture(aiTextureType_EMISSIVE, "Emissive");
            DebugTexture(aiTextureType_HEIGHT, "Height");
            DebugTexture(aiTextureType_NORMALS, "Normals");
            DebugTexture(aiTextureType_SHININESS, "Shininess");
            DebugTexture(aiTextureType_OPACITY, "Opacity");
            DebugTexture(aiTextureType_DISPLACEMENT, "Displacement");
            DebugTexture(aiTextureType_LIGHTMAP, "Lightmap");
            DebugTexture(aiTextureType_REFLECTION, "Reflection");
            DebugTexture(aiTextureType_BASE_COLOR, "Base Color");
            DebugTexture(aiTextureType_NORMAL_CAMERA, "Normal Camera");
            DebugTexture(aiTextureType_EMISSION_COLOR, "Emission Color");
            DebugTexture(aiTextureType_METALNESS, "Metalness");
            DebugTexture(aiTextureType_DIFFUSE_ROUGHNESS, "Diffuse Roughness");
            DebugTexture(aiTextureType_AMBIENT_OCCLUSION, "Ambient occlusion");
            DebugTexture(aiTextureType_SHEEN, "Sheen");
            DebugTexture(aiTextureType_CLEARCOAT, "Clearcoat");
            DebugTexture(aiTextureType_TRANSMISSION, "Tranmission");
            DebugTexture(aiTextureType_UNKNOWN, "Unknown");

            std::unordered_map<std::string, int> property_indexes;
            for (uint32_t i = 0; i < in_material->mNumProperties; ++i) {
                aiMaterialProperty* property = in_material->mProperties[i];
                std::string data;
                if (property->mType == aiPropertyTypeInfo::aiPTI_String) {
                    aiString str;
                    in_material->Get(property->mKey.C_Str(), property->mType, property->mIndex, str);
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
                        int index = property_indexes[property->mKey.C_Str()]++;
                        std::cout << "    Property[" << index << "]." << property->mKey.C_Str() << " = " << data << "\n";
                    }
                }
            }
        }

        (void)out_material;
    }

    void AssimpImporter::ProcessMesh(u32 mesh_index)
    {
        auto& in_mesh = asset->mMeshes[mesh_index];
        auto& out_mesh = scene.meshes[mesh_index];

        if (!in_mesh->HasPositions()) {
            NOVA_LOG("Mesh [{}] has no positions, skipping...", in_mesh->mName.C_Str());
            return;
        }

        NOVA_LOG("Mesh[{}]: {}", mesh_index, in_mesh->mName.C_Str());
        NOVA_LOG("  vertices = {}", in_mesh->mNumVertices);
        NOVA_LOG("  faces: {}",  in_mesh->mNumFaces);

        out_mesh.material_idx = in_mesh->mMaterialIndex;

        // Indices

        if (in_mesh->mNumFaces > 0) {
            out_mesh.indices.resize(in_mesh->mNumFaces * 3);
            for (u32 i = 0; i < in_mesh->mNumFaces; ++i) {
                if (in_mesh->mFaces[i].mNumIndices != 3) {
                    NOVA_THROW("Invalid face, num indices = {}", in_mesh->mFaces[i].mNumIndices);
                }
                out_mesh.indices[i * 3 + 0] = in_mesh->mFaces[i].mIndices[0];
                out_mesh.indices[i * 3 + 1] = in_mesh->mFaces[i].mIndices[1];
                out_mesh.indices[i * 3 + 2] = in_mesh->mFaces[i].mIndices[2];
            }
        } else {
            u32 safe_indices = in_mesh->mNumVertices - (in_mesh->mNumVertices % 3);
            out_mesh.indices.resize(safe_indices);
            for (u32 i = 0; i < safe_indices; ++i) {
                out_mesh.indices[i] = i;
                NOVA_LOGEXPR(i);
            }
        }

        // Positions

        out_mesh.positions.resize(in_mesh->mNumVertices);
        for (u32 i = 0; i < in_mesh->mNumVertices; ++i) {
            auto pos = in_mesh->mVertices[i];
            out_mesh.positions[i] = Vec3(pos.x, pos.y, pos.z);
        }

        // Normals

        if (in_mesh->HasNormals()) {
            out_mesh.normals.resize(in_mesh->mNumVertices);
            for (u32 i = 0; i < in_mesh->mNumVertices; ++i) {
                auto nrm = in_mesh->mNormals[i];
                out_mesh.normals[i] = Vec3(nrm.x, nrm.y, nrm.z);
            }
        }

        // Tex Coords

        if (in_mesh->HasTextureCoords(0)) {
            out_mesh.tex_coords.resize(in_mesh->mNumVertices);
            for (u32 i = 0; i < in_mesh->mNumVertices; ++i) {
                auto uv = in_mesh->mTextureCoords[0][i];
                out_mesh.tex_coords[i] = Vec2(uv.x, uv.y);
            }
        }
    }

    void AssimpImporter::ProcessNode(aiNode* node, Mat4 parent_transform)
    {
        auto transform = std::bit_cast<Mat4>(node->mTransformation);
        transform = glm::transpose(transform);
        transform = parent_transform * transform;

        for (u32 i = 0; i < node->mNumMeshes; ++i) {
            if (scene.meshes[node->mMeshes[i]].indices.empty())
                continue;

            scene.instances.push_back(scene_ir::Instance {
                .mesh_idx = node->mMeshes[i],
                .transform = transform,
            });
        }

        for (u32 i = 0; i < node->mNumChildren; ++i) {
            ProcessNode(node->mChildren[i], transform);
        }
    }
}