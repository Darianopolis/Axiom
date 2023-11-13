#include <cstdint>

using byte_t = unsigned char;

using f32_t = float;
using f16_t = uint16_t;

using u64_t = uint64_t;
using u32_t = uint32_t;
using u16_t = uint16_t;

struct vec3_t {
    f32_t x, y, z;
};

struct mat4x3_t {
    vec3_t cols[4];
};

struct shading_attributes_t {
    u32_t normal_x       : 10;
    u32_t normal_y       : 10;
    u32_t normal_sign    : 1;
    u32_t tangent_angle  : 10;
    u32_t bitangent_sign : 1;

    f16_t u;
    f16_t v;
};

struct mesh_t {
    u32_t vertex_offset;
    u32_t index_offset;
    u32_t index_count;
    u32_t material;
};

struct texture_t {
    u64_t data_offset;
    u16_t width;
    u16_t height;
    u16_t type;
    u16_t flags;
};

struct material_t {
    u32_t albedo_alpha;
    u32_t normal;
    u32_t metalness_roughness;
    u32_t emissive;

    u32_t flags;
    f32_t ior;
};

struct node_t {
    mat4x3_t transform;
    u32_t    parent;
    u32_t    first_instance;
    u32_t    mesh_count;
};

template<class T>
struct span_t {
    T*    first;
    u64_t count;
};

struct scene_t {
    span_t<vec3_t>               pos_attributes;
    span_t<shading_attributes_t> shading_attributes;
    span_t<u32_t>                vertex_indices;

    span_t<mesh_t>               meshes;
    span_t<byte_t>               texture_data;
    span_t<texture_t>            textures;
    span_t<material_t>           materials;

    span_t<u32_t>                mesh_instances;
    span_t<node_t>               nodes;
};
