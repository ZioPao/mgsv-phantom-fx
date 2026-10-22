#pragma once

#include <cstdint>
#include <iterator>
#include <map>
#include <optional>

namespace phantom_fx
{
// Hands out ranges of [0, capacity), first fit, and merges a released range with its free
// neighbours. The global illumination keeps its captured geometry and acceleration structures in
// a few large buffers this way.
class RangeAllocator
{
public:
    explicit RangeAllocator(std::uint64_t capacity = 0) { reset(capacity); }

    // Frees everything and sets the capacity.
    void reset(std::uint64_t capacity)
    {
        free.clear();
        if (capacity) free.emplace(0, capacity);

        total = capacity;
        used = 0;
    }

    // The start of `size` units at a multiple of `alignment`; nullopt when no free range has room.
    std::optional<std::uint64_t> allocate(std::uint64_t size, std::uint64_t alignment = 1)
    {
        if (!size || !alignment) return std::nullopt;

        for (auto range = free.begin(); range != free.end(); ++range)
        {
            const auto [start, length] = *range;
            const auto aligned = (start + alignment - 1) / alignment * alignment;
            const auto end = start + length;
            if (aligned + size > end) continue;

            free.erase(range);
            if (aligned > start) free.emplace(start, aligned - start);
            if (aligned + size < end) free.emplace(aligned + size, end - aligned - size);
            used += size;

            return aligned;
        }

        return std::nullopt;
    }

    // Gives back a range allocate() returned.
    void release(std::uint64_t offset, std::uint64_t size)
    {
        if (!size) return;

        used -= size;
        auto next = free.lower_bound(offset);
        if (next != free.begin())
        {
            auto previous = std::prev(next);
            if (previous->first + previous->second == offset)
            {
                offset = previous->first;
                size += previous->second;
                free.erase(previous);
            }
        }

        if (next != free.end() && offset + size == next->first)
        {
            size += next->second;
            free.erase(next);
        }

        free.emplace(offset, size);
    }

    std::uint64_t capacity() const { return total; }
    std::uint64_t usedSize() const { return used; }

private:
    std::map<std::uint64_t, std::uint64_t> free; // Start to length
    std::uint64_t total{}, used{};
};
}
