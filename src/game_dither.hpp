#pragma once

#include <d3d11.h>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

// The game adds a fixed 8x8 dither to the colour of its sky draws, its resolve in fog and its forward
// materials, before the upscaler, which enlarges it into a grid over dark areas. With HDR's float scene,
// which does not band, those shaders are made with the dither read as its average. docs/DEVELOPMENT.md,
// "Dither".
namespace phantom_fx::game_dither
{
// A pixel shader about to be made: the code to make it from instead, kept for the session, or null to
// keep the code given.
const std::vector<std::uint8_t>* undithered(ID3D11Device* device, const void* code, std::size_t size) noexcept;
// The code undithered was given, for the code it returned or a copy of it, as HDR keeps; other code as
// it is.
std::pair<const void*, std::size_t> originalOf(const void* code, std::size_t size) noexcept;
}
