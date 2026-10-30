#include "features/path_tracing/settings.hpp"

namespace phantom_fx
{
namespace
{
constexpr PathTracing Defaults{};
}

void describe(PathTracing& pathTracing, SettingVisitor& visit)
{
    visit.feature(pathTracing.on,
        {.menuId = "pathTracing",
            .iniKey = "path_tracing",
            .section = Section::Lighting,
            .name = "Path tracing",
            .tech = "PT",
            .tag = "Experimental",
            .tip = "With DLSS Ray Reconstruction denoising, the reflections are traced too: a ray from every pixel along its surface's "
                   "reflection, sharp on smooth surfaces and spread on rough ones, in place of the game's screen-space and sky reflections. "
                   "Water and glass keep the game's reflections. Needs the global illumination on and DLSS Ray Reconstruction running.",
            .summary = "Traced reflections on every surface, in place of the game's screen-space and sky reflections.",
            .keywords = "PT path tracing reflections ray reconstruction",
            .needsSupportedBuild = true});
    visit.slider(pathTracing.reflections,
        {.iniKey = "path_traced_reflections",
            .label = "Reflections",
            .max = 4,
            .standard = Defaults.reflections,
            .tip = "How strongly surfaces reflect. 1 is physically based; higher makes the reflections stronger, 0 leaves them out."});
}
}
