#pragma once

#include "passes/contact_shadow_trace.hpp"

#include <d3d11.h>

// The sun's contact shadows (docs/DEVELOPMENT.md, "Screen-space shadows"): Bend Studio's screen-space
// shadow traced towards the sun (contact_shadow_trace.hpp), once a frame, which the sun light pass
// multiplies into the sun's shadow (sun_light_pass.hpp).
namespace phantom_fx::sun_contact_shadows
{
// Releases everything; only when no Direct3D work can run (device teardown, unload).
void detach() noexcept;
// For the budget: unavailable when the trace or the sun light pass did not compile (see PhantomFX.log).
Health health();

// ---- Recording, on the game's render and worker threads ----
// The sun's shadow traced from `depth`, once per depth that a context records: both parts of the sun
// pass read the same shadow. Null when the trace is unavailable.
ID3D11ShaderResourceView* trace(ID3D11DeviceContext* context, ID3D11ShaderResourceView* depth, const contact_shadow_trace::PassConstants& constants);
// FinishCommandList on `context`: its next sun pass traces again.
void contextFinished(ID3D11DeviceContext* context) noexcept;
void present() noexcept;
}
