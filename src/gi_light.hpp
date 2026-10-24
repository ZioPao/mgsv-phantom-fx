#pragma once

#include "gi_d3d12.hpp"
#include "shaders/gi_shared.h"

#include <d3d12.h>
#include <cstdint>

// GI-1.2's passes on the global illumination's Direct3D 12 device (docs/DEVELOPMENT.md, "Global
// illumination", "Light"): screen probes, the two-level hash grid cache and the denoiser, with
// their memory at one render size (shaders/gi_light.hlsl).
namespace phantom_fx::gi_light
{
class Light
{
public:
    // Makes the memory for a render size. Throws on Direct3D 12 errors.
    Light(ID3D12Device* device, UINT width, UINT height);

    UINT width() const { return renderWidth; }
    UINT height() const { return renderHeight; }

    // The probe and hash grid constants; `verticalFov` in radians.
    void fill(gi::GIFrameConstants& constants, float verticalFov, std::uint32_t parity) const;
    // Its views in the frame's tables, by the slots shaders/gi_light.hlsl gives them.
    void describe(gi_d3d12::Descriptors& descriptors, std::uint32_t parity) const;
    // The passes, from the probes to the light in the output (u0). `depth` and `normal` are the
    // frame's shared inputs in the non-pixel shader resource state, which they are left in.
    void record(gi_d3d12::Recorder& recorder, std::uint32_t parity, ID3D12Resource* depth, ID3D12Resource* normal);

private:
    UINT renderWidth, renderHeight;
    UINT probesAcross, probesDown, maskMips;
    std::uint32_t probeCount, spawnCount, rayCount;
    bool fresh{true}; // Nothing in its memory yet: the first frame starts it

    gi_d3d12::ComPtr<ID3D12Resource> probes[2], masks[2], probeSH[2], spawns[2], spawnScan, spawnIndex, spawnProbe, spawnSample,
        spawnRadiance, emptyTiles, overrideTiles, cachedTiles, cachedTileIndex, lru[2], lruFlags, lruIndex, mru, list, listCount,
        listIndex, listElements, probeChange;
    gi_d3d12::ComPtr<ID3D12Resource> hash, decay, values, indirectValues, updateTiles, updateValues, updateIndirectValues, visibility,
        visibilityCells, visibilityQueries, visibilityRadiance, multibounce, lights, packedTiles[2];
    gi_d3d12::ComPtr<ID3D12Resource> blurMask, colors[2], colorDeltas[2], previousDepth, previousNormal;
    gi_d3d12::ComPtr<ID3D12Resource> scanInput, scanOutput, scanBlockSums;
};

// The hash grid's size: 2^12 buckets of 16 tiles, tiles of 8 x 8 cells and three smaller mips.
constexpr std::uint32_t HashBuckets = 1u << 12, HashTilesPerBucket = 1u << 4, HashTileCellRatio = 8;
}
