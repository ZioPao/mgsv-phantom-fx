#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace mgsv_dlss
{
// Opens the log at `file`, replacing a previous game session's.
void openLog(const std::filesystem::path& file);
// Appends a line to the log, from any thread.
void log(std::string_view message);
std::string hex(std::uint64_t value);
}
