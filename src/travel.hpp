#pragma once

#include <cstdint>

// The Developer page's travel: free roam in another of the game's three maps after one loading
// screen, without the helicopter and the sortie screens. At the main loop's frame start it runs Lua in
// the game's script state (game_lua.hpp): the mission clear the game's scripts reserve when free roam
// ends, with the player's arrival moved to a fixed spot on foot. docs/DEVELOPMENT.md, "Travel".
namespace phantom_fx::travel
{
enum class Map : std::uint8_t { Afghanistan, Africa, MotherBase, Count };

const char* name(Map map) noexcept;
// Leaves for the map at the next frame start. A toast says whether the game went or why not.
void go(Map map) noexcept;
// The game thread, at the main loop's frame start.
void onFrameStart() noexcept;
}
