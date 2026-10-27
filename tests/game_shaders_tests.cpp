#include "game_shaders.hpp"

#include <Windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace
{
using namespace phantom_fx::game_shaders;
using Microsoft::WRL::ComPtr;

void expect(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

struct Gpu
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
};

Gpu warp()
{
    Gpu gpu;
    expect(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, gpu.device.GetAddressOf(), nullptr,
               gpu.context.GetAddressOf())),
        "Create a WARP device.");

    return gpu;
}

ComPtr<ID3DBlob> compiled(const std::string& source, const char* target)
{
    ComPtr<ID3DBlob> code, errors;
    expect(SUCCEEDED(D3DCompile(source.data(), source.size(), "test", nullptr, nullptr, "main", target, 0, 0, code.GetAddressOf(), errors.GetAddressOf())),
        "Compile a test shader.");

    return code;
}

// A shader of its own for each `variant`, as the game makes a new one each time it loads a shader.
ComPtr<ID3D11PixelShader> pixelShader(const Gpu& gpu, int variant)
{
    const auto code = compiled("float4 main() : SV_Target { return " + std::to_string(variant) + "; }", "ps_5_0");
    ComPtr<ID3D11PixelShader> shader;
    expect(SUCCEEDED(gpu.device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, shader.GetAddressOf())),
        "Create a pixel shader.");

    return shader;
}

ComPtr<ID3D11VertexShader> vertexShader(const Gpu& gpu, int variant)
{
    const auto code = compiled("float4 main() : SV_Position { return " + std::to_string(variant) + "; }", "vs_5_0");
    ComPtr<ID3D11VertexShader> shader;
    expect(SUCCEEDED(gpu.device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, shader.GetAddressOf())),
        "Create a vertex shader.");

    return shader;
}

const GameShader& rowNamed(std::string_view name)
{
    const auto found = std::ranges::find_if(GameShaders, [&](const GameShader& row) { return name == row.name; });
    expect(found != std::end(GameShaders), "The table has the shader.");

    return *found;
}

// The game creates `shader` from code with the row's checksum: a DXBC container's 32-byte header.
void createdFrom(const GameShader& row, ID3D11PixelShader* shader)
{
    std::array<std::uint8_t, 32> header{'D', 'X', 'B', 'C'};
    const std::string_view checksum = row.checksum;
    for (std::size_t i = 0; i < 16; ++i) header[4 + i] = static_cast<std::uint8_t>(std::stoi(std::string(checksum.substr(2 * i, 2)), nullptr, 16));

    pixelShaderCreated(header.data(), header.size(), shader);
}

const GameShader* rowDrawnWith(const Gpu& gpu, ID3D11PixelShader* shader)
{
    gpu.context->PSSetShader(shader, nullptr, 0);
    DrawnShader drawn(gpu.context.Get());

    return drawn.row();
}

ULONG referencesOf(IUnknown* object)
{
    object->AddRef();
    return object->Release();
}

void identifiesSunByChecksum(const Gpu& gpu)
{
    const auto& sun = rowNamed("SunLight_DL_CS_ES");
    const auto shader = pixelShader(gpu, 1);
    createdFrom(sun, shader.Get());
    gpu.context->PSSetShader(shader.Get(), nullptr, 0);
    DrawnShader drawn(gpu.context.Get());
    expect(drawn.row() == &sun && sun.pass == GamePass::Sun && sun.isShadowMapped && sun.reflectsSky,
        "A sun shader is known by its checksum: inside the shadow range, with the sky's reflection.");
    expect(drawn.object() == shader.Get(), "The draw keeps the game's shader, to bind it again.");
    forget();
}

void unknownShaderHasNoRow(const Gpu& gpu)
{
    const auto code = compiled("float4 main() : SV_Target { return 2; }", "ps_5_0");
    ComPtr<ID3D11PixelShader> shader;
    expect(SUCCEEDED(gpu.device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, shader.GetAddressOf())),
        "Create a pixel shader.");
    pixelShaderCreated(code->GetBufferPointer(), code->GetBufferSize(), shader.Get());
    gpu.context->PSSetShader(shader.Get(), nullptr, 0);
    DrawnShader drawn(gpu.context.Get());
    expect(!drawn.row() && drawn.object() == shader.Get(), "A shader of no row is drawn as the game's, unknown.");
    forget();
}

void nothingBound(const Gpu& gpu)
{
    gpu.context->PSSetShader(nullptr, nullptr, 0);
    DrawnShader drawn(gpu.context.Get());
    expect(!drawn.row() && !drawn.object(), "A draw without a pixel shader has no row.");
}

// The upscaler and the sun light pass once kept four copies of a shader, and missed the fifth.
void everyCopyIsKnown(const Gpu& gpu)
{
    const auto& fxaa = rowNamed("Fxaa");
    std::array<ComPtr<ID3D11PixelShader>, 5> copies;
    for (int i = 0; i < 5; ++i)
    {
        copies[i] = pixelShader(gpu, 10 + i);
        createdFrom(fxaa, copies[i].Get());
    }

    expect(std::ranges::all_of(copies, [&](const auto& copy) { return rowDrawnWith(gpu, copy.Get()) == &fxaa; }), "Every copy of a shader is known.");
    forget();
}

void notDxbcIsIgnored(const Gpu& gpu)
{
    const auto shader = pixelShader(gpu, 3);
    std::array<std::uint8_t, 32> header{'D', 'X', 'B', 'X'};
    pixelShaderCreated(header.data(), header.size(), shader.Get());
    pixelShaderCreated(header.data(), 16, shader.Get());
    expect(!rowDrawnWith(gpu, shader.Get()), "Code that is not a DXBC container names no shader.");
    forget();
}

// The ray-traced lights draw with the first shader the game made for the twin of each kind of light.
void firstCreatedKeepsTheFirst(const Gpu& gpu)
{
    // Read at run time: MSVC once lost the contents of constexpr tables' members there.
    const auto twin = std::ranges::find_if(GameShaders, [](const GameShader& row) {
        return row.light && phantom_fx::game_lights::isTwin(*row.light) && row.light->twin == Twin::Point2;
    });
    expect(twin != std::end(GameShaders) && std::string_view(twin->name) == "SSLighting2_Point_NoShadow", "The quartic point light's twin has no shadow map.");
    expect(!firstCreated(*twin), "No shader before the game makes one.");

    const auto first = pixelShader(gpu, 4), second = pixelShader(gpu, 5);
    createdFrom(*twin, first.Get());
    createdFrom(*twin, second.Get());
    expect(firstCreated(*twin) == first.Get() && rowDrawnWith(gpu, second.Get()) == &*twin, "The first shader stays the first; the second is known too.");
    forget();
}

void forgetReleases(const Gpu& gpu)
{
    const auto& grade = rowNamed("Tonemap");
    const auto pixel = pixelShader(gpu, 6);
    const auto vertex = vertexShader(gpu, 6);
    const auto pixelReferences = referencesOf(pixel.Get()), vertexReferences = referencesOf(vertex.Get());
    createdFrom(grade, pixel.Get());
    const std::string_view source = "C:\\fox\\Release\\fox3ddf_blin_vs.hlsl";
    vertexShaderCreated(source.data(), source.size(), vertex.Get());
    expect(referencesOf(pixel.Get()) > pixelReferences && referencesOf(vertex.Get()) > vertexReferences, "Known shaders are held.");

    forget();
    expect(referencesOf(pixel.Get()) == pixelReferences && referencesOf(vertex.Get()) == vertexReferences, "Forgetting lets every shader go.");
    expect(!rowDrawnWith(gpu, pixel.Get()) && !firstCreated(grade) && vertexShaderOf(vertex.Get()) == VertexShader::Other,
        "Forgotten shaders are unknown.");
}

void vertexShaderBySourceName(const Gpu& gpu)
{
    struct Case
    {
        std::string_view debugData;
        VertexShader kind;
    };

    const Case cases[]{
        {"C:\\fox\\Release\\Draw2D_Ui2_BL_vs.hlsl", VertexShader::Draw2D},
        {"C:\\fox\\Release\\fox3ddf_skin_vs.hlsl", VertexShader::Model},
        {"C:\\fox\\Draw2D_Common.hlsli C:\\fox\\Release\\tpp3ddf_blin_vs.hlsl", VertexShader::Model}, // Draw2D only through a header
        {"C:\\fox\\Release\\ModelForward_ShadowOpaque_vs.hlsl", VertexShader::Model},
        {"C:\\fox\\Release\\NewTerrain_Depth_vs.hlsl", VertexShader::Terrain},
        {"C:\\fox\\Release\\Fxaa_vs.hlsl", VertexShader::Other},
    };

    int variant = 20;
    for (const auto& known : cases)
    {
        const auto shader = vertexShader(gpu, variant++);
        vertexShaderCreated(known.debugData.data(), known.debugData.size(), shader.Get());
        expect(vertexShaderOf(shader.Get()) == known.kind, "A vertex shader is known by its source name.");
    }

    expect(vertexShaderOf(nullptr) == VertexShader::Other, "No vertex shader is another one.");
    forget();
}
}

int main()
{
    try
    {
        const auto gpu = warp();
        identifiesSunByChecksum(gpu);
        unknownShaderHasNoRow(gpu);
        nothingBound(gpu);
        everyCopyIsKnown(gpu);
        notDxbcIsIgnored(gpu);
        firstCreatedKeepsTheFirst(gpu);
        forgetReleases(gpu);
        vertexShaderBySourceName(gpu);
        std::cout << "Game shader tests passed.\n";

        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';

        return 1;
    }
}
