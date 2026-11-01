// RenderHook.asi: wakes and ripples - a height field around the camera in the game's world axes (src/water_sim.cpp).
//
// The state (RGBA16F, N x N texels of `dx` metres; texel (i, j): i east, j south, row 0 the north edge): r the height
// now, g the height one step before, b the foam. Full-screen passes (vs_main):
//   ps_wake_step    one step of the damped wave equation (h' = h + (h - h_prev) damp + c2 laplacian), scrolled by whole
//                   texels as the field follows the camera, with the step's sources: hulls pressing the water down
//                   (capsules) and one-off splashes (rings); foam from the sources and from fast-moving water, fading
//   ps_wake_out     the field the water shader reads: slope x / y, height, foam - faded to 0 at the border
//   ps_wake_show    the debug view: the field in a corner of the picture (height blue down / red up, foam white)
//   ps_wake_water   the debug stand-in for the water shader: pixels at the water's level shaded with the field's normal
//                   and foam (MS=1: a multisampled Z buffer)

#ifndef MS
#define MS 0
#endif
#define MAX_SOURCES 32

struct VSOut
{
    float4 pos : SV_Position;
};

VSOut vs_main(uint id : SV_VertexID)
{
    VSOut o;
    const float2 c = float2((id << 1) & 2, id & 2);
    o.pos = float4(c.x * 2.0 - 1.0, 1.0 - c.y * 2.0, 0.0, 1.0);
    return o;
}

cbuffer WakeStep : register(b0)
{
    float4 grid;  // N, dx (m), c2 = (c dt / dx)^2, damping of the velocity per step
    float4 steps; // dt (s), foam kept per step, foam made per m/s of vertical speed above `make_from`, reset (1: zeros)
    float4 more;  // make_from (m/s), foam spread per step, height kept per step (relaxation to the level), -
    int4 shift;   // the source texel = this texel + shift.xy (the field moved); z = source count
    float4 seg[MAX_SOURCES];  // a.xy, b.xy: the capsule's ends, metres from the field's centre (x east, y north)
    float4 shape[MAX_SOURCES]; // radius (m), depth (m), foam (0..1), kind (0 hull, 1 splash ring)
};

Texture2D<float4> state : register(t0);

float4 cell(int2 p, int n)
{
    if (any(p < 0) || any(p >= n))
        return 0.0;
    return state.Load(int3(p, 0));
}

float segDist(float2 p, float2 a, float2 b)
{
    const float2 ab = b - a;
    const float t = saturate(dot(p - a, ab) / max(dot(ab, ab), 1e-6));
    return length(p - (a + ab * t));
}

float4 ps_wake_step(VSOut i) : SV_Target0
{
    if (steps.w > 0.5)
        return 0.0;
    const int n = (int)grid.x;
    const int2 p = int2(i.pos.xy);
    const int2 q = p + shift.xy;
    const float4 c = cell(q, n);
    const float4 e = cell(q + int2(1, 0), n), w = cell(q - int2(1, 0), n);
    const float4 s = cell(q + int2(0, 1), n), nn = cell(q - int2(0, 1), n);
    const float h = c.r, hp = c.g;
    const float lap = e.r + w.r + s.r + nn.r - 4.0 * h;
    // the wave equation keeps the water a hull pushed aside: a slow relaxation to the level lets it go
    float hn = (h + (h - hp) * grid.w + grid.z * lap) * more.z;

    // foam: kept, spread a little, made where the water moves fast
    float foam = lerp(c.b, 0.25 * (e.b + w.b + s.b + nn.b), more.y) * steps.y;
    const float vz = abs(hn - h) / steps.x;
    foam += steps.z * steps.x * max(vz - more.x, 0.0);

    // the sources
    const float2 at = float2(p.x + 0.5 - grid.x * 0.5, grid.x * 0.5 - (p.y + 0.5)) * grid.y;
    for (int k = 0; k < shift.z; ++k)
    {
        const float4 sh = shape[k];
        const float d = segDist(at, seg[k].xy, seg[k].zw);
        if (sh.w < 0.5)
        {
            // a hull: the water pressed down to `depth` inside, eased out to the radius
            const float inside = 1.0 - smoothstep(sh.x * 0.45, sh.x, d);
            hn = lerp(hn, min(hn, -sh.y), inside);
            foam = max(foam, sh.z * inside);
        }
        else
        {
            // a splash: a dip of `depth` (gaussian of the radius) that rings outwards
            const float g = exp(-2.0 * (d * d) / (sh.x * sh.x));
            hn -= sh.y * g;
            foam = max(foam, sh.z * g);
        }
    }

    // the border absorbs (no reflections off the field's edge)
    const float edge = min(min(p.x, n - 1 - p.x), min(p.y, n - 1 - p.y));
    const float keep = lerp(0.85, 1.0, saturate(edge / 12.0));
    return float4(clamp(hn * keep, -3.0, 3.0), h * keep, saturate(foam), 0.0);
}

float4 ps_wake_out(VSOut i) : SV_Target0
{
    const int n = (int)grid.x;
    const int2 p = int2(i.pos.xy);
    const float h = cell(p, n).r;
    const float sx = (cell(p + int2(1, 0), n).r - cell(p - int2(1, 0), n).r) / (2.0 * grid.y);
    const float sy = (cell(p - int2(0, 1), n).r - cell(p + int2(0, 1), n).r) / (2.0 * grid.y);
    const float edge = min(min(p.x, n - 1 - p.x), min(p.y, n - 1 - p.y));
    const float fade = saturate((edge - 1.0) / 16.0);
    return float4(sx, sy, h, cell(p, n).b) * fade;
}

// ---- debug views ---------------------------------------------------------------------------------------------------
cbuffer WakeView : register(b1)
{
    float4 proj;  // fx, fy, cx, cy
    float4 zmap;  // a, b, scale, -
    float4 r0, r1, r2; // world_to_view's rows (xyz)
    float4 field; // cam_x, cam_y, size, water_z (relative to the camera)
    float4 rect;  // the corner view: x, y, width, height (target pixels)
    float4 look;  // strength of the stand-in, -, -, -
};

Texture2D<float4> wake : register(t2);
SamplerState wakeSampler : register(s2);

float4 ps_wake_show(VSOut i) : SV_Target0
{
    const float2 uv = (i.pos.xy - rect.xy) / rect.zw;
    const float4 f = wake.SampleLevel(wakeSampler, uv, 0);
    const float h = f.b * 4.0;
    float3 col = float3(0.08, 0.12, 0.2) + float3(saturate(h), 0.25 * abs(h), saturate(-h));
    col += 0.5 * float3(saturate(f.r * 3.0), saturate(f.g * 3.0), 0.0);
    col = lerp(col, 1.0, f.a);
    // the camera
    const float2 cam = 0.5 + float2(field.x, -field.y) / field.z;
    if (length((uv - cam) * rect.zw) < 3.0)
        col = float3(1.0, 1.0, 0.0);
    return float4(col, 1.0);
}

#if MS
Texture2DMS<float> zbuffer : register(t1);
#else
Texture2D<float> zbuffer : register(t1);
#endif

float4 ps_wake_water(VSOut i) : SV_Target0
{
#if MS
    const float z = zbuffer.Load(int2(i.pos.xy), 0);
#else
    const float z = zbuffer.Load(int3(i.pos.xy, 0));
#endif
    const float t = z / zmap.z - zmap.x;
    if (!(z > 0.0) || t == 0.0)
        discard;
    const float wv = zmap.y / t;
    const float3 v = float3((i.pos.x - proj.z) / proj.x * wv, (i.pos.y - proj.w) / proj.y * wv, wv);
    const float3 d = v.x * r0.xyz + v.y * r1.xyz + v.z * r2.xyz;
    if (abs(d.z - field.w) > 0.45 + 0.004 * wv)
        discard;
    const float2 uv = 0.5 + float2(d.x + field.x, -(d.y + field.y)) / field.z;
    if (any(uv < 0.0) || any(uv > 1.0))
        discard;
    const float4 f = wake.SampleLevel(wakeSampler, uv, 0);
    const float3 nrm = normalize(float3(-f.r, -f.g, 1.0));
    const float3 sun = normalize(float3(0.45, 0.3, 0.84));
    const float3 eye = normalize(-d);
    const float lit = dot(nrm, sun) - sun.z;
    const float spec = pow(saturate(dot(reflect(-sun, nrm), eye)), 40.0) - pow(saturate(dot(reflect(-sun, float3(0, 0, 1)), eye)), 40.0);
    const float3 shade = float3(0.6, 0.75, 0.8) * saturate(0.5 + 2.0 * lit) + spec;
    const float3 col = lerp(shade, float3(0.95, 0.97, 1.0), saturate(f.a * 1.2));
    const float a = saturate(look.x * (abs(lit) * 6.0 + abs(spec) + f.a));
    return float4(col, a);
}
