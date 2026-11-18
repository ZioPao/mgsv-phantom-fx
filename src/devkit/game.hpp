#pragma once

#include <Windows.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <span>

// The game's own layer, as the add-on that uses the devkit has it: the build it knows, and checks on
// the code it expects to find. PhantomFX answers isSupportedBuild from src/device_check.cpp.
namespace devkit::game
{
// Whether mgsvtpp.exe is the build the add-on knows. Its addresses are the build's alone.
bool isSupportedBuild() noexcept;

inline std::uint8_t* image() noexcept
{
    return reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
}

// Code the game's build has at an address in its image.
struct Expected
{
    std::uintptr_t rva;
    std::span<const unsigned char> code;
};

inline bool matches(std::span<const Expected> expected) noexcept
{
    return std::ranges::all_of(expected, [](const Expected& one) { return std::memcmp(image() + one.rva, one.code.data(), one.code.size()) == 0; });
}
}
