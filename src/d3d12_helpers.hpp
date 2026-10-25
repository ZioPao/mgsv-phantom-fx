#pragma once

#include "hresult.hpp"

#include <Windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <utility>

// Direct3D 12 helpers shared by the global illumination, FSR and the presenter: barriers, and the
// Direct3D 12 side of textures and fences Direct3D 11 shares.
namespace phantom_fx
{
// ReShade's proxies return their native object for this interface (IID_UnwrappedObject).
constexpr GUID UnwrappedObject{0x7f2c9a11, 0x3b4e, 0x4d6a, {0x81, 0x2f, 0x5e, 0x9c, 0xd3, 0x7a, 0x1b, 0x42}};
// Milliseconds a fence wait allows the GPU before the add-on gives up.
constexpr DWORD FenceTimeout = 10'000;

struct HandleCloser
{
    void operator()(HANDLE handle) const { CloseHandle(handle); }
};

inline D3D12_RESOURCE_BARRIER transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;

    return barrier;
}

inline D3D12_RESOURCE_BARRIER uavBarrier(ID3D12Resource* resource = nullptr)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = resource;

    return barrier;
}

// A Direct3D 12 device on Direct3D 11's GPU. ReShade wraps a device it sees made, which other add-ons
// would take for a second game device, so the add-on works on the native one. `wrapper` (the same
// device when ReShade does not wrap it) must stay alive while `native` is used.
struct Device12
{
    Microsoft::WRL::ComPtr<ID3D12Device> wrapper, native;
};

inline Device12 createDevice12(ID3D11Device* device11)
{
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
    check(device11->QueryInterface(IID_PPV_ARGS(&dxgi)), "Finding the DXGI device");
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    check(dxgi->GetAdapter(adapter.GetAddressOf()), "Finding the GPU");
    Device12 made;
    check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&made.wrapper)), "Creating the Direct3D 12 device");
    if (FAILED(made.wrapper->QueryInterface(UnwrappedObject, reinterpret_cast<void**>(made.native.GetAddressOf())))) made.native = made.wrapper;

    return made;
}

// Opens a shared handle in Direct3D 12, and closes it. This and the texture openings below return the
// first failure rather than throw: the presenter turns it into its own exception, which carries the
// result back to the game.
template<class Interface> [[nodiscard]] HRESULT openSharedHandle(ID3D12Device* device, HANDLE handle, Microsoft::WRL::ComPtr<Interface>& opened)
{
    const auto result = device->OpenSharedHandle(handle, IID_PPV_ARGS(&opened));
    CloseHandle(handle);

    return result;
}

// Opens a texture Direct3D 11 made shared through an NT handle.
[[nodiscard]] inline HRESULT openInD3D12(ID3D12Device* device, ID3D11Texture2D* texture, Microsoft::WRL::ComPtr<ID3D12Resource>& opened)
{
    Microsoft::WRL::ComPtr<IDXGIResource1> resource;
    auto result = texture->QueryInterface(IID_PPV_ARGS(&resource));
    if (FAILED(result)) return result;

    HANDLE handle{};
    result = resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle);
    if (FAILED(result)) return result;

    return openSharedHandle(device, handle, opened);
}

// Direct3D 12's view of a texture Direct3D 11 shares, opened again when Direct3D 11 replaces it.
// Holding the texture keeps its address from being reused by another.
struct OpenedTexture
{
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;

    // Opens `replacement`, unless it is the texture already open. Keeps the last one on failure.
    [[nodiscard]] HRESULT open(ID3D12Device* device, ID3D11Texture2D* replacement)
    {
        if (texture.Get() == replacement) return S_OK;

        Microsoft::WRL::ComPtr<ID3D12Resource> opened;
        const auto result = openInD3D12(device, replacement, opened);
        if (FAILED(result)) return result;

        texture = replacement;
        resource = std::move(opened);

        return S_OK;
    }
};
}
