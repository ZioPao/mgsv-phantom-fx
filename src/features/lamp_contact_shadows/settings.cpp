#include "features/lamp_contact_shadows/settings.hpp"

namespace phantom_fx
{
void describe(LampContactShadows& shadows, SettingVisitor& visit)
{
    visit.feature(shadows.on,
        {.menuId = "lampContactShadows",
            .iniKey = "light_screen_space_shadows",
            .section = Section::Shadows,
            .name = "Lamp contact shadows",
            .tip = "Bend Studio's screen-space shadows for point and spot lights: the short shadows of feet, rocks and folds of cloth, which a "
                   "lamp's shadow map does not resolve. Only lamps that cast a shadow get them. One trace per light, so it costs most in a base "
                   "at night.",
            .summary = "Screen-space shadows for point and spot lights.",
            .keywords = "screen-space SSS",
            .effect = Effect::LampContactShadows,
            .replacedBy = Effect::RayTracedLights});
}
}
