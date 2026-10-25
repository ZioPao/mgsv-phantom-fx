# fxui

A settings window for ReShade add-ons, built on the Dear ImGui that ReShade gives add-ons.

The window is a frame budget:

- **Top strip:** a master switch, the frame readout, and one rendered frame: the GPU's work split by effect, then the time the GPU sits idle.
- **Rail:** search, then one entry per section, each with its milliseconds and a cost bar.
- **Pages:** one per section. Each page is a list of line items: switch, name, state, cost, summary, then the item's settings.
- **Budget page:** every effect with its state, cost, share of the frame and passes.

Every bar uses one scale: 1 ms is the same length everywhere.

## Add it to an add-on

Copy `src/fxui/` and add `fxui/fxui.cpp` to the add-on's sources. It needs ReShade's `include` and `imgui` folders on the include path, C++20, and nothing else.

## Each frame

1. Fill a `fxui::Budget`: one `Line` per effect whose GPU work you time.
2. Open a `fxui::Window`.
3. Draw your pages. Items show their settings while they are on.

```cpp
#include "fxui/fxui.hpp"

#include <imgui.h>
#include <reshade.hpp> // After imgui.h: routes ImGui calls through ReShade.

enum class Section { Budget, Lighting, Diagnostics };

constexpr fxui::Section sections[]{
    {"Budget", 0, true},            // The overview: the budget page
    {"Lighting", fxui::hue::Blue},  // Has timed lines, so it gets a hue
    {"Diagnostics", 0, false, true} // Set apart at the bottom of the rail
};

void drawSettings(Settings& wanted, const Costs& costs)
{
    fxui::Budget budget;
    budget.gpuMs = costs.gpuFrame;
    budget.frameMs = costs.frame;
    budget.lines.push_back({
        .id = "ao",
        .name = "Ambient occlusion",
        .tech = "GTAO",
        .section = static_cast<int>(Section::Lighting),
        .state = wanted.ao ? fxui::State::Working : fxui::State::Off,
        .ms = costs.ao,
        .passes = {{"March", costs.aoMarch}, {"Denoise", costs.aoDenoise}},
        .toggle = &wanted.ao,
    });

    const fxui::Window window(sections, budget, {.name = "My Mod"});

    if (const fxui::Page page(Section::Budget, "Where the GPU's frame goes."); page)
        fxui::ledger();

    if (const fxui::Page page(Section::Lighting, "Light and shade."); page)
        if (const fxui::Item item({.id = "ao", .summary = "Shade where surfaces meet."}); item)
            fxui::slider("Radius", wanted.aoRadius, {.min = 0.25f, .max = 4, .standard = 1, .format = "%.2f m"});
}
```

An item and a budget line with the same `id` belong together: the line gives the item its switch, state, cost and message, and its name and technical name where the item leaves them out.

## Building blocks

| Call | What it draws |
| --- | --- |
| `Window` | The strip, the frame bar, the rail, and the page area |
| `Page` | A section's page: title, total and description |
| `Item`, `item()` | A line item, with or without settings |
| `slider` | Label, track, readout and a reset button while it differs from its default |
| `segmented` | Side-by-side buttons for a few choices, optionally with drawn icons |
| `combo`, `beginCombo`, `option`, `endCombo` | A dropdown for longer lists |
| `searchCombo` | A dropdown with a search box, for very long lists |
| `checkbox`, `button` | The usual |
| `hint`, `note`, `statusLine`, `fact`, `subheading`, `link` | Text in the window's styles |
| `ledger`, `costRow` | The budget table, and rows for other cost tables |
| `CostOverlay` | The costs in a corner over the game, for the `reshade_overlay` event |
| `toast`, `drawToast` | A short message over the game for a few seconds, such as "Frame captured". Call `toast` from anywhere; call `drawToast` in the `reshade_overlay` event, with a side or corner of the screen (default `Right`) |

## Window sizes

The layout follows the window's width, in pixels at the 13 px font:

- **640 and up:** the full rail, with the hotkeys at its foot.
- **500 to 640:** a narrower rail without the hotkeys.
- **Below 500:** a dropdown of sections beside the search, and the page uses the full width.

Inside a page:
- Items put their state and cost under the name when a row would be cramped.
- Settings stack: the label and value on one line, the control full width below.
- The budget drops its share column and shows each state as a dot.

## States

`Working`, `Off`, `Held` (off by the master switch), `Replaced` (by the line in `replacedBy`), `Restart`, `NotRunning` and `Unavailable`. Set `message` to say why an effect is not running.

## Rules that keep the look

- Put every setting inside an item. During a search, only items show.
- Give sections with timed lines a hue from `fxui::hue`. Leave the others at 0.
- Use plain names. Put the technique's name in `tech`, such as "GTVBAO".
- Keep summaries to one line. Long explanations go in `tip`.
- One window per add-on: fxui keeps its page, search and scroll position between frames.
