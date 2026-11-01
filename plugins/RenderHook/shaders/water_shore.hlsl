// RenderHook.asi: the shore - foam where the sea meets the sand, waves that steepen, break and run up the beach, and
// caustics on the seabed under shallow water (src/water_shore.cpp; the port's docs/RENDERHOOK_PLAN.md "Water").
//
// All of it from heights: the game's water is one plane at z 6.0 (docs/game-internals.md 6q) and the sea's mesh runs on
// under the sand, so the seabed's depth below the water - from the water hook's seabed depth (water.h Frame.info.z) -
// says how near the shore a water pixel is, and the ground's height above 6.0 where the waves run up.
//
// ps_shore_extras  into the water's extras (water.h Frame.extras, RGBA8, opaque): r = foam 0..1, g/b = the shore waves'
//                  slope dh/dx, dh/dy (world) as 0.5 + slope, a = caustic light on the seabed 0..1. The water surface
//                  shader (stage 1) applies them.
// ps_shore_swash   into the picture after the water, before peds and vehicles (VCS_RENDER_BLEND_ALPHA): the waves'
//                  run-up on the sand above the waterline - a thin sheet of water, its foam edge, wet sand behind it.
//                  MS=1: a multisampled Z buffer.
// ps_shore_show    the debug view (a BEFORE_POST pass, over the finished surface), opaque: on the water the seabed depth
//                  in 0.5 m bands (blue), crests red, foam white, caustics yellow; on land the height above the water
//                  level in 0.1 m bands - below it blue, up to the run-up's reach green, above orange; sky dark. MS=1.
//
// The waves: crests are lines of equal seabed depth D (they run along the shore), phase D / spacing + t / period,
// warped by a slow noise so they do not look like contour lines; they grow as D falls to the breaking depth, break into
// foam there and reach the waterline at phase 0, where the run-up starts. The swash pass uses the same phase at D = 0.

#ifndef MS
#define MS 0
#endif

cbuffer Shore : register(b0)
{
    float4 proj;     // fx, fy, cx, cy (target pixels)
    float4 zmap;     // a, b, scale, far: GS z = d / scale = a + b / w
    float4 rowX;     // world_to_view row 0 (view = R world)
    float4 rowY;     // row 1
    float4 rowZ;     // row 2
    float4 camera;   // the camera in game world coordinates; w = the water level (6.0)
    float4 sun;      // towards the sun (world); w = daylight 0..1
    float4 anim;     // time (s, wrapped), wave period (s), crest spacing (m of seabed depth), breaking depth (m)
    float4 amount;   // foam, waves, caustics (0..1 each; 0 = off), run-up height (m)
    float4 fade;     // fog bias, fog scale (0: no fog), fade start, fade end (m)
    int4 size;       // the picture's width, height; the water rect's x0, y0
};

struct VSOut
{
    float4 pos : SV_Position;
};

static const float PI = 3.14159265;
static const float TAU = 6.28318531;

Texture2D<float4> info : register(t0); // water.h Frame.info: x mask, y surface view depth, z seabed view depth (1e4: none)
#if MS
Texture2DMS<float> zbuffer : register(t1);
#else
Texture2D<float> zbuffer : register(t1);
#endif

// ---- view / world ----------------------------------------------------------------------------------------------------

float3 viewPos(float2 centre, float w)
{
    return float3((centre - proj.zw) / proj.xy * w, w);
}

// view space -> game world (R^T v + camera)
float3 toWorld(float3 v)
{
    return camera.xyz + rowX.xyz * v.x + rowY.xyz * v.y + rowZ.xyz * v.z;
}

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

// the game's distance fog and the effect's own fade with distance (1 near, 0 gone)
float distanceFade(float w)
{
    const float far = 1.0 - smoothstep(fade.z, fade.w, w);
    const float fog = fade.y != 0.0 ? saturate((fade.x + fade.y * w) / 255.0) : 1.0;
    return far * fog;
}

// ---- noise -------------------------------------------------------------------------------------------------------------

float hash12(float2 p)
{
    float3 q = frac(float3(p.xyx) * 0.1031);
    q += dot(q, q.yzx + 33.33);
    return frac((q.x + q.y) * q.z);
}

float2 hash22(float2 p)
{
    float3 q = frac(float3(p.xyx) * float3(0.1031, 0.1030, 0.0973));
    q += dot(q, q.yzx + 33.33);
    return frac((q.xx + q.yz) * q.zy);
}

float valueNoise(float2 p)
{
    const float2 i = floor(p), f = frac(p);
    const float2 u = f * f * (3.0 - 2.0 * f);
    const float a = hash12(i), b = hash12(i + float2(1.0, 0.0)), c = hash12(i + float2(0.0, 1.0)), d = hash12(i + float2(1.0, 1.0));
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

float fbm(float2 p)
{
    return 0.5 * valueNoise(p) + 0.3 * valueNoise(p * 2.03 + 17.1) + 0.2 * valueNoise(p * 4.01 + 41.7);
}

// distance to the nearest cell border of an animated Voronoi pattern (the feature points circle in their cells)
float voronoiEdge(float2 p, float t)
{
    const float2 i = floor(p), f = frac(p);
    float2 nearest = 0.0, nearestCell = 0.0;
    float best = 8.0;
    [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x)
        {
            const float2 g = float2(x, y);
            const float2 h = hash22(i + g);
            const float2 o = 0.5 + 0.4 * sin(t * (0.6 + 0.5 * h) + TAU * h);
            const float2 r = g + o - f;
            const float d = dot(r, r);
            if (d < best)
            {
                best = d;
                nearest = r;
                nearestCell = g;
            }
        }
    float edge = 8.0;
    [unroll] for (int y2 = -1; y2 <= 1; ++y2)
        [unroll] for (int x2 = -1; x2 <= 1; ++x2)
        {
            const float2 g = nearestCell + float2(x2, y2);
            const float2 h = hash22(i + g);
            const float2 o = 0.5 + 0.4 * sin(t * (0.6 + 0.5 * h) + TAU * h);
            const float2 r = g + o - f;
            const float2 m = r - nearest;
            if (dot(m, m) > 1e-5)
                edge = min(edge, dot(0.5 * (nearest + r), normalize(m)));
        }
    return edge;
}

// caustic light at seabed point p (world xy, m): two layers of bright cell borders crossing, 0..1
float caustics(float2 p, float t)
{
    const float a = voronoiEdge(p * 0.85 + float2(0.11, 0.07) * t, t * 0.9);
    const float b = voronoiEdge(p * 1.30 - float2(0.05, 0.12) * t + 7.3, t * 1.1 + 3.0);
    const float la = pow(saturate(1.0 - a * 4.0), 3.0), lb = pow(saturate(1.0 - b * 5.0), 3.0);
    return saturate(0.65 * la + 0.55 * lb + 1.2 * la * lb);
}

// ---- the shore waves ---------------------------------------------------------------------------------------------------

// the waves' phase at seabed depth D (m) and world point p: crests at whole numbers, travelling towards D = 0. Spaced by
// sqrt(D): VCS's beaches drop steeply into the sea (the seabed 5 m down within ~8 m of the waterline at (862, -300), ~1:15
// at (870, 600): game-internals.md 6q), so crests evenly spaced in depth would crowd into a few metres; anim.z = the
// spacing in sqrt(m) (0.5: crests at D 0.25, 1, 2.25, 4, 6.25, 9 m)
float wavePhase(float D, float2 p)
{
    const float warp = 0.8 * fbm(p * 0.035) + 0.25 * fbm(p * 0.11 + 5.0);
    return sqrt(D) / anim.z + anim.x / anim.y + warp;
}

// the wave's profile 0..1 over a phase's fraction s (0 / 1: the crest; s grows seawards): a steep face towards the shore
// (s from 1 - FRONT to 1), a long back seawards; and its derivative d/ds
static const float FRONT = 0.12;
float profileAt(float s)
{
    const float b = 1.0 - FRONT;
    return s < b ? (1.0 - s / b) * (1.0 - s / b) : 1.0 - ((1.0 - s) / FRONT) * ((1.0 - s) / FRONT);
}
float profileSlope(float s)
{
    const float b = 1.0 - FRONT;
    return s < b ? -2.0 * (1.0 - s / b) / b : 2.0 * (1.0 - s) / (FRONT * FRONT);
}

struct Shore
{
    float foam, crest, caustic;
    float2 slope;
    float D; // seabed depth below the water (m; large: deep)
};

Shore shoreAt(float2 pixel)
{
    Shore o;
    o.foam = 0.0;
    o.crest = 0.0;
    o.caustic = 0.0;
    o.slope = 0.0;
    o.D = 1e4;
    const float4 w = info.Load(int3(pixel, 0));
    const bool valid = w.x >= 0.5 && w.z > 0.0 && w.z < 1e3;
    const float3 seabed = toWorld(viewPos(pixel + 0.5, valid ? w.z : 1.0));
    const float D = camera.w - seabed.z;
    // the gradient of D across the seabed (world xy), from the pixel quad's derivatives (before any early return: they
    // need the whole quad): the direction to the shore
    const float2 dx = ddx(seabed.xy), dy = ddy(seabed.xy);
    const float dDx = ddx(D), dDy = ddy(D);
    if (!valid)
        return o;
    o.D = D;
    const float det = dx.x * dy.y - dx.y * dy.x;
    float2 grad = abs(det) > 1e-6 ? float2(dDx * dy.y - dDy * dx.y, dDy * dx.x - dDx * dy.x) / det : 0.0;
    const float gl = length(grad);
    grad = gl > 1.5 ? grad * (1.5 / gl) : grad;
    if (D < -0.25 || D > 12.0)
        return o;
    const float t = anim.x;
    const float Dc = max(D, 0.0);
    // the waves: grow towards the breaking depth (anim.w), gone in deep water (past ~9 m)
    const float phase = wavePhase(Dc, seabed.xy);
    const float s = frac(phase);
    const float grow = 1.0 - smoothstep(anim.w * 2.0, anim.w * 9.0, Dc);
    const float broken = 1.0 - smoothstep(0.3 * anim.w, 1.5 * anim.w, Dc); // past the breaking depth: foam, lower, flatter
    const float height = grow * (0.6 - 0.4 * broken);
    // h(s) = height * profile(s); dh/dxy = height * profile'(s) * dphase/dD * grad D, dphase/dD = 1 / (2 sqrt(D) spacing)
    const float dPhase = 1.0 / (2.0 * sqrt(max(Dc, 0.05)) * anim.z);
    o.slope = amount.y * height * 0.35 * profileSlope(s) * min(dPhase, 4.0) * grad;
    o.crest = grow * profileAt(s) * profileAt(s);
    // foam: a band along the waterline, the breaking crests and the foam they leave behind (trailing to the shore)
    const float2 fp = seabed.xy * 1.7 + float2(0.21, -0.13) * t;
    const float n = fbm(fp), n2 = fbm(seabed.xy * 4.3 - float2(0.4, 0.3) * t);
    const float edge = (1.0 - smoothstep(0.0, 0.2 + 0.25 * n, Dc)) * smoothstep(0.3, 0.6, n * 0.6 + n2 * 0.6);
    // the breaking crest: a white line on the wave's face and just behind it, torn up by the noise
    const float onCrest = max(saturate(1.0 - s / 0.22), smoothstep(1.0 - FRONT * 0.7, 1.0, s));
    const float breaking = broken * grow * onCrest * smoothstep(0.3, 0.6, n * 0.5 + n2 * 0.7) * 1.3;
    // the foam it leaves, thinning out behind it on the way to the shore
    const float trail = broken * saturate(1.0 - s / 0.7) * smoothstep(0.5, 0.85, n2 * 0.7 + n * 0.5) * 0.7;
    o.foam = amount.x * saturate(edge + breaking + trail);
    // caustics: light focused by the waves onto the seabed, along the sun's way through the water (refracted: steeper)
    if (amount.z > 0.0 && D > 0.0)
    {
        const float2 along = sun.xy / max(sun.z, 0.25) * 0.75;
        const float c = caustics(seabed.xy * 0.9 + along * D, t);
        const float depthFade = smoothstep(0.0, 0.15, D) * exp(-D / 3.0);
        o.caustic = amount.z * sun.w * c * depthFade;
    }
    return o;
}

float4 ps_shore_extras(VSOut i) : SV_Target0
{
    const float2 p = floor(i.pos.xy);
    const Shore s = shoreAt(p);
    const float4 w = info.Load(int3(p, 0));
    const float f = distanceFade(w.y);
    return float4(saturate(s.foam * f), saturate(0.5 + s.slope * f), saturate(s.caustic * f));
}

// (ps_shore_show is below the run-up: it uses its helpers)

// ---- the run-up on the sand --------------------------------------------------------------------------------------------

// the pixel's ground normal (world) from its neighbours' depths, as in shadows.hlsl
float3 groundNormal(int2 p, float3 P)
{
    const float wl = viewDepthAt(p - int2(1, 0)), wr = viewDepthAt(p + int2(1, 0));
    const float wu = viewDepthAt(p - int2(0, 1)), wd = viewDepthAt(p + int2(0, 1));
    const float3 L = viewPos(float2(p) + float2(-0.5, 0.5), wl), Rr = viewPos(float2(p) + float2(1.5, 0.5), wr);
    const float3 U = viewPos(float2(p) + float2(0.5, -0.5), wu), Dd = viewPos(float2(p) + float2(0.5, 1.5), wd);
    float3 dxv = (wl > 0.0 && (wr <= 0.0 || abs(P.z - L.z) <= abs(Rr.z - P.z))) ? P - L : (wr > 0.0 ? Rr - P : float3(1, 0, 0));
    float3 dyv = (wu > 0.0 && (wd <= 0.0 || abs(P.z - U.z) <= abs(Dd.z - P.z))) ? P - U : (wd > 0.0 ? Dd - P : float3(0, 1, 0));
    float3 N = cross(dyv, dxv);
    const float len = length(N);
    N = len > 1e-12 ? N / len : -normalize(P);
    if (dot(N, P) > 0.0)
        N = -N;
    return rowX.xyz * N.x + rowY.xyz * N.y + rowZ.xyz * N.z; // to the world
}

// the run-up's reach above the waterline (m) at time phase u (0: a crest reaches the waterline)
float runUp(float u)
{
    const float rise = 0.28;
    return u < rise ? sin(u / rise * PI * 0.5) : 1.0 - smoothstep(rise, 1.0, u);
}

float4 ps_shore_swash(VSOut i) : SV_Target0
{
    const int2 p = int2(i.pos.xy);
    if (info.Load(int3(p, 0)).x >= 0.5)
        return 0.0; // the water's own pixels: the surface shader's
    const float w = viewDepthAt(p);
    if (w <= 0.0)
        return 0.0;
    const float3 P = viewPos(float2(p) + 0.5, w);
    const float3 W = toWorld(P);
    const float h = W.z - camera.w; // above the water level
    const float reach = amount.w;
    if (h < -0.3 || h > reach * 1.3)
        return 0.0;
    const float3 N = groundNormal(p, P);
    const float flat = smoothstep(0.75, 0.92, N.z);
    if (flat <= 0.0)
        return 0.0;
    const float t = anim.x;
    // the phase the waves arrive with at the waterline here (the same warp as wavePhase at D = 0)
    const float u = frac(t / anim.y + 0.8 * fbm(W.xy * 0.035) + 0.25 * fbm(W.xy * 0.11 + 5.0));
    const float r = reach * runUp(u) - 0.05; // the sheet's edge, height above the level
    const float n = fbm(W.xy * 1.7 + float2(0.21, -0.13) * t), n2 = fbm(W.xy * 4.3 - float2(0.4, 0.3) * t);
    const float edgeJitter = (n - 0.5) * 0.06;
    const float under = smoothstep(0.0, 0.03, r + edgeJitter - h);           // covered by the sheet
    const float edgeLine = exp(-abs(r + edgeJitter - h) / 0.02) * (u < 0.5 ? 1.0 : 1.0 - smoothstep(0.5, 0.9, u));
    const float wetTo = reach * 0.9;                                          // the highest the sand stays wet
    const float wet = 1.0 - smoothstep(wetTo * 0.6, wetTo, h);
    const float day = lerp(0.3, 1.0, sun.w);
    const float foamA = saturate(amount.y * edgeLine * smoothstep(0.3, 0.6, n2 + 0.2) * 0.8 + amount.x * under * smoothstep(0.6, 0.85, n2) * 0.3);
    const float sheet = amount.y * under * 0.3;  // a thin film: the sand shows through, a little darker and bluer
    const float dark = amount.y * wet * 0.22;    // wet sand
    // target * (1 - dark) * (1 - sheet) * (1 - foam) + water * sheet * (1 - foam) + foam colour * foam
    const float k = (1.0 - dark) * (1.0 - sheet) * (1.0 - foamA);
    const float3 waterColour = float3(0.30, 0.43, 0.41) * day;
    const float3 add = waterColour * sheet * (1.0 - foamA) + float3(0.92, 0.95, 0.95) * day * foamA;
    const float a = (1.0 - k) * flat * distanceFade(w);
    const float aa = 1.0 - k;
    return aa > 1e-4 ? float4(add / aa, a) : 0.0;
}

// ---- caustics in pools ---------------------------------------------------------------------------------------------------
// VCS's pools are no water to the game (game-internals.md 6q): a flat blue animated texture in the world's own draws, not
// in the sea's mask. ps_pool_caustics (VCS_RENDER_BLEND_ADD, BEFORE_POST: t0 the resolved picture, t1 the Z buffer) finds
// them by look - level ground (the normal from depth), above the sea, a strongly cyan-blue colour over a neighbourhood -
// and adds the caustic pattern in the picture's own blue. MS=1: a multisampled Z buffer.
Texture2D<float4> picture : register(t2);

// how pool-like the colour is: saturated cyan-blue (the pools' texture), 0..1
float poolColour(float3 c)
{
    // measured on the Starfish Island pool at noon before the post effects: ~(0.20, 0.38, 0.45), lines ~(0.25, 0.44, 0.49)
    const float blue = c.b - max(c.r, 0.6 * c.g);
    return smoothstep(0.06, 0.16, blue) * smoothstep(0.18, 0.32, c.b) * (1.0 - smoothstep(0.45, 0.7, c.r)) *
           smoothstep(0.0, 0.06, c.b - c.g);
}

float4 ps_pool_caustics(VSOut i) : SV_Target0
{
    const int2 p = int2(i.pos.xy);
    const float w = viewDepthAt(p);
    if (w <= 0.0 || amount.z <= 0.0 || sun.w <= 0.0)
        return 0.0;
    const float3 c = picture.Load(int3(p, 0)).rgb;
    float look = poolColour(c);
    const bool debug = amount.w > 0.0; // rhshore poolview 1: colour test red, neighbours green, level ground blue
    if (look <= 0.0 && !debug)
        return 0.0;
    if (amount.w > 1.5)
        return float4(c, 1.0); // poolview 2: the picture as read (added once more)
    // the neighbourhood agrees (a pool, not a blue car's edge or a sign): 4 samples a few pixels away
    const int r = 6;
    look *= saturate((poolColour(picture.Load(int3(clamp(p + int2(r, 0), int2(0, 0), size.xy - 1), 0)).rgb) +
                      poolColour(picture.Load(int3(clamp(p - int2(r, 0), int2(0, 0), size.xy - 1), 0)).rgb) +
                      poolColour(picture.Load(int3(clamp(p + int2(0, r), int2(0, 0), size.xy - 1), 0)).rgb) +
                      poolColour(picture.Load(int3(clamp(p - int2(0, r), int2(0, 0), size.xy - 1), 0)).rgb)) / 3.0);
    const float colourOnly = poolColour(c);
    const float3 P = viewPos(float2(p) + 0.5, w);
    const float3 W = toWorld(P);
    const float3 N = groundNormal(p, P);
    const float level = smoothstep(0.93, 0.98, N.z) * smoothstep(0.6, 1.5, W.z - camera.w);
    if (debug)
        return float4(0.5 * colourOnly, 0.5 * (colourOnly > 0.0 ? look / colourOnly : 0.0), 0.5 * level, 1.0);
    look *= level;
    if (look <= 0.0)
        return 0.0;
    const float cs = caustics(W.xy * 1.6, anim.x * 1.2);
    return float4((c * 1.4 + float3(0.05, 0.09, 0.09)) * (amount.z * sun.w * look * cs * distanceFade(w)), 1.0);
}

float4 ps_shore_show(VSOut i) : SV_Target0
{
    const float2 p = floor(i.pos.xy);
    const Shore s = shoreAt(p);
    if (size.z == 2) // rhshore view 2, for measuring: r = seabed depth / 20 m (water) or 0, g = view depth / 200 m, b = (height
    {                // above the water level + 2 m) / 20 m (land)
        const float4 inf = info.Load(int3(p, 0));
        const float wv = s.D < 1e3 ? inf.y : viewDepthAt(int2(p));
        const float hv = wv > 0.0 ? toWorld(viewPos(p + 0.5, wv)).z - camera.w : -2.0;
        return float4(s.D < 1e3 ? saturate(s.D / 20.0) : 0.0, saturate(wv / 200.0), s.D < 1e3 ? 0.0 : saturate((hv + 2.0) / 20.0), 1.0);
    }
    if (s.D < 1e3)
    {
        const float band = frac(max(s.D, 0.0) * 2.0) < 0.5 ? 0.35 : 0.45;
        float3 c = float3(0.1, band, 0.6);
        c = lerp(c, float3(1.0, 0.3, 0.2), s.crest);
        c = lerp(c, float3(1.0, 1.0, 1.0), s.foam);
        c += float3(0.9, 0.8, 0.2) * s.caustic;
        return float4(saturate(c), 1.0);
    }
    const float w = viewDepthAt(int2(p));
    if (w <= 0.0)
        return float4(0.1, 0.1, 0.15, 1.0);
    const float3 P = viewPos(p + 0.5, w);
    const float h = toWorld(P).z - camera.w;
    const float3 N = groundNormal(int2(p), P);
    if (abs(h) > 2.0)
        return float4(0.3, 0.3, 0.3, 1.0) * (0.6 + 0.4 * N.z);
    const float3 hue = h < 0.0 ? float3(0.2, 0.3, 0.9) : h < amount.w ? float3(0.2, 0.9, 0.2) : float3(0.9, 0.55, 0.2);
    const float band = frac(h * 10.0) < 0.5 ? 0.65 : 1.0;
    return float4(hue * band * (0.5 + 0.5 * saturate(N.z)), 1.0);
}
