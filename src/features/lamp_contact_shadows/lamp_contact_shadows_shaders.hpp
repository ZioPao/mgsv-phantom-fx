#pragma once

// HLSL for the lamps' contact shadows (lamp_contact_shadows.hpp), compiled at runtime with
// d3dcompiler_47.
namespace phantom_fx::lamp_contact_shadows::shaders
{
// A point or spot light's screen-space shadow into the alpha of both light targets, drawn with the
// light's own volume; nothing reads that alpha after the lights. The trace wrote only the pixels
// within its bounds (BendShadow's constants, in b6); the light does not reach the others, which are
// taken as lit.
inline constexpr char LightShadow[] = R"(
cbuffer Parameters : register(b6)
{
    float2 DepthBounds : packoffset(c3.x);
    int2 BoundsMin : packoffset(c3.z);
    int2 BoundsMax : packoffset(c4.x);
};
Texture2D<float> g_DepthTexture : register(t2);
Texture2D<float> g_ScreenSpaceShadow : register(t7);

void main(float4 position : SV_Position, out float4 diffuse : SV_Target0, out float4 specular : SV_Target1)
{
    int2 pixel = int2(position.xy);
    float depth = g_DepthTexture.Load(int3(pixel, 0));
    bool traced = all(pixel >= BoundsMin) && all(pixel < BoundsMax) && depth > DepthBounds.x && depth < DepthBounds.y;
    diffuse = specular = traced ? g_ScreenSpaceShadow.Load(int3(pixel, 0)) : 1;
}
)";
}
