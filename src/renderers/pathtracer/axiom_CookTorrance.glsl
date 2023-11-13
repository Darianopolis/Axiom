#include "axiom_Common.glsl"

float DistributionGGX(vec3 N, vec3 H, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;

    float num = a2;
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    denom = PI * denom * denom;

    return num / denom;
}

float GeometrySchlickGGX(float NdotV, float roughness)
{
    float r = (roughness + 1.0);
    float k = (r * r) / 8.0;

    float num = NdotV;
    float denom = NdotV * (1.0 - k) + k;

    return num / denom;
}

float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness)
{
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    float ggx2 = GeometrySchlickGGX(NdotV, roughness);
    float ggx1 = GeometrySchlickGGX(NdotL, roughness);

    return ggx1 * ggx2;
}

vec3 FresnelSchlick(float cosTheta, vec3 F0)
{
    float a = clamp(1.0 - cosTheta, 0.0, 1.0);
    float a2 = a * a;
    return F0 + (1.0 - F0) * a2 * a2 * a;
}

vec3 CookTorranceBrdf(
    vec3 N, vec3 V, vec3 L,
    vec3 albedo, float roughness, float metallic, float ior)
{
    float R = pow((1 - ior) / (1 + ior), 2);
    vec3 F0 = mix(vec3(R), albedo, metallic);
    vec3 H = normalize(V + L);

    // Average microfacet alignment with halfway vector
    float NDF = DistributionGGX(N, H, roughness);

    // Geometric term - self shadowing
    float G = GeometrySmith(N, V, L, roughness);

    // Fresnel term - ratio of reflection to refraction
    vec3 F = FresnelSchlick(max(dot(H, V), 0.0), F0);
    // vec3 kS = F;
    // vec3 kD = vec3(1.0) - kS;
    // kD *= 1.0 - metallic;

    // vec3 num = NDF * G * F;
    // float denom = 4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0) + 0.0001;
    // vec3 specular = num / denom;

    // vec3 diffuse = kD * albedo / PI;

    // vec3 value = specular;
    // if (includeDiffuse)
    //     value += diffuse;

    // return value;

    vec3 num = NDF * G * F;
    float denom = 4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0) + 0.0001;
    return num / denom;
}

const vec3 loc_ZAxis = vec3(0, 0, 1);
const vec3 loc_XAxis = vec3(1, 0, 0);

float loc_BsdfNDot(vec3 v)
{
    return dot(vec3(0, 0, 1), v);
}

vec3 loc_SphericalToCartesian(float theta, float phi)
{
    float sin_theta = sin(theta);
    float cos_theta = cos(theta);
    float sin_phi = sin(phi);
    float cos_phi = cos(phi);

    vec3 xyz;
    xyz.x = sin_theta * cos_phi;
    xyz.y = sin_theta * sin_phi;
    xyz.z = cos_theta;

    return xyz;
}

float loc_SmithGGXMasking(vec3 wi, vec3 wo, float a2)
{
    float dotNV = loc_BsdfNDot(wo);
    float denomC = sqrt(a2 + (1.0 - a2) * dotNV * dotNV) + dotNV;

    return 2.0 * dotNV / denomC;
}

float loc_SmithGGXMaskingShadowing(vec3 wi, vec3 wo, float a2)
{
    float dotNL = loc_BsdfNDot(wi + loc_ZAxis);
    float dotNV = loc_BsdfNDot(wo + loc_ZAxis);

    // float dotNL = 1.0;
    // float dotNV = 0.0;
    // a2 = 1.0;

    float denomA = dotNV * sqrt(a2 + (1.0 - a2) * dotNL * dotNL);
    float denomB = dotNL * sqrt(a2 + (1.0 - a2) * dotNV * dotNV);

    return 2.0 * dotNL * dotNV / (denomA + denomB);
}

vec3 loc_GgxVndf(vec3 wo, float roughness, float u1, float u2)
{
    vec3 v = normalize(vec3(wo.x * roughness,
                            wo.y * roughness,
                            wo.z));

    vec3 t1 = (v.z < 0.999) ? normalize(cross(v, loc_ZAxis)) : loc_XAxis;
    vec3 t2 = cross(t1, v);

    float a = 1.0 / (1.0 + v.z);
    float r = sqrt(u1);
    float phi = (u2 < a) ? ((u2 / a) * PI)
                         : (PI + (u2 - a) / (1.0 - a) * PI);
    float p1 = r * cos(phi);
    float p2 = r * sin(phi) * ((u2 < a) ? 1.0 : v.z);

    vec3 n = p1 * t1 + p2 * t2
        + sqrt(max(0.0, 1.0 - p1 * p1 - p2 * p2)) * v;

    return normalize(vec3(roughness * n.x,
                          roughness * n.y,
                          max(0.001, n.z)));
}

void loc_ImportanceSampleGgxVdn(vec3 wo,
    vec3 albedo, float roughness, float metallic, float ior,
    out vec3 wi, out vec3 reflectance)
{
    float R = pow((1 - ior) / (1 + ior), 2);
    vec3 F0 = mix(vec3(R), albedo, metallic);

    float a2 = roughness * roughness;

    vec3 wm = loc_GgxVndf(wo, roughness, RandomUNorm(), RandomUNorm());
    // vec3 wm = vec3(0, 0, 1);

    wi = -reflect(wo, wm);

    if (loc_BsdfNDot(wi) > 0.0) {
        vec3 F = FresnelSchlick(dot(wi, wm), F0);
        float G1 = loc_SmithGGXMasking(wi, wo, a2);
        float G2 = loc_SmithGGXMaskingShadowing(wi, wo, a2);

        // reflectance = F * (G2 / G1);
        reflectance = F ;
    } else {
        reflectance = vec3(0);
    }
}

void loc_ImportanceSampleGgxD(vec3 wo,
    vec3 albedo, float roughness, float metallic, float ior,
    out vec3 wi, out vec3 reflectance)
{
    float R = pow((1 - ior) / (1 + ior), 2);
    vec3 F0 = mix(vec3(R), albedo, metallic);

    float a2 = roughness * roughness;

    float e0 = RandomUNorm();
    float e1 = RandomUNorm();

    float theta = acos(sqrt((1.0 - e0) / ((a2 - 1.0) * e0 + 1.0)));
    float phi   = PI * 2.0 * e1;

    vec3 wm = loc_SphericalToCartesian(theta, phi);

    wi = reflect(wo, wm);

    if (loc_BsdfNDot(wi) > 0.0 && dot(wi, wm) > 0.0) {
        float dotWiWm = dot(wi, wm);

        vec3 F = FresnelSchlick(dotWiWm, F0);
        float G = loc_SmithGGXMaskingShadowing(wi, wo, a2);
        float weight = abs(dot(wo, wm)) / (loc_BsdfNDot(wo) * loc_BsdfNDot(wm));

        reflectance = F * G * weight;
        // reflectance = albedo;
    } else {
        reflectance = vec3(0);
    }
}

void loc_ImportanceSampleLambert(vec3 wo,
    vec3 albedo, float roughness, float metallic, float ior,
    out vec3 wi, out vec3 reflectance)
{
    wi = CosineSampleHemisphere();

    float R = pow((1 - ior) / (1 + ior), 2);
    vec3 F0 = mix(vec3(R), albedo, metallic);
    vec3 H = normalize(wo + wi);
    vec3 F = FresnelSchlick(max(dot(H, wo), 0.0), F0);
    vec3 kS = F;
    vec3 kD = vec3(1.0) - kS;
    kD *= 1.0 - metallic;

    reflectance = kD * albedo;
}

void loc_CalculateLobePdfs(
    float metallic,
    out float pSpecular, out float pDiffuse)
{
    float metallicBRDF   = metallic;
    float dielectricBRDF = 1 - metallic;

    float specularWeight = metallicBRDF + dielectricBRDF;
    float diffuseWeight  = dielectricBRDF;

    float norm = 1.0 / (specularWeight + diffuseWeight);

    pSpecular = specularWeight * norm;
    pDiffuse  = diffuseWeight   * norm;
}