#include "features/contact_shadow_shape/settings.hpp"

namespace phantom_fx
{
namespace
{
constexpr ContactShadowShape Defaults{};
constexpr Effect ShownIn[]{Effect::SunContactShadows, Effect::LampContactShadows};
}

void describe(ContactShadowShape& shape, SettingVisitor& visit)
{
    visit.slider(shape.contrast,
        {.iniKey = "contact_shadow_contrast",
            .label = "Contact shadow contrast",
            .min = 0.5f,
            .max = 4,
            .standard = Defaults.contrast,
            .format = "%.1f",
            .tip = "How dark and how hard-edged the contact shadows come out. The trace always finds a distance to a surface, and this decides "
                   "how much of that distance still counts as shadow: higher darkens more and sharpens the edge, lower leaves a soft grey wash. 1 "
                   "is the trace itself, the softest it gets; Bend Studio's own value is 4.",
            .shownIn = ShownIn});
    visit.slider(shape.reachPixels,
        {.iniKey = "contact_shadow_reach",
            .label = "Contact shadow reach",
            .min = 8,
            .max = 120,
            .standard = Defaults.reachPixels,
            .format = "%.0f px",
            .tip = "How far along the screen the contact shadows are traced, in pixels of the render size. Longer picks up casters further "
                   "from what they shadow, and costs more; shorter is the cheap end. At 4K the same pixels reach half as far in the world as at "
                   "1080p.",
            .shownIn = ShownIn});
    visit.slider(shape.thickness,
        {.iniKey = "contact_shadow_thickness",
            .label = "Contact shadow thickness",
            .min = 0.002f,
            .max = 0.025f,
            .standard = Defaults.thickness,
            .format = "%.1f%%",
            .readoutScale = 100,
            .tip = "How deep a surface has to be behind the receiver to count as a contact. Bend Studio's 0.5% is thin: the trace only sees "
                   "something sitting almost exactly at the receiver's depth, which is why its contact shadows are so tight. Raising it widens "
                   "what counts, and darkens and softens every contact.",
            .shownIn = ShownIn});
    visit.slider(shape.fadeMetres,
        {.iniKey = "contact_shadow_fade",
            .label = "Contact shadow fade",
            .min = 50,
            .max = 700,
            .standard = Defaults.fadeMetres,
            .format = "%.0f m",
            .tip = "How far away the contact shadows reach, in metres. Full strength underfoot, then a straight fade to nothing by this "
                   "distance, where the shadow map is the better tool. RenoDX fades them the same way; without it they stay as sharp at a "
                   "hundred metres as at your feet.",
            .shownIn = ShownIn});
}
}
