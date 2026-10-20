#pragma once

// HLSL for GTVBAO and SSILVB (indirect_light.hpp), compiled at runtime with d3dcompiler_47. The
// occlusion is ported from three-gtvbao (norio, MIT; GT-VBAO by Mirko Salm, CC0 / MIT; SSRT3 by
// Olivier Therrien, MIT), the light gathering from SSRT3 (CDRIN, MIT), the noise and the denoiser
// from XeGTAO (Intel, MIT). Their licenses are in licenses/.
namespace phantom_fx::indirect_light::shaders
{
// The constant buffer of every compute shader here; each reads the fields it needs.
#define PHANTOM_FX_PARAMETERS R"(
cbuffer Parameters : register(b0)
{
    uint2 Size;              // Render size, the depth buffer's
    float Radius;            // Metres
    float MaxRadiusPixels;   // Caps the march for surfaces close to the camera
    float OcclusionStrength; // GTVBAO: exponent on the visibility
    float LightStrength;     // SSILVB: multiplies the light
    float DiffuseScale;      // SSILVB: 0 leaves the game's diffuse light out, for the diagnostics
    float Padding;
};
)"

// The visibility bitmask march, once per pixel at render resolution: for each slice, 32 sectors of
// the hemisphere around the view vector, set where a depth sample, taken as `thickness` deep,
// covers them. The horizons go through the cosine-weighted measure's CDF first (GT-VBAO), so the
// share of set sectors is the cosine-weighted occlusion. Also writes XeGTAO's depth edges for the
// denoiser.
//
// With LIGHT 0 (GTVBAO) it writes the visibility. With LIGHT 1 (SSILVB) it writes the light the
// sectors receive: each sample lights the sectors it covers first, as nearer samples hide what lies
// behind them, with the diffuse light its surface reflects. Equal sectors carry equal cosine weight,
// so the sum is the irradiance over pi, in the units of the game's diffuse light, which the resolve
// multiplies by the albedo.
//
// Fox Engine's view space looks down +z and mirrors x (P00 < 0). Flipping z, and x when mirrored,
// turns it into the usual view space looking down -z with x right and y up, which the maths below
// assumes; positions and the G-buffer's view-space normals take the same flip.
inline constexpr char March[] = PHANTOM_FX_PARAMETERS R"(
// The game's cPSScene as the lighting passes bind it (docs/DEVELOPMENT.md, "Constant buffers"):
// c7 = (0, 0, 1, 0) for the main view's perspective, c21 = (1 / P00, 1 / P11, B, A), c28 the
// jitter in NDC, which the reconstruction takes off as the engine's own passes do.
cbuffer Scene : register(b1)
{
    float4 Scene[30];
};

Texture2D<float> Depth : register(t0);    // Reversed-Z, 0 on the sky
Texture2D<float4> Normals : register(t1); // G-buffer view-space normals
#if LIGHT
Texture2D<float4> Diffuse : register(t2); // The game's diffuse light, as the resolve reads it
Texture2D<float4> Albedo : register(t3);  // Through the resolve's sRGB view, so linear
RWTexture2D<float3> OutLight : register(u0);
#else
RWTexture2D<unorm float> OutVisibility : register(u0);
#endif
// The G-buffer's specular target, whose z is the material; unbound until a sun pass has bound it.
Texture2D<float4> Specular : register(t4);
RWTexture2D<unorm float> OutEdges : register(u1);

static const float Pi = 3.14159265;
static const uint Sectors = 32;
static const uint SliceCount = 2;
static const uint StepCount = 8;
// Fox Engine's material for grass, bushes and tree leaves, in 255ths.
static const float FoliageMaterial = 43;

float distanceAt(int2 pixel)
{
    float depth = Depth[clamp(pixel, 0, int2(Size) - 1)];
    return Scene[21].z / (depth - Scene[21].w);
}

float3 viewPosition(float2 pixelCentre, float distance)
{
    float2 ndc = pixelCentre / Size * float2(2, -2) + float2(-1, 1) - Scene[28].xy;
    return float3(ndc * float2(abs(Scene[21].x), Scene[21].y), -1) * distance;
}

// DecodeViewSpaceNormal, as the game's own ambient obscurance reads the G-buffer.
float3 viewNormal(uint2 pixel)
{
    float3 encoded = Normals[pixel].xyz;
    float z = encoded.z * encoded.z * 2 - 1;
    float2 xy = encoded.xy * 2 - 1;
    xy *= sqrt(saturate(1 - z * z)) * rsqrt(max(dot(xy, xy), 1e-8));
    return normalize(float3(sign(Scene[21].x) * xy.x, xy.y, -z));
}

// XeGTAO: Hilbert curve order over 64 x 64 pixels driving the R2 sequence, so the 3 x 3 neighbourhood
// the denoiser averages holds well spread values.
float2 spatialNoise(uint2 pixel)
{
    uint x = pixel.x % 64, y = pixel.y % 64, index = 0;
    for (uint level = 32; level > 0; level /= 2)
    {
        uint regionX = (x & level) > 0;
        uint regionY = (y & level) > 0;
        index += level * level * ((3 * regionX) ^ regionY);
        if (regionY == 0)
        {
            if (regionX == 1)
            {
                x = 63 - x;
                y = 63 - y;
            }

            uint swap = x;
            x = y;
            y = swap;
        }
    }

    return frac(0.5 + index * float2(0.75487766624669276005, 0.5698402909980532659114));
}

float interleavedGradientNoise(float2 position)
{
    return frac(52.9829189 * frac(dot(position, float2(0.06711056, 0.00583715))));
}

float4 calculateEdges(float centre, float left, float right, float top, float bottom)
{
    float4 edges = float4(left, right, top, bottom) - centre;
    float slopeLR = (edges.y - edges.x) * 0.5;
    float slopeTB = (edges.w - edges.z) * 0.5;
    float4 slopeAdjusted = edges + float4(slopeLR, -slopeLR, slopeTB, -slopeTB);
    edges = min(abs(edges), abs(slopeAdjusted));
    return saturate(1.25 - edges / (centre * 0.011));
}

// Two bits per edge: 0, 1/3, 2/3 or 1.
float packEdges(float4 edges)
{
    edges = round(saturate(edges) * 2.9);
    return dot(edges, float4(64.0, 16.0, 4.0, 1.0) / 255.0);
}

// acos(x) / pi, the polynomial three-gtvbao and XeGTAO use.
float2 acosOverPi(float2 x)
{
    float2 r = (abs(x) * (-0.156583 / Pi) + 0.5) * sqrt(1 - abs(x));
    return x >= 0 ? r : 1 - r;
}

// GT-VBAO's slice-relative CDF of the cosine-weighted measure, for the horizon cosines h of one march
// direction (d = 1 along the slice direction, -1 against it): equal sectors then carry equal weight.
float2 remapHorizons(float2 h, float d, float normalAngle, float cosN, float sinN, float invNormalization)
{
    float3 n = d > 0 ? float3(1, 1, 0) : float3(3, -1, 4);
    h = max(h, sinN * d);
    float2 sinTheta = sqrt(1 - h * h);
    float2 theta = acosOverPi(h) * Pi;
    float2 cosTwoPsiPlusN = (2 * h * h - 1) * cosN - 2 * d * h * sinTheta * sinN;
    float2 t = cosN * n.x + cosTwoPsiPlusN * n.y + (theta * 2 * n.y * d + normalAngle * (n.z + 2 * n.y) - Pi) * sinN;
    return saturate(t * invNormalization);
}

[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
    if (any(pixel >= Size)) return;

    int2 p = pixel;
    float distance = distanceAt(p);
    OutEdges[pixel] = packEdges(calculateEdges(distance, distanceAt(p + int2(-1, 0)), distanceAt(p + int2(1, 0)), distanceAt(p + int2(0, -1)),
        distanceAt(p + int2(0, 1))));

    // The sky, and a pass whose constants are not the main view's, stay unoccluded and unlit.
    if (Depth[pixel] <= 0 || any(Scene[7] != float4(0, 0, 1, 0)))
    {
#if LIGHT
        OutLight[pixel] = 0;
#else
        OutVisibility[pixel] = 1;
#endif
        return;
    }

    float2 centre = pixel + 0.5;
    float3 position = viewPosition(centre, distance);
    float3 normal = viewNormal(pixel);
    float3 viewDir = -position * rsqrt(dot(position, position));

    // The radius projected at this distance; each step moves at least a pixel.
    float radiusPixels = min(Radius * 0.5 * Size.y / (Scene[21].y * distance), MaxRadiusPixels);
    float marchPixels = max(radiusPixels, StepCount) / (StepCount + 1) * max(1, StepCount - 1);
    // Occluders are taken as this deep behind the depth buffer, more for distant ones (three-gtvbao's
    // linear thickness), never past the radius. Foliage is thin: as deep as the rest, a clump of
    // blades reads as a solid block and blacks out the ground and the blades behind it.
    float thickness = clamp(0.12 * distance, 0.05, Radius);
    float foliageThickness = clamp(0.02 * distance, 0.02, Radius);
    float2 noise = spatialNoise(pixel);

    // GT-VBAO spreads the slices evenly around the view vector, not in the image plane, and projects them.
    float a = 1 / (1 + viewDir.z);
    float b = -viewDir.x * viewDir.y * a;
    float3 frameTangent = float3(1 - viewDir.x * viewDir.x * a, b, -viewDir.x);
    float3 frameBitangent = float3(b, 1 - viewDir.y * viewDir.y * a, -viewDir.y);

    float occlusion = 0, weightSum = 0;
    float3 light = 0;
    [unroll]
    for (uint slice = 0; slice < SliceCount; ++slice)
    {
        float angle = (slice + noise.x) * Pi / SliceCount;
        float3 frameDirection = frameTangent * cos(angle) + frameBitangent * sin(angle);
        float3 sliceDir = float3(normalize(frameDirection.xy * viewDir.z - viewDir.xy * frameDirection.z), 0);

        float3 planeNormal = normalize(cross(sliceDir, viewDir));
        float3 tangent = cross(viewDir, planeNormal);
        float3 projectedNormal = normal - planeNormal * dot(normal, planeNormal);
        float projectedLength = length(projectedNormal);
        bool degenerate = projectedLength < 1e-4;
        float cosN = clamp(dot(projectedNormal / max(projectedLength, 1e-4), viewDir), -1, 1);
        float normalSign = -sign(dot(projectedNormal, tangent));
        float normalAngle = degenerate ? 0 : normalSign * acos(cosN);
        float sinN = degenerate ? 0 : normalSign * sqrt(1 - cosN * cosN);
        cosN = degenerate ? 1 : cosN;
        float invNormalization = 0.25 / max(cosN + normalAngle * sinN, 1e-4);
        // Shifts both edges of every sample's sector range alike, so the occluded width stays unbiased.
        float dither = interleavedGradientNoise(centre + normalAngle);

        uint occluded = 0;
        float3 sliceLight = 0;
        [unroll]
        for (uint side = 0; side < 2; ++side)
        {
            float d = side == 0 ? 1 : -1;
            float2 pixelDirection = sliceDir.xy * float2(1, -1) * d;
            [loop]
            for (uint i = 0; i < StepCount; ++i)
            {
                float t = (i + noise.y) / max(1, StepCount - 1);
                float2 samplePixel = centre + pixelDirection * max(t * t * marchPixels, i + 1);
                if (any(samplePixel <= 0) || any(samplePixel >= float2(Size))) break;

                int2 texel = int2(samplePixel);
                float3 toSample = viewPosition(texel + 0.5, distanceAt(texel)) - position;
                float lengthSq = dot(toSample, toSample);
                float invLength = rsqrt(max(lengthSq, 1e-8));
                float sampleDistance = lengthSq * invLength;
                // Beyond the radius, a sample does not occlude: steps forced to a pixel on distant
                // surfaces, and foreground objects far in front.
                if (sampleDistance > Radius) continue;

                float projected = dot(toSample, viewDir);
                bool foliage = round(Specular[texel].z * 255) == FoliageMaterial;
                float sampleThickness = min(foliage ? foliageThickness : thickness, sampleDistance * 0.8);
                float front = projected * invLength;
                float back = (projected - sampleThickness)
                    * rsqrt(max(lengthSq - 2 * sampleThickness * projected + sampleThickness * sampleThickness, 1e-8));
                float2 horizons = remapHorizons(clamp(float2(front, back), -1, 1), d, normalAngle, cosN, sinN, invNormalization);
                uint start = uint(clamp(floor(min(horizons.x, horizons.y) * Sectors + dither), 0, Sectors));
                uint end = uint(clamp(floor(max(horizons.x, horizons.y) * Sectors + dither), 0, Sectors));
                uint width = end - start;
                // A shift by 32 is a shift by 0 in HLSL, hence the test for an empty range.
                uint covered = width > 0 ? (0xFFFFFFFFu >> (Sectors - width)) << start : 0;
#if LIGHT
                // The sample's surface sends its light this way by the cosine at the sample (SSRT3);
                // its back sends none.
                uint lit = covered & ~occluded;
                float facing = lit != 0 ? saturate(dot(viewNormal(texel), -toSample * invLength)) : 0;
                if (facing > 0) sliceLight += countbits(lit) * facing * Diffuse[texel].rgb * Albedo[texel].rgb;
#endif
                occluded |= covered;
                if (occluded == 0xFFFFFFFFu) break;
            }
        }

        // The slice's cosine-weighted integral weighs it; a degenerate slice drops out, and so does one
        // whose normal faces away from the camera, as normal maps make some do on surfaces seen edge
        // on: its negative weight could cancel the other slice's, and the light divided by the near
        // zero sum would flare into white specks.
        float sliceWeight = max(projectedLength * (cosN + normalAngle * sinN), 0);
        occlusion += countbits(occluded) / float(Sectors) * sliceWeight;
        light += sliceLight / Sectors * sliceWeight;
        weightSum += sliceWeight;
    }

#if LIGHT
    OutLight[pixel] = light / max(weightSum, 1e-4);
#else
    float visibility = 1 - saturate(occlusion / max(weightSum, 1e-4));
    OutVisibility[pixel] = OcclusionStrength > 0 ? pow(visibility, OcclusionStrength) : 1;
#endif
}
)";

// XeGTAO's edge-aware 3 x 3 denoiser, one pixel per thread, on the visibility (LIGHT 0) or the light
// (LIGHT 1). The passes before the last blur less (beta / 5), as XeGTAO's do. SSILVB's last pass
// writes the game's diffuse light with the light added, for the resolve to read in its place.
inline constexpr char Denoise[] = PHANTOM_FX_PARAMETERS R"(
#if LIGHT
#define Value float3
#else
#define Value float
#endif

Texture2D<Value> Source : register(t0);
Texture2D<float> Edges : register(t1);
#if LIGHT && FINAL
Texture2D<float4> Diffuse : register(t2);
RWTexture2D<float4> OutDiffuse : register(u0);
#elif LIGHT
RWTexture2D<float3> Output : register(u0);
#else
RWTexture2D<unorm float> Output : register(u0);
#endif

static const float BlurAmount = FINAL ? 1.2 : 1.2 / 5;
static const float DiagonalWeight = 0.85 * 0.5;

float4 unpackEdges(float packed)
{
    uint value = uint(packed * 255.5);
    return saturate(float4((value >> 6) & 3, (value >> 4) & 3, (value >> 2) & 3, value & 3) / 3.0);
}

[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
    if (any(pixel >= Size)) return;

    int2 p = pixel, last = int2(Size) - 1;
    int2 left = clamp(p + int2(-1, 0), 0, last), right = clamp(p + int2(1, 0), 0, last);
    int2 top = clamp(p + int2(0, -1), 0, last), bottom = clamp(p + int2(0, 1), 0, last);
    float4 edgesL = unpackEdges(Edges[left]), edgesR = unpackEdges(Edges[right]);
    float4 edgesT = unpackEdges(Edges[top]), edgesB = unpackEdges(Edges[bottom]);

    // Both sides of an edge must agree, then a little leaks through where three or four edges meet,
    // which keeps thin features from aliasing.
    float4 edges = unpackEdges(Edges[p]) * float4(edgesL.y, edgesR.x, edgesT.w, edgesB.z);
    float edginess = saturate(4 - 2.5 - dot(edges, 1)) / (4 - 2.5) * 0.5;
    edges = saturate(edges + edginess);

    float weightTL = DiagonalWeight * (edges.x * edgesL.z + edges.z * edgesT.x);
    float weightTR = DiagonalWeight * (edges.z * edgesT.y + edges.y * edgesR.z);
    float weightBL = DiagonalWeight * (edges.w * edgesB.x + edges.x * edgesL.w);
    float weightBR = DiagonalWeight * (edges.y * edgesR.w + edges.w * edgesB.y);

    Value sum = Source[p] * BlurAmount;
    float weight = BlurAmount;
    sum += Source[left] * edges.x + Source[right] * edges.y + Source[top] * edges.z + Source[bottom] * edges.w;
    weight += dot(edges, 1);
    sum += Source[clamp(p + int2(-1, -1), 0, last)] * weightTL + Source[clamp(p + int2(1, -1), 0, last)] * weightTR;
    sum += Source[clamp(p + int2(-1, 1), 0, last)] * weightBL + Source[clamp(p + int2(1, 1), 0, last)] * weightBR;
    weight += weightTL + weightTR + weightBL + weightBR;

#if LIGHT && FINAL
    float4 diffuse = Diffuse[pixel];
    OutDiffuse[pixel] = float4(diffuse.rgb * DiffuseScale + sum / weight * LightStrength, diffuse.a);
#else
    Output[pixel] = sum / weight;
#endif
}
)";

// Drawn with the game's upscale vertex shader right after it, with a multiplying blend into the
// diffuse light: the input signature is that vertex shader's output.
inline constexpr char Apply[] = R"(
Texture2D<float> Visibility : register(t0);

float4 main(float4 texcoord0 : TEXCOORD0, float4 texcoord1 : TEXCOORD1, float4 position : SV_Position) : SV_Target
{
    return Visibility[uint2(position.xy)];
}
)";

// Diagnostics: drawn in place of the deferred resolve, GTVBAO's occlusion (LIGHT 0) or SSILVB's light
// (LIGHT 1) alone. Every resolve vertex shader outputs the position first. The resolve writes through
// an sRGB view: the visibility is linearised first and stored as it is, rather than brightened by
// the encoding; SSILVB's light, which the last denoise pass writes without the game's diffuse light,
// is linear already.
inline constexpr char Show[] = R"(
#if LIGHT
Texture2D<float4> Light : register(t9);
#else
Texture2D<float> Visibility : register(t13);
#endif

float4 main(float4 position : SV_Position) : SV_Target
{
#if LIGHT
    float3 colour = Light[uint2(position.xy)].rgb;
#else
    float3 colour = pow(saturate(Visibility[uint2(position.xy)]), 2.2).xxx;
#endif
    // Alpha as the resolve writes it: a luminance code that exposure, bloom and depth of field read.
    // With 1 they took every pixel for a bright light and blurred the whole view.
    float luminance = dot(colour, float3(0.2125, 0.7154, 0.0721)) / 32;
    return float4(colour, luminance * rsqrt(max(luminance, 1.0 / 512)));
}
)";

#undef PHANTOM_FX_PARAMETERS
}
