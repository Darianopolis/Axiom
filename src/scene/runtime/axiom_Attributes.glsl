struct axiom_TangentSpace { uint packed; };
struct axiom_TexCoords    { uint packed; };

vec3 axiom_SignedOctDecode(float x, float y, float s)
{
    vec3 n;
    n.x = (x - y);
    n.y = (x + y) - 1.0;
    n.z = s * 2.0 - 1.0;
    n.z = n.z * (1.0 - abs(n.x) - abs(n.y));

    return normalize(n);
}

vec2 axiom_DecodeDiamond(float p)
{
    vec2 v;

    // Remap p to the appropriate segment on the diamond
    float p_sign = sign(p - 0.5);
    v.x = -p_sign * 4.0 * p + 1.0 + p_sign * 2.0;
    v.y = p_sign * (1.0 - abs(v.x));

    // Normalization extends the point on the diamond back to the unit circle
    return normalize(v);
}

vec3 axiom_DecodeTangent(vec3 normal, float diamondTangent, uint choice)
{
    // As in the encode step, find our canonical tangent basis span(t1, t2)
    vec3 t1;
    // if (abs(normal.y) > abs(normal.z)) {
    if (choice != 0) {
        t1 = vec3(normal.y, -normal.x, 0.f);
    } else {
        t1 = vec3(normal.z, 0.f, -normal.x);
    }
    t1 = normalize(t1);

    vec3 t2 = cross(t1, normal);

    // Recover the coordinates used with t1 and t2
    vec2 packedTangent = axiom_DecodeDiamond(diamondTangent);

    return packedTangent.x * t1 + packedTangent.y * t2;
}

void axiom_UnpackTangentSpace(axiom_TangentSpace ts, out vec3 normal, out vec3 tangent)
{
    vec3 _normal = axiom_SignedOctDecode(
        float(bitfieldExtract(ts.packed, 0, 10)) / 1023.0,
        float(bitfieldExtract(ts.packed, 10, 10)) / 1023.0,
        float(bitfieldExtract(ts.packed, 20, 1)));
    normal = _normal;

    tangent = axiom_DecodeTangent(_normal,
        float(bitfieldExtract(ts.packed, 21, 10)) / 1023.0,
        bitfieldExtract(ts.packed, 31, 1));
}

// float axiom_UnpackBitangentSign(axiom_TangentSpace ts)
// {
//     return float(bitfieldExtract(ts.packed, 31, 1)) * 2.0 - 1.0;
// }

vec2 axiom_UnpackTexCoords(axiom_TexCoords uv)
{
    return unpackHalf2x16(uv.packed);
}