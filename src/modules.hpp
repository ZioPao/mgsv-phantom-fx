#pragma once

#include "phantom_fx_settings.hpp"

#include <d3d11.h>

// Phantom FX's feature and pass modules, one row each (modules.cpp): what each does as the game's device
// comes and goes, at each change of the settings and at each present. The upscaler, HDR, the core and
// the cost measurement keep their own places around them (addon.cpp, core.cpp).
namespace phantom_fx::modules
{
void attach(ID3D11Device* device, ID3D11DeviceContext* immediate) noexcept;
void detach() noexcept;
void configure(const Configuration& configuration) noexcept;
void present() noexcept;
}
