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
            .tip = "Bend Studio's screen-space shadows for every lamp, spot light and flashlight, which gives lights that have no shadow of their "
                   "own one. Costly: one trace per light, so the frame rate drops where many lights are on screen, as in a base at night.",
            .summary = "Contact shadows for every lamp and flashlight. One trace per light, so it costs more where many lights are on screen.",
            .keywords = "screen-space SSS",
            .effect = Effect::LampContactShadows,
            .replacedBy = Effect::RayTracedLights});
}
}
