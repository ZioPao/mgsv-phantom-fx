#pragma once

#include <Windows.h>
#include <cstdint>
#include <cstring>
#include <optional>

// Reads of the game's own objects (engine_scene.cpp, engine_brush.cpp): guarded, as another thread may
// free what is read.
namespace phantom_fx::game_memory
{
inline bool readRaw(void* to, std::uintptr_t from, std::size_t size) noexcept
{
    if (from < 0x10000 || from > 0x7FFFFFFF0000ull || size > 0x7FFFFFFF0000ull - from) return false;

    __try
    {
        std::memcpy(to, reinterpret_cast<const void*>(from), size);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

template<class T> std::optional<T> read(std::uintptr_t from) noexcept
{
    T value{};
    if (!readRaw(&value, from, sizeof(T))) return std::nullopt;

    return value;
}

// Whether an object's first word is this vtable (an address in the game's image).
inline bool isA(std::uintptr_t object, std::uintptr_t vtable) noexcept
{
    const auto first = read<std::uintptr_t>(object);
    return first && *first == vtable;
}
}
