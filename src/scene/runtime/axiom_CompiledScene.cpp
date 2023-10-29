#include "axiom_CompiledScene.hpp"

namespace axiom
{
    void CompiledScene::Compile(imp::Scene& scene)
    {
        auto defaultMaterial = Ref<UVMaterial>::Create();
        materials.push_back(defaultMaterial);

        nova::HashMap<u32, u32> singlePixelTextures;

        auto createPixelImage = [&](Vec4 value) {
            std::array<u8, 4> data {
                u8(value.r * 255.f),
                u8(value.g * 255.f),
                u8(value.b * 255.f),
                u8(value.a * 255.f),
            };

            u32 encoded = std::bit_cast<u32>(data);

            if (singlePixelTextures.contains(encoded)) {
                return textures[singlePixelTextures.at(encoded)];
            }

            auto image = Ref<UVTexture>::Create();
            image->size = Vec2(1);
            image->data = { b8(data[0]), b8(data[1]), b8(data[2]), b8(data[3]) };

            textures.push_back(image);
            singlePixelTextures.insert({ encoded, u32(textures.size() - 1) });

            return image;
        };

        defaultMaterial->baseColor_alpha = createPixelImage({ 1.f, 0.f, 1.f, 1.f });
        defaultMaterial->normals = createPixelImage({ 0.5f, 0.5f, 1.f, 1.f });
        defaultMaterial->metalness_roughness = createPixelImage({ 0.f, 0.5f, 0.f, 1.f });
        defaultMaterial->emissivity = createPixelImage({ 0.f, 0.f, 0.f, 1.f });
        defaultMaterial->transmission = createPixelImage({ 0.f, 0.f, 0.f, 255.f });

        for (u32 i = 0; i < scene.geometry_ranges.count; ++i) {
            auto& range = scene.geometry_ranges[i];
            auto& geometry = scene.geometries[range.geometry_idx];

            u32 index_count = range.triangle_count * 3;
            u32 vertex_count = range.max_vertex + 1;

            auto outMesh = Ref<TriMesh>::Create();
            meshes.emplace_back(outMesh);

            outMesh->indices.resize(index_count);
            outMesh->positionAttributes.resize(vertex_count);
            outMesh->shadingAttributes.resize(vertex_count);

            geometry.indices
                .Slice(range.first_index, index_count)
                .CopyTo({ outMesh->indices.data(), index_count });

            geometry.positions
                .Slice(range.vertex_offset, vertex_count)
                .CopyTo({ outMesh->positionAttributes.data(), vertex_count });

            geometry.tangent_spaces
                .Slice(range.vertex_offset, vertex_count)
                .CopyTo({ (imp::Basis*)&outMesh->shadingAttributes[0].tangentSpace, vertex_count, sizeof(outMesh->shadingAttributes[0]) });

            geometry.tex_coords
                .Slice(range.vertex_offset, vertex_count)
                .CopyTo({ (imp::Vec2<imp::Float16>*)&outMesh->shadingAttributes[0].texCoords, vertex_count, sizeof(outMesh->shadingAttributes[0]) });

            outMesh->subMeshes.emplace_back(TriSubMesh {
                .vertexOffset = 0,
                .maxVertex = vertex_count - 1,
                .firstIndex = 0,
                .indexCount = index_count,
                .material = defaultMaterial,
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