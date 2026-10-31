#pragma once

#include "hresult.hpp"

#include <Windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <cstdlib>
#include <string>
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

// Development only: an Agility SDK runtime for this process, chosen from the environment (the game's exe
// cannot export the two symbols the Agility SDK reads). `PHANTOM_FX_D3D12_CORE` is the folder holding
// `D3D12Core.dll`, relative to the game's exe (".\\D3D12\\619\\"); `PHANTOM_FX_D3D12_VERSION` is its
// D3D12SDKVersion (619). ID3D12SDKConfiguration::SetSDKVersion needs Windows Developer Mode, must run
// before the process's first Direct3D 12 device (a later call removes it), and leaves the OS runtime in
// place when it fails. Players never set either variable; the add-on's own gate decides on the runtime
// the OS gives it. `log` says what happened; returns whether the runtime was asked for.
inline bool selectRuntime()
{
    static bool done{};
    if (std::exchange(done, true)) return false;

    char path[512]{}, version[16]{};
    if (!GetEnvironmentVariableA("PHANTOM_FX_D3D12_CORE", path, sizeof(path))) return false;
    if (!GetEnvironmentVariableA("PHANTOM_FX_D3D12_VERSION", version, sizeof(version))) return false;

    const CLSID sdkConfiguration{0x7cda6aca, 0xa03e, 0x49c8, {0x94, 0x58, 0x03, 0x34, 0xd2, 0x0e, 0x07, 0xce}};
    ID3D12SDKConfiguration* configuration{};
    if (FAILED(D3D12GetInterface(sdkConfiguration, IID_PPV_ARGS(&configuration))) || !configuration)
    {
        log(std::string("Direct3D 12 runtime ") + version + " not selected: this Direct3D 12 has no SDK configuration interface.");
        return false;
    }

    const auto result = configuration->SetSDKVersion(static_cast<UINT>(std::atoi(version)), path);
    configuration->Release();
    log(std::string("Direct3D 12 runtime ") + version + " at " + path + (SUCCEEDED(result) ? " selected." : " not selected (0x" + hex(static_cast<std::uint32_t>(result)) + "): Developer Mode is off?"));
    return SUCCEEDED(result);
}

// Development only: PIX's GPU capturer, loaded before the process's first Direct3D 12 device (it hooks
// device creation, so loading it later makes PIXGpuCaptureNextFrames fail). PHANTOM_FX_PIX_CAPTURE asks
// for it; gi_renderer takes the capture itself.
inline void loadGpuCapturer()
{
    if (!GetEnvironmentVariableA("PHANTOM_FX_PIX_CAPTURE", nullptr, 0)) return;

    wchar_t module[MAX_PATH]{};
    GetModuleFileNameW(nullptr, module, MAX_PATH);
    std::wstring directory(module);
    directory = directory.substr(0, directory.find_last_of(L"\\\\") + 1);
    const auto capturer = LoadLibraryW((directory + L"WinPixGpuCapturer.dll").c_str());
    log(capturer ? "PIX's GPU capturer loaded at start." : "PIX's GPU capturer could not be loaded.");
}

inline Device12 createDevice12(ID3D11Device* device11)
{
    selectRuntime();

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
