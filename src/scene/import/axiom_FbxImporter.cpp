#include "axiom_FbxImporter.hpp"

#include <ufbx.h>

namespace axiom
{
    FbxImporter::~FbxImporter()
    {
        ufbx_free(fbx);
    }

    void FbxImporter::Reset()
    {
        scene.Clear();
        fbxMeshOffsets.clear();
        textureIndices.clear();
        materialIndices.clear();
        triIndices.clear();
        uniqueVertices.clear();
        vertexIndices.clear();
    }

    Scene FbxImporter::Import(const std::filesystem::path& path)
    {
        Reset();
        dir = path.parent_path();

        ufbx_load_opts opts{};
        ufbx_error error;
        fbx = ufbx_load_file(path.string().c_str(), &opts, &error);

        scene.textures.resize(fbx->textures.count);
        for (u32 i = 0; i < fbx->textures.count; ++i) {
            ProcessTexture(i);
        }

        scene.materials.resize(fbx->materials.count);
        for (u32 i = 0; i < fbx->materials.count; ++i) {
            ProcessMaterial(i);
        }

        fbxMeshOffsets.resize(fbx->meshes.count);
        for (u32 i = 0; i < fbx->meshes.count; ++i) {
            auto mesh = fbx->meshes[i];
            fbxMeshOffsets[i].first = u32(scene.meshes.size());
            fbxMeshOffsets[i].second = u32(mesh->materials.count);
            for (u32 j = 0; j < mesh->materials.count; ++j) {
                ProcessMesh(i, j);
            }
        }

        ProcessNode(fbx->root_node, Mat4(1.f));

        return std::move(scene);
    }

    void FbxImporter::ProcessTexture(u32 texIdx)
    {
        auto& inTexture = fbx->textures[texIdx];
        auto& outTexture = scene.textures[texIdx];

        textureIndices[inTexture] = texIdx;

        if (inTexture->content.size > 0) {
            outTexture.data = ImageFileBuffer {
                .data = std::vector(
                    (const u8*)inTexture->content.data,
                    (const u8*)inTexture->content.data + inTexture->content.size),
            };
        } else if (inTexture->has_file) {
            outTexture.data = ImageFileURI(std::string(inTexture->filename.data, inTexture->filename.length));
        } else {
            NOVA_THROW("Non-file images not currently supported");
        }
    }

    void FbxImporter::ProcessMaterial(u32 matIdx)
    {
        auto& inMaterial = fbx->materials[matIdx];
        auto& outMaterial = scene.materials[matIdx];

        materialIndices[inMaterial] = matIdx;

        auto addProperty = [&](
                std::string_view name,
                const ufbx_material_map& map) {

            if (map.texture_enabled && map.texture) {
                outMaterial.properties.emplace_back(name, TextureSwizzle{ .textureIdx = u32(textureIndices[map.texture]) });
            }

            if (map.has_value) {
                switch (map.value_components) {
                    break;case 1: outMaterial.properties.emplace_back(name, f32(map.value_real));
                    break;case 2: outMaterial.properties.emplace_back(name, Vec2(f32(map.value_vec2.x), f32(map.value_vec2.y)));
                    break;case 3: outMaterial.properties.emplace_back(name, Vec3(f32(map.value_vec3.x), f32(map.value_vec3.y), f32(map.value_vec3.z)));
                    break;case 4: outMaterial.properties.emplace_back(name, Vec4(f32(map.value_vec4.x), f32(map.value_vec4.y), f32(map.value_vec4.z), f32(map.value_vec4.w)));
                    break;default: NOVA_THROW("Invalid number of value components: {}", map.value_components);
                }
            }
        };

        addProperty(property::BaseColor, inMaterial->pbr.base_color);
        addProperty(property::Normal,    inMaterial->fbx.normal_map);
        addProperty(property::Emissive,  inMaterial->pbr.emission_color);

        addProperty(property::Metallic,  inMaterial->pbr.metalness);
        addProperty(property::Roughness, inMaterial->pbr.roughness);

        addProperty(property::SpecularColor, inMaterial->fbx.specular_color);

        outMaterial.properties.emplace_back(property::AlphaMask, inMaterial->features.opacity.enabled);
    }

    void FbxImporter::ProcessMesh(u32 fbxMeshIdx, u32 primIdx)
    {
        auto& inMesh = fbx->meshes[fbxMeshIdx];
        auto& faces = inMesh->materials[primIdx];

        auto& outMesh = scene.meshes.emplace_back();
        outMesh.materialIdx = materialIndices[inMesh->materials[primIdx].material];

        triIndices.resize(inMesh->max_face_triangles * 3);
        uniqueVertices.clear();
        vertexIndices.clear();
        u32 vertexCount = 0;

        for (u32 i = 0; i < faces.face_indices.count; ++i) {
            auto faceIdx = faces.face_indices[i];
            auto face = inMesh->faces[faceIdx];
            u32 numTris = ufbx_triangulate_face(triIndices.data(), triIndices.size(), inMesh, face);

            for (u32 j = 0; j < numTris * 3; ++j) {
                u32 index = triIndices[j];

                FbxVertex v;
                {
                    auto pos = inMesh->vertex_position.values[inMesh->vertex_position.indices[index]];
                    v.pos = { pos.x, pos.y, pos.z };

                    if (inMesh->vertex_uv.exists) {
                        auto uv = inMesh->vertex_uv.values[inMesh->vertex_uv.indices[index]];
                        v.uv = { uv.x, uv.y };
                    }
                    if (inMesh->vertex_normal.exists) {
                        auto nrm = inMesh->vertex_normal.values[inMesh->vertex_normal.indices[index]];
                        v.nrm = { nrm.x, nrm.y, nrm.z };
                    }
                }

                auto& cached = uniqueVertices[v];
                if (cached.value == InvalidIndex) {
                    cached.value = vertexCount++;
                    vertexIndices.push_back(v);
                }
                outMesh.indices.push_back(cached.value);
            }
        }

        outMesh.positions.resize(vertexCount);
        for (u32 i = 0; i < vertexCount; ++i) {
            outMesh.positions[i] = vertexIndices[i].pos;
        }

        if (inMesh->vertex_uv.exists) {
            outMesh.texCoords.resize(vertexCount);
            for (u32 i = 0; i < vertexCount; ++i) {
                outMesh.texCoords[i] = vertexIndices[i].uv;
            }
        }

        if (inMesh->vertex_normal.exists) {
            outMesh.normals.resize(vertexCount);
            for (u32 i = 0; i < vertexCount; ++i) {
                outMesh.normals[i] = vertexIndices[i].nrm;
            }
        }
    }

    void FbxImporter::ProcessNode(ufbx_node* inNode, Mat4 parentTransform)
    {
        auto fbxt = inNode->local_transform;
        Mat4 transform = Mat4(1.f);
        {
            auto tv = fbxt.translation;
            auto tr = fbxt.rotation;
            auto ts = fbxt.scale;
            auto t = glm::translate(Mat4(1.f), Vec3(f32(tv.x), f32(tv.y), f32(tv.z)));
            auto r = glm::mat4_cast(Quat(f32(tr.w), f32(tr.x), f32(tr.y), f32(tr.z)));
            auto s = glm::scale(Mat4(1.f), Vec3(f32(ts.x), f32(ts.y), f32(ts.z)));
            transform = t * r * s;
        }
        transform = parentTransform * transform;

        if (inNode->mesh) {
            auto meshIter = std::ranges::find(fbx->meshes, inNode->mesh);
            if (meshIter == fbx->meshes.end()) {
                NOVA_THROW("Could not find node {} in meshes!", (void*)inNode->mesh);
            }

            auto[meshIdx, meshCount] = fbxMeshOffsets[u32(std::distance(fbx->meshes.begin(), meshIter))];
            for (u32 i = 0; i < meshCount; ++i) {
                scene.instances.emplace_back(Instance {
                    .meshIdx = meshIdx + i,
                    .transform = transform,
                });
            }
        }

        for (auto* child : inNode->children) {
            ProcessNode(child, transform);
        }
    }
}