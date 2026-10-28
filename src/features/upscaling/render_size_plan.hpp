#pragma once

#include "features/upscaling/settings.hpp"
#include "render_scale.hpp"

#include <cstdint>
#include <optional>

// The render size plan: the render size the engine is asked for (render_scale.hpp), from the chosen
// upscaler, mode and output size. NGX stays outside: DLSS's own optimal size comes in as a value.
namespace phantom_fx::upscaler
{
// Values match NVSDK_NGX_PerfQuality_Value, as Preset's match the render presets.
enum class DlssQuality : std::uint8_t { MaxPerformance, Balanced, MaxQuality, UltraPerformance, UltraQuality, Dlaa };

// The render size the upscaler reads and the engine is asked for, the output size it writes, and the
// settings it was planned for.
struct RenderSizePlan
{
    render_scale::Size render, output;
    DlssQuality quality{DlssQuality::Dlaa};
    Upscaler upscaler{};
    Mode mode{};
    float customScale{};

    // Only the upscaler, the mode, the custom scale and the output size decide the render size: anything
    // else would rebuild the engine's targets for nothing.
    bool isFor(const Upscaling& wanted, render_scale::Size outputSize) const;
};

// The quality DLSS is created with for a mode; Custom's follows its scale.
DlssQuality dlssQuality(Mode mode, float scale);
// DLSS's own optimal render size (`dlssOptimal`, from NGX) for its preset modes, the nominal scale for
// Custom, FSR and without one. Odd sizes are rounded up to even, as the engine rounds its own
// (0x2496A0), up to the output size.
RenderSizePlan planRenderSize(const Upscaling& wanted, render_scale::Size output, std::optional<render_scale::Size> dlssOptimal);
}
