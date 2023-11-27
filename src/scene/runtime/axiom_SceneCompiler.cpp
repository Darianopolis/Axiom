#include "axiom_SceneCompiler.hpp"

namespace axiom
{
    void SceneCompiler::Compile(scene_ir::Scene& in_scene, CompiledScene& out_scene)
    {
        auto default_material = Ref<UVMaterial>::Create();
        out_scene.materials.push_back(default_material);

        // BaseColor + Alpha = BC7
        // Normals           = BC5
        // Metal     + Rough = BC5
        // Emissivity        = BC6h
        // Transmission      = BC4

        u32 texture_offset = u32(out_scene.textures.size());

        out_scene.textures.resize(texture_offset + in_scene.textures.size());
#pragma omp parallel for
        for (u32 i = 0; i < in_scene.textures.size(); ++i) {
            auto& in_texture = in_scene.textures[i];
            auto out_texture = Ref<UVTexture>::Create();
            out_scene.textures[texture_offset + i] = out_texture;

            ImageProcess processes = {};
            if (flip_normal_map_z) {
                for (auto& material : in_scene.materials) {
                    // TODO: This should follow the mapping process
                    if (auto p = material.GetProperty<scene_ir::TextureSwizzle>(scene_ir::property::Normal); p && p->texture_idx == i) {
                        processes |= ImageProcess::FlipNrmZ;
                        break;
                    }
                }
            }

            // constexpr u32 MaxDim = 512;
            constexpr u32 MaxDim = 4096;

            if (auto uri = std::get_if<scene_ir::ImageFileURI>(&in_texture.data)) {
                auto path = std::filesystem::path(uri->uri);
                if (path.extension() == ".dds") {
                    path.replace_extension(".png");
                }
                if (!std::filesystem::exists(path)) {
                    NOVA_LOG("Cannot find file: {}", path.string());
                    continue;
                }
                path = std::filesystem::canonical(path);
                S_ImageProcessor.ProcessImage(path.string().c_str(), 0, ImageType::ColorAlpha, MaxDim, processes);
            } else if (auto file = std::get_if<scene_ir::ImageFileBuffer>(&in_texture.data)) {
                S_ImageProcessor.ProcessImage((const char*)file->data.data(), file->data.size(), ImageType::ColorAlpha, MaxDim, {});
            } else if (auto buffer = std::get_if<scene_ir::ImageBuffer>(&in_texture.data)) {
                NOVA_THROW("Buffer data source not currently supported");
            }

            out_texture->data.resize(S_ImageProcessor.GetImageDataSize());
            std::memcpy(out_texture->data.data(), S_ImageProcessor.GetImageData(), out_texture->data.size());
            out_texture->size = S_ImageProcessor.GetImageDimensions();
            out_texture->min_alpha = S_ImageProcessor.GetMinAlpha();
            out_texture->max_alpha = S_ImageProcessor.GetMaxAlpha();
            out_texture->format = S_ImageProcessor.GetImageFormat();
        }

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
                return out_scene.textures[single_pixel_textures.at(encoded)];
            }

            auto image = Ref<UVTexture>::Create();
            image->size = Vec2(1);
            image->data = { b8(data[0]), b8(data[1]), b8(data[2]), b8(data[3]) };

            out_scene.textures.push_back(image);
            single_pixel_textures.insert({ encoded, u32(out_scene.textures.size() - 1) });

            return image;
        };

        default_material->basecolor_alpha = CreatePixelImage({ 1.f, 0.f, 1.f, 1.f });
        default_material->normals = CreatePixelImage({ 0.5f, 0.5f, 1.f, 1.f });
        default_material->metalness_roughness = CreatePixelImage({ 0.f, 0.5f, 0.f, 1.f });
        default_material->emissivity = CreatePixelImage({ 0.f, 0.f, 0.f, 1.f });
        default_material->transmission = CreatePixelImage({ 0.f, 0.f, 0.f, 255.f });

        auto total_base_color = 0;

        u32 material_offset = u32(out_scene.materials.size());
        for (auto& in_material : in_scene.materials) {
            auto out_material = Ref<UVMaterial>::Create();
            out_scene.materials.push_back(out_material);

            auto GetImage = [&](std::string_view property, nova::types::Ref<UVTexture> fallback) {

                auto* texture = in_material.GetProperty<scene_ir::TextureSwizzle>(property);

                if (texture) {
                    auto tex = out_scene.textures[texture_offset + texture->texture_idx];
                    if (tex->data.size()) {
                        if (property == scene_ir::property::BaseColor) {
                            total_base_color++;
                        }
                        return tex;
                    }
                }

                // NOVA_LOG("Using fallback!");

                Vec4 data;

                if (Vec4* v4 = in_material.GetProperty<Vec4>(property)) {
                    data = *v4;
                } else if (Vec3* v3 = in_material.GetProperty<Vec3>(property)) {
                    data = Vec4(*v3, 1.f);
                } else if (Vec2* v2 = in_material.GetProperty<Vec2>(property)) {
                    data = Vec4(*v2, 0.f, 1.f);
                } else if (f32* v = in_material.GetProperty<f32>(property)) {
                    data = Vec4(*v, *v, *v, 1.f);
                } else {
                    return fallback;
                }

                return CreatePixelImage(data);
            };

            // TODO: Channel remapping!

            out_material->basecolor_alpha = GetImage(scene_ir::property::BaseColor, default_material->basecolor_alpha);
            out_material->normals = GetImage(scene_ir::property::Normal, default_material->normals);
            {
                if (auto* tex = in_material.GetProperty<scene_ir::TextureSwizzle>(scene_ir::property::Metallic);
                        tex && out_scene.textures[texture_offset + tex->texture_idx]->data.size()) {
                    // TODO: Fixme
                    out_material->metalness_roughness = out_scene.textures[texture_offset + tex->texture_idx];
                } else if (tex = in_material.GetProperty<scene_ir::TextureSwizzle>(scene_ir::property::SpecularColor);
                        tex && out_scene.textures[texture_offset + tex->texture_idx]->data.size()) {
                    // TODO: Fixme
                    out_material->metalness_roughness = out_scene.textures[texture_offset + tex->texture_idx];
                } else {
                    auto* _metalness = in_material.GetProperty<f32>(scene_ir::property::Metallic);
                    auto* _roughness = in_material.GetProperty<f32>(scene_ir::property::Roughness);

                    f32 metalness = _metalness ? *_metalness : 0.f;
                    f32 roughness = _roughness ? *_roughness : 0.5f;

                    out_material->metalness_roughness = CreatePixelImage({ 0.f, roughness, metalness, 1.f });
                }
            }
            out_material->emissivity = GetImage(scene_ir::property::Emissive, default_material->emissivity);
            out_material->transmission = GetImage("", default_material->transmission);

            out_material->alpha_cutoff = [](f32*v){return v?*v:0.5f;}(in_material.GetProperty<f32>(scene_ir::property::AlphaCutoff));

            out_material->alpha_mask = in_material.GetProperty<bool>(scene_ir::property::AlphaMask) ||
                out_material->basecolor_alpha->min_alpha < out_material->alpha_cutoff;
        }

        NOVA_LOGEXPR(total_base_color);

        u64 total_tangent_spaces = 0;
        ankerl::unordered_dense::set<u64> unique_tangent_space;

        u32 mesh_offset = u32(out_scene.meshes.size());
        for (auto& in_mesh : in_scene.meshes) {
            auto out_mesh = Ref<TriMesh>::Create();
            out_scene.meshes.push_back(out_mesh);

            out_mesh->position_attributes.resize(in_mesh.positions.size());
            std::memcpy(out_mesh->position_attributes.data(),
                in_mesh.positions.data(), in_mesh.positions.size() * sizeof(Vec3));

            out_mesh->shading_attributes.resize(in_mesh.positions.size());
            for (u32 i : in_mesh.indices) out_mesh->indices.push_back(i);

            usz vertex_count = out_mesh->position_attributes.size();
            usz index_count = out_mesh->indices.size();

            S_MeshProcessor.flip_uvs = flip_uvs;
            S_MeshProcessor.ProcessMesh(
                { &out_mesh->position_attributes[0], sizeof(out_mesh->position_attributes[0]), vertex_count },
                !in_mesh.normals.empty()
                    ? InStridedRegion{ &in_mesh.normals[0], sizeof(in_mesh.normals[0]), vertex_count }
                    : InStridedRegion{},
                !in_mesh.tex_coords.empty()
                    ? InStridedRegion{ &in_mesh.tex_coords[0], sizeof(in_mesh.tex_coords[0]), vertex_count }
                    : InStridedRegion{},
                { &out_mesh->indices[0], sizeof(out_mesh->indices[0]), index_count },
                { &out_mesh->shading_attributes[0].tangent_space, sizeof(out_mesh->shading_attributes[0]), vertex_count },
                { &out_mesh->shading_attributes[0].tex_coords, sizeof(out_mesh->shading_attributes[0]), vertex_count });

            for (u32 i = 0; i < vertex_count; ++i) {
                total_tangent_spaces++;
                std::pair<u32, u32> ts;
                ts.first = std::bit_cast<u32>(out_mesh->shading_attributes[i].tangent_space);
                ts.second = std::bit_cast<u32>(out_mesh->shading_attributes[i].tex_coords);
                unique_tangent_space.insert(std::bit_cast<u64>(ts));
            }

            out_mesh->sub_meshes.push_back(TriSubMesh {
                .vertex_offset = 0,
                .max_vertex = u32(in_mesh.positions.size() - 1),
                .first_index = 0,
                .index_count = u32(in_mesh.indices.size()),
                .material = in_mesh.material_idx == scene_ir::InvalidIndex
                    ? out_scene.materials[material_offset - 1]
                    : out_scene.materials[material_offset + in_mesh.material_idx],
            });
        }

        NOVA_LOG("Unique shading attributes: {} / {} ({:.2f}%)", unique_tangent_space.size(), total_tangent_spaces, (100.0 * unique_tangent_space.size()) / total_tangent_spaces);

        for (auto& in_instance : in_scene.instances) {
            auto out_instance = Ref<TriMeshInstance>::Create();
            out_scene.instances.push_back(out_instance);

            out_instance->mesh = out_scene.meshes[mesh_offset + in_instance.mesh_idx];
            out_instance->transform = in_instance.transform;
        }

        // {
        //     auto joined_mesh = Ref<TriMesh>::Create();

        //     for (auto& in_instance : in_scene.instances) {
        //         auto& in_mesh = out_scene.meshes[in_instance.mesh_idx];
        //         u32 vertex_offset = u32(joined_mesh->position_attributes.size());
        //         u32 index_offset = u32(joined_mesh->indices.size());
        //         for (u32 i = 0; i < in_mesh->position_attributes.size(); ++i) {
        //             auto pos = in_mesh->position_attributes[i];
        //             auto sa = in_mesh->shading_attributes[i];
        //             joined_mesh->position_attributes.push_back(in_instance.transform * Vec4(pos, 1.f));
        //             joined_mesh->shading_attributes.push_back(sa);
        //         }
        //         for (u32 i = 0; i < in_mesh->indices.size(); ++i) {
        //             auto index = in_mesh->indices[i];
        //             joined_mesh->indices.push_back(index);
        //         }
        //         auto sub_mesh = in_mesh->sub_meshes.front();
        //         sub_mesh.vertex_offset = vertex_offset;
        //         sub_mesh.first_index = index_offset;

        //         joined_mesh->sub_meshes.push_back(sub_mesh);
        //     }

        //     auto out_instance = Ref<TriMeshInstance>::Create();
        //     out_instance->mesh = joined_mesh;
        //     out_instance->transform = Mat4(1.f);

        //     out_scene.instances.clear();
        //     out_scene.meshes.clear();

        //     out_scene.instances.push_back(out_instance);
        //     out_scene.meshes.push_back(joined_mesh);
        // }
    }
}