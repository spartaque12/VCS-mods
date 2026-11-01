// RenderHook.asi: the water surface (src/water.cpp; docs/RENDERHOOK_PLAN.md "Water"). Standard techniques, written for
// this port: summed directional sine waves (deep-water dispersion) as normals, depth-based absorption towards the game's
// own water tint, Schlick's Fresnel, screen-space reflections of the picture before the water with an edge fallback,
// sun glitter (a sharp and a broad Blinn-Phong lobe).
//
// Full-screen passes (vs_main from shaders/gtao.hlsl):
//   ps_raw_depth     BEFORE_WATER: the Z buffer (d = GS z / 2^24) into R32F as it is (MS=1: sample 0 of a multisampled one)
//   ps_info          AFTER_WATER: x = 1 where the Z buffer is nearer than before (the water in front), y = its view depth,
//                    z = the view depth behind it before the water (1e4 = nothing: sky), w = 0 (RGBA16F)
//   ps_extras_clear  the other stages' screen-space inputs cleared: (foam 0, slope 0.5, 0.5, caustics 0)
//   ps_water         the surface over the game's water (discards elsewhere); ps_water_show the debug views

#ifndef MS
#define MS 0
#endif

cbuffer Water : register(b0)
{
    float4 proj;    // fx, fy, cx, cy (target pixels)
    float4 zmap;    // a, b, scale, far: GS z = d / scale = a + b / w
    float4 fog;     // bias, scale (visibility (bias + scale w) / 255), -, -
    float4 rot0, rot1, rot2; // world_to_view rows (view = R world)
    float4 cam;     // camera in the world (x, y, z), time (s)
    float4 sun;     // towards the sun (world), daylight 0..1
    float4 tint;    // the game's water tint (rgb, 1 = 0x80), the deep colour's brightness
    float4 params;  // wave strength (0..2), reflection strength, glitter strength, refraction (m)
    float4 params2; // absorption per metre, wake valid, debug view (1 mask / depth, 2 normals, 3 reflections), wind angle
    float4 wake;    // stage 3's field: cam_x, cam_y, 1 / size, -
    int4 size;      // the picture's width, height
};

struct VSOut
{
    float4 pos : SV_Position;
};

static const float PI = 3.14159265;
static const float FAR = 10000.0;

float viewDepth(float d)
{
    const float t = d / zmap.z - zmap.x;
    return (d > 0.0 && t > 0.0) ? zmap.y / t : FAR;
}

float3 viewPoint(float2 px, float w) { return float3((px - proj.zw) / proj.xy * w, w); }
float3 toWorld(float3 v) { return rot0.xyz * v.x + rot1.xyz * v.y + rot2.xyz * v.z; } // R^T v
float3 toView(float3 d) { return float3(dot(rot0.xyz, d), dot(rot1.xyz, d), dot(rot2.xyz, d)); }

// ---- BEFORE_WATER ----------------------------------------------------------------------------------------------------
#if MS
Texture2DMS<float> zbuffer : register(t0);
float loadZ(int2 p) { return zbuffer.Load(p, 0); }
#else
Texture2D<float> zbuffer : register(t0);
float loadZ(int2 p) { return zbuffer.Load(int3(p, 0)); }
#endif

float ps_raw_depth(VSOut i) : SV_Target0 { return loadZ(int2(i.pos.xy)); }

// ---- AFTER_WATER: where the water is ---------------------------------------------------------------------------------
Texture2D<float> zbefore : register(t1);

float4 ps_info(VSOut i) : SV_Target0
{
    const int2 p = int2(i.pos.xy);
    const float after = loadZ(p), before = zbefore.Load(int3(p, 0));
    const bool water = after > before * (1.0 + 1e-5) + 1e-9;
    return float4(water ? 1.0 : 0.0, viewDepth(after), viewDepth(before), 0.0);
}

float4 ps_extras_clear(VSOut i) : SV_Target0 { return float4(0.0, 0.5, 0.5, 0.0); }

// ---- the surface -----------------------------------------------------------------------------------------------------
Texture2D<float4> info : register(t0);
Texture2D<float4> under : register(t1);
Texture2D<float4> after : register(t2);
Texture2D<float4> extras : register(t3);
Texture2D<float4> wakeField : register(t4);
SamplerState linearSampler : register(s1);  // under's (the API binds a sampler per texture slot)
SamplerState linearSampler4 : register(s4); // the wake field's

// The waves' slope (dh/dx, dh/dy) at world point p: 6 swell waves around the wind and 4 short ones, each faded out when
// its wavelength is under ~2 pixels (footprint: metres per pixel along the surface).
float2 waveSlope(float2 p, float t, float footprint)
{
    static const float kWave[10][3] = { // wavelength (m), angle off the wind (rad), steepness
        {23.0, 0.00, 0.10}, {14.5, 0.55, 0.10}, {9.7, -0.45, 0.11}, {6.3, 1.05, 0.10}, {4.1, -0.95, 0.10},
        {2.9, 0.25, 0.09}, {1.7, -0.30, 0.08}, {1.1, 0.80, 0.07}, {0.73, -0.70, 0.06}, {0.47, 0.15, 0.05}};
    float2 g = 0.0;
    [unroll] for (int k = 0; k < 10; ++k)
    {
        const float len = kWave[k][0], a = params2.w + kWave[k][1];
        const float kk = 2.0 * PI / len;
        const float2 dir = float2(cos(a), sin(a));
        const float omega = sqrt(9.81 * kk);
        const float amp = kWave[k][2] / kk; // steepness = A k
        const float fade = saturate(len / (4.0 * footprint) - 0.5);
        const float phase = dot(dir, p) * kk - omega * t + k * 1.7;
        g += dir * (amp * kk * cos(phase) * fade);
    }
    return g * params.x;
}

float fresnel(float c) { return 0.02 + 0.98 * pow(1.0 - saturate(c), 5.0); }

// Screen-space reflection: the reflected ray from view point v along r (view space) marched against the picture before the
// water (info.z); `hit` 0..1 with its colour, else the colour where the ray's far end lands on the screen's edge.
float3 reflection(float3 v, float3 r, out float hit)
{
    hit = 0.0;
    float t = 0.5, prevT = 0.0;
    float2 uv = 0.0;
    const float2 sz = float2(size.xy);
    [loop] for (int s = 0; s < 24; ++s)
    {
        const float3 q = v + r * t;
        if (q.z < 0.3)
            break;
        const float2 px = proj.zw + q.xy / q.z * proj.xy;
        if (any(px < 0.0) || any(px >= sz))
            break;
        const float scene = info.Load(int3(px, 0)).z;
        if (q.z > scene && q.z - scene < max(1.5, 0.08 * q.z + t * 0.1))
        {
            // refine between the last two steps
            float lo = prevT, hi = t;
            [unroll] for (int b = 0; b < 4; ++b)
            {
                const float mid = 0.5 * (lo + hi);
                const float3 m = v + r * mid;
                const float2 mp = proj.zw + m.xy / m.z * proj.xy;
                if (m.z > info.Load(int3(mp, 0)).z)
                    hi = mid;
                else
                    lo = mid;
            }
            const float3 h = v + r * hi;
            uv = (proj.zw + h.xy / h.z * proj.xy) / sz;
            const float2 edge = min(uv, 1.0 - uv);
            hit = saturate(min(edge.x, edge.y) * 12.0) * saturate(1.0 - s / 24.0 * 0.3);
            return under.SampleLevel(linearSampler, uv, 0).rgb;
        }
        prevT = t;
        t = t * 1.35 + 0.4;
    }
    // the edge fallback: where the ray heads, clamped to the picture (mostly the sky above the horizon)
    const float3 far = v + r * 2000.0;
    float2 fpx = far.z > 1.0 ? proj.zw + far.xy / far.z * proj.xy : float2(proj.z + r.x * 4096.0, -1.0);
    fpx = clamp(fpx, 1.0, sz - 2.0);
    return under.SampleLevel(linearSampler, fpx / sz, 0).rgb;
}

struct Surface
{
    float3 colour;
    float3 normal; // world
    float mask;
};

Surface shade(float2 pos)
{
    Surface o;
    const int2 p = int2(pos);
    const float4 inf = info.Load(int3(p, 0));
    o.mask = inf.x;
    o.colour = 0.0;
    o.normal = float3(0.0, 0.0, 1.0);
    if (inf.x < 0.5)
        return o;
    const float w = inf.y;
    const float3 v = viewPoint(pos, w);
    const float3 d = toWorld(v);       // from the camera, world axes
    const float dist = length(d);
    const float3 eye = d / dist;       // camera -> point
    const float2 world = cam.xy + d.xy;
    const float cosView = max(abs(eye.z), 0.12);
    const float footprint = w / proj.y / cosView;
    float2 slope = waveSlope(world, cam.w, footprint);
    // the other stages
    const float4 ex = extras.Load(int3(p, 0));
    slope += (ex.gb - 0.5);
    float foam = ex.r;
    if (params2.y > 0.5)
    {
        const float2 uv = 0.5 + float2(d.x + wake.x, -(d.y + wake.y)) * wake.z;
        const float4 wk = wakeField.SampleLevel(linearSampler4, uv, 0);
        slope += wk.rg * 2.0; // swim strokes stand out from the swell
        foam = max(foam, wk.a);
    }
    const float3 n = normalize(float3(-slope, 1.0));
    o.normal = n;
    const float3 toEye = -eye;
    const float nv = dot(n, toEye);
    const float F = fresnel(max(nv, 0.0)) * params.y;

    // refraction: the seabed shifted by the waves' slope, absorbed by the water's thickness along the view ray
    const float3 nView = toView(n - float3(0.0, 0.0, 1.0));
    float2 rp = pos + nView.xy * (params.w * proj.y / w) * saturate((inf.z - w) * 0.5);
    rp = clamp(rp, 0.5, float2(size.xy) - 0.5);
    float4 rinf = info.Load(int3(rp, 0));
    if (rinf.z < w) // a point in front of the water: take the unshifted one
    {
        rp = pos;
        rinf = inf;
    }
    float3 seabed = under.SampleLevel(linearSampler, rp / float2(size.xy), 0).rgb;
    seabed *= 1.0 + ex.a * 1.5; // caustics (stage 2)
    const float thickness = rinf.z >= FAR * 0.5 ? 1e4 : max(rinf.z - w, 0.0) * dist / w;
    const float3 absorb = params2.x * float3(1.6, 0.55, 0.42);
    const float3 T = exp(-absorb * thickness) * 0.7; // 30% turbid at any depth, as the game's water is milky
    const float3 deep = tint.rgb * tint.w;
    const float3 refracted = seabed * T + deep * (1.0 - T);

    // reflection (world r, view rv); flattened towards up so it never points into the water
    float3 r = reflect(eye, n);
    r.z = max(r.z, 0.02);
    r = normalize(r);
    float hit;
    const float3 sky = reflection(v, toView(r), hit);
    float3 col = lerp(refracted, sky, F);

    // the sun: glitter (a sharp lobe on each wave facet) and its path (a broad one), in daylight
    if (sun.w > 0.0)
    {
        const float3 h = normalize(sun.xyz + toEye);
        const float nh = saturate(dot(n, h));
        const float3 sunColour = lerp(float3(1.0, 0.55, 0.3), float3(1.0, 0.96, 0.88), saturate(sun.z * 3.0));
        const float spec = pow(nh, 3000.0) * 30.0 + pow(nh, 400.0) * 0.12;
        col += sunColour * spec * fresnel(saturate(dot(h, toEye))) * 4.0 * sun.w * params.z;
    }
    // foam (stages 2, 3), lit by day
    const float3 foamColour = float3(0.86, 0.9, 0.92) * lerp(0.18, 1.0, sun.w);
    col = lerp(col, foamColour, saturate(foam) * 0.85);
    // the game's fog: far away the game's own (fogged) water
    const float vis = fog.y != 0.0 ? saturate((fog.x + fog.y * w) / 255.0) : 1.0;
    o.colour = lerp(after.Load(int3(p, 0)).rgb, col, vis);
    if (params2.z > 2.5)
        o.colour = hit > 0.0 ? sky : float3(0.6, 0.0, 0.0);
    return o;
}

float4 ps_water(VSOut i) : SV_Target0
{
    const Surface s = shade(i.pos.xy);
    if (s.mask < 0.5)
        discard;
    return float4(s.colour, 1.0);
}

float4 ps_water_show(VSOut i) : SV_Target0
{
    const float4 inf = info.Load(int3(int2(i.pos.xy), 0));
    if (params2.z > 3.5) // 4: the picture before the water (refraction / reflection source)
        return float4(under.Load(int3(int2(i.pos.xy), 0)).rgb, 1.0);
    if (params2.z < 1.5) // the mask, the water's thickness as blue, the sky behind it as cyan
    {
        if (inf.x < 0.5)
            return float4(after.Load(int3(int2(i.pos.xy), 0)).rgb * 0.3, 1.0);
        return inf.z >= FAR * 0.5 ? float4(0.0, 0.8, 0.8, 1.0) : float4(0.0, 0.0, saturate((inf.z - inf.y) / 20.0), 1.0);
    }
    const Surface s = shade(i.pos.xy);
    if (s.mask < 0.5)
        discard;
    return float4(params2.z < 2.5 ? s.normal * 0.5 + 0.5 : s.colour, 1.0);
}
