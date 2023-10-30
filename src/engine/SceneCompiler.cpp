// #include "SceneCompiler.hpp"
// #include <scene/runtime/axiom_Attributes.hpp>

// namespace axiom
// {
//     Scene Compile(scene_ir::Scene& inScene)
//     {
//         bool flipNormalMapZ = false;
//         bool flipUVs = false;

//         Scene scene;

//         // BaseColor + Alpha = BC7
//         // Normals           = BC5
//         // Metal     + Rough = BC5
//         // Emissivity        = BC6h
//         // Transmission      = BC4

//         HashMap<u32, Index<Texture>> texture_lookup;

//         for (u32 i = 0; i < inScene.textures.size(); ++i) {
//             auto& inTexture = inScene.textures[i];

//             ImageProcess processes = {};
//             if (flipNormalMapZ) {
//                 for (auto& material : inScene.materials) {
//                     // TODO: This should follow the mapping process
//                     if (auto p = material.GetProperty<scene_ir::TextureSwizzle>(scene_ir::property::Normal); p && p->textureIdx == i) {
//                         processes |= ImageProcess::FlipNrmZ;
//                         break;
//                     }
//                 }
//             }

//             // constexpr u32 MaxDim = 512;
//             constexpr u32 MaxDim = 4096;

//             if (auto uri = std::get_if<scene_ir::ImageFileURI>(&inTexture.data)) {
//                 auto path = std::filesystem::path(uri->uri);
//                 if (path.extension() == ".dds") {
//                     path.replace_extension(".png");
//                 }
//                 if (!std::filesystem::exists(path)) {
//                     NOVA_LOG("Cannot find file: {}", path.string());
//                     continue;
//                 }
//                 path = std::filesystem::canonical(path);
//                 s_ImageProcessor.ProcessImage(path.string().c_str(), 0, ImageType::ColorAlpha, MaxDim, processes);
//             } else if (auto file = std::get_if<scene_ir::ImageFileBuffer>(&inTexture.data)) {
//                 s_ImageProcessor.ProcessImage((const char*)file->data.data(), file->data.size(), ImageType::ColorAlpha, MaxDim, {});
//             } else if (auto buffer = std::get_if<scene_ir::ImageBuffer>(&inTexture.data)) {
//                 NOVA_THROW("Buffer data source not currently supported");
//             }

//             Index<Texture> index = scene.textures.size();
//             auto& outTexture = scene.textures.emplace_back();
//             texture_lookup.insert({ i, index });

//             outTexture.data.resize(s_ImageProcessor.GetImageDataSize());
//             std::memcpy(outTexture.data.data(), s_ImageProcessor.GetImageData(), outTexture.data.size());
//             outTexture.size = s_ImageProcessor.GetImageDimensions();
//             outTexture.format = s_ImageProcessor.GetImageFormat();
//         }

//         nova::HashMap<u32, Index<Texture>> singlePixelTextures;

//         auto createPixelImage = [&](Vec4 value) -> Index<Texture> {
//             std::array<u8, 4> data {
//                 u8(value.r * 255.f),
//                 u8(value.g * 255.f),
//                 u8(value.b * 255.f),
//                 u8(value.a * 255.f),
//             };

//             u32 encoded = std::bit_cast<u32>(data);

//             if (singlePixelTextures.contains(encoded)) {
//                 return singlePixelTextures.at(encoded);
//             }

//             Index<Texture> index = scene.textures.size();
//             auto& image = scene.textures.emplace_back();
//             image.size = Vec2(1);
//             image.data = { b8(data[0]), b8(data[1]), b8(data[2]), b8(data[3]) };
//             image.format = nova::Format::RGBA8_UNorm;

//             singlePixelTextures.insert({ encoded, index });

//             return index;
//         };

//         Index<Material> default_material = 0;
//         {
//             auto& defaultMaterial = scene.materials.emplace_back();
//             defaultMaterial.albedo_alpha = createPixelImage({ 1.f, 0.f, 1.f, 1.f });
//             defaultMaterial.normal = createPixelImage({ 0.5f, 0.5f, 1.f, 1.f });
//             defaultMaterial.metalness_roughness = createPixelImage({ 0.f, 0.5f, 0.f, 1.f });
//             defaultMaterial.emission = createPixelImage({ 0.f, 0.f, 0.f, 1.f });
//             defaultMaterial.transmission = createPixelImage({ 0.f, 0.f, 0.f, 255.f });
//         }

//         u32 materialOffset = 1;
//         for (auto& inMaterial : inScene.materials) {
//             auto& outMaterial = scene.materials.emplace_back();

//             auto getImage = [&](std::string_view property, Index<Texture> fallback) -> Index<Texture> {

//                 auto* texture = inMaterial.GetProperty<scene_ir::TextureSwizzle>(property);

//                 if (texture && texture_lookup.contains(texture->textureIdx)) {
//                     return texture->textureIdx;
//                 }

//                 // NOVA_LOG("Using fallback!");

//                 Vec4 data;

//                 if (Vec4* v4 = inMaterial.GetProperty<Vec4>(property)) {
//                     data = *v4;
//                 } else if (Vec3* v3 = inMaterial.GetProperty<Vec3>(property)) {
//                     data = Vec4(*v3, 1.f);
//                 } else if (Vec2* v2 = inMaterial.GetProperty<Vec2>(property)) {
//                     data = Vec4(*v2, 0.f, 1.f);
//                 } else if (f32* v = inMaterial.GetProperty<f32>(property)) {
//                     data = Vec4(*v, *v, *v, 1.f);
//                 } else {
//                     return fallback;
//                 }

//                 return createPixelImage(data);
//             };

//             // TODO: Channel remapping!

//             auto& defaultMaterial = default_material.into(scene.materials);

//             outMaterial.albedo_alpha = getImage(scene_ir::property::BaseColor, defaultMaterial.albedo_alpha);
//             outMaterial.normal = getImage("sdfggsdgsdfg", defaultMaterial.normal);
//             {
//                 if (auto* tex = inMaterial.GetProperty<scene_ir::TextureSwizzle>(scene_ir::property::Metallic);
//                         tex && texture_lookup.contains(tex->textureIdx)) {
//                     // TODO: Fixme
//                     outMaterial.metalness_roughness = tex->textureIdx;
//                 } else if (tex = inMaterial.GetProperty<scene_ir::TextureSwizzle>(scene_ir::property::SpecularColor);
//                         tex && texture_lookup.contains(tex->textureIdx)) {
//                     // TODO: Fixme
//                     outMaterial.metalness_roughness = tex->textureIdx;
//                 } else {
//                     auto* _metalness = inMaterial.GetProperty<f32>(scene_ir::property::Metallic);
//                     auto* _roughness = inMaterial.GetProperty<f32>(scene_ir::property::Roughness);

//                     f32 metalness = _metalness ? *_metalness : 0.f;
//                     f32 roughness = _roughness ? *_roughness : 0.5f;

//                     outMaterial.metalness_roughness = createPixelImage({ 0.f, roughness, metalness, 1.f });
//                 }
//             }
//             outMaterial.emission = getImage(scene_ir::property::Emissive, defaultMaterial.emission);
//             outMaterial.transmission = getImage("", defaultMaterial.transmission);

//             outMaterial.alpha_cutoff = [](f32*v){return v?*v:0.5f;}(inMaterial.GetProperty<f32>(scene_ir::property::AlphaCutoff));

//             // outMaterial.alphaBlend = inMaterial.alphaBlend;
//             // outMaterial. = inMaterial.GetProperty<bool>(scene_ir::property::AlphaMask) ||
//             //     outMaterial.baseColor_alpha->minAlpha < outMaterial.alphaCutoff;
//             // outMaterial.decal = inMaterial.decal;
//         }

//         for (auto& inMesh : inScene.meshes) {
//             Index<Geometry> geom_idx = scene.geometries.size();
//             auto& geometry = scene.geometries.emplace_back();
//             auto& range = scene.geometry_ranges.emplace_back();

//             u32 vertex_count = u32(inMesh.positions.size());
//             u32 index_count = u32(inMesh.indices.size());

//             geometry.position_attributes.resize(vertex_count);
//             std::memcpy(geometry.position_attributes.data(),
//                 inMesh.positions.data(), vertex_count * sizeof(Vec3));

//             geometry.indices.resize(index_count);

//             u32 max_vertex = 0;
//             for (u32 i = 0; i < index_count; ++i) {
//                 max_vertex = std::max(max_vertex, inMesh.indices[i]);
//                 geometry.indices[i] = inMesh.indices[i];
//             }

//             range.geometry = geom_idx;
//             range.vertex_offset = 0;
//             range.max_vertex = max_vertex;
//             range.first_index = 0;
//             range.triangle_count = u32(geometry.indices.size()) / 3;

//             geometry.shading_attributes.resize(vertex_count);

//             s_MeshProcessor.flipUVs = flipUVs;
//             s_MeshProcessor.ProcessMesh(
//                 { &geometry.position_attributes[0], sizeof(geometry.position_attributes[0]), vertex_count },
//                 !inMesh.normals.empty()
//                     ? InStridedRegion{ &inMesh.normals[0], sizeof(inMesh.normals[0]), vertex_count }
//                     : InStridedRegion{},
//                 !inMesh.texCoords.empty()
//                     ? InStridedRegion{ &inMesh.texCoords[0], sizeof(inMesh.texCoords[0]), vertex_count }
//                     : InStridedRegion{},
//                 { &geometry.indices[0], sizeof(geometry.indices[0]), index_count },
//                 { &geometry.shading_attributes[0].tangent_space, sizeof(geometry.shading_attributes[0]), vertex_count },
//                 { &geometry.shading_attributes[0].tex_coords, sizeof(geometry.shading_attributes[0]), vertex_count });
//         }

//         for (auto& inInstance : inScene.instances) {
//             Index<TransformNode> transform_idx = scene.transform_nodes.size();
//             auto& out_mesh = scene.meshes.emplace_back();
//             auto& transform = scene.transform_nodes.emplace_back();

//             out_mesh.geometry_range = inInstance.meshIdx;
//             out_mesh.transform = transform_idx;

//             auto mat = inScene.meshes[inInstance.meshIdx].materialIdx;
//             out_mesh.material = materialOffset + (mat != axiom::scene_ir::InvalidIndex ? mat : 0);

//             transform.transform = inInstance.transform;
//         }

//         return scene;
//     }
// }