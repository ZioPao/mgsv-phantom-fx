#include "features/lamp_soft_shadows/settings.hpp"

namespace phantom_fx
{
void describe(LampSoftShadows& shadows, SettingVisitor& visit)
{
    visit.feature(shadows.on,
        {.menuId = "lampSoftShadows",
            .iniKey = "soft_local_shadows",
            .section = Section::Shadows,
            .name = "Lamp soft shadows",
            .tech = "PCSS",
            .tip = "Contact-hardening soft shadows from each lamp's own shadow map: sharp where the light meets a surface, softer the further "
                   "the shadow falls, as the lamp's size gives. Points and spots, in a rebuilt copy of the game's own shader. Only the lamps the "
                   "game gives a shadow map take it; most of its point lights have none, and keep their light. Off keeps the game's evenly sharp "
                   "edges.",
            .summary = "A lamp's shadow softens with distance, as a real bulb's does.",
            .keywords = "PCSS point spot lamp soft",
            .effect = Effect::LampSoftShadows,
            .replacedBy = Effect::RayTracedLights});
}
}
