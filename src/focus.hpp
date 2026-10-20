#pragma once

#include "camera.hpp"

#include <d3d11.h>

// Photo mode's autofocus: the distance to what is at the centre of the screen, read back from the
// depth DLSS takes (upscaler.hpp) a few frames late. Without DLSS there is no reading.
namespace phantom_fx::focus
{
// Off, nothing is read back and the last reading is forgotten.
void setActive(bool on) noexcept;
// On the immediate context, after DLSS's evaluation: `depth` is its reversed-Z depth (R32_FLOAT)
// as seen from `view`.
void depthReady(ID3D11DeviceContext* immediate, ID3D11Texture2D* depth, const camera::View& view) noexcept;
// Metres along the view; 0 before the first reading.
float distance() noexcept;
// Releases the read-back textures, with the device.
void detach() noexcept;
}
