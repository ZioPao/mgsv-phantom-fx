#pragma once

#include "game_shaders.hpp"
#include "phantom_fx_settings.hpp"
#include "ui_redirect.hpp"

#include <array>

// The sun's ray-traced shadow on the forward pass's reflective surfaces. Rivers, liquids and glass blend
// over the lit scene and sample the game's shadow map alone, so their water kept the map's shadow while the
// rays' lit the ground (docs/DEVELOPMENT.md, "Forward shadows"). At such a draw the map's shadow is left
// out and the pixels the surface drew are multiplied by the rays' shadow.
namespace phantom_fx::forward_shadows
{
void configure(const Configuration& configuration) noexcept;
bool draw(ID3D11DeviceContext* context, const ui_redirect::DrawCall& call, game_shaders::DrawnShader& shader) noexcept;

// Where the forward surfaces drew: their depth (the game's reversed-Z) and the surface's kind where one
// drew (1 a river or a liquid, 0.5 glass), for the rays traced at their pixels (`ray_tracing::forwardSurface`).
// Null until a river or glass drew a frame.
ID3D11ShaderResourceView* forwardDepth() noexcept;
ID3D11ShaderResourceView* forwardSurface() noexcept;

// Empties the guide targets, for the forward pass to write this frame's into. The ray tracing's kick calls
// it after copying them (once a frame, before the forward pass runs), so what it copies is the last frame's.
void clearGuides(ID3D11DeviceContext* context) noexcept;

// The cameras the forward pass reprojects its reflection with: `viewProjection` is the one the water is drawn
// with this frame, `previousViewProjection` the one its guides were written with (the kick's). Filled by the
// kick, so the forward draw reads them without a lock of its own.
struct Cameras
{
    float viewProjection[16];
    float viewProjectionInverse[16];
    float previousViewProjection[16];
    float previousViewProjectionInverse[16];
    float eye[4];
    float dimensions[2];
    float padding[2];
};
void setCameras(const Cameras& cameras) noexcept;

}
