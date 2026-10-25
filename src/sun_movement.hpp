#pragma once

#include <string>

// The game moves its sun and moon light in steps, which makes every shadow tick forward about once a
// second. A patch on the light's update makes it follow the sky's sun every frame, as the engine
// already does when a weather asks for no steps. docs/DEVELOPMENT.md, "Sun movement".
namespace phantom_fx::sun_movement
{
// Patches the update on the supported build; returns what was done, for the log.
std::string makeSmooth() noexcept;
}
