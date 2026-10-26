#pragma once

#include "camera.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

// The game's point and spot light shaders of its light pass (docs/DEVELOPMENT.md, "Ray-traced lights"),
// and the constants they read. Lamp contact shadows (shadows.hpp) and the ray-traced lights
// (local_lights.hpp) both recognise them.
namespace phantom_fx::game_lights
{
enum class Shape : std::uint8_t { Point, Spot };
// How a light's shader fades its light to zero with the distance d to the light, from the disassembly
// (docs/DEVELOPMENT.md, "Screen-space shadows"). A point adds cPSLight c6.x to d first; a spot does not.
// - Quartic (SSLighting2_*): 1 / d^2 - d^2 c6.w, zero at d = c6.w^(-1/4).
// - Sixteenth (Lighting_*_NA): (1 - d^16 c6.z) / d^2, zero at d = c6.z^(-1/16).
enum class Falloff : std::uint8_t { Quartic, Sixteenth };
// The game's shader for each kind of light with specular and without a shadow map (isTwin), which draws
// the lights of that kind again.
enum class Twin : std::uint8_t { PointNA, SpotNA, MaskedSpotNA, Point2, Spot2, MaskedSpot2, Count };

struct GameLight
{
    const char* checksum;
    Shape shape;
    Falloff falloff;
    Twin twin;
    bool specular;       // The game's shader adds specular; without, its light's specular scale is 0
    bool shadowMap;      // The game's shader has a shadow map: its shadows are sharp, whatever the light's size
    bool flashlight{};   // The weapon flashlights' shader: every light it drew in the captures was one
};

// Every point and spot light shader of the game's light pass.
constexpr GameLight GameLights[]{
    {"e61cc726c5c877555261f03dbc4aa967", Shape::Point, Falloff::Sixteenth, Twin::PointNA, true, true},       // Lighting_Point_NA
    {"32bdd328651cf4f11ebcc47c0b65dfde", Shape::Point, Falloff::Sixteenth, Twin::PointNA, true, false},      // Lighting_Point_NA_NShdw
    {"f037f71a002e42f0cb07ad2768f7b8ac", Shape::Point, Falloff::Sixteenth, Twin::PointNA, false, true},      // Lighting_Point_NA_NoSpec
    {"259fff504d9caef06a467ff6ce9ee1bf", Shape::Point, Falloff::Sixteenth, Twin::PointNA, false, false},     // Lighting_Point_NA_NoSpec_NShdw
    {"57b7365fcfb89c3f91fffcebf1a6b1e5", Shape::Spot, Falloff::Sixteenth, Twin::SpotNA, true, true},         // Lighting_Spot_NA
    {"dddab560572d7352181cd83f00f6dd63", Shape::Spot, Falloff::Sixteenth, Twin::SpotNA, true, false},        // Lighting_Spot_NA_NShdw
    {"7d2ed256fcff999707444bb115321c7f", Shape::Spot, Falloff::Sixteenth, Twin::SpotNA, false, true},        // Lighting_Spot_NA_NoSpec
    {"91bc5dd518df1368e60d53c5f065e9f4", Shape::Spot, Falloff::Sixteenth, Twin::SpotNA, false, false},       // Lighting_Spot_NA_NoSpec_NShdw
    {"d53793289e8c2824800876d8525aab2c", Shape::Spot, Falloff::Sixteenth, Twin::MaskedSpotNA, true, true},   // Lighting_Spot_NA_Mask
    {"dcc81c5f2c97e20003a27a9e7db54376", Shape::Spot, Falloff::Sixteenth, Twin::MaskedSpotNA, true, false},  // Lighting_Spot_NA_Mask_NShdw
    {"a435549d3334a5715c165a68083ea66e", Shape::Spot, Falloff::Sixteenth, Twin::MaskedSpotNA, false, true},  // Lighting_Spot_NA_Mask_NSpec
    {"fd570742642f09e699d193ffb02bf0df", Shape::Spot, Falloff::Sixteenth, Twin::MaskedSpotNA, false, false, true}, // Lighting_Spot_NA_Mask_NSpec_NShdw (flashlights)
    {"afbc12167037e4e0d653da184c7e3e82", Shape::Point, Falloff::Quartic, Twin::Point2, true, true},          // SSLighting2_Point
    {"4eb687f92330ee7b11ccf8e32950e4a9", Shape::Point, Falloff::Quartic, Twin::Point2, true, false},         // SSLighting2_Point_NoShadow
    {"a1c56a0978c706222ebeacbf6fc9ebe4", Shape::Point, Falloff::Quartic, Twin::Point2, false, true},         // SSLighting2_Point_NoSpec
    {"e9cd14a10e3f1df895cb6799c232f99e", Shape::Point, Falloff::Quartic, Twin::Point2, false, false},        // SSLighting2_Point_NoSpec_NoShadow
    {"ac0947110cb0f17f4ffe930a6a11617d", Shape::Spot, Falloff::Quartic, Twin::Spot2, true, true},            // SSLighting2_Spot
    {"74063ef2a991625184fdf283ba58c43f", Shape::Spot, Falloff::Quartic, Twin::Spot2, true, false},           // SSLighting2_Spot_NoShadow
    {"cdc559ec2ca4d52a54697cf5ca023667", Shape::Spot, Falloff::Quartic, Twin::Spot2, false, true},           // SSLighting2_Spot_NoSpec
    {"a823ffb58661c508e6270227d4910c91", Shape::Spot, Falloff::Quartic, Twin::Spot2, false, false},          // SSLighting2_Spot_NoSpec_NoShadow
    {"9ead0f83217db6af698f860fd3233441", Shape::Spot, Falloff::Quartic, Twin::MaskedSpot2, true, true},      // SSLighting2_Spot_Mask
    {"2d985165a56e0622d42f086a28c305d0", Shape::Spot, Falloff::Quartic, Twin::MaskedSpot2, true, false},     // SSLighting2_Spot_Mask_NShdw
    {"fa6ad11cfc35a2281a22b92c1a2aa6db", Shape::Spot, Falloff::Quartic, Twin::MaskedSpot2, false, true},     // SSLighting2_Spot_Mask_NSpec
    {"74872741b699fdefa13ae71349f41ec9", Shape::Spot, Falloff::Quartic, Twin::MaskedSpot2, false, false},    // SSLighting2_Spot_Mask_NSpec_NShdw
};

constexpr bool isTwin(const GameLight& light)
{
    return light.specular && !light.shadowMap;
}

static_assert(
    [] {
        std::array<int, static_cast<std::size_t>(Twin::Count)> twins{};
        for (const auto& light : GameLights)
            if (isTwin(light)) ++twins[static_cast<std::size_t>(light.twin)];

        return std::ranges::all_of(twins, [](int count) { return count == 1; });
    }(),
    "every kind of light needs exactly one twin");

// The light shader with this checksum (toHex's form); null for any other shader.
inline const GameLight* findGameLight(std::string_view checksum)
{
    const auto found = std::find_if(std::begin(GameLights), std::end(GameLights), [&](const GameLight& light) { return checksum == light.checksum; });

    return found != std::end(GameLights) ? found : nullptr;
}

// cPSScene (b2, camera::SceneConstants), in floats: c8 to c10 take the light passes' view space to the
// world, c12 to c15 (m_shadowProjection) a shadow-mapped light's view space to its shadow map, rows applied
// as dot(row, v), c20 is the eye, c21 the projection's parameters (1 / P00, 1 / P11, B, A), c28 the jitter
// in NDC.
constexpr std::uint32_t SceneConstantsSize = sizeof(camera::SceneConstants); // Bytes
constexpr std::size_t ViewToWorld = 8 * 4, ShadowProjection = 12 * 4, Eye = 20 * 4, ProjectionParam = 21 * 4, CenterOffset = 28 * 4;
// cPSLight (b3), in floats, view space: c0 the way towards the sun (the sun passes), c3 the light, c4 its
// colour and size, c5 the way a spot points, c6 its falloff, c7 a spot's cone (x the cosine past which it
// cuts its light off, y 1 / its penumbra, z the exponent), c8.x its specular's scale.
constexpr std::uint32_t LightConstantsSize = 176; // Bytes
constexpr std::size_t LightDirection = 0, LightPosition = 3 * 4, LightColor = 4 * 4, LightSize = 4 * 4 + 3, LightAxis = 5 * 4,
                      LightFalloff = 6 * 4, LightCone = 7 * 4, LightUmbra = 7 * 4, SpecularScale = 8 * 4;

// How far a light reaches when its falloff does not end, metres.
constexpr float UnendingRange = 1000.0f;

// How far a light reaches, from the falloff in its cPSLight `light` (see Falloff). The ray-traced lights
// take it by the light's shape, never below 0, and UnendingRange where unknown; lamp contact shadows by
// eitherShapeRangeOf.
inline float rangeOf(const float* light, const GameLight& game)
{
    const float* falloff = light + LightFalloff;
    const float factor = game.falloff == Falloff::Quartic ? falloff[3] : falloff[2];
    if (!(factor > 0)) return UnendingRange;

    const float reach = std::pow(factor, game.falloff == Falloff::Quartic ? -1.0f / 4 : -1.0f / 16);

    return std::max(game.shape == Shape::Point ? reach - falloff[0] : reach, 0.0f);
}

// How far a light reaches for lamp contact shadows, which do not look at its shape: the larger of a
// point's range and a spot's, infinite or NaN where unknown, which their trace takes as the whole screen.
inline float eitherShapeRangeOf(const float* light, Falloff falloff)
{
    const float* fade = light + LightFalloff;
    const float reach = falloff == Falloff::Quartic ? std::pow(fade[3], -1.0f / 4) : std::pow(fade[2], -1.0f / 16);

    return reach - std::min(fade[0], 0.0f);
}

// Pixels from `min` to `max` (exclusive) on each axis.
struct PixelRect
{
    std::array<int, 2> min, max;
};

// The pixels a sphere at view-space `center` covers on a screen of `size` pixels, as the light passes'
// projection in cPSScene `scene` puts it: on each axis, the planes through the eye that touch the
// sphere, at x / z (or y / z) = t, bound it. None when it is off the screen. The sphere must lie past
// the camera plane (center z > radius).
inline std::optional<PixelRect> sphereOnScreen(const float* scene, const std::array<float, 3>& center, float radius, std::array<float, 2> size)
{
    const float* projection = scene + ProjectionParam;
    const float* offset = scene + CenterOffset;
    const float z = center[2];
    const float clearance = z * z - radius * radius;
    PixelRect rect{};
    for (int axis = 0; axis < 2; ++axis)
    {
        const float across = center[axis];
        const float spread = radius * std::sqrt(across * across + clearance);
        const float scale = axis == 0 ? 0.5f : -0.5f;
        const auto pixelAt = [&](float t) { return ((t / projection[axis] + offset[axis]) * scale + 0.5f) * size[axis]; };
        const float first = pixelAt((across * z - spread) / clearance), second = pixelAt((across * z + spread) / clearance);
        const auto clamped = [&](float pixel) { return static_cast<int>(std::clamp(pixel, 0.0f, size[axis])); };
        rect.min[axis] = clamped(std::floor(std::min(first, second)) - 1);
        rect.max[axis] = clamped(std::ceil(std::max(first, second)) + 1);
        if (rect.min[axis] >= rect.max[axis]) return std::nullopt;
    }

    return rect;
}
}
