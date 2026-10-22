#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

// The overlay's character swapper: play as any of the game's player characters (Venom Snake, the
// avatar, a male or female Diamond Dogs soldier with any face, Ocelot, Quiet) in any outfit made for
// them. It writes the player variables the game's own scripts set for this (vars.playerType and the
// rest) on the game thread at the main loop's frame start (game_loop.hpp); the game sees the change
// and reloads the player's model. docs/DEVELOPMENT.md, "Character".
namespace phantom_fx::character
{
// vars.playerType
enum class PlayerType : std::uint8_t { Snake, MaleSoldier, FemaleSoldier, Avatar, Liquid, Ocelot, Quiet };

struct Character
{
    PlayerType type;
    std::uint8_t parts; // vars.playerPartsType: the outfit
    std::string_view name;
};

struct Face
{
    std::uint16_t id; // vars.playerFaceId
    std::string name;
};

// The game's state at the last frame start.
struct State
{
    bool available{}; // In a mission or free roam
    PlayerType type{};
    std::uint8_t parts{};
    std::uint16_t face{};
};

// Lets Ocelot and Quiet, the FOB characters, be played outside FOB, where the player update would
// turn them back into Venom Snake. Patches the engine at start-up; returns what was done, for the log.
std::string allowFobCharacters() noexcept;
// The supported build with the expected engine code; otherwise the menu cannot change anything.
bool supported() noexcept;
State state() noexcept;
std::span<const Character> characters() noexcept;
// The faces a soldier of this type can wear; empty for the other types.
std::span<const Face> faces(PlayerType type) noexcept;
// These apply at the next frame start.
void choose(const Character& character) noexcept;
void chooseFace(std::uint16_t face) noexcept;
// The game thread, at the main loop's frame start: applies what the menu asked for and records the state.
void onFrameStart() noexcept;
}
