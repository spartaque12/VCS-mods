// RenderHook.asi: ground-truth ambient occlusion (GTAO: Jimenez et al. 2016, "Practical Real-Time Strategies for Accurate
// Indirect Occlusion", the horizon search and integral as in Intel's XeGTAO) from the game's own Z buffer, multiplied into
// the 3D picture before the game's post effects (api/vcs_mod_api.h VCS_RENDER_BEFORE_POST).
//
// Full-screen passes (vs_main: one triangle over the viewport, set to the picture's width x height from 0, 0):
//   ps_view_depth   the Z buffer -> view depth in metres (R32F; 0 = nothing drawn: the sky). MS=1: a multisampled Z buffer
//                   (sample 0)
//   ps_gtao         the visibility (RGBA8 .r) from the view depth: SLICES directions per pixel rotated in a 4x4 pattern,
//                   STEPS samples each way along each - noisy until the next pass
//   ps_apply        a depth-aware 4x4 blur (every 4x4 window holds the 16 rotations once) -> alpha, multiplied into the
//                   picture (VCS_RENDER_BLEND_MULTIPLY)
//   ps_show         the blurred visibility as grey, opaque (the debug view)

#ifndef MS
#define MS 0
#endif
#define SLICES 2
#define STEPS 4

cbuffer Ao : register(b0)
{
    float4 proj;   // fx, fy, cx, cy (target pixels): pixel centre c at view depth w is ((c - (cx, cy)) / (fx, fy) * w, w)
    float4 zmap;   // a, b, scale, far: GS z = d / scale = a + b / w (d = 0: nothing drawn); nothing beyond far
    float4 params; // radius (m), strength (0..1), power, largest radius on screen (pixels)
    float4 fade;   // fog bias, fog scale (visibility (bias + scale w) / 255; scale 0: no fog), fade start, fade end (m)
    int4 size;     // the picture's width, height (pixels)
};

struct VSOut
{
    float4 pos : SV_Position;
};

VSOut vs_main(uint id : SV_VertexID)
{
    VSOut o;
    const float2 c = float2((id << 1) & 2, id & 2); // (0, 0), (2, 0), (0, 2)
    o.pos = float4(c.x * 2.0 - 1.0, 1.0 - c.y * 2.0, 0.0, 1.0);
    return o;
}

static const float PI = 3.14159265;
static const float HALF_PI = 1.57079633;

// ---- ps_view_depth -------------------------------------------------------------------------------------------------
#if MS
Texture2DMS<float> zbuffer : register(t0);
#else
Texture2D<float> zbuffer : register(t0);
#endif

float ps_view_depth(VSOut i) : SV_Target0
{
#if MS
    const float d = zbuffer.Load(int2(i.pos.xy), 0);
#else
    const float d = zbuffer.Load(int3(i.pos.xy, 0));
#endif
    const float t = d / zmap.z - zmap.x;
    const float w = (d > 0.0 && t != 0.0) ? zmap.y / t : 0.0;
    return (w > 0.0 && w < zmap.w) ? w : 0.0;
}

// ---- ps_gtao ---------------------------------------------------------------------------------------------------------
Texture2D<float> viewDepth : register(t0);

float depthAt(int2 p)
{
    return viewDepth.Load(int3(p, 0));
}

float3 viewPos(float2 centre, float w)
{
    return float3((centre - proj.zw) / proj.xy * w, w);
}

// XeGTAO's acos approximation (max error ~1e-4 rad)
float fastAcos(float x)
{
    float r = -0.156583 * abs(x) + HALF_PI;
    r *= sqrt(1.0 - abs(x));
    return x >= 0.0 ? r : PI - r;
}

// the 4x4 pattern of slice rotations (Bayer)
static const float kPattern[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};

float4 ps_gtao(VSOut i) : SV_Target0
{
    const int2 p = int2(i.pos.xy);
    const float w = depthAt(p);
    if (w <= 0.0)
        return 1.0;
    const float2 centre = float2(p) + 0.5;
    const float3 P = viewPos(centre, w);

    // the normal from the neighbours: on each axis the one nearer in depth (no normal across a depth edge)
    const float wl = depthAt(p - int2(1, 0)), wr = depthAt(p + int2(1, 0));
    const float wu = depthAt(p - int2(0, 1)), wd = depthAt(p + int2(0, 1));
    float3 dx = float3(w / proj.x, 0.0, 0.0), dy = float3(0.0, w / proj.y, 0.0);
    if (wl > 0.0 && (wr <= 0.0 || abs(wl - w) <= abs(wr - w)))
        dx = P - viewPos(centre - float2(1.0, 0.0), wl);
    else if (wr > 0.0)
        dx = viewPos(centre + float2(1.0, 0.0), wr) - P;
    if (wu > 0.0 && (wd <= 0.0 || abs(wu - w) <= abs(wd - w)))
        dy = P - viewPos(centre - float2(0.0, 1.0), wu);
    else if (wd > 0.0)
        dy = viewPos(centre + float2(0.0, 1.0), wd) - P;
    float3 N = normalize(cross(dx, dy));
    const float3 V = normalize(-P);
    if (dot(N, V) < 0.0)
        N = -N;

    const float radius = params.x;
    const float radiusPx = min(radius * proj.y / w, params.w);
    if (radiusPx < 2.0)
        return 1.0;
    // a sample's weight: 1 up to 40% of the radius, 0 at the radius (XeGTAO's falloff)
    const float falloffRange = 0.6 * radius;
    const float falloffMul = -1.0 / falloffRange, falloffAdd = (radius - falloffRange) / falloffRange + 1.0;

    const float rot = kPattern[(p.y & 3) * 4 + (p.x & 3)];
    const float sliceNoise = (rot + 0.5) / 16.0;
    const float stepNoise = frac(rot * 0.6180339887 + 0.31);

    float visibility = 0.0;
    [unroll] for (int s = 0; s < SLICES; ++s)
    {
        const float phi = (s + sliceNoise) * (PI / SLICES);
        const float2 omega = float2(cos(phi), sin(phi)); // view y is down like the screen's: one direction for both
        const float3 dir = float3(omega, 0.0);
        const float3 ortho = dir - dot(dir, V) * V;
        const float3 axis = normalize(cross(ortho, V));
        const float3 projN = N - axis * dot(N, axis);
        const float projLen = length(projN);
        const float cosN = saturate(dot(projN, V) / max(projLen, 1e-6));
        const float n = (dot(ortho, projN) < 0.0 ? -1.0 : 1.0) * fastAcos(cosN);
        // the horizons start at the tangent plane: + side (along omega) at n + pi/2, - side at n - pi/2
        const float low0 = cos(n + HALF_PI), low1 = cos(n - HALF_PI);
        float hc0 = low0, hc1 = low1;
        [unroll] for (int k = 0; k < STEPS; ++k)
        {
            float t = (k + stepNoise) / STEPS;
            t *= t; // denser near the pixel
            const int2 o = int2(round(omega * max(t * radiusPx, k + 1.0)));
            [unroll] for (int side = 0; side < 2; ++side)
            {
                const int2 q = side == 0 ? p + o : p - o;
                if (any(q < 0) || any(q >= size.xy))
                    continue;
                const float qw = depthAt(q);
                if (qw <= 0.0)
                    continue; // the sky occludes nothing
                const float3 delta = viewPos(float2(q) + 0.5, qw) - P;
                const float dist = length(delta);
                const float shc = dot(delta, V) / max(dist, 1e-6);
                const float weight = saturate(dist * falloffMul + falloffAdd);
                if (side == 0)
                    hc0 = max(hc0, lerp(low0, shc, weight));
                else
                    hc1 = max(hc1, lerp(low1, shc, weight));
            }
        }
        const float h0 = -fastAcos(hc1), h1 = fastAcos(hc0);
        const float arc0 = (cosN + 2.0 * h0 * sin(n) - cos(2.0 * h0 - n)) * 0.25;
        const float arc1 = (cosN + 2.0 * h1 * sin(n) - cos(2.0 * h1 - n)) * 0.25;
        visibility += lerp(projLen, 1.0, 0.05) * (arc0 + arc1);
    }
    float ao = pow(saturate(visibility / SLICES), params.z);
    // the game fogs the world after its colours: occlusion only on the part that is not fog; faded out with distance
    const float fog = fade.y != 0.0 ? saturate((fade.x + fade.y * w) / 255.0) : 1.0;
    const float far = saturate((fade.w - w) / max(fade.w - fade.z, 1e-3));
    ao = lerp(1.0, ao, params.y * fog * far);
    return float4(ao, 0.0, 0.0, 1.0);
}

// ---- ps_apply / ps_show ----------------------------------------------------------------------------------------------
Texture2D<float4> rawAo : register(t0);
Texture2D<float> blurDepth : register(t1);

// the 4x4 window from -1 to +2: neighbours on the pixel's plane (its depth gradient: slanted ground keeps its weight)
float blurredAo(int2 p, float w)
{
    const float wl = blurDepth.Load(int3(max(p - int2(1, 0), 0), 0)), wr = blurDepth.Load(int3(min(p + int2(1, 0), size.xy - 1), 0));
    const float wu = blurDepth.Load(int3(max(p - int2(0, 1), 0), 0)), wd = blurDepth.Load(int3(min(p + int2(0, 1), size.xy - 1), 0));
    const float gx = (wl > 0.0 && (wr <= 0.0 || abs(w - wl) <= abs(wr - w))) ? w - wl : (wr > 0.0 ? wr - w : 0.0);
    const float gy = (wu > 0.0 && (wd <= 0.0 || abs(w - wu) <= abs(wd - w))) ? w - wu : (wd > 0.0 ? wd - w : 0.0);
    const float tolerance = 0.02 * w + 0.05;
    float sum = 0.0, weights = 0.0;
    [unroll] for (int y = -1; y <= 2; ++y)
    {
        [unroll] for (int x = -1; x <= 2; ++x)
        {
            const int2 q = clamp(p + int2(x, y), 0, size.xy - 1);
            const float qw = blurDepth.Load(int3(q, 0));
            const float weight = qw > 0.0 ? saturate(1.0 - abs(qw - (w + gx * x + gy * y)) / tolerance) : 0.0;
            sum += rawAo.Load(int3(q, 0)).r * weight;
            weights += weight;
        }
    }
    return weights > 0.0 ? sum / weights : 1.0;
}

float4 ps_apply(VSOut i) : SV_Target0
{
    const int2 p = int2(i.pos.xy);
    const float w = blurDepth.Load(int3(p, 0));
    return float4(0.0, 0.0, 0.0, w > 0.0 ? blurredAo(p, w) : 1.0);
}

float4 ps_show(VSOut i) : SV_Target0
{
    const int2 p = int2(i.pos.xy);
    const float w = blurDepth.Load(int3(p, 0));
    const float ao = w > 0.0 ? blurredAo(p, w) : 1.0;
    return float4(ao, ao, ao, 1.0);
}
