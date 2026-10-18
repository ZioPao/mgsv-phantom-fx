#include "shader_bytecode.hpp"

#include <cstring>

namespace mgsv_dlss
{
std::optional<DxbcHash> dxbcHashOf(const void* code, std::size_t size)
{
    if (!code || size < 32 || std::memcmp(code, "DXBC", 4)) return std::nullopt;

    DxbcHash hash{};
    std::memcpy(hash.data(), static_cast<const std::uint8_t*>(code) + 4, hash.size());

    return hash;
}

std::string toHex(const DxbcHash& hash)
{
    constexpr char Digits[] = "0123456789abcdef";
    std::string text;
    for (const auto byte : hash)
    {
        text += Digits[byte >> 4];
        text += Digits[byte & 0xF];
    }

    return text;
}
}
