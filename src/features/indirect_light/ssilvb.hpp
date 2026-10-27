#pragma once

#include "passes/visibility_bitmask.hpp"
#include "phantom_fx_settings.hpp"

#include <d3d11.h>
#include <string>

// SSILVB, screen-space indirect lighting with visibility bitmask (docs/DEVELOPMENT.md, "SSILVB"), adds
// the light that nearby surfaces on screen reflect onto each other: the light the visibility bitmask
// march's sectors receive (visibility_bitmask.hpp), at half the render size. At the resolve, once the
// sun and the local lights are in the diffuse light, the resolve reads the diffuse light with that
// bounce added (indirect_light_pass.hpp).
namespace phantom_fx::ssilvb
{
void attach(ID3D11Device* device) noexcept;
// Releases everything; only when no Direct3D work can run (device teardown, unload).
void detach() noexcept;
// Applies from the frame recorded after the next present.
void configure(const Configuration& configuration) noexcept;
// For the budget (fine while off).
Health health();
void present() noexcept;

// ---- Recording, on the game's render and worker threads (indirect_light_pass.cpp) ----
// The game's diffuse light with SSILVB's added, into `lit` at the render size, from what the resolve
// binds. False leaves the resolve the game's diffuse light this frame.
bool compute(ID3D11DeviceContext* context, ID3D11ShaderResourceView* depth, ID3D11ShaderResourceView* normals, ID3D11ShaderResourceView* diffuse,
    ID3D11ShaderResourceView* albedo, ID3D11ShaderResourceView* specular, ID3D11Buffer* scene, visibility_bitmask::Target& lit);
}
