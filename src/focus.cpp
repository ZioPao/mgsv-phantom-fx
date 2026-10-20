#include "focus.hpp"

#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <mutex>

namespace phantom_fx::focus
{
namespace
{
using Microsoft::WRL::ComPtr;

// A small square at the centre; its median keeps a blade of grass from taking the focus.
constexpr UINT Size = 5;
// A copy is read back when its slot comes round again, by when the GPU has long made it.
constexpr std::size_t Slots = 3;

struct Readback
{
    ComPtr<ID3D11Texture2D> texture;
    // The projection's depth = A + B / z, and the far plane, of the frame copied.
    float a{}, b{}, farPlane{};
    bool pending{};
};

std::atomic<bool> active{};
std::atomic<float> metres{};

std::mutex mutex;
std::array<Readback, Slots> slots;
std::size_t next{};

// The median distance of the copy, or 0 when the GPU has not made it yet.
float read(ID3D11DeviceContext* immediate, const Readback& slot)
{
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(immediate->Map(slot.texture.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped))) return 0;

    std::array<float, Size * Size> distances{};
    for (UINT y = 0; y < Size; ++y)
    {
        const auto* row = reinterpret_cast<const float*>(static_cast<const std::byte*>(mapped.pData) + y * mapped.RowPitch);
        for (UINT x = 0; x < Size; ++x)
        {
            // Reversed-Z: 1 at the near plane, 0 at the far one.
            const float depth = row[x] - slot.a;
            distances[y * Size + x] = depth > 0 ? std::min(slot.b / depth, slot.farPlane) : slot.farPlane;
        }
    }

    immediate->Unmap(slot.texture.Get(), 0);
    const auto middle = distances.begin() + distances.size() / 2;
    std::nth_element(distances.begin(), middle, distances.end());

    return *middle;
}
}

void setActive(bool on) noexcept
{
    active = on;
    if (on) return;

    std::lock_guard lock(mutex);
    for (auto& slot : slots) slot.pending = false;
    metres = 0;
}

void depthReady(ID3D11DeviceContext* immediate, ID3D11Texture2D* depth, const camera::View& view) noexcept
{
    if (!active || !depth) return;

    D3D11_TEXTURE2D_DESC desc{};
    depth->GetDesc(&desc);
    if (desc.Format != DXGI_FORMAT_R32_FLOAT || desc.Width < Size || desc.Height < Size) return;

    std::lock_guard lock(mutex);
    auto& slot = slots[next];
    if (slot.pending)
    {
        const float distance = read(immediate, slot);
        if (distance <= 0) return;

        metres = distance;
    }

    if (!slot.texture)
    {
        const D3D11_TEXTURE2D_DESC staging{Size, Size, 1, 1, DXGI_FORMAT_R32_FLOAT, {1, 0}, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0};
        ComPtr<ID3D11Device> device;
        immediate->GetDevice(device.GetAddressOf());
        if (FAILED(device->CreateTexture2D(&staging, nullptr, slot.texture.GetAddressOf()))) return;
    }

    const UINT left = (desc.Width - Size) / 2, top = (desc.Height - Size) / 2;
    const D3D11_BOX box{left, top, 0, left + Size, top + Size, 1};
    immediate->CopySubresourceRegion(slot.texture.Get(), 0, 0, 0, 0, depth, 0, &box);
    slot.a = static_cast<float>(view.projection[10]);
    slot.b = static_cast<float>(view.projection[11]);
    slot.farPlane = view.farPlane;
    slot.pending = true;
    next = (next + 1) % Slots;
}

float distance() noexcept
{
    return metres;
}

void detach() noexcept
{
    std::lock_guard lock(mutex);
    for (auto& slot : slots) slot = {};
    metres = 0;
}
}
