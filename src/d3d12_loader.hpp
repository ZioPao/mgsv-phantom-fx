#pragma once

#include <d3d12.h>
#include <windows.h>

#include <string>

// Direct3D 12 is loaded here rather than linked: which runtime a process runs is chosen by two exports
// the loader reads from the game's module (D3D12SDKVersion, D3D12SDKPath), and they must be in place
// before D3D12.dll initialises. Adding them to the module in memory lets D3D12.dll run the runtime the
// add-on ships (D3D12\<version> in the game's folder) without Windows Developer Mode (which
// ID3D12SDKConfiguration::SetSDKVersion needs) and without writing to the game's files. Proven in
// docs/opacity-micromaps.md, "The runtime": patched before D3D12.dll loads, the process runs the shipped
// runtime; patched after, it keeps Windows' own.
namespace phantom_fx::d3d12_loader
{
// Adds the exports, then loads D3D12.dll and finds its entry points; false when the process has no
// usable Direct3D 12, which the caller says in the log, and the game carries on without it.
bool load();
PFN_D3D12_CREATE_DEVICE createDevice();
PFN_D3D12_GET_INTERFACE getInterface();
PFN_D3D12_GET_DEBUG_INTERFACE getDebugInterface();
PFN_D3D12_SERIALIZE_ROOT_SIGNATURE serializeRootSignature();

// The runtime the process runs and its SDK version, for the log: the shipped one, or Windows' own when
// the exports could not be placed or D3D12.dll was already loaded.
std::string core();
}
