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

// Writes the game's own memory, for the settings the add-on takes over: false when the address
// cannot be written.
inline bool writeRaw(std::uintptr_t to, const void* from, std::size_t size) noexcept
{
    if (to < 0x10000 || to > 0x7FFFFFFF0000ull || size > 0x7FFFFFFF0000ull - to) return false;

    __try
    {
        std::memcpy(reinterpret_cast<void*>(to), from, size);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

template<class T> bool write(std::uintptr_t to, const T& value) noexcept
{
    return writeRaw(to, &value, sizeof(T));
}

// Whether an object's first word is this vtable (an address in the game's image).
inline bool isA(std::uintptr_t object, std::uintptr_t vtable) noexcept
{
    const auto first = read<std::uintptr_t>(object);
    return first && *first == vtable;
}
}
