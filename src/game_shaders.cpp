#include "game_shaders.hpp"

#include "log.hpp"
#include "shader_bytecode.hpp"

#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <cctype>

namespace phantom_fx::game_shaders
{
namespace
{
using Microsoft::WRL::ComPtr;

// The shaders the game created, by the object it binds, and the first of each row. Holding them keeps
// their addresses from being reused.
std::shared_mutex mutex;
std::unordered_map<IUnknown*, const GameShader*> rows;
std::array<ComPtr<ID3D11PixelShader>, std::size(GameShaders)> firsts;
std::unordered_map<IUnknown*, VertexShader> vertexShaders;
std::vector<ComPtr<IUnknown>> owners;

const GameShader* rowOf(std::string_view checksum)
{
    const auto found = std::ranges::find_if(GameShaders, [&](const GameShader& row) { return checksum == row.checksum; });

    return found != std::end(GameShaders) ? found : nullptr;
}

std::size_t indexOf(const GameShader& row)
{
    return static_cast<std::size_t>(&row - std::begin(GameShaders));
}

// The forward pass's reflective surfaces, by their source path since every variant of them is not listed:
// the rivers and other liquids ("...Prim_TppPrimRiver..." and "...Prim_TppRiverModel...") sample their
// shadow map at t14, the glass ("fox3dfw_glass") at t8, both with the comparison sampler s15
// (docs/DEVELOPMENT.md, "Forward shadows").
const GameShader RiverShader{.pass = GamePass::ForwardWater, .name = "Prim_TppPrimRiver", .checksum = "", .shadowSlot = 14, .cubeSlot = 2};
const GameShader GlassShader{.pass = GamePass::ForwardWater, .name = "fox3dfw_glass", .checksum = "", .shadowSlot = 8, .cubeSlot = 11, .guideValue = 0.5f};
// Every other glass: the effects' glass ("Prim_TppFxGlassMat...") and the metallic bacteria glass
// ("tpp3dfw_MetalicBacteriaGlass...") spell Glass with a capital, which the names above do not. Their slots
// are not this add-on's to name, so the draw finds them in what it binds (forward_shadows' discovery).
const GameShader OtherGlassShader{
    .pass = GamePass::ForwardWater, .name = "Glass", .checksum = "", .shadowSlot = 0, .cubeSlot = 0, .guideValue = 0.5f};

const GameShader* forwardRow(std::string_view code)
{
    const auto name = sourceName(code, "_ps.hlsl");
    std::string lower(name);
    std::ranges::transform(lower, lower.begin(), [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    if (lower.find("river") != std::string::npos) return &RiverShader;

    // The glass families the game has (14 pixel shaders in a run's frame log): the forward glass under
    // fox3dfw_glass, the effects' glass under Prim_TppFxGlassMat, the metallic bacteria glass under
    // tpp3dfw_MetalicBacteriaGlass. Named by family rather than by "glass" alone, so a glass shader of
    // another pass (a shadow or depth one) is not taken for this one.
    constexpr std::string_view GlassPrefixes[]{"fox3dfw_glass", "Prim_TppFxGlassMat", "tpp3dfw_MetalicBacteriaGlass"};
    if (std::ranges::any_of(GlassPrefixes, [&](std::string_view prefix) { return name.starts_with(prefix); }))
        return name.starts_with("fox3dfw_glass") ? &GlassShader : &OtherGlassShader;

    return nullptr;
}

VertexShader kindOf(std::string_view code)
{
    if (draw2DSource(code, "_vs.hlsl")) return VertexShader::Draw2D;

    const auto name = sourceName(code, "_vs.hlsl");
    if (name.starts_with("NewTerrain_Depth") || name.starts_with("NewTerrain_Shadow")) return VertexShader::Terrain;
    if (name.starts_with("fox3ddf_") || name.starts_with("tpp3ddf_") || name.starts_with("ModelForward_Shadow")) return VertexShader::Model;

    return VertexShader::Other;
}
}

void pixelShaderCreated(const void* code, std::size_t size, IUnknown* shader) noexcept
{
    const auto checksum = dxbcHashOf(code, size);
    if (!checksum || !shader) return;

    try
    {
        const auto* row = rowOf(toHex(*checksum));
        const bool listed = row != nullptr;
        if (!row) row = forwardRow(std::string_view(static_cast<const char*>(code), size));
        if (!row) return;

        bool isFirst{};
        {
            std::unique_lock lock(mutex);
            if (listed)
            {
                auto& first = firsts[indexOf(*row)];
                isFirst = !first;
                if (isFirst) shader->QueryInterface(IID_PPV_ARGS(first.GetAddressOf()));
            }
            else isFirst = std::ranges::none_of(rows, [&](const auto& entry) { return entry.second == row; });

            rows[shader] = row;
            owners.emplace_back(shader);
        }

        if (isFirst) log(std::string("Identified ") + row->name + ".");
    }
    catch (...)
    {
    }
}

void vertexShaderCreated(const void* code, std::size_t size, IUnknown* shader) noexcept
{
    if (!code || !shader) return;

    const auto kind = kindOf(std::string_view(static_cast<const char*>(code), size));
    if (kind == VertexShader::Other) return;

    try
    {
        std::unique_lock lock(mutex);
        vertexShaders[shader] = kind;
        owners.emplace_back(shader);
    }
    catch (...)
    {
    }
}

VertexShader vertexShaderOf(IUnknown* shader) noexcept
{
    if (!shader) return VertexShader::Other;

    std::shared_lock lock(mutex);
    const auto found = vertexShaders.find(shader);

    return found != vertexShaders.end() ? found->second : VertexShader::Other;
}

ID3D11PixelShader* firstCreated(const GameShader& row) noexcept
{
    std::shared_lock lock(mutex);
    return firsts[indexOf(row)].Get();
}

void forget() noexcept
{
    std::unique_lock lock(mutex);
    rows.clear();
    vertexShaders.clear();
    for (auto& first : firsts) first.Reset();
    owners.clear();
}

const GameShader* DrawnShader::row() noexcept
{
    lookUp();
    return found;
}

ID3D11PixelShader* DrawnShader::object() noexcept
{
    lookUp();
    return shader.Get();
}

void DrawnShader::lookUp() noexcept
{
    if (isLookedUp) return;

    isLookedUp = true;
    context->PSGetShader(shader.GetAddressOf(), nullptr, nullptr);
    if (!shader) return;

    std::shared_lock lock(mutex);
    if (const auto known = rows.find(shader.Get()); known != rows.end()) found = known->second;
}
}
