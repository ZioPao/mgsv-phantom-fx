// GI-1.2's passes; gi_light.hpp describes them. The order follows AMD Capsaicin's GI1::render
// (gi1.cpp), without its glossy reflections, world-space ReSTIR and debug passes.
#include "gi_light.hpp"

#include <algorithm>
#include <bit>
#include <cmath>

namespace phantom_fx::gi_light
{
namespace
{
using gi_d3d12::Kernel;

constexpr UINT ProbeSize = 8, SpawnTileSize = 16;
constexpr std::uint32_t HashTiles = HashBuckets * HashTilesPerBucket;
constexpr std::uint32_t CellsPerTile = HashTileCellRatio * HashTileCellRatio + 16 + 4 + 1; // Mips 0 to 3
constexpr std::uint32_t HashCells = HashTiles * CellsPerTile;
// Capsaicin's defaults: cells of 32 pixels on screen, no smaller than 10 cm, 16 samples a cell, and
// 70% of the multibounce rays left out.
constexpr float HashCellPixels = 32, HashMinCellSize = 0.1f, MaxSamples = 16, DiscardMultibounce = 0.7f;
// Hits a cell takes its light from each frame (shaders/gi_light.hlsl, HashGridCache_QueueLight).
constexpr std::uint32_t CellSamplesPerFrame = 4;

std::uint32_t groups(std::uint64_t threads, std::uint32_t size)
{
    return static_cast<std::uint32_t>((threads + size - 1) / size);
}

D3D12_RESOURCE_STATES storage()
{
    return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
}
}

Light::Light(ID3D12Device* device, UINT width, UINT height) : renderWidth(width), renderHeight(height)
{
    probesAcross = (width + ProbeSize - 1) / ProbeSize;
    probesDown = (height + ProbeSize - 1) / ProbeSize;
    maskMips = static_cast<UINT>(std::bit_width(std::max(probesAcross, probesDown)));
    maskMips = std::min(maskMips, 12u);
    probeCount = probesAcross * probesDown;
    spawnCount = ((width + SpawnTileSize - 1) / SpawnTileSize) * ((height + SpawnTileSize - 1) / SpawnTileSize);
    rayCount = spawnCount * ProbeSize * ProbeSize;

    const auto texture = [&](UINT w, UINT h, DXGI_FORMAT format, UINT16 mips, const char* what) {
        return gi_d3d12::createTexture(device, w, h, format, mips, storage(), what);
    };
    const auto buffer = [&](std::uint64_t bytes, const char* what) { return gi_d3d12::createStorage(device, bytes, what); };

    for (int i = 0; i < 2; ++i)
    {
        probes[i] = texture(probesAcross * ProbeSize, probesDown * ProbeSize, DXGI_FORMAT_R16G16B16A16_FLOAT, 1, "Creating the screen probes");
        masks[i] = texture(probesAcross, probesDown, DXGI_FORMAT_R32_UINT, static_cast<UINT16>(maskMips), "Creating the probe mask");
        probeSH[i] = buffer(std::uint64_t{probeCount} * 9 * 8, "Creating the probes' harmonics");
        spawns[i] = buffer(std::uint64_t{spawnCount} * 4, "Creating the probe spawns");
        lru[i] = buffer(std::uint64_t{probeCount} * 4, "Creating the cached probes' order");
        packedTiles[i] = buffer(std::uint64_t{HashTiles} * 4, "Creating the packed tiles");
        colors[i] = texture(width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, 1, "Creating the denoiser's colour");
        colorDeltas[i] = texture(width, height, DXGI_FORMAT_R16_FLOAT, 1, "Creating the denoiser's colour change");
    }

    spawnIndex = buffer(std::uint64_t{spawnCount} * 4, "Creating the spawn index");
    spawnProbe = buffer(std::uint64_t{spawnCount} * 8, "Creating the spawned probes");
    spawnSample = buffer(std::uint64_t{rayCount} * 8, "Creating the probe samples");
    spawnRadiance = buffer(std::uint64_t{rayCount} * 8, "Creating the probe radiance");
    emptyTiles = buffer(std::uint64_t{probeCount} * 4, "Creating the empty tiles");
    overrideTiles = buffer(std::uint64_t{spawnCount} * 4, "Creating the override tiles");
    cachedTiles = texture(probesAcross * ProbeSize, probesDown * ProbeSize, DXGI_FORMAT_R16G16B16A16_FLOAT, 1, "Creating the cached probes");
    cachedTileIndex = texture(probesAcross, probesDown, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, "Creating the cached probes' places");
    lruFlags = buffer(std::uint64_t{probeCount + 1} * 4, "Creating the cached probes' flags");
    lruIndex = buffer(std::uint64_t{probeCount + 1} * 4, "Creating the cached probes' index");
    mru = buffer(std::uint64_t{probeCount} * 4, "Creating the recently used probes");
    list = buffer(std::uint64_t{probeCount} * 4, "Creating the cached probe list");
    listCount = buffer(std::uint64_t{probeCount} * 4, "Creating the cached probe counts");
    listIndex = buffer(std::uint64_t{probeCount} * 4, "Creating the cached probe offsets");
    listElements = buffer(std::uint64_t{probeCount} * 16, "Creating the cached probe elements");

    hash = buffer(std::uint64_t{HashTiles} * 4, "Creating the hash grid");
    decay = buffer(std::uint64_t{HashTiles} * 4, "Creating the hash grid's ages");
    values = buffer(std::uint64_t{HashCells} * 8, "Creating the hash grid's light");
    indirectValues = buffer(std::uint64_t{HashCells} * 8, "Creating the hash grid's indirect light");
    updateTiles = buffer(std::uint64_t{HashTiles} * 4, "Creating the updated tiles");
    updateValues = buffer(std::uint64_t{HashCells} * 16, "Creating the hash grid's new light");
    updateIndirectValues = buffer(std::uint64_t{HashCells} * 16, "Creating the hash grid's new indirect light");
    visibility = buffer(std::uint64_t{rayCount} * 2 * 16, "Creating the hits");
    visibilityCells = buffer(std::uint64_t{rayCount} * 2 * 4, "Creating the hits' cells");
    visibilityQueries = buffer(std::uint64_t{rayCount} * 2 * 4, "Creating the hits' rays");
    visibilityRadiance = buffer(std::uint64_t{rayCount} * 8, "Creating the hits' light");
    multibounce = buffer(std::uint64_t{rayCount} * 8, "Creating the multibounce weights");
    lights = buffer(std::uint64_t{rayCount} * 2 * 4, "Creating the hits to light");

    blurMask = texture(width, height, DXGI_FORMAT_R16_FLOAT, 1, "Creating the denoiser's blur");
    previousDepth = gi_d3d12::createTexture(device, width, height, DXGI_FORMAT_R32_FLOAT, 1, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        "Creating the last depth");
    previousNormal = gi_d3d12::createTexture(device, width, height, DXGI_FORMAT_R10G10B10A2_UNORM, 1,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "Creating the last normals");

    const auto scanSize = std::max<std::uint64_t>(probeCount + 1, spawnCount);
    scanInput = buffer(scanSize * 4, "Creating the scan input");
    scanOutput = buffer(scanSize * 4, "Creating the scan output");
    scanBlockSums = buffer(1024 * 4, "Creating the scan's block sums");
}

void Light::fill(gi::GIFrameConstants& constants, float verticalFov, std::uint32_t parity) const
{
    const float width = static_cast<float>(renderWidth), height = static_cast<float>(renderHeight);
    const float scale = std::max(1.0f / height, height / (width * width));
    const float hashCell = std::tan(verticalFov * HashCellPixels * scale);
    constants.probeLayout = {ProbeSize, SpawnTileSize, probesAcross, probesDown};
    constants.probeLimits = {maskMips, spawnCount, rayCount, parity};
    constants.cellSizes = {std::tan(verticalFov * ProbeSize * scale), hashCell, HashMinCellSize, hashCell * HashTileCellRatio};
    constants.hashCounts = {HashBuckets, HashTiles, HashCells, HashTilesPerBucket};
    constants.hashTileSizes = {HashTileCellRatio, HashTileCellRatio / 2, HashTileCellRatio / 4, HashTileCellRatio / 8};
    constants.hashTileCells = {64, 16, 4, 1};
    constants.hashTileFirstCells = {0, 64, 80, 84};
    constants.hashMisc = {CellsPerTile, parity, CellSamplesPerFrame, 0};
    constants.hashSamples = {MaxSamples, DiscardMultibounce, MaxSamples, static_cast<float>(HashTileCellRatio)};
    constants.counts.w = fresh ? 1 : 0;
}

void Light::describe(gi_d3d12::Descriptors& d, std::uint32_t parity) const
{
    const auto now = parity, last = 1 - parity;
    d.texture(7, previousDepth.Get(), DXGI_FORMAT_R32_FLOAT);
    d.texture(8, previousNormal.Get(), DXGI_FORMAT_R10G10B10A2_UNORM);
    d.texture(9, masks[now].Get(), DXGI_FORMAT_R32_UINT, maskMips);

    d.outputTexture(7, probes[now].Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
    d.outputTexture(8, probes[last].Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
    d.outputTexture(9, masks[now].Get(), DXGI_FORMAT_R32_UINT);
    d.outputTexture(10, masks[last].Get(), DXGI_FORMAT_R32_UINT);
    d.outputBuffer(11, probeSH[now].Get(), 8, std::uint64_t{probeCount} * 9);
    d.outputBuffer(12, probeSH[last].Get(), 8, std::uint64_t{probeCount} * 9);
    d.outputBuffer(13, spawns[0].Get(), 4, spawnCount);
    d.outputBuffer(14, spawns[1].Get(), 4, spawnCount);
    d.outputBuffer(16, spawnIndex.Get(), 4, spawnCount);
    d.outputBuffer(17, spawnProbe.Get(), 8, spawnCount);
    d.outputBuffer(18, spawnSample.Get(), 8, rayCount);
    d.outputBuffer(19, spawnRadiance.Get(), 8, rayCount);
    d.outputBuffer(20, emptyTiles.Get(), 4, probeCount);
    d.outputBuffer(21, overrideTiles.Get(), 4, spawnCount);
    d.outputTexture(22, cachedTiles.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
    d.outputTexture(23, cachedTileIndex.Get(), DXGI_FORMAT_R32G32B32A32_FLOAT);
    d.outputBuffer(24, lru[now].Get(), 4, probeCount);
    d.outputBuffer(25, lru[last].Get(), 4, probeCount);
    d.outputBuffer(26, lruFlags.Get(), 4, probeCount + 1);
    d.outputBuffer(27, lruIndex.Get(), 4, probeCount + 1);
    d.outputBuffer(28, mru.Get(), 4, probeCount);
    d.outputBuffer(29, list.Get(), 4, probeCount);
    d.outputBuffer(30, listCount.Get(), 4, probeCount);
    d.outputBuffer(31, listIndex.Get(), 4, probeCount);
    d.outputBuffer(32, listElements.Get(), 16, probeCount);
    d.outputBuffer(33, hash.Get(), 4, HashTiles);
    d.outputBuffer(34, decay.Get(), 4, HashTiles);
    d.outputBuffer(35, values.Get(), 8, HashCells);
    d.outputBuffer(36, indirectValues.Get(), 8, HashCells);
    d.outputBuffer(37, updateTiles.Get(), 4, HashTiles);
    d.outputBuffer(38, updateValues.Get(), 4, std::uint64_t{HashCells} * 4);
    d.outputBuffer(39, updateIndirectValues.Get(), 4, std::uint64_t{HashCells} * 4);
    d.outputBuffer(40, visibility.Get(), 16, std::uint64_t{rayCount} * 2);
    d.outputBuffer(41, visibilityCells.Get(), 4, std::uint64_t{rayCount} * 2);
    d.outputBuffer(42, visibilityQueries.Get(), 4, std::uint64_t{rayCount} * 2);
    d.outputBuffer(43, visibilityRadiance.Get(), 8, rayCount);
    d.outputBuffer(44, multibounce.Get(), 8, rayCount);
    d.outputBuffer(45, packedTiles[now].Get(), 4, HashTiles);
    d.outputBuffer(46, packedTiles[last].Get(), 4, HashTiles);
    d.outputTexture(47, blurMask.Get(), DXGI_FORMAT_R16_FLOAT);
    d.outputTexture(48, colors[now].Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
    d.outputTexture(49, colors[last].Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
    d.outputTexture(50, colorDeltas[now].Get(), DXGI_FORMAT_R16_FLOAT);
    d.outputTexture(51, colorDeltas[last].Get(), DXGI_FORMAT_R16_FLOAT);
    d.outputTexture(52, probes[now].Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
    d.outputTexture(53, probes[last].Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
    const auto scanSize = std::max<std::uint64_t>(probeCount + 1, spawnCount);
    d.outputBuffer(54, scanInput.Get(), 4, scanSize);
    d.outputBuffer(55, scanOutput.Get(), 4, scanSize);
    d.outputBuffer(56, scanBlockSums.Get(), 4, 1024);
    d.outputBuffer(57, lights.Get(), 4, std::uint64_t{rayCount} * 2);
    for (UINT mip = 0; mip < maskMips; ++mip) d.outputTexture(64 + mip, masks[now].Get(), DXGI_FORMAT_R32_UINT, mip);
}

void Light::record(gi_d3d12::Recorder& r, std::uint32_t parity, ID3D12Resource* depth, ID3D12Resource* normal)
{
    const auto scan = [&](std::uint32_t count) {
        r.dispatch(Kernel::ScanBlocks, groups(count, 1024), 1, {count});
        r.barrier();
        r.dispatch(Kernel::ScanBlockSums, 1, 1, {count});
        r.barrier();
        r.dispatch(Kernel::ScanAddOffsets, groups(count, 1024), 1, {count});
        r.barrier();
    };
    const auto mipSize = [](UINT size, UINT mip) { return std::max(size >> mip, 1u); };
    const auto pixels = [&](Kernel kernel, std::array<std::uint32_t, 4> parameters = {}) {
        r.dispatch(kernel, groups(renderWidth, 8), groups(renderHeight, 8), parameters);
        r.barrier();
    };

    // A fresh start: no probe yet, every cached probe free.
    if (fresh)
    {
        for (UINT mip = 0; mip < maskMips; ++mip)
            r.dispatch(Kernel::ClearProbeMask, groups(mipSize(probesAcross, mip), 8), groups(mipSize(probesDown, mip), 8), {mip});

        r.dispatch(Kernel::InitCachedTileLRU, groups(probeCount, 64));
        r.barrier();
    }

    r.mark("probe reprojection and spawning");
    r.dispatch(Kernel::ClearCounters, 1);
    r.barrier();
    // Tiles nothing touched for 50 frames leave the cache.
    r.indirect(Kernel::PurgeTiles, GI_COUNTER_PACKED_TILES + 1 - parity, 64);
    r.dispatch(Kernel::ReprojectScreenProbes, probesAcross, probesDown);
    r.barrier();

    // The cached probes that fall on each probe tile.
    r.dispatch(Kernel::ClearBuffer, groups(probeCount, 64), 1, {probeCount, 0, 0});
    r.barrier();
    r.dispatch(Kernel::CountScreenProbes, groups(probeCount, 64));
    r.barrier();
    r.copyBuffer(scanInput.Get(), listCount.Get(), std::uint64_t{probeCount} * 4);
    scan(probeCount);
    r.copyBuffer(listIndex.Get(), scanOutput.Get(), std::uint64_t{probeCount} * 4);
    r.indirect(Kernel::ScatterScreenProbes, GI_COUNTER_LIST_ELEMENTS, 64);

    // New probes, one a spawn tile, and empty tiles taking the place of some.
    r.dispatch(Kernel::SpawnScreenProbes, groups(spawnCount, 64));
    r.barrier();
    scan(spawnCount);
    r.dispatch(Kernel::CompactScreenProbes, groups(spawnCount, 64));
    r.barrier();
    r.dispatch(Kernel::StoreSpawnCount, 1);
    r.barrier();
    r.indirect(Kernel::PatchScreenProbes, GI_COUNTER_EMPTY_TILES, 64);

    // The probes' rays: guided by last frame's light, traced into the hash grid cache.
    r.mark("probe ray guiding");
    r.dispatch(Kernel::SampleScreenProbes, spawnCount);
    r.barrier();
    r.dispatch(Kernel::LearnAlbedo, groups(spawnCount, 64));
    r.barrier();
    r.mark("probe rays");
    r.dispatch(Kernel::PopulateScreenProbes, groups(rayCount, 32));
    r.barrier();
    r.mark("multibounce rays");
    r.indirect(Kernel::PopulateMultibounceCells, GI_COUNTER_VISIBILITY0, 32);
    r.mark("hit lighting (shadow rays)");
    r.indirect(Kernel::PopulateCells, GI_COUNTER_LIGHTS, 32);
    r.mark("cache update");
    r.indirect(Kernel::UpdateTiles, GI_COUNTER_UPDATE_TILES, 1);
    r.indirect(Kernel::UpdateMultibounceCells, GI_COUNTER_VISIBILITY1, 32);
    r.indirect(Kernel::ResolveCells, GI_COUNTER_VISIBILITY0, 64);

    // Into the probes, with the cached ones kept in order of use.
    r.mark("probe blending and filtering");
    r.dispatch(Kernel::BlendScreenProbes, spawnCount);
    r.barrier();
    r.dispatch(Kernel::CopyProbes, probesAcross, probesDown);
    r.barrier();
    r.copyBuffer(scanInput.Get(), lruFlags.Get(), std::uint64_t{probeCount + 1} * 4);
    scan(probeCount + 1);
    r.dispatch(Kernel::ReorderScreenProbes, groups(probeCount, 64));
    r.barrier();

    // The probe mask's mips, for finding probes far away; read through its view from here on.
    for (UINT mip = 1; mip < maskMips; ++mip)
    {
        r.dispatch(Kernel::FilterProbeMask, groups(mipSize(probesAcross, mip), 8), groups(mipSize(probesDown, mip), 8), {mip});
        r.barrier();
    }

    r.transition(masks[parity].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    // Across, then down: pair entry 1 is written from 0, then 0 from 1.
    r.dispatch(Kernel::FilterScreenProbes, spawnCount, 1, {1, 0, 1, 0});
    r.barrier();
    r.dispatch(Kernel::FilterScreenProbes, spawnCount, 1, {0, 1, 0, 1});
    r.barrier();
    r.dispatch(Kernel::ProjectScreenProbes, spawnCount);
    r.barrier();
    r.mark("interpolation");
    pixels(Kernel::InterpolateScreenProbes, {0});
    r.transition(masks[parity].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // The denoiser: this frame's light with the last, then a blur where history is short.
    r.mark("denoiser");
    pixels(Kernel::ReprojectGI, {0, 1});
    pixels(Kernel::FilterGI, {1, 0, 0, 0});
    pixels(Kernel::FilterGI, {0, 1, 1, 1});

    // This frame's depth and normals for the next.
    r.mark("copies for the next frame");
    auto& commands = r.commands();
    D3D12_RESOURCE_BARRIER copy[]{
        gi_d3d12::transition(depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE),
        gi_d3d12::transition(normal, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE),
        gi_d3d12::transition(previousDepth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
        gi_d3d12::transition(previousNormal.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
    };
    commands.ResourceBarrier(static_cast<UINT>(std::size(copy)), copy);
    commands.CopyResource(previousDepth.Get(), depth);
    commands.CopyResource(previousNormal.Get(), normal);
    for (auto& barrier : copy) std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    commands.ResourceBarrier(static_cast<UINT>(std::size(copy)), copy);

    fresh = false;
}
}
