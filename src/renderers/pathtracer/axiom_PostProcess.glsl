#version 460
#extension GL_GOOGLE_include_directive : require

#include "axiom_Common.glsl"

layout(push_constant, scalar) uniform pc_ {
    uvec2  size;
    uint source;
    uint target;
} pc;

layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;
void main()
{
    ivec2 pos = ivec2(gl_GlobalInvocationID);
    if (pos.x >= pc.size.x || pos.y >= pc.size.y)
        return;

    vec4 source = imageLoad(RWImage2D[pc.source], pos);

    vec3 mapped = Apply_sRGB_OETF(source.rgb);

    imageStore(RWImage2D[pc.target], pos, vec4(mapped, 1.0));
}