#include "features/soft_shadows/settings.hpp"

namespace phantom_fx
{
void describe(SoftShadows& shadows, SettingVisitor& visit)
{
    visit.feature(shadows.on,
        {.menuId = "softShadows",
            .iniKey = "soft_shadows",
            .section = Section::Shadows,
            .name = "Soft shadows",
            .tech = "PCSS",
            .tip = "Contact-hardening soft shadows from the sun's shadow map: sharp where an object meets the ground, softer the further the "
                   "shadow falls from it, as the sun's disc gives. Off keeps the game's evenly sharp shadow edges.",
            .summary = "Sharp where an object meets the ground, softer the further the shadow falls.",
            .keywords = "sun size",
            .effect = Effect::SoftShadows,
            .replacedBy = Effect::RayTracedShadows});
}
}
