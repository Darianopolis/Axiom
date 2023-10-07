#version 460
#extension GL_EXT_fragment_shader_barycentric : require

layout(location = 0) in pervertexEXT vec3 inPosition[3];
layout(location = 0) out vec4 outColor;

void main()
{
    vec3 v01 = inPosition[1] - inPosition[0];
    vec3 v02 = inPosition[2] - inPosition[0];
    vec3 nrm = normalize(cross(v01, v02));
    if (!gl_FrontFacing) {
        nrm = -nrm;
    }
    outColor = vec4((nrm * 0.5 + 0.5) * 0.75, 1.0);
}