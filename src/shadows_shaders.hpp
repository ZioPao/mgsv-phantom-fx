#pragma once

// HLSL for the shadows (shadows.hpp), compiled at runtime with d3dcompiler_47.
namespace phantom_fx::shadows::shaders
{
// Bend Studio's screen-space shadows: bend_sss_gpu.h from "Inside Bend: Screen Space Shadows",
// Copyright 2023 Sony Interactive Entertainment, Apache License 2.0 (Bend-Studio-SSS-LICENSE.txt).
// Changed for FXC and Direct3D 11: the configuration macros are set here; the forward declaration
// and a (void) cast that FXC rejects are removed; the output is unorm; and the early-out votes
// through groupshared memory, as shader model 5 has no wave intrinsics. Split into literals, as
// MSVC limits each to 16 KB.
inline constexpr char BendShadow[] = R"(
#define WAVE_SIZE 64
#define SAMPLE_COUNT 120
#define HARD_SHADOW_SAMPLES 4
#define FADE_OUT_SAMPLES 8
#define USE_HALF_PIXEL_OFFSET 1
#define USE_UV_PIXEL_BIAS 1

// This is the list of runtime properties to pass to the shader
// Wherever possible, it is highly recommended to have these values be compile-time constants
struct DispatchParameters
{
    // Visual configuration:
    // These values will require manual tuning.
    // All shadow computation is performed in non-linear depth space (not in world space), so tuned value choices will depend on scene depth distribution (as determined by the Projection Matrix setup).

    float SurfaceThickness;             // This is the assumed thickness of each pixel for shadow-casting, measured as a percentage of the difference in non-linear depth between the sample and FarDepthValue.
                                        // Recommended starting value: 0.005 (0.5%)

    float BilinearThreshold;            // Percentage threshold for determining if the difference between two depth values represents an edge, and should not perform interpolation.
                                        // To tune this value, set 'DebugOutputEdgeMask' to true to visualize where edges are being detected.
                                        // Recommended starting value: 0.02 (2%)

    float ShadowContrast;               // A contrast boost is applied to the transition in/out of shadow.
                                        // Recommended starting value: 2 or 4. Values >= 1 are valid.

    float Reach;                        // How many samples along the ray to march, one a pixel of the render
                                        // size. Bend's shader always takes SAMPLE_COUNT; this is how much of it.

    float Coverage;                     // 1: count the share of the ray that lies inside a surface, rather than
                                        // the gap to the nearest one, so a grazed edge reads soft

    float Gain;                         // How far that share reaches full darkness: a shadowed ray spends most
                                        // of its length in empty space, so the mean is small

    bool IgnoreEdgePixels;              // If an edge is detected, the edge pixel will not contribute to the shadow.
                                        // If a very flat surface is being lit and rendered at an grazing angles, the edge detect may incorrectly detect multiple 'edge' pixels along that flat surface.
                                        // In these cases, the grazing angle of the light may subsequently produce aliasing artefacts in the shadow where these incorrect edges were detected.
                                        // Setting this value to true would mean that those pixels would not cast a shadow, however it can also thin out otherwise valid shadows, especially on foliage edges.
                                        // Recommended starting value: false, unless typical scenes have numerous large flat surfaces, in which case true.

    bool UsePrecisionOffset;            // A small offset is applied to account for an imprecise depth buffer (recommend off)

    bool BilinearSamplingOffsetMode;    // There are two modes to compute bilinear samples for shadow depth:
                                        // true = sampling points for pixels are offset to the wavefront shared ray, shadow depths and starting depths are the same. Can project more jagged/aliased shadow lines in some cases.
                                        // false = sampling points for pixels are not offset and start from pixel centers. Shadow depths are biased based on depth gradient across the current pixel bilinear sample. Has more issues in back-face / grazing areas.
                                        // Both modes have subtle visual differences, which may / may not exaggerate depth buffer aliasing that gets projected in to the shadow.
                                        // Evaluating the visual difference between each mode is recommended, then hard-coding the mode used to optimize the shader.
                                        // Recommended starting value: false

    // Debug views
    bool DebugOutputEdgeMask;           // Use this to visualize edges, for tuning the 'BilinearThreshold' value.
    bool DebugOutputThreadIndex;        // Debug output to visualize layout of compute threads
    bool DebugOutputWaveIndex;          // Debug output to visualize layout of compute wavefronts, useful to sanity check the Light Coordinate is being computed correctly.

    // Culling / Early out:
    float2 DepthBounds;                 // Depth Bounds (min, max) for the on-screen volume of the light. Typically (0,1) for directional lights. Only used when 'UseEarlyOut' is true.

    bool UseEarlyOut;                   // Set to true to early-out when depth values are not within [DepthBounds] - otherwise DepthBounds is unused
                                        // [Optionally customize the 'EarlyOutPixel()' function to perform your own early-out logic, e.g. skipping pixels that a shadow map indicates are already fully occluded]
                                        // This can dramatically reduce cost when only a small portion of the pixels need a shadow term (e.g., cull out sky pixels), however it does have some overhead (~15%) in worst-case where nothing early-outs
                                        // Note; Early-out is most efficient when WAVE_SIZE matches the hardware wavefront size - otherwise cross wave communication is required.

    // Set sensible starting tuning values
    void SetDefaults()
    {
        SurfaceThickness            = 0.005;
        BilinearThreshold           = 0.02;
        ShadowContrast              = 4;
        Reach                       = SAMPLE_COUNT;
        Coverage                    = 0;
        Gain                        = 1;
        IgnoreEdgePixels            = false;
        UsePrecisionOffset          = false;
        BilinearSamplingOffsetMode  = false;
        DebugOutputEdgeMask         = false;
        DebugOutputThreadIndex      = false;
        DebugOutputWaveIndex        = false;
        DepthBounds                 = float2(0,1);
        UseEarlyOut                 = false;
    }

    // Runtime data returned from BuildDispatchList():
    float4 LightCoordinate;             // Values stored in DispatchList::LightCoordinate_Shader by BuildDispatchList()
    int2 WaveOffset;                    // Values stored in DispatchData::WaveOffset_Shader by BuildDispatchList()

    // Renderer Specific Values:
    float FarDepthValue;                // Set to the Depth Buffer Value for the far clip plane, as determined by renderer projection matrix setup (typically 0).
    float NearDepthValue;               // Set to the Depth Buffer Value for the near clip plane, as determined by renderer projection matrix setup (typically 1).

    // Sampling data:
    float2 InvDepthTextureSize;         // Inverse of the texture dimensions for 'DepthTexture' (used to convert from pixel coordinates to UVs)
                                        // If 'PointBorderSampler' is an Unnormalized sampler, then this value can be hard-coded to 1.
                                        // The 'USE_HALF_PIXEL_OFFSET' macro might need to be defined if sampling at exact pixel coordinates isn't precise (e.g., if odd patterns appear in the shadow).

    Texture2D<float> DepthTexture;      // Depth Buffer Texture (rasterized non-linear depth)
    RWTexture2D<unorm float> OutputTexture; // Output screen-space shadow buffer (typically single-channel, 8bit)

    SamplerState PointBorderSampler;    // A point sampler, with Wrap Mode set to Clamp-To-Border-Color (D3D12_TEXTURE_ADDRESS_MODE_BORDER), and Border Color set to "FarDepthValue" (typically zero), or some other far-depth value out of DepthBounds.
                                        // If you have issues where invalid shadows are appearing from off-screen, it is likely that this sampler is not correctly setup
};
)" R"(
static bool EarlyOutPixel(struct DispatchParameters inParameters, int2 pixel_xy, float depth)
{
    //OPTIONAL TODO; customize this function to return true if the pixel should early-out for custom reasons. E.g., A shadow map pass already found the pixel was in shadow / backfaced, etc.
    // Recommended to keep this code very simple!

    // Example:
    // return inParameters.CustomShadowMapTerm[pixel_xy] == 0;

    // The compiled code will be more optimal if the 'depth' value is not referenced.
    return depth >= inParameters.DepthBounds.y || depth <= inParameters.DepthBounds.x;
}

// Gets the start pixel coordinates for the pixels in the wavefront
// Also returns the delta to get to the next pixel after WAVE_COUNT pixels along the ray
static void ComputeWavefrontExtents(DispatchParameters inParameters, int3 inGroupID, uint inGroupThreadID, out float2 outDeltaXY, out float2 outPixelXY, out float outPixelDistance, out bool outMajorAxisX)
{
    int2 xy = inGroupID.yz * WAVE_SIZE + inParameters.WaveOffset.xy;

    //integer light position / fractional component
    float2 light_xy = floor(inParameters.LightCoordinate.xy) + 0.5;
    float2 light_xy_fraction = inParameters.LightCoordinate.xy - light_xy;
    bool reverse_direction = inParameters.LightCoordinate.w > 0.0f;

    int2 sign_xy = sign(xy);
    bool horizontal = abs(xy.x + sign_xy.y) < abs(xy.y - sign_xy.x);

    int2 axis;
    axis.x = horizontal ? (+sign_xy.y) : (0);
    axis.y = horizontal ? (0) : (-sign_xy.x);

    // Apply wave offset
    xy = axis * (int)inGroupID.x + xy;
    float2 xy_f = (float2)xy;

    // For interpolation to the light center, we only really care about the larger of the two axis
    bool x_axis_major = abs(xy_f.x) > abs(xy_f.y);
    float major_axis = x_axis_major ? xy_f.x : xy_f.y;

    float major_axis_start = abs(major_axis);
    float major_axis_end = abs(major_axis) - (float)WAVE_SIZE;

    float ma_light_frac = x_axis_major ? light_xy_fraction.x : light_xy_fraction.y;
    ma_light_frac = major_axis > 0 ? -ma_light_frac : ma_light_frac;

    // back in to screen direction
    float2 start_xy = xy_f + light_xy;

    // For the very inner most ring, we need to interpolate to a pixel centered UV, so the UV->pixel rounding doesn't skip output pixels
    float2 end_xy = lerp(inParameters.LightCoordinate.xy, start_xy, (major_axis_end + ma_light_frac) / (major_axis_start + ma_light_frac));

    // The major axis should be a round number
    float2 xy_delta = (start_xy - end_xy);

    // Inverse the read order when reverse direction is true
    float thread_step = (float)(inGroupThreadID ^ (reverse_direction ? 0 : (WAVE_SIZE - 1)));

    float2 pixel_xy = lerp(start_xy, end_xy, thread_step / (float)WAVE_SIZE);
    float pixel_distance = major_axis_start - thread_step + ma_light_frac;

    outPixelXY = pixel_xy;
    outPixelDistance = pixel_distance;
    outDeltaXY = xy_delta;
    outMajorAxisX = x_axis_major;
}
)" R"(
// Number of bilinear sample reads performed per-thread
#define READ_COUNT (SAMPLE_COUNT / WAVE_SIZE + 2)

// How many thickness windows in front of the ray a surface still counts as blocking it. Unbounded,
// a ray that grazes a slope counts its whole length; RenoDX bounds it too (its caster separation).
static const float CoverageBand = 4;

// Common shared data
groupshared float DepthData[READ_COUNT * WAVE_SIZE];
groupshared bool LdsEarlyOut;

// Generate the shadow
//  Call this function from a compute shader with thread dimensions: numthreads[WAVE_SIZE, 1, 1]
//
//  (int3)  inGroupID:          Compute shader group id register (SV_GroupID)
//  (int)   inGroupThreadId:    Compute shader group thread id register (SV_GroupThreadID)
void WriteScreenSpaceShadow(DispatchParameters inParameters, int3 inGroupID, int inGroupThreadID)
{
    float2 xy_delta;
    float2 pixel_xy;
    float pixel_distance;
    bool x_axis_major;  // major axis is x axis? abs(xy_delta.x) > abs(xy_delta.y).

    ComputeWavefrontExtents(inParameters, (int3)inGroupID, inGroupThreadID.x, xy_delta, pixel_xy, pixel_distance, x_axis_major);

    // Read in the depth values
    float sampling_depth[READ_COUNT];
    float shadowing_depth[READ_COUNT];
    float depth_thickness_scale[READ_COUNT];
    float sample_distance[READ_COUNT];

    const float direction = -inParameters.LightCoordinate.w;
    const float z_sign = inParameters.NearDepthValue > inParameters.FarDepthValue ? -1 : +1;

    int i;
    bool is_edge = false;
    bool skip_pixel = false;
    float2 write_xy = floor(pixel_xy);

    [unroll] for (i = 0; i < READ_COUNT; i++)
    {
        // We sample depth twice per pixel per sample, and interpolate with an edge detect filter
        // Interpolation should only occur on the minor axis of the ray - major axis coordinates should be at pixel centers
        float2 read_xy = floor(pixel_xy);
        float minor_axis = x_axis_major ? pixel_xy.y : pixel_xy.x;

        // If a pixel has been detected as an edge, then optionally (inParameters.IgnoreEdgePixels) don't include it in the shadow
        const float edge_skip = 1e20;   // if edge skipping is enabled, apply an extreme value/blend on edge samples to push the value out of range

        float2 depths;
        float bilinear = frac(minor_axis) - 0.5;

    #if USE_HALF_PIXEL_OFFSET
        read_xy += 0.5;
    #endif

    #if USE_UV_PIXEL_BIAS
        float bias = bilinear > 0 ? 1 : -1;
        float2 offset_xy = float2(x_axis_major ? 0 : bias, x_axis_major ? bias : 0);

        // HLSL enforces that a pixel offset is a compile-time constant, which isn't strictly required (and can sometimes be a bit faster)
        // So this fallback will use a manual uv offset instead
        depths.x = inParameters.DepthTexture.SampleLevel(inParameters.PointBorderSampler, read_xy * inParameters.InvDepthTextureSize, 0);
        depths.y = inParameters.DepthTexture.SampleLevel(inParameters.PointBorderSampler, (read_xy + offset_xy) * inParameters.InvDepthTextureSize, 0);
    #else
        int bias = bilinear > 0 ? 1 : -1;
        int2 offset_xy = int2(x_axis_major ? 0 : bias, x_axis_major ? bias : 0);

        depths.x = inParameters.DepthTexture.SampleLevel(inParameters.PointBorderSampler, read_xy * inParameters.InvDepthTextureSize, 0);
        depths.y = inParameters.DepthTexture.SampleLevel(inParameters.PointBorderSampler, read_xy * inParameters.InvDepthTextureSize, 0, offset_xy);
    #endif

        // Depth thresholds (bilinear/shadow thickness) are based on a fractional ratio of the difference between sampled depth and the far clip depth
        depth_thickness_scale[i] = abs(inParameters.FarDepthValue - depths.x);

        // If depth variance is more than a specific threshold, then just use point filtering
        bool use_point_filter = abs(depths.x - depths.y) > depth_thickness_scale[i] * inParameters.BilinearThreshold;

        // Store for debug output when inParameters.DebugOutputEdgeMask is true
        if (i == 0) is_edge = use_point_filter;

        if (inParameters.BilinearSamplingOffsetMode)
        {
            bilinear = use_point_filter ? 0 : bilinear;
            //both shadow depth and starting depth are the same in this mode, unless shadow skipping edges
            sampling_depth[i] = lerp(depths.x, depths.y, abs(bilinear));
            shadowing_depth[i] = (inParameters.IgnoreEdgePixels && use_point_filter) ? edge_skip : sampling_depth[i];
        }
        else
        {
            // The pixel starts sampling at this depth
            sampling_depth[i] = depths.x;

            float edge_depth = inParameters.IgnoreEdgePixels ? edge_skip : depths.x;
            // Any sample in this wavefront is possibly interpolated towards the bilinear sample
            // So use should use a shadowing depth that is further away, based on the difference between the two samples
            float shadow_depth = depths.x + abs(depths.x - depths.y) * z_sign;

            // Shadows cast from this depth
            shadowing_depth[i] = use_point_filter ? edge_depth : shadow_depth;
        }

        // Store for later
        sample_distance[i] = pixel_distance + (WAVE_SIZE * i) * direction;

        // Iterate to the next pixel along the ray. This will be WAVE_SIZE pixels along the ray...
        pixel_xy += xy_delta * direction;
    }

    // Using early out, and no debug mode is enabled?
    if (inParameters.UseEarlyOut && (inParameters.DebugOutputWaveIndex == false && inParameters.DebugOutputThreadIndex == false && inParameters.DebugOutputEdgeMask == false))
    {
        // read the depth of the pixel we are shadowing, and early-out
        // The compiler will typically rearrange this code to put it directly after the first depth read
        skip_pixel = EarlyOutPixel(inParameters, (int2)write_xy, sampling_depth[0]);

        // Changed: the whole group votes through groupshared memory, as without wave intrinsics.
        LdsEarlyOut = true;
        GroupMemoryBarrierWithGroupSync();
        [branch] if (!skip_pixel)
            LdsEarlyOut = false;
        GroupMemoryBarrierWithGroupSync();
        [branch] if (LdsEarlyOut)
            return;
    }

    // Write the shadow depths to LDS
    [unroll] for (i = 0; i < READ_COUNT; i++)
    {
        // Perspective correct the shadowing depth, in this space, all light rays are parallel
        float stored_depth = (shadowing_depth[i] - inParameters.LightCoordinate.z) / sample_distance[i];

        if (i != 0)
        {
            // For pixels within sampling distance of the light, it is possible that sampling will
            // overshoot the light coordinate for extended reads. We want to ignore these samples
            stored_depth = sample_distance[i] > 0 ? stored_depth : 1e10;
        }

        // Store the depth values in groupshared
        int idx = (i * WAVE_SIZE) + inGroupThreadID.x;
        DepthData[idx] = stored_depth;
    }

    // Sync wavefronts now groupshared DepthData is written
    GroupMemoryBarrierWithGroupSync();

    // If the starting depth isn't in depth bounds, then we don't need a shadow
    if (skip_pixel)
        return;

    float start_depth = sampling_depth[0];

    // lerp away from far depth by a tiny fraction?
    if (inParameters.UsePrecisionOffset)
        start_depth = lerp(start_depth, inParameters.FarDepthValue, -1.0 / 0xFFFF);

    // perspective correct the depth
    start_depth = (start_depth - inParameters.LightCoordinate.z) / sample_distance[0];

    // Start by reading the next value
    int sample_index = inGroupThreadID.x + 1;

    float4 shadow_value = 1;
    float hard_shadow = 1;

    // This is the inverse of how large the shadowing window is for the projected sample data.
    // All values in the LDS sample list are scaled by 1.0 / sample_distance, such that all light directions become parallel.
    // The multiply by sample_distance[0] here is to compensate for the projection divide in the data.
    // The 1.0 / inParameters.SurfaceThickness is to adjust user selected thickness. So a 0.5% thickness will scale depth values from [0,1] to [0,200]. The shadow window is always 1 wide.
    // 1.0 / depth_thickness_scale[0] is because SurfaceThickness is percentage of remaining depth between the sample and the far clip - not a percentage of the full depth range.
    // The min() function is to make sure the window is a minimum width when very close to the light. The +direction term will bias the result so the pixel at the very center of the light is either fully lit or shadowed
    float depth_scale = min(sample_distance[0] + direction, 1.0 / inParameters.SurfaceThickness) * sample_distance[0] / depth_thickness_scale[0];

    start_depth = start_depth * depth_scale - z_sign;

    // The first number of hard shadow samples, a single pixel can produce a full shadow
    [unroll] for (i = 0; i < HARD_SHADOW_SAMPLES; i++)
    {
        float depth_delta = abs(start_depth - DepthData[sample_index + i] * depth_scale);

        // We want to find the distance of the sample that is closest to the reference depth
        hard_shadow = min(hard_shadow, depth_delta);
    }

    // Brute force go!
    // The main shadow samples, averaged in to a set of 4 shadow values, out to the reach. Bend's shader
    // always runs SAMPLE_COUNT of them; the add-on's reach stops the march early, so a shorter reach is
    // cheaper as well as tighter, and the last FADE_OUT_SAMPLES of the reach fade out in case a light
    // stands within it. Four samples a pass, one per accumulator, so the loop never indexes the vector
    // by a runtime value: that would force FXC to unroll it to the full sample count whatever the reach.
    const int reach = clamp((int)inParameters.Reach, HARD_SHADOW_SAMPLES, SAMPLE_COUNT);
    const int fadeFrom = max(reach - FADE_OUT_SAMPLES, HARD_SHADOW_SAMPLES);
    // The share of the ray that lies inside a surface, for the coverage mode below.
    float coverSum = 0;
    int covered = 0;
    i = HARD_SHADOW_SAMPLES;
    [loop] for (; i + 3 < reach; i += 4)
    {
        float4 sample_depth;
        sample_depth.x = DepthData[sample_index + i + 0] * depth_scale;
        sample_depth.y = DepthData[sample_index + i + 1] * depth_scale;
        sample_depth.z = DepthData[sample_index + i + 2] * depth_scale;
        sample_depth.w = DepthData[sample_index + i + 3] * depth_scale;
        const float4 depth_delta = abs(start_depth - sample_depth);

        // Add the fade value to the samples of the reach's last stretch.
        float4 fade_out;
        fade_out.x = (i + 0) < fadeFrom ? 0 : (float)(i + 1 - fadeFrom) / (float)(FADE_OUT_SAMPLES + 1) * 0.75;
        fade_out.y = (i + 1) < fadeFrom ? 0 : (float)(i + 2 - fadeFrom) / (float)(FADE_OUT_SAMPLES + 1) * 0.75;
        fade_out.z = (i + 2) < fadeFrom ? 0 : (float)(i + 3 - fadeFrom) / (float)(FADE_OUT_SAMPLES + 1) * 0.75;
        fade_out.w = (i + 3) < fadeFrom ? 0 : (float)(i + 4 - fadeFrom) / (float)(FADE_OUT_SAMPLES + 1) * 0.75;

        // By using 4 values, the average shadow can be taken, which can help soften single-pixel shadows.
        shadow_value = min(shadow_value, depth_delta + fade_out);

        if (inParameters.Coverage > 0)
        {
            // Bend's own measure is the gap to a surface at the receiver's depth, which only a caster
            // touching it can give. RenoDX counts any surface in front of the ray point, so that is what
            // this sums: saturate(sample - start + 1) is 0 with nothing in front and 1 from one window
            // in front through everything beyond it, so the mean is the share of the ray that is blocked.
            const float4 front = sample_depth - start_depth + 1;
            coverSum += dot(saturate(front) - saturate(front - CoverageBand), 1);
            covered += 4;
        }
    }

    // The fewer than four samples left of the reach. Four samples a pass start at 4, so each lands in the
    // accumulator its index would have had in Bend's unrolled loop.
    for (; i < reach; ++i)
    {
        const float depth_delta = abs(start_depth - DepthData[sample_index + i] * depth_scale);
        const float fade_out = i < fadeFrom ? 0 : (float)(i + 1 - fadeFrom) / (float)(FADE_OUT_SAMPLES + 1) * 0.75;

        if ((i & 3) == 0) shadow_value.x = min(shadow_value.x, depth_delta + fade_out);
        else if ((i & 3) == 1) shadow_value.y = min(shadow_value.y, depth_delta + fade_out);
        else if ((i & 3) == 2) shadow_value.z = min(shadow_value.z, depth_delta + fade_out);
        else shadow_value.w = min(shadow_value.w, depth_delta + fade_out);

        if (inParameters.Coverage > 0)
        {
            const float front = DepthData[sample_index + i] * depth_scale - start_depth + 1;
            coverSum += saturate(front) - saturate(front - CoverageBand);
            ++covered;
        }
    }

    // Apply the contrast value.
    // A value of 0 indicates a sample was exactly matched to the reference depth (and the result is fully shadowed)
    // We want some boost to this range, so samples don't have to exactly match to produce a full shadow.
    shadow_value = saturate(shadow_value * (inParameters.ShadowContrast) + (1 - inParameters.ShadowContrast));
    hard_shadow = saturate(hard_shadow * (inParameters.ShadowContrast) + (1 - inParameters.ShadowContrast));

    float result = 0;

    // Take the average of 4 samples, this is useful to reduces aliasing noise in the source depth, especially with long shadows.
    result = dot(shadow_value, 0.25);

    // If the first samples are always producing a hard shadow, then compute this value separately.
    result = min(hard_shadow, result);

    // Coverage: the share of the ray that lies inside a surface, in place of the gap to the nearest one.
    // renodx-mgsv's march counts the samples that hit something and divides; this is the same measure in
    // Bend's window units, so a pixel the ray only grazes comes out partly shadowed instead of either
    // lit or dark, and the edge reads soft. Its own switch, because it changes the shape of every
    // contact shadow rather than their strength.
    if (inParameters.Coverage > 0 && covered > 0)
    {
        // `mean` is the share of the ray that lies inside a surface: 0 with nothing along it, high behind a
        // caster. The gain belongs on that share, not on its complement: a shadowed ray is mostly empty
        // space, so the share is small and has to be scaled up before it is subtracted from 1.
        const float mean = coverSum / (float)covered;
        const float lit = saturate(1 - mean * inParameters.Gain);
        result = lerp(result, saturate(lit * inParameters.ShadowContrast + (1 - inParameters.ShadowContrast)),
            saturate(inParameters.Coverage));
    }

    //write the result
    {
        if (inParameters.DebugOutputEdgeMask)
            result = is_edge ? 1 : 0;
        if (inParameters.DebugOutputThreadIndex)
            result = (inGroupThreadID / (float)WAVE_SIZE);
        if (inParameters.DebugOutputWaveIndex)
            result = frac(inGroupID.x / (float)WAVE_SIZE);

        // Asking the GPU to write scattered single-byte pixels isn't great,
        // But thankfully the latency is hidden by all the work we're doing...
        inParameters.OutputTexture[(int2)write_xy] = result;
    }
}

cbuffer Parameters : register(b0)
{
    float4 LightCoordinate; // DispatchList::LightCoordinate_Shader
    int2 WaveOffset;        // DispatchData::WaveOffset_Shader
    float2 InvDepthTextureSize;
    float SurfaceThickness;
    float BilinearThreshold;
    float ShadowContrast;
    float Reach;     // In samples
    float2 DepthBounds;     // Pixels at or beyond these depths are skipped
    int2 BoundsMin;         // The pixels BuildDispatchList covers, for the light shadow shader
    int2 BoundsMax;
    float Coverage;         // 1: coverage rather than the gap to the nearest surface
    float Gain;             // How far the coverage reaches full darkness
};
Texture2D<float> Depth : register(t0);
RWTexture2D<unorm float> Shadow : register(u0);
SamplerState PointBorder : register(s0); // Border colour 0, the far plane

[numthreads(WAVE_SIZE, 1, 1)]
void main(int3 groupId : SV_GroupID, uint3 groupThreadId : SV_GroupThreadID)
{
    DispatchParameters parameters;
    parameters.SetDefaults();
    parameters.SurfaceThickness = SurfaceThickness;
    parameters.BilinearThreshold = BilinearThreshold;
    parameters.ShadowContrast = ShadowContrast;
    parameters.Reach = Reach;
    parameters.Coverage = Coverage;
    parameters.Gain = Gain;
    // Skips the sky, which is at the far plane, and for a local light what it cannot reach.
    parameters.UseEarlyOut = true;
    parameters.DepthBounds = DepthBounds;
    parameters.LightCoordinate = LightCoordinate;
    parameters.WaveOffset = WaveOffset;
    // Fox Engine's depth is reversed: 1 at the near plane, 0 at the far plane.
    parameters.FarDepthValue = 0;
    parameters.NearDepthValue = 1;
    parameters.InvDepthTextureSize = InvDepthTextureSize;
    parameters.DepthTexture = Depth;
    parameters.OutputTexture = Shadow;
    parameters.PointBorderSampler = PointBorder;
    WriteScreenSpaceShadow(parameters, groupId, groupThreadId.x);
}
)";

// Fox Engine's sun light pass, as the game's SunLight_DL_CS_ES (SHADOW_MAP 1, inside the shadow
// range) and SunLight_DL_ES (SHADOW_MAP 0, beyond it) compute it, rebuilt from their disassembly
// with the same bindings; in rain the game draws SunLight_DL_CS and SunLight_DL, the same without
// the sky reflection (SKY_REFLECTION 0). Plus the add-on's options in b6: soft shadows filter the
// shadow map, the screen-space shadow in t7 multiplies the sun's shadow, and the ray-traced shadow in
// t6 takes the place of the game's. With all off they give the game's output bit for bit (checked on
// WARP with random inputs). Split into literals, as MSVC limits each to 16 KB.
inline constexpr char SunLight[] = R"(
cbuffer cPSScene : register(b2)
{
    float4x4 m_projectionView;
    float4x4 m_projection;
    float4x4 m_view;
    float4 m_shadowProjection[4];   // Rows: view position to the first cascade
    float4 m_cascades[4];           // m_shadowProjection2: columns x, y, z place cascades 1 to 3 (offset, scale in row 3)
    float4 m_eyepos;
    float4 m_projectionParam;       // 1 / P00, 1 / P11, B, A
    float4 m_viewportSize;
    float4 m_exposure;
    float4 m_fogParam[3];
    float4 m_fogColor;
    float4 m_cameraCenterOffset;
    float4 m_shadowMapResolutions;
};

cbuffer cPSLight : register(b3)
{
    float4 m_lightParams[11];
};

cbuffer cPSObject : register(b5)
{
    float4 m_objectParams[13];
};

cbuffer cSunShadows : register(b6) // The add-on's options
{
    float g_softShadows;        // 1: contact-hardening soft shadows from the shadow map
    float g_sunTangent;         // Tangent of the sun's angular radius, for the soft shadows
    float g_frame;              // Turns the soft shadows' samples: the frame modulo 64 while DLSS accumulates frames, else 0
    float g_screenSpaceShadows; // 1: the screen-space shadow in t7 multiplies the sun's shadow
    float g_rayTraced;          // 1: the ray-traced shadow in t6 is the sun's shadow (see SunShadow)
    float3 g_padding;
};

SamplerState g_samplerPoint_Wrap : register(s8);
SamplerState g_samplerPoint_Clamp : register(s9);
SamplerState g_samplerLinear_Wrap : register(s10);
SamplerState g_samplerLinear_Clamp : register(s11);
SamplerComparisonState g_samplerComparisonLess_Linear_Clamp : register(s15);

Texture2D<float4> g_NormalTexture : register(t0);
Texture2D<float4> g_SpecularTexture : register(t1);
Texture2D<float4> g_DepthTexture : register(t2);
Texture2D<float> g_ShadowTexture : register(t3);
Texture2D<float4> g_CloudTexture : register(t4);
Texture2D<float> g_RayTracedShadow : register(t6);
Texture2D<float> g_ScreenSpaceShadow : register(t7);
Texture3D<float4> g_lut2 : register(t8);
Texture2D<float4> g_lut1 : register(t9);
Texture2D<float4> g_MaterialTexture : register(t10);
Texture2D<float4> g_RandomTexture : register(t12);
#if SKY_REFLECTION
Texture2D<float4> g_SkyReflectTexture : register(t15);
#endif

#if SHADOW_MAP
static const int BlockerSamples = 16;     // Gathers of four texels each
static const int FilterSamples = 24;      // Bilinear comparisons
static const float OccluderReach = 20;    // Metres: occluders this far towards the sun widen the penumbra
static const float MaxRadiusTexels = 128; // Keeps the search and the filter cache-friendly
static const float MaxSlope = 4;          // The receiver's steepest rise towards the sun, per unit across
static const float BiasTexels = 1;        // Depth bias, in texels of height, besides the slope across a bilinear footprint
static const float BiasPerOffset = 0.1;   // Depth bias per unit of offset, for curved receivers

// Point `index` of a Vogel disc of `count` points with radius 1, turned by `turn` radians.
float2 VogelDisc(int index, int count, float turn)
{
    float s, c;
    sincos(index * 2.39996323 + turn, s, c); // The golden angle
    return sqrt((index + 0.5) / count) * float2(c, s);
}

// Jorge Jimenez's interleaved gradient noise, in [0, 1).
float InterleavedGradientNoise(float2 pixel)
{
    return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

// Contact-hardening soft shadows (Randima Fernando, "Percentage-Closer Soft Shadows", 2005) in the
// cascade the game chose. A cascade's x, y and z share one scale, z towards the sun, so all of it is
// in cascade units: an atlas uv is a quarter of a unit, and depth is shadowDepth x (z + 1), larger
// towards the sun. `slope` is the receiver's rise towards the sun per unit across; `tileMin` and
// `tileMax` keep the samples in the cascade's quarter of the atlas. Returns the share of the sun
// that reaches the receiver.
float SoftShadow(float2 uv, float receiverDepth, float2 tileMin, float2 tileMax, float2 slope, float unitsPerMetre, float turn)
{
    float shadowDepth = m_cascades[0].w;
    float texel = 4 / m_shadowMapResolutions.x;
    float searchRadius = clamp(OccluderReach * unitsPerMetre * g_sunTangent, texel, MaxRadiusTexels * texel);
    // The four texels a gather or a comparison reads lie up to a texel from the plane's point.
    float bias = (BiasTexels + abs(slope.x) + abs(slope.y)) * texel;

    // The mean height above the receiver's plane of what blocks the sun around it.
    float blockerHeight = 0, blockers = 0;
    [unroll] for (int i = 0; i < BlockerSamples; ++i)
    {
        float2 offset = VogelDisc(i, BlockerSamples, turn) * searchRadius;
        float plane = receiverDepth + shadowDepth * (dot(slope, offset) + bias + BiasPerOffset * length(offset));
        float4 depths = g_ShadowTexture.Gather(g_samplerPoint_Clamp, clamp(uv + float2(offset.x, -offset.y) * 0.25, tileMin, tileMax));
        float4 blocking = depths > plane ? 1.0 : 0.0;
        blockerHeight += dot(blocking, depths - plane);
        blockers += dot(blocking, 1);
    }

    if (blockers == 0) return 1;
    if (blockers == BlockerSamples * 4) return 0;

    // The penumbra's radius grows with the blockers' height, at the sun's angular radius.
    float radius = clamp(blockerHeight / (blockers * shadowDepth) * g_sunTangent, texel, searchRadius);
    float blocked = 0;
    [unroll] for (int j = 0; j < FilterSamples; ++j)
    {
        float2 offset = VogelDisc(j, FilterSamples, turn) * radius;
        float plane = receiverDepth + shadowDepth * (dot(slope, offset) + bias + BiasPerOffset * length(offset));
        float2 sampleUv = clamp(uv + float2(offset.x, -offset.y) * 0.25, tileMin, tileMax);
        blocked += g_ShadowTexture.SampleCmp(g_samplerComparisonLess_Linear_Clamp, sampleUv, plane);
    }

    return 1 - blocked / FilterSamples;
}
)" R"(
// The four cascades share one atlas, two by two. Returns 1 where the sun reaches, after the
// distance fade, the clouds and the height fade. `tangentX` and `tangentY` lie on the receiver, in
// view space, for the soft shadows. With ray-traced shadows, `rayTraced` is the rays' shadow, which
// leaves out the alpha-tested meshes (leaves, grass): the shadow map gives theirs, so the darker of
// the two counts, and the map fades to the rays alone towards the end of its range.
float SunShadow(float3 viewPosition, float2 pixel, float3 tangentX, float3 tangentY, float rayTraced)
{
    float4 position = float4(viewPosition, 1);
    float resolution = m_shadowMapResolutions.x;
    float2 halfPixelSize = 0.5 / resolution;
    float3 shadowPosition = float3(dot(position, m_shadowProjection[0]), dot(position, m_shadowProjection[1]),
        dot(position, m_shadowProjection[2]));

    // Cascade selection, dithered across the cascade borders when cascade blend is on.
    float texel = 1 / resolution;
    float area = 1 - texel * 8;
    float areaFirst = area * area;
    float areaOther = areaFirst;
    float randomValue = 0;
    if (m_cascades[3].w != 0)
    {
        float2 centerOffset = g_RandomTexture.Sample(g_samplerLinear_Wrap, pixel * 0.03125).xy;
        float random = max(centerOffset.y, centerOffset.x);
        randomValue = random * 0.0025;
        float shrunk = area - texel * (random * resolution / 4);
        areaOther = shrunk * shrunk * 1;
    }

    float shadowDepth = m_cascades[0].w;
    float4 level0 = float4(shadowPosition, 0);
    float4 level1 = float4(m_cascades[0].x, m_cascades[1].x, m_cascades[2].x, m_cascades[3].x);
    float4 level2 = float4(m_cascades[0].y, m_cascades[1].y, m_cascades[2].y, m_cascades[3].y);
    float4 level3 = float4(m_cascades[0].z, m_cascades[1].z, m_cascades[2].z, m_cascades[3].z);
    level1 = float4(level1.xyz + shadowPosition * level1.w, 1);
    level2 = float4(level2.xyz + shadowPosition * level2.w, 2);
    level3 = float4(level3.xyz + shadowPosition * level3.w, 3);
    level0.z = shadowDepth * (level0.z + 1);
    level1.z = shadowDepth * (level1.z + 1);
    level2.z = shadowDepth * (level2.z + 1);
    level3.z = shadowDepth * (level3.z + 1);

    float3 inside0 = float3(areaFirst >= level0.xy * level0.xy, level0.z >= randomValue);
    float3 inside1 = float3(areaOther >= level1.xy * level1.xy, level1.z >= randomValue);
    float3 inside2 = float3(areaOther >= level2.xy * level2.xy, level2.z >= randomValue);
    float mask0 = inside0.y * inside0.x * inside0.z;
    float hit = 1 - mask0;
    float mask1 = inside1.y * inside1.x * inside1.z * hit;
    hit = (1 - mask1) * hit;
    float mask2 = hit * (inside2.y * inside2.x * inside2.z);
    hit = hit * (1 - mask2);
    float4 cascade = level0 * mask0 + level1 * mask1 + level2 * mask2 + level3 * hit;

    float level = cascade.w;
    float compareDepth = max(cascade.z, 0.000001);
    float2 uv = float2(cascade.x * 0.5 + 0.5, 1 - (cascade.y * 0.5 + 0.5)) * 0.5;
    uv = min(uv, 1 - halfPixelSize * 2);
    float row = saturate(level - 1);
    uv += float2(level / 2 - row, row * 0.5);
    uv = halfPixelSize + uv;

    float shadow;
    [branch] if (g_softShadows)
    {
        float2 tileMin = float2(level / 2 - row, row * 0.5) + texel;
        float2 tileMax = tileMin + 0.5 - texel * 2;
        float unitsPerMetre = length(m_shadowProjection[0].xyz) * dot(float4(mask0, mask1, mask2, hit), float4(1, m_cascades[3].xyz));
        // The receiver's plane in cascade units, from its tangents; the cascade's scale cancels out.
        float3x3 toCascade = float3x3(m_shadowProjection[0].xyz, m_shadowProjection[1].xyz, m_shadowProjection[2].xyz);
        float3 normal = cross(mul(toCascade, tangentX), mul(toCascade, tangentY));
        float2 slope = clamp(-normal.xy * normal.z / max(normal.z * normal.z, 1e-12), -MaxSlope, MaxSlope);
        float turn = 6.2831853 * InterleavedGradientNoise(pixel + 5.588238 * g_frame);
        shadow = SoftShadow(uv, compareDepth, tileMin, tileMax, slope, unitsPerMetre, turn);
    }
    else
    {
        // Four comparisons around the position, with a 2x2 pixel dither.
        float4 dither = (frac(pixel.xyxy / 2) >= 0.3 ? 1.0 : 0.0) * 2 - 1;
        float4 offset = (dither * 0.125 + float4(0.5, 0.5, -0.5, -0.5)) * (1 / resolution);
        float sum = g_ShadowTexture.SampleCmp(g_samplerComparisonLess_Linear_Clamp, offset.xy + uv, compareDepth) + 0;
        sum = g_ShadowTexture.SampleCmp(g_samplerComparisonLess_Linear_Clamp, offset.zy + uv, compareDepth) + sum;
        sum = g_ShadowTexture.SampleCmp(g_samplerComparisonLess_Linear_Clamp, offset.xw + uv, compareDepth) + sum;
        sum = g_ShadowTexture.SampleCmp(g_samplerComparisonLess_Linear_Clamp, offset.zw + uv, compareDepth) + sum;
        shadow = 1 - sum * 0.25 * 1;
        shadow = shadow * shadow;
    }

    // Fades to lit towards the end of the shadow range.
    float3 fromMaxLevel = viewPosition - float3(0, 0, m_lightParams[8].x);
    float pastShadowStart = max(length(fromMaxLevel) - m_lightParams[8].y, 0);
    float inViewRange = saturate(pastShadowStart * m_lightParams[6].x) * (fromMaxLevel.z >= 0 ? 1.0 : 0.0);
    inViewRange *= inViewRange;
    [branch] if (g_rayTraced)
    {
        shadow = min(shadow, rayTraced);
        shadow = (rayTraced - shadow) * inViewRange + shadow;
    }
    else
        shadow = (1 - shadow) * inViewRange + shadow;
    shadow = saturate(shadow - m_lightParams[6].w);

    float2 cloudPosition = float2(dot(m_objectParams[9], position), dot(m_objectParams[10], position));
    float cloud = g_CloudTexture.Sample(g_samplerLinear_Wrap, cloudPosition).x;
    float cloudAmount = saturate((1 / (m_objectParams[12].z - 0)) * (cloud - 0));
    cloudAmount = cloudAmount * cloudAmount * (cloudAmount * -2 + 3);
    shadow = saturate(m_objectParams[12].w * cloudAmount) * shadow;

    float heightFade = 1 - saturate((viewPosition.z - m_objectParams[11].x) * m_objectParams[11].y);
    return heightFade * heightFade * shadow;
}

float3 ViewPosition(float2 screenPosition, float depth)
{
    float viewZ = m_projectionParam.z / (depth - m_projectionParam.w);
    return float3(screenPosition * m_projectionParam.xy * viewZ, viewZ);
}

// The surface's tangent along a screen axis, towards the neighbour nearer in depth, so a pixel at an
// edge takes the side it belongs to.
float3 Tangent(float3 center, float3 before, float3 after)
{
    return abs(after.z - center.z) < abs(center.z - before.z) ? after - center : center - before;
}
#endif
)" R"(

void main(float4 position : SV_Position, float4 clipPosition : TEXCOORD0, float4 samplingPosition : TEXCOORD1,
    out float4 outDiffuse : SV_Target0, out float4 outSpecular : SV_Target1)
{
    // G-buffer
    float2 texcoord = samplingPosition.xy / samplingPosition.w;
    float3 encodedNormal = g_NormalTexture.Sample(g_samplerPoint_Wrap, texcoord).xyz;
    float4 specularMap = g_SpecularTexture.Sample(g_samplerPoint_Wrap, texcoord);
    float depth = g_DepthTexture.Sample(g_samplerPoint_Wrap, texcoord).x;
    float2 normalXY = encodedNormal.xy * 2 - 1;
    float normalZ = encodedNormal.z * encodedNormal.z * 2 - 1;
    float oneMinusZz = 1 - normalZ * normalZ;
    float3 normal = float3(rsqrt(dot(normalXY, normalXY) * oneMinusZz + 1e-7) * (oneMinusZz * normalXY), normalZ);
    float roughness = specularMap.x;
    float specularIntensity = specularMap.y;
    float materialIndex = specularMap.z;
    float translucent = specularMap.w;

    float2 screenPosition = clipPosition.xy / clipPosition.w - m_cameraCenterOffset.xy;
    float viewZ = m_projectionParam.z / (depth - m_projectionParam.w);
    float3 viewPosition = float3(screenPosition * m_projectionParam.xy * viewZ, viewZ);
    float3 eyeDirection = normalize(viewPosition);

    float3 lightDirection = m_lightParams[0].xyz;
    float4 lightColor = float4(m_exposure.z * m_lightParams[1].xyz, m_lightParams[1].w);

    float shadow = 1;
    [branch] if (g_screenSpaceShadows) shadow = g_ScreenSpaceShadow.Load(int3(position.xy, 0));
    float rayTraced = 1;
    [branch] if (g_rayTraced) rayTraced = g_RayTracedShadow.Load(int3(position.xy, 0));
#if SHADOW_MAP
    // The receiver's tangents for the soft shadows, from its neighbours' depth.
    float2 pixelStep = float2(ddx(screenPosition.x), ddy(screenPosition.y));
    float3 tangentX = 0, tangentY = 0;
    [branch] if (g_softShadows)
    {
        int2 pixel = int2(position.xy);
        float3 left = ViewPosition(screenPosition - float2(pixelStep.x, 0), g_DepthTexture.Load(int3(pixel - int2(1, 0), 0)).x);
        float3 right = ViewPosition(screenPosition + float2(pixelStep.x, 0), g_DepthTexture.Load(int3(pixel + int2(1, 0), 0)).x);
        float3 above = ViewPosition(screenPosition - float2(0, pixelStep.y), g_DepthTexture.Load(int3(pixel - int2(0, 1), 0)).x);
        float3 below = ViewPosition(screenPosition + float2(0, pixelStep.y), g_DepthTexture.Load(int3(pixel + int2(0, 1), 0)).x);
        tangentX = Tangent(viewPosition, left, right);
        tangentY = Tangent(viewPosition, above, below);
    }
    shadow *= SunShadow(viewPosition, position.xy - 0.5, tangentX, tangentY, rayTraced);
    float specularShadow = max(shadow, m_lightParams[7].x);
#else
    // The game's pass beyond the shadow range has 1 here; the rays give the shadow it lacks.
    shadow *= rayTraced;
    float specularShadow = shadow < 1 ? max(shadow, m_lightParams[7].x) : 1;
#endif

    // The sun as a disc: the light direction moves towards the reflection by its angular size.
    float LN = dot(lightDirection, normal);
    float3 toEye = -eyeDirection;
    float toEyeN = dot(toEye, normal);
    float3 reflection = -(-(toEyeN + toEyeN) * normal + toEye);
    float rate = min(max(lightColor.w / length(reflection - lightDirection), 0), 1);
    float3 discDirection = normalize((reflection - lightDirection) * rate + lightDirection);
    float3 halfway = normalize(toEye + discDirection);
    float HN = saturate(dot(halfway, normal));
    float LH = dot(halfway, lightDirection);

    float4 material1 = g_MaterialTexture.Sample(g_samplerPoint_Clamp, float2(materialIndex, 0.25));
    float4 material0 = g_MaterialTexture.Sample(g_samplerPoint_Clamp, float2(materialIndex, 0.75));
    float F0 = material0.x;
    float anisoRoughness = material0.w;
    float lookupFresnel = g_lut1.Sample(g_samplerLinear_Clamp, float2(LH, F0)).x;
    float LNTranslucent = saturate(max(translucent * material1.w, translucent * (1 - LN * LN) + LN));
    float3 specularColor = specularIntensity * material1.xyz;

    float3 lutCoordinate = float3(saturate(HN), roughness, anisoRoughness);
    lutCoordinate = lutCoordinate * lutCoordinate * float3(0.984375, 0.9375, 0.9375) + float3(0.0078125, 0.03125, 0.03125);
    float2 lutValue = g_lut2.Sample(g_samplerLinear_Clamp, lutCoordinate).xy;
    lutValue *= lutValue;
    float specularPower = saturate(LN * 4) * (lutValue.x / lutValue.y * lookupFresnel);
    float3 specular = specularPower * (lightColor.xyz * specularColor);
    float3 diffuse = lightColor.xyz * (LNTranslucent * 1 * 1);

#if SKY_REFLECTION
    float eyeN = dot(eyeDirection, normal);
    float3 reflected = -(eyeN + eyeN) * normal + eyeDirection;
    float EH = saturate(dot(toEye, normal));
    float fresnel = g_lut1.Sample(g_samplerLinear_Clamp, float2(EH, max(F0, m_lightParams[2].z))).x;
    float2 skyUv = (-reflected.xy * 0.96875 * 0.5 + 0.5) * float2(0.25, 0.125);
    skyUv += float2(reflected.z > 0 ? 0.25 : 0, 0);
    float2 skyRate = g_SkyReflectTexture.Sample(g_samplerLinear_Clamp, skyUv).xy * 4;
    float skyRoughness = saturate(roughness * 1.428 - 0.3);
    float3 skyColor = (skyRoughness * (skyRate.y - skyRate.x) + skyRate.x) * m_lightParams[3].xyz;
    float specularRate = specularIntensity * (fresnel * ((1 - roughness) * 0.5));
    float3 environmentSpecular = material1.xyz * (specularRate * skyColor);

    float environmentRate = max(shadow, m_lightParams[2].x) * m_lightParams[2].y;
    outDiffuse = float4(shadow * diffuse * 1 + -0.0, 1);
    outSpecular = float4(specularShadow * specular * 1 + environmentRate * environmentSpecular, 1);
#else
    outDiffuse = float4(shadow * diffuse * 1 + -0.0, 1);
    outSpecular = float4(specularShadow * specular * 1, 1);
#endif
}
)";

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

// The trace's mask, softened by a five-tap cross. The sun and light shaders load the mask with a point
// read rather than a filtered one, so the bilinear filtering inside the trace cannot widen an edge once
// it is written: this pass is the only place a softer edge can come from, and it is why the effect can
// keep Bend's cheap shared-memory march. Outside the light's bounds the mask is copied as it is.
inline constexpr char SoftenShadow[] = R"(
cbuffer Soften : register(b0)
{
    int2 Size;        // The mask's size
    int2 BoundsMin;   // The pixels the trace covered
    int2 BoundsMax;
    float Radius;     // Between the centre tap and the outer ones, in pixels
    int Padding;
};
Texture2D<float> Source : register(t0);
RWTexture2D<unorm float> Dest : register(u0);

// A read outside the mask, at its edges.
float At(int2 pixel)
{
    return Source.Load(int3(clamp(pixel, int2(0, 0), Size - 1), 0));
}

[numthreads(8, 8, 1)]
void main(int3 id : SV_DispatchThreadID)
{
    const int2 pixel = id.xy;
    if (any(pixel >= Size)) return;
    if (any(pixel < BoundsMin) || any(pixel >= BoundsMax))
    {
        Dest[pixel] = At(pixel);
        return;
    }

    const int step = max((int)Radius, 1);
    const float sum = At(pixel) * 2
        + At(pixel + int2(step, 0)) + At(pixel - int2(step, 0))
        + At(pixel + int2(0, step)) + At(pixel - int2(0, step));
    Dest[pixel] = sum / 6;
}
)";
}
