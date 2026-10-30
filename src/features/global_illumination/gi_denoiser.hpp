#pragma once

#include "passes/ray_tracing/gi_d3d12.hpp"

#include <d3d12.h>
#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace nrd
{
struct Instance;
}

// NVIDIA's Real-Time Denoisers (NRD, docs/DEVELOPMENT.md, "Global illumination", "Denoiser") on the
// global illumination's Direct3D 12 device: RELAX's diffuse denoiser in its spherical harmonics form,
// integrated natively, without NVIDIA's NRI. It keeps its own root signature, pipelines, descriptor heap
// and textures; the caller binds its own again afterwards.
namespace phantom_fx::gi_denoiser
{
// The camera as NRD takes it: matrices for column vectors, stored by columns, the projections without
// the upscaler's jitter (as NRD requires). The engine's view mirrors x (P00 < 0); the mirror moves from
// the projection into the view, so the projection is the usual left-handed one and P x V is unchanged
// (as camera::streamlineCamera does).
struct Camera
{
    std::array<float, 16> viewToClip{}, previousViewToClip{}, worldToView{}, previousWorldToView{};
    std::array<float, 2> jitter{}, previousJitter{}; // Where a pixel's sample lies from its centre, in the denoiser's pixels
    bool restart{}; // A camera cut or a fresh start: the history goes
};

// The textures a frame denoises, at the denoiser's size, all in the unordered access state, which
// they are left in: the guides (motion, normal and roughness, view depth), the noisy light's two
// spherical harmonics textures and the denoised ones, and NRD's validation layer when it is wanted.
struct Textures
{
    ID3D12Resource* motion{};          // RGBA16_FLOAT: the last frame's uv minus this one's, and view z's change
    ID3D12Resource* normalRoughness{}; // R10G10B10A2_UNORM, NRD_FrontEnd_PackNormalAndRoughness
    ID3D12Resource* viewZ{};           // R32_FLOAT
    ID3D12Resource* light0{}, *light1{};       // RGBA16_FLOAT, RELAX_FrontEnd_PackSh
    ID3D12Resource* denoised0{}, *denoised1{}; // RGBA16_FLOAT, RELAX_BackEnd_UnpackSh
    ID3D12Resource* validation{};              // RGBA8_UNORM, OUT_VALIDATION; null leaves the layer out
};

class Denoiser
{
public:
    // Throws on Direct3D 12 or NRD errors.
    Denoiser(ID3D12Device* device, UINT width, UINT height);
    ~Denoiser();
    Denoiser(const Denoiser&) = delete;
    Denoiser& operator=(const Denoiser&) = delete;

    // Records the frame's dispatches on `list`, with `frameSlot`'s room for descriptors and constants
    // (gi_scene::FramesInFlight slots). `framesPerSecond` is the real frame rate, smoothed: the history
    // holds half a second.
    void record(ID3D12GraphicsCommandList4& list, const Camera& camera, const Textures& textures, std::uint32_t frameSlot, float framesPerSecond);

private:
    D3D12_CPU_DESCRIPTOR_HANDLE cpuAt(UINT index) const;
    D3D12_GPU_DESCRIPTOR_HANDLE gpuAt(UINT index) const;

    ID3D12Device* device;
    UINT width, height;
    nrd::Instance* instance{};
    std::uint32_t frameIndex{};

    gi_d3d12::ComPtr<ID3D12RootSignature> rootSignature;
    std::vector<gi_d3d12::ComPtr<ID3D12PipelineState>> pipelines;
    std::vector<gi_d3d12::ComPtr<ID3D12Resource>> permanentPool, transientPool;
    gi_d3d12::ComPtr<ID3D12DescriptorHeap> heap;
    UINT descriptorSize{}, texturesPerSet{}, storagePerSet{};
    gi_d3d12::ComPtr<ID3D12Resource> constants;
    std::uint8_t* constantData{};
    std::uint32_t constantStride{};
    // Every texture's state between dispatches; each starts and ends a frame in the unordered access state.
    std::unordered_map<ID3D12Resource*, D3D12_RESOURCE_STATES> states;
};
}
