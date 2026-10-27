#pragma once

#include "phantom_fx_settings.hpp"
#include "ui_redirect.hpp"

#include <d3d11.h>
#include <cstddef>

// The lamps' contact shadows (docs/DEVELOPMENT.md, "Screen-space shadows"). A point or spot light draws
// its volume twice, to mark the stencil and then to light. Before the first, the add-on traces Bend
// Studio's screen-space shadow towards the light within its reach (contact_shadow_trace.hpp) and draws
// it into the light targets' unused alpha; the second's light is then scaled by it. One trace per
// light, so it costs far more where many lights are on screen.
namespace phantom_fx::lamp_contact_shadows
{
void attach(ID3D11Device* device) noexcept;
// Releases everything; only when no Direct3D work can run (device teardown, unload).
void detach() noexcept;
// Applies from the frame recorded after the next present.
void configure(const Configuration& configuration) noexcept;
// For the budget: unavailable when its shaders or the trace did not compile, so the lights keep the
// game's shadows (see PhantomFX.log).
Health health();

// ---- Recording, on the game's render and worker threads ----
void shaderCreated(const void* code, std::size_t size, IUnknown* shader) noexcept;
// Before a draw: a point or spot light's draws, while the effect runs, are drawn here instead; returns
// true when the game's draw must be skipped.
bool draw(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call) noexcept;
// FinishCommandList on `context`.
void contextFinished(ID3D11DeviceContext* context) noexcept;
void present() noexcept;
}
