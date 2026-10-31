#pragma once

#include "log.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>

// A ring of the last GPU operations the add-on recorded, dumped when a frame never finishes (the fence's
// timeout) or the device is lost. PIX cannot run this game, so this stands in for its event list: the last
// entry is the operation the GPU was in when it stopped finishing frames. Off unless PHANTOM_FX_TRACE is
// set, since it costs a string an operation.
namespace phantom_fx::trace
{
inline constexpr std::size_t Entries = 1024;

struct Ring
{
    std::mutex mutex;
    std::array<std::string, Entries> lines;
    std::size_t written{};
    bool on = std::getenv("PHANTOM_FX_TRACE") != nullptr;
};

inline Ring& ring()
{
    static Ring instance;
    return instance;
}

inline void add(std::string line)
{
    auto& r = ring();
    if (!r.on) return;

    std::lock_guard lock(r.mutex);
    r.lines[r.written % Entries] = std::move(line);
    ++r.written;
}

// The last `count` operations, oldest first, one log line each.
inline void dump(std::size_t count = 64)
{
    auto& r = ring();
    if (!r.on) return;

    std::lock_guard lock(r.mutex);
    const auto first = r.written > count ? r.written - count : 0;
    for (auto i = first; i < r.written; ++i)
        log("trace " + std::to_string(i) + ": " + r.lines[i % Entries]);
}
}
