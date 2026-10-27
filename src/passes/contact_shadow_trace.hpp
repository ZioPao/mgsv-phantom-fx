#pragma once

#include "d3d11_helpers.hpp"
#include "game_lights.hpp"
#include "phantom_fx_settings.hpp"

#include <d3d11.h>
#include <array>
#include <cstdint>
#include <optional>
#include <string>

// Bend Studio's screen-space shadow, traced through the depth buffer towards a light
// (docs/DEVELOPMENT.md, "Screen-space shadows"). The sun's contact shadows trace it once a frame
// (sun_contact_shadows.hpp), the lamps' once per light (lamp_contact_shadows.hpp).
namespace phantom_fx::contact_shadow_trace
{
// What a trace covers: pixels from `min` to `max` (exclusive) whose depth lies between the depth
// bounds (exclusive). It writes no others.
struct TraceBounds
{
    std::int32_t min[2], max[2];
    float depthMin, depthMax;
};

// A screen-space shadow, 1 where the light reaches, at the depth buffer's size.
using ShadowTarget = ComputeTexture;

// What this thread wrote to the light pass's cPSScene (b2) and cPSLight (b3), each at its full size.
struct PassConstants
{
    std::array<float, game_lights::SceneConstantsSize / 4> scene{};
    std::array<float, game_lights::LightConstantsSize / 4> light{};
};

// What a trace dispatched: where its light is on screen, and how many dispatches it took.
struct Traced
{
    float lightPixel[2];
    int dispatches;
};

// Who traces this frame, so the light passes' constants are kept for them (gi_inputs.hpp).
enum class Tracer : std::uint8_t { Sun, Lamps };

void attach(ID3D11Device* device) noexcept;
// Releases everything; only when no Direct3D work can run (device teardown, unload).
void detach() noexcept;
// Compiles the trace while the sun's or the lamps' contact shadows run, before their first frame.
void configure(const Configuration& configuration) noexcept;
// The trace did not compile (see PhantomFX.log).
bool failed() noexcept;
void tracesThisFrame(Tracer tracer, bool traces) noexcept;

// None where unknown.
std::optional<PassConstants> passConstants(ID3D11DeviceContext* context);
// Bend's inLightProjection: a view-space point (w 1) or direction (w 0) through the pass's own
// projection, c21 and c28, which it reconstructs positions from, so it matches the depth.
std::array<float, 4> project(const float* scene, const float (&light)[4]);
// The width and height of the depth buffer `view` shows; throws when it is not a 2D texture, as the
// trace needs one.
std::array<int, 2> depthSize(ID3D11ShaderResourceView* view);
// What a light at view-space `center` that reaches `range` can light on a screen of `size`: its
// sphere's bounds on screen and in depth, the whole screen when the range is unknown. None when the
// sphere is behind the camera or off the screen.
std::optional<TraceBounds> boundsOf(const float* scene, const std::array<float, 3>& center, float range, std::array<int, 2> size);
// Traces the screen-space shadow towards `lightProjection` from `depth` into `target` on `context`,
// with the game's compute state saved around it; `specular` (may be null) tells foliage, which casts
// none. With `bounds`, it writes only within them, and the shadow draw takes the rest as lit;
// without, it covers the whole screen and the sky stays lit. None when the trace is unavailable.
std::optional<Traced> trace(ID3D11DeviceContext* context, ID3D11ShaderResourceView* depth, ID3D11ShaderResourceView* specular,
    std::array<float, 4> lightProjection, const std::optional<TraceBounds>& bounds, ShadowTarget& target);
// The last trace's constants, which the lamps' shadow draw reads its bounds from.
ID3D11Buffer* parameters() noexcept;
// For the log.
std::string describe(const Traced& traced);
}
