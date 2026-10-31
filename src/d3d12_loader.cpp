// D3D12 loader; d3d12_loader.hpp describes why it exists. The two exports it adds are the ones the
// service's loader reads from the game's main module: D3D12SDKVersion (a UINT) and D3D12SDKPath (a
// pointer to a UTF-8 path relative to the game's exe). The loader binary-searches the export names, so
// they are written in order, and the module's own exports are kept as they are.
#include "d3d12_loader.hpp"

#include "log.hpp"

#include <psapi.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <utility>
#include <vector>

namespace phantom_fx::d3d12_loader
{
namespace
{
// The runtime the add-on ships in the game's folder. PHANTOM_FX_D3D12_VERSION and _CORE choose another
// for development, as they did when the runtime was selected with SetSDKVersion.
constexpr UINT DefaultSdkVersion = 619;
constexpr const char* DefaultSdkPath = ".\\D3D12\\619";

HMODULE d3d12{};
PFN_D3D12_CREATE_DEVICE makeDevice{};
PFN_D3D12_GET_INTERFACE getInterfaceAt{};
PFN_D3D12_GET_DEBUG_INTERFACE getDebugInterfaceAt{};
PFN_D3D12_SERIALIZE_ROOT_SIGNATURE serializeRootAt{};

// Adds D3D12SDKVersion and D3D12SDKPath to the module's export table, in memory.
bool addExports(HMODULE module, UINT version, const char* path)
{
    auto* base = reinterpret_cast<BYTE*>(module);
    DWORD blockProtection{};
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];

    // What the module already exports (a game's DRM may): its functions and names stay, its ordinals too.
    std::vector<DWORD> functions;
    std::vector<std::pair<std::string, WORD>> names;
    WORD ordinalBase = 1;
    std::string moduleName;
    if (directory.VirtualAddress && directory.Size)
    {
        auto* old = reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(base + directory.VirtualAddress);
        ordinalBase = static_cast<WORD>(old->Base);
        const auto* oldFunctions = reinterpret_cast<const DWORD*>(base + old->AddressOfFunctions);
        const auto* oldNames = reinterpret_cast<const DWORD*>(base + old->AddressOfNames);
        const auto* oldOrdinals = reinterpret_cast<const WORD*>(base + old->AddressOfNameOrdinals);
        functions.assign(oldFunctions, oldFunctions + old->NumberOfFunctions);
        // PHANTOM_FX_D3D12_REPLACE_EXPORTS drops the module's own export names: if its old export is still
        // found afterwards, the loader is not reading this table at all (a development probe).
        static const bool replaceExports = GetEnvironmentVariableA("PHANTOM_FX_D3D12_REPLACE_EXPORTS", nullptr, 0) != 0;
        if (!replaceExports)
            for (DWORD i = 0; i < old->NumberOfNames; ++i)
                names.emplace_back(reinterpret_cast<const char*>(base + oldNames[i]), oldOrdinals[i]);
        if (old->Name) moduleName = reinterpret_cast<const char*>(base + old->Name);
    }

    const auto existing = static_cast<WORD>(functions.size());
    functions.push_back(0); // Filled once the block is placed: the version and the path's pointer live in it
    functions.push_back(0);
    names.emplace_back("D3D12SDKVersion", existing);
    names.emplace_back("D3D12SDKPath", static_cast<WORD>(existing + 1));
    std::sort(names.begin(), names.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

    // The version, the path's pointer and the path itself sit in the block too: an export's value must be
    // an offset from the game's module, and the add-on's own data is not (2026-10-31, a crash in the game).
    std::size_t size = sizeof(IMAGE_EXPORT_DIRECTORY) + 4 * functions.size() + 4 * names.size() + 2 * names.size() + moduleName.size() + 1
        + sizeof(UINT) + sizeof(const char*) + std::strlen(path) + 1;
    for (const auto& [name, ordinal] : names) size += name.size() + 1;
    size = (size + 15) & ~std::size_t{15};

    // Inside the image, in room no section uses: the loader reads these addresses as the module's own.
    // (A block placed after the image was found by the module itself but not by another module's
    // GetProcAddress, nor by D3D12.dll's loader: 2026-10-31.)
    auto* block = static_cast<BYTE*>(nullptr);
    const auto sectionAlignment = nt->OptionalHeader.SectionAlignment;
    const auto sections = IMAGE_FIRST_SECTION(nt);

    // A discardable section the runtime never reads (the linker's debug data): its bytes are the module's
    // own, inside the section's virtual size, which is where the loader takes what it reads.
    for (std::size_t i = 0; i < nt->FileHeader.NumberOfSections && !block; ++i)
    {
        const auto& section = sections[i];
        if (!(section.Characteristics & IMAGE_SCN_MEM_DISCARDABLE) || section.Misc.VirtualSize < size) continue;

        auto* candidate = base + section.VirtualAddress;
        DWORD protect{};
        if (!VirtualProtect(candidate, size, PAGE_READWRITE, &protect)) continue;
        block = candidate;
        blockProtection = protect;
    }

    for (std::size_t i = 0; i < nt->FileHeader.NumberOfSections && !block; ++i)
    {
        const auto& section = sections[i];
        // The bytes of the section's last page past its virtual size: mapped for the image, owned by nothing.
        const auto from = static_cast<std::size_t>(section.VirtualAddress) + section.Misc.VirtualSize;
        const auto page = (from + sectionAlignment - 1) / sectionAlignment * sectionAlignment;
        const auto to = std::min<std::size_t>(page, i + 1 < nt->FileHeader.NumberOfSections
                ? sections[i + 1].VirtualAddress
                : nt->OptionalHeader.SizeOfImage);
        if (from + size > to) continue;

        auto* candidate = base + from;
        DWORD protect{};
        if (!VirtualProtect(candidate, size, PAGE_READWRITE, &protect)) continue;
        block = candidate;
        blockProtection = protect;
    }
    if (!block) return false;

    BYTE* at = block;
    const auto take = [&](std::size_t bytes) {
        bytes = (bytes + 7) & ~std::size_t{7};
        auto* taken = at;
        at += bytes;
        return taken;
    };
    const auto offsetOf = [&](const void* address) { return static_cast<DWORD>(static_cast<const BYTE*>(address) - base); };

    auto* exported = reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(take(sizeof(IMAGE_EXPORT_DIRECTORY)));
    auto* functionTable = reinterpret_cast<DWORD*>(take(4 * functions.size()));
    auto* nameTable = reinterpret_cast<DWORD*>(take(4 * names.size()));
    auto* ordinalTable = reinterpret_cast<WORD*>(take(2 * names.size()));
    if (!exported || !functionTable || !nameTable || !ordinalTable) return false;

    for (std::size_t i = 0; i < functions.size(); ++i) functionTable[i] = functions[i];
    for (std::size_t i = 0; i < names.size(); ++i)
    {
        auto* copy = reinterpret_cast<char*>(take(names[i].first.size() + 1));
        if (!copy) return false;
        std::memcpy(copy, names[i].first.c_str(), names[i].first.size() + 1);
        nameTable[i] = offsetOf(copy);
        ordinalTable[i] = names[i].second;
    }

    DWORD moduleNameAt{};
    if (!moduleName.empty())
    {
        auto* copy = reinterpret_cast<char*>(take(moduleName.size() + 1));
        if (!copy) return false;
        std::memcpy(copy, moduleName.c_str(), moduleName.size() + 1);
        moduleNameAt = offsetOf(copy);
    }

    // The directory ends here, before the values: an export whose address lies inside the directory's
    // range is a forwarder ("Module.Function") to the loader, and is not found (2026-10-31).
    const auto* directoryEnd = at;

    auto* versionAt = reinterpret_cast<UINT*>(take(sizeof(UINT)));
    auto* pathAt = reinterpret_cast<const char**>(take(sizeof(const char*)));
    auto* pathText = reinterpret_cast<char*>(take(std::strlen(path) + 1));
    if (!versionAt || !pathAt || !pathText) return false;
    *versionAt = version;
    std::memcpy(pathText, path, std::strlen(path) + 1);
    *pathAt = pathText;
    functionTable[existing] = offsetOf(versionAt);
    functionTable[existing + 1] = offsetOf(pathAt);

    exported->Base = ordinalBase;
    exported->NumberOfFunctions = static_cast<DWORD>(functions.size());
    exported->NumberOfNames = static_cast<DWORD>(names.size());
    exported->AddressOfFunctions = offsetOf(functionTable);
    exported->AddressOfNames = offsetOf(nameTable);
    exported->AddressOfNameOrdinals = offsetOf(ordinalTable);
    exported->Name = moduleNameAt;

    DWORD protect{};
    if (!VirtualProtect(&directory, sizeof(directory), PAGE_READWRITE, &protect)) return false;
    directory.VirtualAddress = offsetOf(exported);
    directory.Size = static_cast<DWORD>(directoryEnd - block);
    VirtualProtect(&directory, sizeof(directory), protect, &protect);
    log("Direct3D 12: " + std::to_string(names.size()) + " exports, directory at base+" + std::to_string(directory.VirtualAddress)
        + " size " + std::to_string(directory.Size) + ", names " + std::to_string(exported->NumberOfNames) + ", functions "
        + std::to_string(exported->NumberOfFunctions) + ", base " + std::to_string(exported->Base) + "; found: only the old "
        + (GetProcAddress(module, "ExportProductMetadata") ? "yes" : "no") + ", version " + (GetProcAddress(module, "D3D12SDKVersion") ? "yes" : "no")
        + ", path " + (GetProcAddress(module, "D3D12SDKPath") ? "yes" : "no") + ".");
    return true;
}

// The shipped runtime's folder, relative to the game's exe, and whether its D3D12Core.dll is there.
bool shippedRuntime(UINT& version, const char*& path)
{
    char text[64]{};
    if (GetEnvironmentVariableA("PHANTOM_FX_D3D12_VERSION", text, sizeof(text))) version = static_cast<UINT>(std::atoi(text));
    else version = DefaultSdkVersion;

    static std::string chosen;
    if (GetEnvironmentVariableA("PHANTOM_FX_D3D12_CORE", text, sizeof(text))) chosen = text;
    else if (version == DefaultSdkVersion) chosen = DefaultSdkPath;
    else chosen = ".\\D3D12\\" + std::to_string(version);
    path = chosen.c_str();

    wchar_t exe[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return false;
    std::filesystem::path core = std::filesystem::path(exe).parent_path();
    for (const auto& part : std::filesystem::path(chosen).relative_path()) core /= part;
    core /= "D3D12Core.dll";
    return std::filesystem::exists(core);
}

bool addExportsIfNeeded()
{
    const auto module = GetModuleHandleW(nullptr);
    if (GetProcAddress(module, "D3D12SDKVersion")) return true;

    UINT version{};
    const char* path{};
    if (!shippedRuntime(version, path))
    {
        log("Direct3D 12: the game's folder has no D3D12Core.dll for the shipped runtime.");
        return false;
    }
    if (!addExports(module, version, path))
    {
        log("Direct3D 12: no room in the game's module for the runtime's exports.");
        return false;
    }
    return true;
}
}

bool load()
{
    if (d3d12) return true;

    // D3D12.dll reads the exports as it initialises: patching after it is loaded changes nothing.
    // Either way the passes run, on Windows' own runtime when the shipped one could not be chosen.
    if (GetModuleHandleW(L"d3d12.dll"))
        log("Direct3D 12 was loaded before the add-on could point it at the runtime it ships: Windows' own is used.");
    else if (!addExportsIfNeeded())
        log("Direct3D 12 runtime not chosen: Windows' own is used.");

    // What the loader will read, before it reads it: a wrong value here names the fault at once.
    if (auto* version = reinterpret_cast<UINT*>(GetProcAddress(GetModuleHandleW(nullptr), "D3D12SDKVersion")))
        log("Direct3D 12 runtime chosen: SDK " + std::to_string(*version) + " at "
            + [&] {
                  auto* path = reinterpret_cast<const char**>(GetProcAddress(GetModuleHandleW(nullptr), "D3D12SDKPath"));
                  return std::string(path && *path ? *path : "?");
              }()
            + ".");

    d3d12 = LoadLibraryW(L"d3d12.dll");
    if (!d3d12)
    {
        const auto error = GetLastError();
        log("Direct3D 12 could not be loaded (" + std::to_string(error) + ").");
        return false;
    }

    makeDevice = reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GetProcAddress(d3d12, "D3D12CreateDevice"));
    getInterfaceAt = reinterpret_cast<PFN_D3D12_GET_INTERFACE>(GetProcAddress(d3d12, "D3D12GetInterface"));
    getDebugInterfaceAt = reinterpret_cast<PFN_D3D12_GET_DEBUG_INTERFACE>(GetProcAddress(d3d12, "D3D12GetDebugInterface"));
    serializeRootAt = reinterpret_cast<PFN_D3D12_SERIALIZE_ROOT_SIGNATURE>(GetProcAddress(d3d12, "D3D12SerializeRootSignature"));
    if (!makeDevice || !getInterfaceAt || !serializeRootAt)
    {
        log("Direct3D 12 is missing its entry points.");
        d3d12 = nullptr;
        return false;
    }

    log("Direct3D 12: " + core() + ".");
    return true;
}

PFN_D3D12_CREATE_DEVICE createDevice() { return makeDevice; }
PFN_D3D12_GET_INTERFACE getInterface() { return getInterfaceAt; }
PFN_D3D12_GET_DEBUG_INTERFACE getDebugInterface() { return getDebugInterfaceAt; }
PFN_D3D12_SERIALIZE_ROOT_SIGNATURE serializeRootSignature() { return serializeRootAt; }

std::string core()
{
    HMODULE modules[256]{};
    DWORD needed{};
    if (!EnumProcessModulesEx(GetCurrentProcess(), modules, sizeof(modules), &needed, LIST_MODULES_ALL)) return "unknown";

    for (DWORD i = 0; i < needed / sizeof(modules[0]); ++i)
    {
        wchar_t name[MAX_PATH]{};
        if (!GetModuleFileNameExW(GetCurrentProcess(), modules[i], name, MAX_PATH)) continue;

        const std::wstring wide(name);
        if (wide.find(L"D3D12Core.dll") == std::wstring::npos) continue;

        std::string path;
        for (const auto& c : wide) path += static_cast<char>(c);
        if (auto* sdk = reinterpret_cast<UINT*>(GetProcAddress(modules[i], "D3D12SDKVersion"))) return path + " (SDK " + std::to_string(*sdk) + ")";
        return path + " (no SDK version export)";
    }

    return "not loaded";
}
}
