#include "features/sun_contact_shadows/settings.hpp"

namespace phantom_fx
{
void describe(SunContactShadows& shadows, SettingVisitor& visit)
{
    visit.feature(shadows.on,
        {.menuId = "sunContactShadows",
            .iniKey = "sun_screen_space_shadows",
            .section = Section::Shadows,
            .name = "Sun contact shadows",
            .tip = "Bend Studio's screen-space shadows for the sun: short shadows traced through the depth buffer. They add the contact shadows "
                   "the sun's shadow map misses (feet, rocks, folds of clothing). Cheap: one trace a frame.",
            .summary = "The short shadows the shadow map misses: feet, rocks, folds of clothing.",
            .keywords = "screen-space SSS",
            .effect = Effect::SunContactShadows,
            .replacedBy = Effect::RayTracedShadows});
}
}
