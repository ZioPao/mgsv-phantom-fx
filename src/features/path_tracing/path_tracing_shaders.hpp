#pragma once

// HLSL for path tracing on Direct3D 11 (path_tracing.hpp), compiled at runtime with d3dcompiler_47.
namespace phantom_fx::path_tracing::shaders
{
// The traced reflections added to the game's specular light, as the resolve reads it.
inline constexpr char AddReflections[] = R"(
cbuffer Parameters : register(b0)
{
    uint2 Size;
    uint2 Padding;
};

Texture2D<float4> Specular : register(t0);   // The game's specular light
Texture2D<float4> Reflection : register(t1); // The traced reflections, in the same units (ray_tracing::reflection)
RWTexture2D<float4> OutSpecular : register(u0);

[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
    if (any(pixel >= Size)) return;

    float4 specular = Specular[pixel];
    OutSpecular[pixel] = float4(specular.rgb + Reflection[pixel].rgb, specular.a);
}
)";

// A triangle over the whole target.
inline constexpr char FullScreen[] = R"(
float4 main(uint id : SV_VertexID) : SV_Position
{
    return float4(id == 1 ? 3.0 : -1.0, id == 2 ? -3.0 : 1.0, 0.0, 1.0);
}
)";

// The lamps' traced light (ray_tracing::lampLight) into both light targets, added as the game adds its
// lights' light.
inline constexpr char AddLampLight[] = R"(
Texture2D<float4> LampDiffuse : register(t0);
Texture2D<float4> LampSpecular : register(t1);

void main(float4 position : SV_Position, out float4 diffuse : SV_Target0, out float4 specular : SV_Target1)
{
    int3 pixel = int3(position.xy, 0);
    diffuse = float4(LampDiffuse.Load(pixel).rgb, 0);
    specular = float4(LampSpecular.Load(pixel).rgb, 0);
}
)";

// A masked spot's mask (the game's t4) into its tile of the atlas the lamps' rays sample (direct_light.hlsl),
// sampled as the game samples it (bilinear, wrapped) at the tile's texels.
inline constexpr char MaskCopy[] = R"(
Texture2D<float4> Mask : register(t0);
SamplerState LinearWrap : register(s0);
RWTexture2D<unorm float4> Atlas : register(u0);

cbuffer Parameters : register(b0)
{
    uint Tile;
    uint3 Padding;
};

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    if (any(id >= GI_LAMP_MASK_SIZE)) return;

    Atlas[uint2(Tile * GI_LAMP_MASK_SIZE + id.x, id.y)] = Mask.SampleLevel(LinearWrap, (id + 0.5) / GI_LAMP_MASK_SIZE, 0);
}
)";

// In place of LocalReflectionAddBuffer (97a9ad7e0faca7b1594a38c331826a94), rebuilt from its disassembly: the
// screen-space reflection blended over the scene by its mask, roughness and strength, then sRGB-encoded as
// the game writes the scene. Only where a surface was drawn in front of the G-buffer's (the scene's depth
// nearer than the G-buffer's): elsewhere the traced reflections took its place in the resolve.
inline constexpr char ReflectionBlend[] = R"(
cbuffer cPSSystem : register(b0)
{
    float4 m_param;
    float4 m_renderInfo;
    float4 m_renderBuffer;
    float4 m_dominantLightDir;
};

// The blend's strength and the roughness's level-of-detail bias are its c12.xy.
cbuffer cPSObject : register(b5)
{
    float4x4 m_viewWorld;
    float4x4 m_world;
    float4 m_useWeightCount;
    float4 m_localParam[4];
};

Texture2D<float4> inColorImage : register(t0);
Texture2D<float4> inRefMapImage : register(t1);
Texture2D<float4> inMaterialImage : register(t2);
Texture2D<float> SceneDepth : register(t3);   // The depth the reflections' mask read, with the surfaces drawn after the G-buffer
Texture2D<float> SurfaceDepth : register(t4); // The G-buffer's depth, as the ray-traced scene's kick packed it
SamplerState g_samplerPoint_Wrap : register(s8);
SamplerState g_samplerLinear_Wrap : register(s10);

float4 main(float4 position : SV_Position) : SV_Target
{
    const float2 uv = (position.xy - 0.5 + 0.496094) * m_renderBuffer.zw;
    const float2 material = inMaterialImage.Sample(g_samplerPoint_Wrap, uv).zw; // Roughness, mask
    const float4 reflected = inRefMapImage.SampleLevel(g_samplerLinear_Wrap, uv, saturate(material.x + m_localParam[3].y) * 5);
    const float3 color = inColorImage.Sample(g_samplerLinear_Wrap, uv).rgb;

    // Reversed Z: a surface in front of the G-buffer's has the greater depth.
    const int3 pixel = int3(position.xy, 0);
    const bool isInFront = SceneDepth.Load(pixel) > SurfaceDepth.Load(pixel) * 1.000001 + 1e-9;
    const float strength = isInFront ? m_localParam[3].x : 0;
    const float3 reflectedColor = material.y * reflected.rgb * strength;
    const float weight = material.y * reflected.a * strength;
    const float3 blended = color * (1 - weight) + weight * reflectedColor;
    const float3 encoded = blended <= 0.003131 ? blended * 12.92 : 1.055 * pow(max(blended, 0.00001), 1 / 2.4) - 0.055;
    return float4(encoded, 1);
}
)";
}
