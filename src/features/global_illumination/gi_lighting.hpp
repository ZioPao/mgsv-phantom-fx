#pragma once

#include <array>

// The game's sky and sun in the global illumination's terms (docs/DEVELOPMENT.md, "Global
// illumination", "Light"). Pure maths, so the tests can check it without the game.
namespace phantom_fx::gi_lighting
{
using Color = std::array<float, 3>;
using SphericalHarmonics = std::array<Color, 9>; // Capsaicin's L2 basis and order

// The game's sky light: SH_SphereMap lights a world normal n with exposure x c^T M c per colour,
// c = (-n.x, -n.z, n.y, 1), M the 4x4 matrices of PsWork's g_psParamSHSky (rows). That is the
// light a surface reflects divided by its albedo: irradiance over pi. Returns the radiance of the
// sky whose irradiance over pi that is, so traced rays that reach the sky light surfaces as the
// game's sky light does where nothing blocks it.
SphericalHarmonics skyRadiance(const std::array<std::array<float, 16>, 3>& matrices, float exposure);

// Evaluates `sh` in a unit world direction, as the kernels do.
Color evaluate(const SphericalHarmonics& sh, const std::array<float, 3>& direction);
}
