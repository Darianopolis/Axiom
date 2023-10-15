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

        auto addChannel = [&](
                ChannelType type,
                const ufbx_material_map& map,
                Span<i8> channels) {

            if (!map.has_value && !map.texture_enabled) {
                return;
            }

            Channel channel{ type };
            if (map.texture_enabled && map.texture) {
                channel.texture.textureIdx = u32(textureIndices[map.texture]);
                for (u32 i = 0; i < channels.size(); ++i)
                    channel.texture.channels[i] = channels[i];
            }

            for (u32 i = 0; i < channels.size(); ++i)
                channel.value[i] = f32(reinterpret_cast<const f64*>(&map.value_vec4)[channels[i]]);

            outMaterial.channels.emplace_back(std::move(channel));
        };

        addChannel(ChannelType::BaseColor, inMaterial->pbr.base_color, { 0, 1, 2, 3 });
        addChannel(ChannelType::Normal, inMaterial->fbx.normal_map, { 0, 1, 2 });
        addChannel(ChannelType::Emissive, inMaterial->pbr.emission_color, { 0, 1, 2 });

        // TODO: This needs to be handled per-model
        addChannel(ChannelType::Metalness, inMaterial->pbr.specular_color, { 2 });
        addChannel(ChannelType::Roughness, inMaterial->pbr.specular_color, { 1 });

        // addChannel(ChannelType::Specular, inMaterial->fbx.specular_color, { 0, 1, 2 });
        // addChannel(ChannelType::Metalness, inMaterial->pbr.metalness, { 0 });
        // addChannel(ChannelType::Roughness, inMaterial->pbr.roughness, { 0 });

        outMaterial.alphaMask = inMaterial->features.opacity.enabled;
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