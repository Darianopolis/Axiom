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
    vec3 albedo, float roughness, float metallic, float ior, bool includeDiffuse)
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
    vec3 kS = F;
    vec3 kD = vec3(1.0) - kS;
    kD *= 1.0 - metallic;

    vec3 num = NDF * G * F;
    float denom = 4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0) + 0.0001;
    vec3 specular = num / denom;

    vec3 diffuse = kD * albedo / PI;

    vec3 value = specular;
    if (includeDiffuse)
        value += diffuse;

    return value;
}