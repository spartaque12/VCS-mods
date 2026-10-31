// VR.asi's copies into the OpenXR swapchain images: a full-target triangle from SV_VertexID.
//   ps_copy: a picture (sRGB-encoded RGBA8) into the image; decoded to linear when the image is an *_SRGB format (the
//            hardware encodes again on write), alpha 1.
//   ps_hud:  the HUD drawn over black (t0) and over white (t1): coverage a = 1 - (white - black), colour = black / a;
//            written premultiplied (OpenXR's default for a layer with source alpha).
cbuffer Params : register(b0)
{
    float4 uvRect; // u0, v0, u1, v1 of the source
    float srgbOut; // 1: the target is an sRGB format (write linear)
    float3 pad;
};

Texture2D t0 : register(t0);
Texture2D t1 : register(t1);
SamplerState s0 : register(s0);

struct VsOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

VsOut vs_main(uint id : SV_VertexID)
{
    VsOut o;
    const float2 p = float2((id << 1) & 2, id & 2); // 0,0  2,0  0,2
    o.pos = float4(p * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    o.uv = lerp(uvRect.xy, uvRect.zw, p);
    return o;
}

float3 toLinear(float3 c)
{
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

float4 ps_copy(VsOut i) : SV_Target
{
    float3 c = t0.SampleLevel(s0, i.uv, 0).rgb;
    if (srgbOut > 0.5)
        c = toLinear(c);
    return float4(c, 1.0);
}

float4 ps_hud(VsOut i) : SV_Target
{
    const float3 b = t0.SampleLevel(s0, i.uv, 0).rgb;
    const float3 w = t1.SampleLevel(s0, i.uv, 0).rgb;
    const float a = saturate(1.0 - dot(w - b, float3(1.0, 1.0, 1.0) / 3.0));
    if (a <= 1.0 / 512.0)
        return float4(0.0, 0.0, 0.0, 0.0);
    float3 c = saturate(b / a);
    if (srgbOut > 0.5)
        c = toLinear(c);
    return float4(c * a, a);
}
