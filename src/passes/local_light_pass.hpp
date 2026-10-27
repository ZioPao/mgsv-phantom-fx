#pragma once

#include "phantom_fx_settings.hpp"
#include "ui_redirect.hpp"

#include <d3d11.h>
#include <cstddef>

// The game's local light pass (docs/DEVELOPMENT.md, "Screen-space shadows" and "Lamp soft shadows"): a
// point or spot light draws its volume twice, to mark the stencil and then to light. While a lamp
// shadow effect runs, the pass draws a light's draws here instead:
//
// - The lamps' contact shadows put the light's screen-space shadow in the light targets' alpha after
//   the marking draw, and scale the lighting draw's light by it (lamp_contact_shadows.hpp).
// - Lamp soft shadows draw the lighting draw with a rebuilt light shader that softens the light's own
//   shadow map (lamp_soft_shadows.hpp).
//
// With neither running the game draws its own.
namespace phantom_fx::local_light_pass
{
// Releases everything; only when no Direct3D work can run (device teardown, unload).
void detach() noexcept;
// Applies from the frame recorded after the next present.
void configure(const Configuration& configuration) noexcept;

// ---- Recording, on the game's render and worker threads ----
void shaderCreated(const void* code, std::size_t size, IUnknown* shader) noexcept;
// Before an indexed draw: a point or spot light's draws, while a lamp shadow effect runs, are drawn
// here instead; returns true when the game's draw must be skipped.
bool draw(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call) noexcept;
// Each present. The game records a whole frame between presents, so all of a frame's lights draw
// with the same effects.
void present() noexcept;
}
