#include "game_dither.hpp"

#include "log.hpp"
#include "shader_bytecode.hpp"

#include <wrl/client.h>
#include <map>
#include <mutex>

namespace phantom_fx::game_dither
{
namespace
{
struct Undithered
{
    std::vector<std::uint8_t> code, original;
};

// By the checksum of the code given, and by the patched code's own, which HDR's copies of it keep.
std::mutex mutex;
std::map<DxbcHash, Undithered> byOriginal;
std::map<DxbcHash, const Undithered*> byPatched;
}

const std::vector<std::uint8_t>* undithered(ID3D11Device* device, const void* code, std::size_t size) noexcept
{
    try
    {
        const auto hash = dxbcHashOf(code, size);
        if (!hash) return nullptr;

        std::scoped_lock lock(mutex);
        if (const auto found = byOriginal.find(*hash); found != byOriginal.end()) return &found->second.code;

        auto patched = withoutDither(code, size);
        if (!patched) return nullptr;

        // Made once here first, so a patch Direct3D refuses leaves the game its own shader.
        Microsoft::WRL::ComPtr<ID3D11PixelShader> made;
        if (!device || FAILED(device->CreatePixelShader(patched->data(), patched->size(), nullptr, made.GetAddressOf())))
        {
            logOnce("Direct3D refused shader " + toHex(*hash) + " without its dither; it keeps the game's.");
            return nullptr;
        }

        const auto* bytes = static_cast<const std::uint8_t*>(code);
        auto& entry = byOriginal[*hash] = Undithered{std::move(*patched), std::vector(bytes, bytes + size)};
        byPatched[*dxbcHashOf(entry.code.data(), entry.code.size())] = &entry;
        log("Took the dither out of shader " + toHex(*hash) + ".");

        return &entry.code;
    }
    catch (...)
    {
        return nullptr;
    }
}

std::pair<const void*, std::size_t> originalOf(const void* code, std::size_t size) noexcept
{
    const auto hash = dxbcHashOf(code, size);
    if (!hash) return {code, size};

    std::scoped_lock lock(mutex);
    const auto found = byPatched.find(*hash);
    if (found == byPatched.end()) return {code, size};

    return {found->second->original.data(), found->second->original.size()};
}
}
