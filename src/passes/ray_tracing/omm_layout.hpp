#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// Where a cut-out mesh's opacity micromaps go (docs/opacity-micromaps.md, "Baking"): a subdivision level
// for each triangle from how many texels it covers, triangles with the same texture coordinates sharing
// a block, and the blocks' places in the micromap array's input. The bake kernel fills the blocks'
// states; nothing here reads a texel.
namespace phantom_fx::omm_layout
{
// About this many texels to a micro-triangle.
constexpr float TexelsPerMicroTriangle = 2.0f;

// Every block starts on a 4-byte boundary, so a block owns whole words and the bake kernel writes one
// block's words without touching another's. (128 was tried for the in-game hang; it changed nothing and
// cost 31 bytes a block.)
constexpr std::uint32_t BlockAlignment = 4;

// The finest level the bake makes (GI_OMM_MAX_LEVEL in shaders/gi_omm.h).
constexpr std::uint32_t MaxLevel = 6;

// A mesh as the scene stores it: its vertices' texture coordinates (two halves, u in the low 16 bits),
// three indices a triangle, and the size of the texture whose alpha cuts it.
struct Mesh
{
    std::span<const std::uint32_t> uvs;
    std::span<const std::uint16_t> indices;
    std::uint32_t textureWidth{}, textureHeight{};
    std::uint64_t maxBytes{}; // The most the blocks may take; levels drop until they fit
};

// One micromap: the three texture coordinates it covers (its first triangle's, in its order), its level
// and where its states start in the array's input.
struct Block
{
    std::array<std::uint32_t, 3> uvs{};
    std::uint32_t level{};
    std::uint32_t byteOffset{};
};

struct Layout
{
    std::vector<Block> blocks;                   // Finest first, as the micromap array's descriptors
    std::vector<std::uint32_t> triangleBlocks;   // Each triangle's block: the linkage's index buffer
    std::array<std::uint32_t, MaxLevel + 1> histogram{}; // Blocks at each level, all 4-state
    std::uint64_t bytes{};                       // The array input's size
    std::uint32_t levelsDropped{};               // How far every level was lowered to fit maxBytes
};

// Whether a geometry with these GI_GEOMETRY_* flags gets micromaps: cut out by its texture's alpha,
// with texture coordinates, and not hair, whose rays must see every layer (PassHair).
bool takesMicromaps(std::uint32_t flags);

// A half's value (IEEE 754 binary16).
float halfToFloat(std::uint16_t half);

// The level a triangle with these texture coordinates gets on a texture of this size.
std::uint32_t levelOf(std::uint32_t uv0, std::uint32_t uv1, std::uint32_t uv2, std::uint32_t textureWidth, std::uint32_t textureHeight);

// Bytes a 4-state block of this level takes, BlockAlignment included.
std::uint32_t blockBytes(std::uint32_t level);

// The mesh's layout; nullopt when it has no triangles, an index out of range, or does not fit maxBytes
// even at level 0.
std::optional<Layout> layOut(const Mesh& mesh);

// A structure's meshes' layouts in one micromap array: their blocks one mesh after another, offsets and
// block numbers across the whole array.
struct ArrayLayout
{
    std::vector<Block> blocks;
    std::vector<std::uint32_t> blockMesh;      // Which of the layouts each block came from
    std::vector<std::uint32_t> triangleBlocks; // Every mesh's triangles, in order
    std::vector<std::uint32_t> firstTriangle;  // Each mesh's first in triangleBlocks
    std::array<std::uint32_t, MaxLevel + 1> histogram{};
    std::uint64_t bytes{};
};

ArrayLayout combine(std::span<const Layout> layouts);
}
