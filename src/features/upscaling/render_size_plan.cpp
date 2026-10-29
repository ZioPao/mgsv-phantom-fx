#include "features/upscaling/render_size_plan.hpp"

#include <algorithm>
#include <cmath>

namespace phantom_fx::upscaler
{
bool RenderSizePlan::isFor(const Upscaling& wanted, render_scale::Size outputSize) const
{
    return upscaler == wanted.upscaler && mode == wanted.mode && customScale == wanted.customRenderScale && output == outputSize;
}

DlssQuality dlssQuality(Mode mode, float scale)
{
    switch (mode)
    {
    case Mode::Quality: return DlssQuality::MaxQuality;
    case Mode::Balanced: return DlssQuality::Balanced;
    case Mode::Performance: return DlssQuality::MaxPerformance;
    case Mode::UltraPerformance: return DlssQuality::UltraPerformance;
    case Mode::Custom:
        return scale >= 0.99f ? DlssQuality::Dlaa : scale >= 0.66f ? DlssQuality::MaxQuality : scale >= 0.58f ? DlssQuality::Balanced
            : scale >= 0.5f ? DlssQuality::MaxPerformance : DlssQuality::UltraPerformance;
    default: return DlssQuality::Dlaa;
    }
}

RenderSizePlan planRenderSize(const Upscaling& wanted, render_scale::Size output, std::optional<render_scale::Size> dlssOptimal)
{
    const auto scale = nominalScale(wanted.upscaler, wanted.mode, wanted.customRenderScale);
    const bool takesDlssSize = wanted.upscaler == Upscaler::Dlss && wanted.mode != Mode::Custom && dlssOptimal && dlssOptimal->width && dlssOptimal->height;
    auto render = takesDlssSize ? *dlssOptimal
                                : render_scale::Size{static_cast<unsigned>(std::lround(output.width * scale)), static_cast<unsigned>(std::lround(output.height * scale))};
    render.width = std::min(render.width + (render.width & 1), output.width);
    render.height = std::min(render.height + (render.height & 1), output.height);

    return {.render = render, .output = output, .quality = dlssQuality(wanted.mode, scale), .upscaler = wanted.upscaler, .mode = wanted.mode,
        .customScale = wanted.customRenderScale};
}
}
