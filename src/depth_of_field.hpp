#pragma once

#include "ui_redirect.hpp"

#include <d3d11.h>
#include <cstddef>
#include <cstdint>
#include <string>

// The depth of field (docs/DEVELOPMENT.md, "Depth of field"). Fox Engine blurs with sprites: one
// quad per half-size pixel, sized by its blur, drawn at down to 1/16 size (DOF_Scatter*). With the
// gather on, its passes are left out and, from the same inputs, a gather runs instead: the blur at
// half size, split into what lies in front of the focus and what lies behind it, and blurs of a few
// pixels at full size. It blurs before the game's tonemap, so lights stay bright in the blur.
//
// Photo mode's lens shot (photo_mode.hpp) also runs here, gather or not: frame after frame, the
// scene at the depth of field is added up and the average shown in place of the game's blur.
namespace phantom_fx::depth_of_field
{
struct Options
{
    bool gather{}; // In place of the game's depth of field
    int blades{};  // Aperture blades; 0 is round
};

// Frames a lens shot has added up, of its total; zero outside a shot.
struct ShotProgress
{
    std::uint32_t frames{}, total{};
};

void attach(ID3D11Device* device) noexcept;
// Releases everything; only when no Direct3D work can run (device teardown, unload).
void detach() noexcept;
// Applies from the frame recorded after the next present.
void configure(const Options& options) noexcept;
// Empty while the gather is off or running; otherwise what it waits for, for the overlay.
std::string problem();
ShotProgress shotProgress() noexcept;

// ---- Recording, on the game's render and worker threads ----
void shaderCreated(const void* code, std::size_t size, IUnknown* shader) noexcept;
// Before a draw, indexed or not. Returns true when the add-on made the draw itself, or left it out,
// so the game's draw must be skipped.
bool draw(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call) noexcept;
void present() noexcept;
}
