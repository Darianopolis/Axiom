// #pragma once

// #include <axiom_Core.hpp>

// namespace axiom
// {
//     enum class TextureFormat
//     {
//         Unknown,
//         RGBA8,
//     };

//     struct FileDataSource
//     {
//         std::string uri;
//     };

//     struct BufferDataSource
//     {
//         std::vector<u8> data;
//     };

//     using DataSource = std::variant<
//         BufferDataSource,
//         FileDataSource>;

//     struct Texture
//     {
//         DataSource      data;
//         TextureFormat format;
//     };

//     constexpr u32 InvalidIndex = UINT_MAX;

//     struct TextureSwizzle
//     {
//         u32             textureIdx = InvalidIndex;
//         std::array<i8, 4> channels{ -1, -1, -1, -1 };
//     };

//     enum class ChannelType
//     {
//         BaseColor,
//         Alpha,
//         Normal,
//         Emissive,
//         Metalness,
//         Roughness,
//         // Transmission,
//         // Subsurface,
//         // SpecularColor,
//         // SpecularStrength,
//         // Specular,
//         // Glossiness,
//         // Clearcoat,
//         // Diffuse,
//         // Ior,
//     };

//     struct Channel
//     {
//         ChannelType       type;
//         TextureSwizzle texture;
//         Vec4             value{ 0.f, 0.f, 0.f, 1.f };
//     };

//     struct Material
//     {
//         std::vector<Channel> channels;

//         Channel* GetChannel(ChannelType type)
//         {
//             for (auto& channel : channels) {
//                 if (channel.type == type) {
//                     return &channel;
//                 }
//             }

//             return nullptr;
//         }

//         f32 alphaCutoff = 0.5f;
//         bool  alphaMask = false;
//         bool alphaBlend = false;
//         bool     volume = false;
//         bool      decal = false;
//     };

// // -----------------------------------------------------------------------------

//     struct Mesh
//     {
//         std::vector<Vec3> positions;
//         std::vector<Vec3>   normals;
//         std::vector<Vec2> texCoords;
//         std::vector<u32>    indices;
//         u32             materialIdx = InvalidIndex;
//     };

//     struct Instance
//     {
//         u32    meshIdx = InvalidIndex;
//         Mat4 transform;
//     };

//     struct Scene
//     {
//         std::vector<Texture>   textures;
//         std::vector<Material> materials;
//         std::vector<Mesh>        meshes;
//         std::vector<Instance> instances;
//     };
// }