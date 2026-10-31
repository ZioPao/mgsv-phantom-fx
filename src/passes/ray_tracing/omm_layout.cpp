// Opacity micromaps' layout; omm_layout.hpp describes it.
#include "passes/ray_tracing/omm_layout.hpp"

#include "passes/ray_tracing/shaders/gi_omm.h"
#include "passes/ray_tracing/shaders/gi_shared.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <unordered_map>

namespace phantom_fx::omm_layout
{
static_assert(MaxLevel == GI_OMM_MAX_LEVEL);

namespace
{
struct Key
{
    std::array<std::uint32_t, 3> uvs;
    std::uint32_t level;

    bool operator==(const Key&) const = default;
};

struct KeyHash
{
    std::size_t operator()(const Key& key) const
    {
        std::uint64_t hash = 0xcbf29ce484222325ull;
        for (const auto value : {key.uvs[0], key.uvs[1], key.uvs[2], key.level})
            hash = (hash ^ value) * 0x100000001b3ull;
        return static_cast<std::size_t>(hash);
    }
};

std::array<float, 2> uvOf(std::uint32_t packed)
{
    return {halfToFloat(static_cast<std::uint16_t>(packed)), halfToFloat(static_cast<std::uint16_t>(packed >> 16))};
}

// The layout with every triangle's level lowered by `drop`.
Layout layOutAt(const Mesh& mesh, const std::vector<std::uint32_t>& levels, std::uint32_t drop)
{
    const auto triangles = mesh.indices.size() / 3;
    Layout layout;
    layout.levelsDropped = drop;
    layout.triangleBlocks.resize(triangles);

    std::unordered_map<Key, std::uint32_t, KeyHash> found;
    for (std::size_t t = 0; t < triangles; ++t)
    {
        const Key key{{mesh.uvs[mesh.indices[3 * t]], mesh.uvs[mesh.indices[3 * t + 1]], mesh.uvs[mesh.indices[3 * t + 2]]},
            levels[t] > drop ? levels[t] - drop : 0u};
        const auto [it, added] = found.try_emplace(key, static_cast<std::uint32_t>(layout.blocks.size()));
        if (added) layout.blocks.push_back({key.uvs, key.level, 0});
        layout.triangleBlocks[t] = it->second;
    }

    // Finest first; a stable order keeps blocks of one level in the order their triangles come.
    std::vector<std::uint32_t> order(layout.blocks.size());
    for (std::uint32_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) { return layout.blocks[a].level > layout.blocks[b].level; });

    std::vector<Block> sorted;
    sorted.reserve(order.size());
    std::vector<std::uint32_t> placeOf(order.size());
    std::uint64_t offset = 0;
    for (std::uint32_t place = 0; place < order.size(); ++place)
    {
        auto block = layout.blocks[order[place]];
        block.byteOffset = static_cast<std::uint32_t>(offset);
        offset += blockBytes(block.level);
        ++layout.histogram[block.level];
        placeOf[order[place]] = place;
        sorted.push_back(block);
    }

    for (auto& block : layout.triangleBlocks) block = placeOf[block];
    layout.blocks = std::move(sorted);
    layout.bytes = offset;
    return layout;
}
}

bool takesMicromaps(std::uint32_t flags)
{
    constexpr std::uint32_t needed = GI_GEOMETRY_CUT_OUT | GI_GEOMETRY_ALPHA_TEXTURE | GI_GEOMETRY_UV;
    return (flags & needed) == needed && !(flags & GI_GEOMETRY_HAIR);
}

float halfToFloat(std::uint16_t half)
{
    const std::uint32_t sign = (half & 0x8000u) << 16;
    const std::uint32_t exponent = (half >> 10) & 0x1Fu;
    std::uint32_t mantissa = half & 0x3FFu;

    std::uint32_t bits;
    if (exponent == 0x1Fu) bits = sign | 0x7F800000u | (mantissa << 13); // Infinity, NaN
    else if (exponent) bits = sign | ((exponent + 112u) << 23) | (mantissa << 13);
    else if (!mantissa) bits = sign;
    else
    {
        // Subnormal: normalise it.
        std::uint32_t shift = 0;
        while (!(mantissa & 0x400u))
        {
            mantissa <<= 1;
            ++shift;
        }
        bits = sign | ((113u - shift) << 23) | ((mantissa & 0x3FFu) << 13);
    }

    return std::bit_cast<float>(bits);
}

std::uint32_t levelOf(std::uint32_t uv0, std::uint32_t uv1, std::uint32_t uv2, std::uint32_t textureWidth, std::uint32_t textureHeight)
{
    const auto a = uvOf(uv0), b = uvOf(uv1), c = uvOf(uv2);
    const float area = 0.5f * std::abs((b[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (b[1] - a[1]));
    const float microTriangles = area * static_cast<float>(textureWidth) * static_cast<float>(textureHeight) / TexelsPerMicroTriangle;
    if (!(microTriangles >= 4.0f)) return 0; // Also NaN from a coordinate that is no number

    // 4^level micro-triangles at most.
    const auto level = static_cast<std::uint32_t>(std::floor(0.5f * std::log2(microTriangles)));
    return std::min(level, MaxLevel);
}

std::uint32_t blockBytes(std::uint32_t level)
{
    const std::uint32_t bits = (1u << (2 * level)) * GI_OMM_STATE_BITS;
    const std::uint32_t bytes = (bits + 7) / 8;
    return (bytes + BlockAlignment - 1) / BlockAlignment * BlockAlignment;
}

std::optional<Layout> layOut(const Mesh& mesh)
{
    const auto triangles = mesh.indices.size() / 3;
    if (!triangles) return std::nullopt;
    for (std::size_t i = 0; i < triangles * 3; ++i)
        if (mesh.indices[i] >= mesh.uvs.size()) return std::nullopt;

    std::vector<std::uint32_t> levels(triangles);
    std::uint32_t finest = 0;
    for (std::size_t t = 0; t < triangles; ++t)
    {
        levels[t] = levelOf(mesh.uvs[mesh.indices[3 * t]], mesh.uvs[mesh.indices[3 * t + 1]], mesh.uvs[mesh.indices[3 * t + 2]], mesh.textureWidth,
            mesh.textureHeight);
        finest = std::max(finest, levels[t]);
    }

    for (std::uint32_t drop = 0; drop <= finest; ++drop)
        if (auto layout = layOutAt(mesh, levels, drop); layout.bytes <= mesh.maxBytes) return layout;

    return std::nullopt;
}

ArrayLayout combine(std::span<const Layout> layouts)
{
    ArrayLayout array;
    for (std::uint32_t mesh = 0; mesh < layouts.size(); ++mesh)
    {
        const auto& layout = layouts[mesh];
        const auto firstBlock = static_cast<std::uint32_t>(array.blocks.size());
        array.firstTriangle.push_back(static_cast<std::uint32_t>(array.triangleBlocks.size()));
        for (auto block : layout.blocks)
        {
            block.byteOffset += static_cast<std::uint32_t>(array.bytes);
            array.blocks.push_back(block);
            array.blockMesh.push_back(mesh);
        }
        for (const auto block : layout.triangleBlocks) array.triangleBlocks.push_back(firstBlock + block);
        for (std::size_t level = 0; level < array.histogram.size(); ++level) array.histogram[level] += layout.histogram[level];
        array.bytes += layout.bytes;
    }

    return array;
}

OpacitySplit splitByOpacity(std::span<const std::uint16_t> indices, std::span<const std::uint32_t> triangleBlocks,
    std::span<const std::uint32_t> blockStates)
{
    OpacitySplit split;
    const auto triangles = indices.size() / 3;
    for (std::size_t t = 0; t < triangles; ++t)
    {
        // A triangle without a block, or a block without its states, keeps the alpha test.
        const auto block = t < triangleBlocks.size() ? triangleBlocks[t] : ~0u;
        const auto found = block < blockStates.size() ? blockStates[block] : 0u;
        const auto corners = indices.subspan(3 * t, 3);
        if (found == GI_OMM_SOME_NOT_TRANSPARENT)
            split.opaque.insert(split.opaque.end(), corners.begin(), corners.end());
        else if (found == GI_OMM_SOME_NOT_OPAQUE)
            ++split.transparent;
        else
            split.mixed.insert(split.mixed.end(), corners.begin(), corners.end());
    }
    return split;
}
}
