// RenderHook.asi: sun shadows - cascaded shadow maps of the scene's own draws (render_shadow_map: the renderer draws the
// frame's opaque native draws from the sun into up to 4 viewports side by side), looked up from the game's Z buffer and
// multiplied into the 3D picture before the game's post effects, as the ambient occlusion is.
//
// ps_shadow       one full-screen pass (shaders/gtao.hlsl vs_main): the view-space point from the Z buffer, its normal from
//                 the neighbours' depths, the cascade its depth falls in, a 3x3-texel filtered compare (4x4 loads with
//                 bilinear weights) at the point moved along its normal by a texel and a half; faces turned from the sun
//                 count as shadowed (side-on ones partly). Out: alpha = the picture's multiplier (VCS_RENDER_BLEND_MULTIPLY).
// ps_shadow_show  the debug view: lit / shadowed as grey, tinted per cascade, opaque. MS=1: a multisampled Z buffer.

#ifndef MS
#define MS 0
#endif

cbuffer Shadows : register(b0)
{
    float4 proj;      // fx, fy, cx, cy (target pixels)
    float4 zmap;      // a, b, scale, far: GS z = d / scale = a + b / w
    float4 sun;       // xyz: towards the sun in view space; w: the darkening (strength x daylight)
    float4 splits;    // the far view depth of cascades 0..3 (m)
    float4 texel;     // a shadow texel's size per cascade (m)
    float4 zbias;     // the compare's bias per cascade (light depth units)
    float4 fade;      // fog bias, fog scale (0: no fog), fade start, fade end (m)
    float4 info;      // cascades, cascade size (texels), blend band (share of a cascade's depth), 0
    float4 light[16]; // per cascade the rows of its view -> clip matrix
    int4 size;        // the picture's width, height
};

struct VSOut
{
    float4 pos : SV_Position;
};

#if MS
Texture2DMS<float> zbuffer : register(t0);
#else
Texture2D<float> zbuffer : register(t0);
#endif
Texture2D<float> shadowMap : register(t1);

float viewDepthAt(int2 p)
{
    p = clamp(p, int2(0, 0), size.xy - 1);
#if MS
    const float d = zbuffer.Load(p, 0);
#else
    const float d = zbuffer.Load(int3(p, 0));
#endif
    const float t = d / zmap.z - zmap.x;
    const float w = (d > 0.0 && t != 0.0) ? zmap.y / t : 0.0;
    return (w > 0.0 && w < zmap.w) ? w : 0.0;
}

float3 viewPos(float2 centre, float w)
{
    return float3((centre - proj.zw) / proj.xy * w, w);
}

// the step to a neighbour on the pixel's plane: the nearer of the two in depth (an edge keeps its own side)
float3 planeStep(float3 P, float3 back, float wb, float3 ahead, float wa)
{
    if (wb > 0.0 && (wa <= 0.0 || abs(P.z - back.z) <= abs(ahead.z - P.z)))
        return P - back;
    return wa > 0.0 ? ahead - P : float3(0.0, 0.0, 0.0);
}

// lit share (0..1) of view point P in cascade c, or -1 outside it
float cascadeLit(int c, float3 P)
{
    const float4 p = float4(P, 1.0);
    const float x = dot(light[c * 4 + 0], p), y = dot(light[c * 4 + 1], p), z = dot(light[c * 4 + 2], p);
    if (abs(x) > 0.98 || abs(y) > 0.98 || z > 1.0)
        return -1.0;
    const float n = info.y;
    const float2 t = float2(x * 0.5 + 0.5, 0.5 - y * 0.5) * n - 0.5;
    const int2 i0 = int2(floor(t));
    const float2 f = t - float2(i0);
    const float wx[4] = {1.0 - f.x, 1.0, 1.0, f.x};
    const float wy[4] = {1.0 - f.y, 1.0, 1.0, f.y};
    const float ref = z + zbias[c];
    float lit = 0.0;
    [unroll] for (int j = 0; j < 4; ++j)
    {
        [unroll] for (int i = 0; i < 4; ++i)
        {
            const int2 q = clamp(i0 + int2(i - 1, j - 1), int2(0, 0), int2(n - 1, n - 1)) + int2(c * int(n), 0);
            lit += (ref >= shadowMap.Load(int3(q, 0)) ? 1.0 : 0.0) * wx[i] * wy[j];
        }
    }
    return lit / 9.0;
}

// the lit share at pixel p (1: sky, out of range), the cascade in `cascade` (-1: none)
float litAt(int2 p, out int cascade, out float w)
{
    cascade = -1;
    w = viewDepthAt(p);
    if (w <= 0.0 || w >= splits[int(info.x) - 1])
        return 1.0;
    const float3 P = viewPos(float2(p) + 0.5, w);
    const float wl = viewDepthAt(p - int2(1, 0)), wr = viewDepthAt(p + int2(1, 0));
    const float wu = viewDepthAt(p - int2(0, 1)), wd = viewDepthAt(p + int2(0, 1));
    const float3 dx = planeStep(P, viewPos(float2(p) + float2(-0.5, 0.5), wl), wl, viewPos(float2(p) + float2(1.5, 0.5), wr), wr);
    const float3 dy = planeStep(P, viewPos(float2(p) + float2(0.5, -0.5), wu), wu, viewPos(float2(p) + float2(0.5, 1.5), wd), wd);
    float3 N = cross(dy, dx);
    const float len = length(N);
    N = len > 1e-12 ? N / len : -normalize(P);
    if (dot(N, P) > 0.0)
        N = -N;
    const float ndl = dot(N, sun.xyz);
    int c = 0;
    [unroll] for (int k = 0; k < 3; ++k)
        c += (k < int(info.x) - 1 && w >= splits[k]) ? 1 : 0;
    cascade = c;
    float lit = cascadeLit(c, P + N * (texel[c] * 1.5));
    lit = lit < 0.0 ? 1.0 : lit;
    // the band before a cascade's end: blended into the next one
    const float start = splits[c] * (1.0 - info.z);
    if (c + 1 < int(info.x) && w > start)
    {
        float next = cascadeLit(c + 1, P + N * (texel[c + 1] * 1.5));
        next = next < 0.0 ? lit : next;
        lit = lerp(lit, next, saturate((w - start) / (splits[c] - start)));
    }
    // faces turned from the sun are in their own shadow; side-on ones (walls along the sun) only partly
    return lit * smoothstep(-0.15, 0.25, ndl);
}

float4 ps_shadow(VSOut i) : SV_Target0
{
    int c;
    float w;
    const float lit = litAt(int2(i.pos.xy), c, w);
    if (c < 0)
        return float4(0.0, 0.0, 0.0, 1.0);
    const float far = 1.0 - smoothstep(fade.z, fade.w, w);
    const float fog = fade.y != 0.0 ? saturate((fade.x + fade.y * w) / 255.0) : 1.0;
    return float4(0.0, 0.0, 0.0, 1.0 - sun.w * (1.0 - lit) * far * fog);
}

float4 ps_shadow_show(VSOut i) : SV_Target0
{
    int c;
    float w;
    const float lit = litAt(int2(i.pos.xy), c, w);
    if (c < 0)
        return float4(0.3, 0.3, 0.6, 1.0);
    const float3 tint[4] = {float3(1.0, 0.8, 0.8), float3(0.8, 1.0, 0.8), float3(0.8, 0.8, 1.0), float3(1.0, 1.0, 0.7)};
    return float4(tint[c] * (0.25 + 0.75 * lit), 1.0);
}
