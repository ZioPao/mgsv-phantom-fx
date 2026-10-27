#pragma once

#include "fxui/fxui.hpp"
#include "performance.hpp"
#include "phantom_fx_settings.hpp"

#include <span>

// Phantom FX's effects as fxui budget lines: the settings window's costs and states, the costs over
// the game, and PhantomFX-performance.txt.
namespace phantom_fx::menu
{
std::span<const fxui::Section> sections();

// Each effect's state from its setting and its module, and its cost from the timed passes. Its switch
// writes into `wanted`, which must outlive the budget.
fxui::Budget budget(Settings& wanted, const performance::Costs& costs);
}
