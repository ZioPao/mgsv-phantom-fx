#include "features/sun/settings.hpp"

namespace phantom_fx
{
namespace
{
constexpr Sun Defaults{};
constexpr Effect ShownIn[]{Effect::RayTracedShadows, Effect::SoftShadows, Effect::GlobalIllumination, Effect::PathTracing};
}

void describe(Sun& sun, SettingVisitor& visit)
{
    visit.slider(sun.diameterDegrees,
        {.iniKey = "sun_size",
            .label = "Sun size",
            .min = 0.1f,
            .max = 4,
            .standard = Defaults.diameterDegrees,
            .format = "%.2f\xC2\xB0",
            .tip = "The sun's angular diameter, which sets how fast shadows soften with distance. It also sets the sun for the global "
                   "illumination. 0.53 is the real sun.",
            .shownIn = ShownIn});
}
}
