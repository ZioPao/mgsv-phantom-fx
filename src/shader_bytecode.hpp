#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace mgsv_dlss
{
// The 16-byte checksum in a DXBC container's header (bytes 4 to 20). It identifies a shader
// across sessions, since the game loads the same precompiled bytecode each time.
using DxbcHash = std::array<std::uint8_t, 16>;

// The checksum of `code`, or nullopt when it is not a DXBC container.
std::optional<DxbcHash> dxbcHashOf(const void* code, std::size_t size);
// The checksum as 32 lowercase hex digits, the form docs/DEVELOPMENT.md lists.
std::string toHex(const DxbcHash& hash);
}
