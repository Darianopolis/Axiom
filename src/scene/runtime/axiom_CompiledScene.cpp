#include "axiom_CompiledScene.hpp"

namespace axiom
{
    void CompiledScene::Compile(imp::Scene& scene)
    {
        auto default_material = Ref<UVMaterial>::Create();
        materials.push_back(default_material);

        nova::HashMap<u32, u32> single_pixel_textures;

        auto CreatePixelImage = [&](Vec4 value) {
            std::array<u8, 4> data {
                u8(value.r * 255.f),
                u8(value.g * 255.f),
                u8(value.b * 255.f),
                u8(value.a * 255.f),
            };

            u32 encoded = std::bit_cast<u32>(data);

            if (single_pixel_textures.contains(encoded)) {
                return textures[single_pixel_textures.at(encoded)];
            }

            auto image = Ref<UVTexture>::Create();
            image->size = Vec2(1);
            image->data = { b8(data[0]), b8(data[1]), b8(data[2]), b8(data[3]) };

            textures.push_back(image);
            single_pixel_textures.insert({ encoded, u32(textures.size() - 1) });

            return image;
        };

        default_material->basecolor_alpha = CreatePixelImage({ 1.f, 0.f, 1.f, 1.f });
        default_material->normals = CreatePixelImage({ 0.5f, 0.5f, 1.f, 1.f });
        default_material->metalness_roughness = CreatePixelImage({ 0.f, 0.5f, 0.f, 1.f });
        default_material->emissivity = CreatePixelImage({ 0.f, 0.f, 0.f, 1.f });
        default_material->transmission = CreatePixelImage({ 0.f, 0.f, 0.f, 255.f });

        for (u32 i = 0; i < scene.geometry_ranges.count; ++i) {
            auto& range = scene.geometry_ranges[i];
            auto& geometry = scene.geometries[range.geometry_idx];

            u32 index_count = range.triangle_count * 3;
            u32 vertex_count = range.max_vertex + 1;

            auto out_mesh = Ref<TriMesh>::Create();
            meshes.emplace_back(out_mesh);

            out_mesh->indices.resize(index_count);
            out_mesh->position_attributes.resize(vertex_count);
            out_mesh->shading_attributes.resize(vertex_count);

            geometry.indices
                .Slice(range.first_index, index_count)
                .CopyTo({ out_mesh->indices.data(), index_count });

            geometry.positions
                .Slice(range.vertex_offset, vertex_count)
                .CopyTo({ out_mesh->position_attributes.data(), vertex_count });

            geometry.tangent_spaces
                .Slice(range.vertex_offset, vertex_count)
                .CopyTo({ (imp::Basis*)&out_mesh->shading_attributes[0].tangent_space, vertex_count, sizeof(out_mesh->shading_attributes[0]) });

            geometry.tex_coords
                .Slice(range.vertex_offset, vertex_count)
                .CopyTo({ (imp::Vec2<imp::Float16>*)&out_mesh->shading_attributes[0].tex_coords, vertex_count, sizeof(out_mesh->shading_attributes[0]) });

            out_mesh->sub_meshes.emplace_back(TriSubMesh {
                .vertex_offset = 0,
                .max_vertex = vertex_count - 1,
                .first_index = 0,
                .index_count = index_count,
                .material = default_material,
            });
        }

        for (u32 i = 0; i < scene.meshes.count; ++i) {
            auto& mesh = scene.meshes[i];

            auto instance = Ref<TriMeshInstance>::Create();
            instances.emplace_back(instance);

            instance->mesh = meshes[mesh.geometry_range_idx];
            instance->transform = Mat4(mesh.transform);
        }
    }
}