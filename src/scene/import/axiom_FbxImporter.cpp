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
        fbx_mesh_offsets.clear();
        texture_indices.clear();
        material_indices.clear();
        tri_indices.clear();
        unique_vertices.clear();
        vertex_indices.clear();
    }

    scene_ir::Scene FbxImporter::Import(const std::filesystem::path& path)
    {
        Reset();
        dir = path.parent_path();

        ufbx_load_opts opts{};
        ufbx_error error;
        NOVA_LOGEXPR(path.string());
        fbx = ufbx_load_file(path.string().c_str(), &opts, &error);

        scene.textures.resize(fbx->textures.count);
        for (u32 i = 0; i < fbx->textures.count; ++i) {
            ProcessTexture(i);
        }

        scene.materials.resize(fbx->materials.count);
        for (u32 i = 0; i < fbx->materials.count; ++i) {
            ProcessMaterial(i);
        }

        fbx_mesh_offsets.resize(fbx->meshes.count);
        for (u32 i = 0; i < fbx->meshes.count; ++i) {
            auto mesh = fbx->meshes[i];
            fbx_mesh_offsets[i].first = u32(scene.meshes.size());
            fbx_mesh_offsets[i].second = u32(mesh->materials.count);
            for (u32 j = 0; j < mesh->materials.count; ++j) {
                ProcessMesh(i, j);
            }
        }

        ProcessNode(fbx->root_node, Mat4(1.f));

        return std::move(scene);
    }

    void FbxImporter::ProcessTexture(u32 tex_idx)
    {
        auto& in_texture = fbx->textures[tex_idx];
        auto& out_texture = scene.textures[tex_idx];

        texture_indices[in_texture] = tex_idx;

        if (in_texture->content.size > 0) {
            out_texture.data = scene_ir::ImageFileBuffer {
                .data = std::vector(
                    (const u8*)in_texture->content.data,
                    (const u8*)in_texture->content.data + in_texture->content.size),
            };
        } else if (in_texture->has_file) {
            out_texture.data = scene_ir::ImageFileURI(std::string(in_texture->filename.data, in_texture->filename.length));
        } else {
            NOVA_THROW("Non-file images not currently supported");
        }
    }

    void FbxImporter::ProcessMaterial(u32 mat_idx)
    {
        auto& in_material = fbx->materials[mat_idx];
        auto& out_material = scene.materials[mat_idx];

        material_indices[in_material] = mat_idx;

        auto AddProperty = [&](
                std::string_view name,
                const ufbx_material_map& map) {

            if (map.texture_enabled && map.texture) {
                out_material.properties.emplace_back(name, scene_ir::TextureSwizzle{ .texture_idx = u32(texture_indices[map.texture]) });
            }

            if (map.has_value) {
                switch (map.value_components) {
                    break;case 1: out_material.properties.emplace_back(name, f32(map.value_real));
                    break;case 2: out_material.properties.emplace_back(name, Vec2(f32(map.value_vec2.x), f32(map.value_vec2.y)));
                    break;case 3: out_material.properties.emplace_back(name, Vec3(f32(map.value_vec3.x), f32(map.value_vec3.y), f32(map.value_vec3.z)));
                    break;case 4: out_material.properties.emplace_back(name, Vec4(f32(map.value_vec4.x), f32(map.value_vec4.y), f32(map.value_vec4.z), f32(map.value_vec4.w)));
                    break;default: NOVA_THROW("Invalid number of value components: {}", map.value_components);
                }
            }
        };

        AddProperty(scene_ir::property::BaseColor, in_material->pbr.base_color);
        AddProperty(scene_ir::property::Normal,    in_material->fbx.normal_map);
        // AddProperty(property::Normal,    in_material->pbr.normal_map);
        // AddProperty(property::Normal,    in_material->fbx.bump);
        AddProperty(scene_ir::property::Emissive,  in_material->pbr.emission_color);

        AddProperty(scene_ir::property::Metallic,  in_material->pbr.metalness);
        AddProperty(scene_ir::property::Roughness, in_material->pbr.roughness);

        AddProperty(scene_ir::property::SpecularColor, in_material->fbx.specular_color);

        out_material.properties.emplace_back(scene_ir::property::AlphaMask, in_material->features.opacity.enabled);
    }

    void FbxImporter::ProcessMesh(u32 fbx_mesh_idx, u32 prim_idx)
    {
        auto& in_mesh = fbx->meshes[fbx_mesh_idx];
        auto& faces = in_mesh->materials[prim_idx];

        auto& out_mesh = scene.meshes.emplace_back();
        out_mesh.material_idx = material_indices[in_mesh->materials[prim_idx].material];

        tri_indices.resize(in_mesh->max_face_triangles * 3);
        unique_vertices.clear();
        vertex_indices.clear();
        u32 vertex_count = 0;

        for (u32 i = 0; i < faces.face_indices.count; ++i) {
            auto face_idx = faces.face_indices[i];
            auto face = in_mesh->faces[face_idx];
            u32 num_tris = ufbx_triangulate_face(tri_indices.data(), tri_indices.size(), in_mesh, face);

            for (u32 j = 0; j < num_tris * 3; ++j) {
                u32 index = tri_indices[j];

                FbxVertex v;
                {
                    auto pos = in_mesh->vertex_position.values[in_mesh->vertex_position.indices[index]];
                    v.pos = { pos.x, pos.y, pos.z };

                    if (in_mesh->vertex_uv.exists) {
                        auto uv = in_mesh->vertex_uv.values[in_mesh->vertex_uv.indices[index]];
                        v.uv = { uv.x, uv.y };
                    }
                    if (in_mesh->vertex_normal.exists) {
                        auto nrm = in_mesh->vertex_normal.values[in_mesh->vertex_normal.indices[index]];
                        v.nrm = { nrm.x, nrm.y, nrm.z };
                    }
                }

                auto& cached = unique_vertices[v];
                if (cached.value == scene_ir::InvalidIndex) {
                    cached.value = vertex_count++;
                    vertex_indices.push_back(v);
                }
                out_mesh.indices.push_back(cached.value);
            }
        }

        out_mesh.positions.resize(vertex_count);
        for (u32 i = 0; i < vertex_count; ++i) {
            out_mesh.positions[i] = vertex_indices[i].pos;
        }

        if (in_mesh->vertex_uv.exists) {
            out_mesh.tex_coords.resize(vertex_count);
            for (u32 i = 0; i < vertex_count; ++i) {
                out_mesh.tex_coords[i] = vertex_indices[i].uv;
            }
        }

        if (in_mesh->vertex_normal.exists) {
            out_mesh.normals.resize(vertex_count);
            for (u32 i = 0; i < vertex_count; ++i) {
                out_mesh.normals[i] = vertex_indices[i].nrm;
            }
        }
    }

    void FbxImporter::ProcessNode(ufbx_node* in_node, Mat4 parent_transform)
    {
        auto fbx_tform = in_node->local_transform;
        Mat4 transform = Mat4(1.f);
        {
            auto tv = fbx_tform.translation;
            auto tr = fbx_tform.rotation;
            auto ts = fbx_tform.scale;
            auto t = glm::translate(Mat4(1.f), Vec3(f32(tv.x), f32(tv.y), f32(tv.z)));
            auto r = glm::mat4_cast(Quat(f32(tr.w), f32(tr.x), f32(tr.y), f32(tr.z)));
            auto s = glm::scale(Mat4(1.f), Vec3(f32(ts.x), f32(ts.y), f32(ts.z)));
            transform = t * r * s;
        }
        transform = parent_transform * transform;

        if (in_node->mesh) {
            auto mesh_iter = std::ranges::find(fbx->meshes, in_node->mesh);
            if (mesh_iter == fbx->meshes.end()) {
                NOVA_THROW("Could not find node {} in meshes!", (void*)in_node->mesh);
            }

            auto[mesh_idx, mesh_count] = fbx_mesh_offsets[u32(std::distance(fbx->meshes.begin(), mesh_iter))];
            for (u32 i = 0; i < mesh_count; ++i) {
                scene.instances.emplace_back(scene_ir::Instance {
                    .mesh_idx = mesh_idx + i,
                    .transform = transform,
                });
            }
        }

        for (auto* child : in_node->children) {
            ProcessNode(child, transform);
        }
    }
}