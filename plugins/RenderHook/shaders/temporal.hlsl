// RenderHook.asi: temporal AA groundwork - camera motion vectors from the Z buffer and the renderer's reprojection
// (VcsRenderFrame.reproject), for DLAA later. Debug views only for now (VCS_RH_MV_VIEW):
//   ps_motion_view   1: the motion vectors as colour (hue = direction, brightness = length; 8 px = full)
//   ps_reproject     2: |current - previous frame warped by the motion vectors| x4 (a static world goes black when the
//                    vectors are right; moving peds and cars stay, they have no vectors of their own yet)
//                    3: the same without the warp (what the vectors remove)
//   ps_copy          the resolved picture into the mod's previous-frame texture
// Full-screen triangles from vs_main (shaders/gtao.hlsl's, same layout).

#ifndef MS
#define MS 0
#endif

cbuffer Temporal : register(b0)
{
    float4 q0, q1, q2, q3; // reprojection rows: h' = Q (u w, v w, z w, w), (u, v) unjittered target pixels, z GS z, w view depth
    float4 zmap;           // a, b, scale, far: GS z = d / scale = a + b / w (d = 0: nothing drawn)
    float4 jitter;         // this frame's offset xy, the last frame's zw (target pixels)
    int4 size;             // width, height, mode (2: warped, 3: not), objects (1: the renderer's per-object motion)
};

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

#if MS
Texture2DMS<float> zbuffer : register(t0);
#else
Texture2D<float> zbuffer : register(t0);
#endif
Texture2D<float4> previous : register(t1);
Texture2D<float4> current : register(t2);
SamplerState linearClamp : register(s1);
Texture2D<float4> velocity : register(t3); // per-object motion: xy pixels moved, z view depth, w covered

// where this pixel's surface was in the last frame (unjittered target pixels); its unjittered position now in `now`
float2 previousPosition(int2 p, out float2 now)
{
#if MS
    const float d = zbuffer.Load(p, 0);
#else
    const float d = zbuffer.Load(int3(p, 0));
#endif
    float z = d / zmap.z;
    float w = (d > 0.0 && z != zmap.x) ? zmap.y / (z - zmap.x) : zmap.w;
    if (!(w > 0.0) || w > zmap.w) // the sky: a point far away along the pixel's ray
    {
        w = zmap.w;
        z = zmap.x + zmap.y / w;
    }
    now = float2(p) + 0.5 - jitter.xy;
    // an object's own motion where its draw is what the Z buffer shows here (same depth)
    if (size.w != 0)
    {
        const float4 v = velocity.Load(int3(p, 0));
        if (v.w > 0.5 && abs(v.z - w) <= 0.02 * w + 0.05)
            return now - v.xy;
    }
    const float4 h = float4(now * w, z * w, w);
    const float4 hp = float4(dot(q0, h), dot(q1, h), dot(q2, h), dot(q3, h));
    return hp.xy / hp.w;
}

float3 hue(float a)
{
    return saturate(abs(frac(a + float3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0);
}

float4 ps_motion_view(VSOut i) : SV_Target0
{
    if (size.z == 4) // the renderer's velocity buffer itself: red = covered, green = moved (8 px = full), blue = depth agrees
    {
        const float4 v = velocity.Load(int3(int2(i.pos.xy), 0));
        float2 n;
        const float2 cam = previousPosition(int2(i.pos.xy), n); // (the depth check below needs the pixel's w)
#if MS
        const float d = zbuffer.Load(int2(i.pos.xy), 0);
#else
        const float d = zbuffer.Load(int3(int2(i.pos.xy), 0));
#endif
        const float z = d / zmap.z;
        const float w = d > 0.0 ? zmap.y / (z - zmap.x) : 0.0;
        return float4(v.w, saturate(length(v.xy) / 8.0), (v.w > 0.5 && abs(v.z - w) <= 0.02 * w + 0.05) ? 1.0 : 0.0, 1.0);
    }
    float2 now;
    const float2 was = previousPosition(int2(i.pos.xy), now);
    const float2 mv = now - was;
    const float len = length(mv);
    return float4(hue(atan2(mv.y, mv.x) / 6.2831853 + 0.5) * saturate(len / 8.0), 1.0);
}

float4 ps_reproject(VSOut i) : SV_Target0
{
    const int2 p = int2(i.pos.xy);
    float2 now;
    const float2 was = size.z == 2 ? previousPosition(p, now) : float2(p) + 0.5 - jitter.xy;
    // the last picture was drawn with its own jitter: the surface sat at was + that offset there
    const float2 uv = (was + jitter.zw) / float2(size.xy);
    const float3 prev = previous.SampleLevel(linearClamp, uv, 0).rgb;
    const float3 cur = current.Load(int3(p, 0)).rgb;
    const bool outside = any(uv < 0.0) || any(uv > 1.0);
    return float4(outside ? float3(0.0, 0.0, 0.3) : saturate(abs(cur - prev) * 4.0), 1.0);
}

float4 ps_copy(VSOut i) : SV_Target0
{
    return current.Load(int3(int2(i.pos.xy), 0));
}

// ---- DLAA's inputs (src/dlaa.cpp) ------------------------------------------------------------------------------------
// ps_dlaa_depth: a reversed depth for DLSS (0.05 / view depth: larger = nearer, 0 = the sky; the GS Z's own scale is
// tiny); ps_dlaa_motion: the motion vectors DLSS wants - where the pixel's surface was in the last frame minus where it is
// (unjittered pixels), from the object's own motion or the camera's
float ps_dlaa_depth(VSOut i) : SV_Target0
{
#if MS
    const float d = zbuffer.Load(int2(i.pos.xy), 0);
#else
    const float d = zbuffer.Load(int3(int2(i.pos.xy), 0));
#endif
    const float z = d / zmap.z;
    const float w = (d > 0.0 && z != zmap.x) ? zmap.y / (z - zmap.x) : 0.0;
    return w > 0.0 ? min(0.05 / w, 1.0) : 0.0;
}

float4 ps_dlaa_motion(VSOut i) : SV_Target0
{
    float2 now;
    const float2 was = previousPosition(int2(i.pos.xy), now);
    return float4(was - now, 0.0, 0.0);
}
