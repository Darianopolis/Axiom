struct axiom_TangentSpace { uint packed; };
struct axiom_TexCoords    { uint packed; };

// uint axiom_MakeDecodeChoice(int x, int y, int s)
// {
//     ivec3 n;
//     n.x = (x - y);
//     n.y = (x + y) - 1023;
//     n.z = (s * 2046) - 1023;
//     n.z = n.z * (1023 - abs(n.x) - abs(n.y));

//     return uint(abs(n.y) > abs(n.z));
// }

vec3 axiom_SignedOctDecode(int _x, int _y, int _s, out uint choice)
{
    precise float x = float(_x) / 1023.0;
    precise float y = float(_y) / 1023.0;
    precise float s = float(_s);

    precise vec3 n;
    n.x = (x - y);
    n.y = (x + y) - 1.0;
    n.z = s * 2.0 - 1.0;
    n.z = n.z * (1.0 - abs(n.x) - abs(n.y));

    choice = uint(abs(n.y) > abs(n.z));

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
    uint choice;
    vec3 _normal = axiom_SignedOctDecode(
        int(bitfieldExtract(ts.packed, 0, 10)),
        int(bitfieldExtract(ts.packed, 10, 10)),
        int(bitfieldExtract(ts.packed, 20, 1)),
        choice);
    normal = _normal;

    // choice = axiom_MakeDecodeChoice(
    //     int(bitfieldExtract(ts.packed, 0, 10)),
    //     int(bitfieldExtract(ts.packed, 10, 10)),
    //     int(bitfieldExtract(ts.packed, 20, 1)));

    tangent = axiom_DecodeTangent(_normal,
        float(bitfieldExtract(ts.packed, 21, 10)) / 1023.0,
        choice);
        // bitfieldExtract(ts.packed, 31, 1));
}

// float axiom_UnpackBitangentSign(axiom_TangentSpace ts)
// {
//     return float(bitfieldExtract(ts.packed, 31, 1)) * 2.0 - 1.0;
// }

vec2 axiom_UnpackTexCoords(axiom_TexCoords uv)
{
    return unpackHalf2x16(uv.packed);
}