#include "features/global_illumination/gi_lighting.hpp"

#include <algorithm>
#include <cmath>

namespace phantom_fx::gi_lighting
{
namespace
{
constexpr double Pi = 3.14159265358979323846;

// Capsaicin's SH_GetCoefficients (passes/ray_tracing/shaders/gi_common.hlsli).
std::array<double, 9> basis(double x, double y, double z)
{
    std::array<double, 9> c{};
    c[0] = 0.2820947917738781;
    c[2] = 0.4886025119029199 * z;
    c[6] = 0.9461746957575601 * z * z - 0.3153915652525201;
    c[3] = -0.48860251190292 * x;
    c[1] = -0.48860251190292 * y;
    c[7] = -1.092548430592079 * z * x;
    c[5] = -1.092548430592079 * z * y;
    c[8] = 0.5462742152960395 * (x * x - y * y);
    c[4] = 0.5462742152960395 * (2 * x * y);
    return c;
}

int band(int coefficient)
{
    return coefficient == 0 ? 0 : coefficient < 4 ? 1 : 2;
}
}

SphericalHarmonics skyRadiance(const std::array<std::array<float, 16>, 3>& matrices, float exposure)
{
    // Projects the light over the sphere numerically (it is a quadratic, so L2 holds it exactly),
    // then divides out the clamped cosine's band factors (Ramamoorthi and Hanrahan): pi, 2 pi / 3
    // and pi / 4, over pi for irradiance over pi.
    constexpr int Rings = 64, Segments = 128;
    std::array<std::array<double, 3>, 9> projected{};
    for (int ring = 0; ring < Rings; ++ring)
    {
        const double theta = (ring + 0.5) * Pi / Rings;
        const double weight = std::sin(theta) * (Pi / Rings) * (2 * Pi / Segments);
        for (int segment = 0; segment < Segments; ++segment)
        {
            const double phi = (segment + 0.5) * 2 * Pi / Segments;
            const double x = std::sin(theta) * std::cos(phi), y = std::sin(theta) * std::sin(phi), z = std::cos(theta);
            const double c[4]{-x, -z, y, 1};
            const auto coefficients = basis(x, y, z);
            for (int color = 0; color < 3; ++color)
            {
                const auto& m = matrices[color];
                double light = 0;
                for (int row = 0; row < 4; ++row)
                    for (int column = 0; column < 4; ++column) light += c[row] * m[row * 4 + column] * c[column];

                light *= exposure;
                for (int k = 0; k < 9; ++k) projected[k][color] += light * coefficients[k] * weight;
            }
        }
    }

    constexpr double Deconvolution[]{1.0, 1.5, 4.0};
    SphericalHarmonics radiance{};
    for (int k = 0; k < 9; ++k)
        for (int color = 0; color < 3; ++color) radiance[k][color] = static_cast<float>(projected[k][color] * Deconvolution[band(k)]);

    return radiance;
}

Color evaluate(const SphericalHarmonics& sh, const std::array<float, 3>& direction)
{
    const auto coefficients = basis(direction[0], direction[1], direction[2]);
    Color result{};
    for (int k = 0; k < 9; ++k)
        for (int color = 0; color < 3; ++color) result[color] += static_cast<float>(coefficients[k] * sh[k][color]);

    for (auto& value : result) value = std::max(value, 0.0f);

    return result;
}

std::array<float, 3> toWorld(const std::array<std::array<float, 4>, 3>& viewToWorld, const std::array<float, 3>& direction)
{
    std::array<float, 3> world{};
    for (int row = 0; row < 3; ++row)
        world[row] = viewToWorld[row][0] * direction[0] + viewToWorld[row][1] * direction[1] + viewToWorld[row][2] * direction[2];

    const float length = std::sqrt(world[0] * world[0] + world[1] * world[1] + world[2] * world[2]);
    if (length > 0)
        for (auto& value : world) value /= length;

    return world;
}
}
