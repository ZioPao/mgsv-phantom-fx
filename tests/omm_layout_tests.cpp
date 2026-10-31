#include "passes/ray_tracing/omm_layout.hpp"
#include "passes/ray_tracing/shaders/gi_omm.h"
#include "passes/ray_tracing/shaders/gi_shared.h"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
using namespace phantom_fx;

void expect(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

// Two halves of exact binary fractions (0, 1/2^k, 1): their bits are the float's top bits.
std::uint16_t halfOf(float value)
{
    if (value == 0.0f) return 0;
    int exponent = 0;
    while (value < 1.0f)
    {
        value *= 2.0f;
        --exponent;
    }
    while (value >= 2.0f)
    {
        value *= 0.5f;
        ++exponent;
    }
    const auto mantissa = static_cast<std::uint16_t>((value - 1.0f) * 1024.0f);
    return static_cast<std::uint16_t>(((exponent + 15) << 10) | mantissa);
}

std::uint32_t uv(float u, float v)
{
    return halfOf(u) | (std::uint32_t{halfOf(v)} << 16);
}

// ---- The curve (shaders/gi_omm.h) ----

// Every cell of every level is one micro-triangle, and they cover the curve once.
void cellsCoverTheCurveOnce()
{
    for (std::uint32_t level = 0; level <= omm_layout::MaxLevel; ++level)
    {
        const std::uint32_t count = 1u << (2 * level);
        std::vector<bool> seen(count);
        for (std::uint32_t cell = 0; cell < count; ++cell)
        {
            float u, v;
            const auto index = gi::OmmCellMicroTriangle(cell, level, u, v);
            expect(index < count, "level " + std::to_string(level) + ": index out of range");
            expect(!seen[index], "level " + std::to_string(level) + ": index " + std::to_string(index) + " twice");
            seen[index] = true;
            expect(u > 0.0f && v > 0.0f && u + v < 1.0f, "centroid outside the triangle");
        }
    }
}

// The specs' order at level 1: nearest vertex 0, the middle one, nearest vertex 1, nearest vertex 2.
void levelOneFollowsTheSpec()
{
    expect(gi::OmmIndex(0.1f, 0.1f, 1) == 0, "vertex 0's corner is not first");
    expect(gi::OmmIndex(0.4f, 0.4f, 1) == 1, "the middle is not second");
    expect(gi::OmmIndex(0.8f, 0.1f, 1) == 2, "vertex 1's corner is not third");
    expect(gi::OmmIndex(0.1f, 0.8f, 1) == 3, "vertex 2's corner is not fourth");
    expect(gi::OmmIndex(0.3f, 0.3f, 0) == 0, "level 0 has one micro-triangle");
}

// The curve is hierarchical: a point's index at one level is its index a level up, times four, plus
// its place among that micro-triangle's four children.
void curveIsHierarchical()
{
    for (std::uint32_t level = 1; level <= omm_layout::MaxLevel; ++level)
        for (std::uint32_t cell = 0; cell < (1u << (2 * level)); ++cell)
        {
            float u, v;
            const auto index = gi::OmmCellMicroTriangle(cell, level, u, v);
            expect(index >> 2 == gi::OmmIndex(u, v, level - 1),
                "level " + std::to_string(level) + ", cell " + std::to_string(cell) + ": its parent is not a level up");
        }
}

// ---- The layout ----

void geometriesThatTakeMicromaps()
{
    const std::uint32_t cutOut = GI_GEOMETRY_CUT_OUT | GI_GEOMETRY_ALPHA_TEXTURE | GI_GEOMETRY_UV;
    expect(omm_layout::takesMicromaps(cutOut), "a textured cut-out mesh takes none");
    expect(omm_layout::takesMicromaps(cutOut | GI_GEOMETRY_LEAF), "leaves take none");
    expect(!omm_layout::takesMicromaps(cutOut | GI_GEOMETRY_HAIR), "hair takes them");
    expect(!omm_layout::takesMicromaps(GI_GEOMETRY_CUT_OUT | GI_GEOMETRY_UV), "an untextured cut-out mesh takes them");
    expect(!omm_layout::takesMicromaps(GI_GEOMETRY_ALPHA_TEXTURE | GI_GEOMETRY_UV), "a solid mesh takes them");
    expect(!omm_layout::takesMicromaps(GI_GEOMETRY_CUT_OUT | GI_GEOMETRY_ALPHA_TEXTURE), "a mesh without coordinates takes them");
}

void halvesDecode()
{
    expect(omm_layout::halfToFloat(0x3C00) == 1.0f, "1");
    expect(omm_layout::halfToFloat(0x3800) == 0.5f, "0.5");
    expect(omm_layout::halfToFloat(0xC000) == -2.0f, "-2");
    expect(omm_layout::halfToFloat(0x0001) == 5.9604645e-8f, "the smallest subnormal");
    expect(omm_layout::halfToFloat(0x0000) == 0.0f, "0");
}

void levelsFollowTexels()
{
    // Half a 512 x 512 texture: 131,072 texels, 65,536 micro-triangles at two texels each, level 8,
    // capped at 6.
    expect(omm_layout::levelOf(uv(0, 0), uv(1, 0), uv(0, 1), 512, 512) == omm_layout::MaxLevel, "a large triangle is not capped");
    // 1/32 x 1/32 of it, halved: 128 texels, 64 micro-triangles, level 3.
    expect(omm_layout::levelOf(uv(0, 0), uv(1.0f / 32, 0), uv(0, 1.0f / 32), 512, 512) == 3, "128 texels is not level 3");
    // 8 texels: 4 micro-triangles, level 1; 6 texels, under 4, level 0.
    expect(omm_layout::levelOf(uv(0, 0), uv(1.0f / 128, 0), uv(0, 1.0f / 128), 512, 512) == 1, "8 texels is not level 1");
    expect(omm_layout::levelOf(uv(0, 0), uv(1.0f / 128, 0), uv(0, 1.0f / 256), 512, 512) == 0, "4 texels is not level 0");
    expect(omm_layout::levelOf(uv(0.5f, 0.5f), uv(0.5f, 0.5f), uv(0.5f, 0.5f), 512, 512) == 0, "a degenerate triangle is not level 0");
    expect(omm_layout::levelOf(uv(0, 0), uv(0, 1), uv(1, 0), 512, 512) == omm_layout::MaxLevel, "winding changes the level");
}

void blockSizes()
{
    expect(omm_layout::blockBytes(0) == 4, "level 0 is not padded to a word");
    expect(omm_layout::blockBytes(1) == 4, "level 1 is not padded to a word");
    expect(omm_layout::blockBytes(2) == 4, "level 2: 16 states, 32 bits");
    expect(omm_layout::blockBytes(3) == 16, "level 3: 64 states, 128 bits");
    expect(omm_layout::blockBytes(6) == 1024, "level 6: 4096 states, 1 KB");
}

// A grass bank: many cards on the same coordinates, one block.
void sharedCoordinatesShareABlock()
{
    std::vector<std::uint32_t> uvs;
    std::vector<std::uint16_t> indices;
    for (std::uint16_t card = 0; card < 50; ++card)
    {
        const auto first = static_cast<std::uint16_t>(uvs.size());
        uvs.insert(uvs.end(), {uv(0, 0), uv(1, 0), uv(0, 1)});
        indices.insert(indices.end(), {first, static_cast<std::uint16_t>(first + 1), static_cast<std::uint16_t>(first + 2)});
    }

    const auto layout = omm_layout::layOut({uvs, indices, 64, 64, 1u << 20});
    expect(layout.has_value(), "no layout");
    expect(layout->blocks.size() == 1, "50 identical cards made " + std::to_string(layout->blocks.size()) + " blocks");
    for (const auto block : layout->triangleBlocks) expect(block == 0, "a card is not on the shared block");
}

// Another vertex order is another mapping of barycentrics to the texture: no sharing.
void otherOrderIsAnotherBlock()
{
    const std::vector<std::uint32_t> uvs{uv(0, 0), uv(1, 0), uv(0, 1)};
    const std::vector<std::uint16_t> indices{0, 1, 2, 1, 2, 0};
    const auto layout = omm_layout::layOut({uvs, indices, 64, 64, 1u << 20});
    expect(layout && layout->blocks.size() == 2, "a rotated triangle shares its block");
}

// Blocks finest first, their offsets in order without overlap, the histogram and size adding up.
void blocksArePacked()
{
    // Three triangles: level 6, level 3 and level 0 on a 512 x 512 texture.
    const std::vector<std::uint32_t> uvs{uv(0, 0), uv(1, 0), uv(0, 1), uv(1.0f / 32, 0), uv(0, 1.0f / 32), uv(1.0f / 256, 0), uv(0, 1.0f / 256)};
    const std::vector<std::uint16_t> indices{0, 5, 6, 0, 3, 4, 0, 1, 2};
    const auto layout = omm_layout::layOut({uvs, indices, 512, 512, 1u << 20});
    expect(layout && layout->blocks.size() == 3, "three triangles did not make three blocks");

    const auto& blocks = layout->blocks;
    expect(blocks[0].level == 6 && blocks[1].level == 3 && blocks[2].level == 0, "blocks are not finest first");
    expect(layout->triangleBlocks[0] == 2 && layout->triangleBlocks[1] == 1 && layout->triangleBlocks[2] == 0,
        "triangles do not follow their blocks into the sorted order");

    std::uint64_t expected = 0;
    for (const auto& block : blocks)
    {
        expect(block.byteOffset == expected, "a block is not right after the last");
        expect(block.byteOffset % omm_layout::BlockAlignment == 0, "a block is not word aligned");
        expected += omm_layout::blockBytes(block.level);
    }
    expect(layout->bytes == expected, "the size is not the blocks' sum");
    expect(layout->bytes == 1024 + 16 + 4, "the size is not 1044 bytes");

    std::uint32_t counted = 0;
    for (const auto count : layout->histogram) counted += count;
    expect(counted == blocks.size(), "the histogram does not count every block");
    expect(layout->histogram[6] == 1 && layout->histogram[3] == 1 && layout->histogram[0] == 1, "the histogram's levels are wrong");
    expect(layout->levelsDropped == 0, "levels dropped with room to spare");
}

// Over budget, every level drops until it fits; at level 0 and still over, no layout.
void budgetLowersLevels()
{
    const std::vector<std::uint32_t> uvs{uv(0, 0), uv(1, 0), uv(0, 1), uv(1, 1)};
    const std::vector<std::uint16_t> indices{0, 1, 2, 1, 3, 2};

    const auto fits = omm_layout::layOut({uvs, indices, 512, 512, 2 * 1024});
    expect(fits && fits->levelsDropped == 0, "two level-6 blocks in 2 KB dropped");

    const auto dropped = omm_layout::layOut({uvs, indices, 512, 512, 600});
    expect(dropped && dropped->levelsDropped == 1, "two level-6 blocks in 600 bytes did not drop to level 5");
    expect(dropped->blocks[0].level == 5 && dropped->bytes == 512, "the dropped blocks are not level 5");

    expect(!omm_layout::layOut({uvs, indices, 512, 512, 4}), "two blocks fit in 4 bytes");
}

void unusableMeshes()
{
    const std::vector<std::uint32_t> uvs{uv(0, 0), uv(1, 0), uv(0, 1)};
    const std::vector<std::uint16_t> outOfRange{0, 1, 3};
    expect(!omm_layout::layOut({uvs, outOfRange, 64, 64, 1u << 20}), "an index past the vertices gave a layout");
    expect(!omm_layout::layOut({uvs, {}, 64, 64, 1u << 20}), "no triangles gave a layout");
}
}

int main()
{
    try
    {
        cellsCoverTheCurveOnce();
        levelOneFollowsTheSpec();
        curveIsHierarchical();
        geometriesThatTakeMicromaps();
        halvesDecode();
        levelsFollowTexels();
        blockSizes();
        sharedCoordinatesShareABlock();
        otherOrderIsAnotherBlock();
        blocksArePacked();
        budgetLowersLevels();
        unusableMeshes();
        std::cout << "Opacity micromap layout tests passed.\n";

        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';

        return 1;
    }
}
