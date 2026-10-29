#pragma once

#include "d3d11_helpers.hpp"
#include "game_constants.hpp"
#include "phantom_fx_settings.hpp"

#include <d3d11.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>

// Bend Studio's screen-space shadow, traced through the depth buffer towards a light
// (docs/DEVELOPMENT.md, "Screen-space shadows"), in the shape the contact shadow settings give it. The
// sun's contact shadows trace it once a frame (sun_contact_shadows.hpp); the lamps' march it themselves,
// in the light's own draw (lamp_contact_shadows.hpp).
namespace phantom_fx::contact_shadow_trace
{
// A screen-space shadow, 1 where the light reaches, at the depth buffer's size.
using ShadowTarget = ComputeTexture;

// What a trace dispatched: where its light is on screen, and how many dispatches it took.
struct Traced
{
    float lightPixel[2];
    int dispatches;
};

// Who traces this frame, so the light passes' constants are kept for them (constant_capture.hpp).
enum class Tracer : std::uint8_t { Sun, Lamps };

void attach(ID3D11Device* device) noexcept;
// Releases everything; only when no Direct3D work can run (device teardown, unload).
void detach() noexcept;
// Compiles the trace while the sun's or the lamps' contact shadows run, before their first frame. The
// shape applies from the frame recorded after the next present.
void configure(const Configuration& configuration) noexcept;
void present() noexcept;
// The shape the trace runs with, which the frame being recorded uses.
ContactShadowShape shape() noexcept;
// The trace did not compile (see PhantomFX.log).
bool failed() noexcept;
void tracesThisFrame(Tracer tracer, bool traces) noexcept;

// A light whose nearest point sits at view-space depth `centerDepth` and reaches `range` stands wholly
// past the contact shadows' `fadeMetres` when it is further than that: the march would leave every pixel
// it can light lit, so it can keep the game's draws instead.
inline bool beyondFade(float centerDepth, float range, float fadeMetres) noexcept
{
    return std::isfinite(range) && range > 0 && centerDepth - range > fadeMetres;
}

// Bend's inLightProjection: a view-space point (w 1) or direction (w 0) through the light pass's own
// projection (projectionParam, cameraCenterOffset), which it reconstructs positions from, so it matches
// the depth.
std::array<float, 4> project(const game_constants::SceneConstants& scene, const float (&light)[4]);
// The width and height of the depth buffer `view` shows; throws when it is not a 2D texture, as the
// trace needs one.
std::array<int, 2> depthSize(ID3D11ShaderResourceView* view);
// Traces the screen-space shadow towards `lightProjection` from `depth` into `target` on `context`,
// with the game's compute state saved around it; `scene` is the light pass's, for the distance fade.
// The whole screen is traced, the sky excepted. None when the trace is unavailable.
std::optional<Traced> trace(ID3D11DeviceContext* context, ID3D11ShaderResourceView* depth, const game_constants::SceneConstants& scene,
    std::array<float, 4> lightProjection, ShadowTarget& target);
// For the log.
std::string describe(const Traced& traced);
}
