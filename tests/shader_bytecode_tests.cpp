#include "shader_bytecode.hpp"

#include <Windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
using phantom_fx::withoutDither;
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

// Compiled as the game's shaders are: with debug data and unoptimised.
std::vector<std::uint8_t> compiled(const std::string& source, const char* target)
{
    ComPtr<ID3DBlob> code, errors;
    const UINT flags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
    if (FAILED(D3DCompile(source.data(), source.size(), "test", nullptr, nullptr, "main", target, flags, 0, code.GetAddressOf(), errors.GetAddressOf())))
        throw std::runtime_error(errors ? static_cast<const char*>(errors->GetBufferPointer()) : "Compile a test shader.");

    const auto* bytes = static_cast<const std::uint8_t*>(code->GetBufferPointer());

    return {bytes, bytes + code->GetBufferSize()};
}

constexpr UINT Size = 8;

// As the game's 8x8 dither: a different value in every pixel, from 2 to 254 in red, averaging a half,
// and from 1 to 127 in alpha, averaging a quarter.
ComPtr<ID3D11ShaderResourceView> ditherTexture(const Gpu& gpu)
{
    std::array<std::uint32_t, Size * Size> texels{};
    for (std::uint32_t i = 0; i < texels.size(); ++i)
    {
        const std::uint32_t step = i * 37 % 64;
        texels[i] = (2 + 4 * step) | 0x00010100 | (1 + 2 * step) << 24;
    }

    const D3D11_TEXTURE2D_DESC desc{Size, Size, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE};
    const D3D11_SUBRESOURCE_DATA data{texels.data(), Size * 4};
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> view;
    expect(SUCCEEDED(gpu.device->CreateTexture2D(&desc, &data, texture.GetAddressOf())), "Create the dither texture.");
    expect(SUCCEEDED(gpu.device->CreateShaderResourceView(texture.Get(), nullptr, view.GetAddressOf())), "View the dither texture.");

    return view;
}

using Image = std::array<float, Size * Size>;

// What `pixelShader` draws over an 8x8 float target with the dither in t8 and t15 and a point wrap
// sampler in s8, as the game binds them: the red of every pixel.
Image drawn(const Gpu& gpu, const std::vector<std::uint8_t>& pixelShader)
{
    const auto vertexCode = compiled(R"(
float4 main(uint id : SV_VertexID) : SV_Position
{
    return float4(id == 1 ? 3 : -1, id == 2 ? -3 : 1, 0, 1);
}
)",
        "vs_5_0");

    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11PixelShader> pixel;
    expect(SUCCEEDED(gpu.device->CreateVertexShader(vertexCode.data(), vertexCode.size(), nullptr, vertex.GetAddressOf())), "Create the vertex shader.");
    expect(SUCCEEDED(gpu.device->CreatePixelShader(pixelShader.data(), pixelShader.size(), nullptr, pixel.GetAddressOf())),
        "Direct3D takes the pixel shader.");

    D3D11_TEXTURE2D_DESC desc{Size, Size, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET};
    ComPtr<ID3D11Texture2D> target, readable;
    ComPtr<ID3D11RenderTargetView> targetView;
    expect(SUCCEEDED(gpu.device->CreateTexture2D(&desc, nullptr, target.GetAddressOf())), "Create the target.");
    expect(SUCCEEDED(gpu.device->CreateRenderTargetView(target.Get(), nullptr, targetView.GetAddressOf())), "View the target.");

    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    expect(SUCCEEDED(gpu.device->CreateTexture2D(&desc, nullptr, readable.GetAddressOf())), "Create the readback.");

    D3D11_SAMPLER_DESC samplerDesc{};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> sampler;
    expect(SUCCEEDED(gpu.device->CreateSamplerState(&samplerDesc, sampler.GetAddressOf())), "Create the sampler.");

    const auto dither = ditherTexture(gpu);
    ID3D11ShaderResourceView* views[]{dither.Get()};
    gpu.context->PSSetShaderResources(8, 1, views);
    gpu.context->PSSetShaderResources(15, 1, views);
    gpu.context->PSSetSamplers(8, 1, sampler.GetAddressOf());

    const D3D11_VIEWPORT viewport{0, 0, Size, Size, 0, 1};
    gpu.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    gpu.context->VSSetShader(vertex.Get(), nullptr, 0);
    gpu.context->PSSetShader(pixel.Get(), nullptr, 0);
    gpu.context->RSSetViewports(1, &viewport);
    gpu.context->OMSetRenderTargets(1, targetView.GetAddressOf(), nullptr);
    gpu.context->Draw(3, 0);
    gpu.context->CopyResource(readable.Get(), target.Get());

    D3D11_MAPPED_SUBRESOURCE mapped{};
    expect(SUCCEEDED(gpu.context->Map(readable.Get(), 0, D3D11_MAP_READ, 0, &mapped)), "Read the target.");
    Image reds{};
    for (UINT y = 0; y < Size; ++y)
        for (UINT x = 0; x < Size; ++x) reds[y * Size + x] = static_cast<const float*>(mapped.pData)[y * mapped.RowPitch / 4 + x * 4];

    gpu.context->Unmap(readable.Get(), 0);

    return reds;
}

float averageOf(const Image& reds)
{
    return std::accumulate(reds.begin(), reds.end(), 0.0f) / reds.size();
}

// Every pixel at `level`, well within an 8-bit step of it.
bool isFlatAt(const Image& reds, float level)
{
    return std::ranges::all_of(reds, [&](float red) { return std::abs(red - level) < 1e-4f; });
}

// The sky draws' and the foggy resolve's form: the dither's red, centred on zero, added to the colour.
void centredDitherIsRemoved(const Gpu& gpu)
{
    const auto code = compiled(R"(
SamplerState g_samplerPoint_Wrap : register(s8);
Texture2D<float4> inMesh : register(t8);

float4 main(float4 position : SV_Position) : SV_Target
{
    float3 color = 0.25;
    color += (inMesh.Sample(g_samplerPoint_Wrap, frac(position.xy * 0.125)).x - 0.5) / 128;
    return float4(color, 1);
}
)",
        "ps_5_0");

    const auto dithered = drawn(gpu, code);
    expect(!isFlatAt(dithered, averageOf(dithered)), "The game's shader dithers its colour.");

    const auto patched = withoutDither(code.data(), code.size());
    expect(patched.has_value(), "A shader with the dither is patched.");
    expect(isFlatAt(drawn(gpu, *patched), averageOf(dithered)), "Without the dither every pixel gets the dithered colour's average.");
}

// The forward materials' form: the dither's alpha added whole to the fog, which keeps its average.
void wholeDitherKeepsItsAverage(const Gpu& gpu)
{
    const auto code = compiled(R"(
SamplerState g_samplerPoint_Wrap : register(s8);
Texture2D<float4> inMesh : register(t15);

float4 main(float4 position : SV_Position) : SV_Target
{
    float4 fog = float4(0.25, 0.25, 0.25, 0.5);
    fog += inMesh.Sample(g_samplerPoint_Wrap, frac(position.xy * 0.125)).w / 128;
    return float4(fog.rgb + fog.a * 0.1, 1);
}
)",
        "ps_5_0");

    const auto dithered = drawn(gpu, code);
    expect(!isFlatAt(dithered, averageOf(dithered)), "The game's shader dithers its fog.");

    const auto patched = withoutDither(code.data(), code.size());
    expect(patched && isFlatAt(drawn(gpu, *patched), averageOf(dithered)), "Without the dither every pixel gets the dithered fog's average.");
}

// The G-buffer's fade reads the same texture as a mask under another name; its average would turn
// the fade into a cut.
void fadeMaskIsKept()
{
    const auto code = compiled(R"(
SamplerState g_samplerPoint_Wrap : register(s8);
Texture2D<float4> g_tex_mesh : register(t15);
cbuffer cPSObject : register(b0) { float threshold; };

float4 main(float4 position : SV_Position) : SV_Target
{
    clip(g_tex_mesh.Sample(g_samplerPoint_Wrap, position.xy * 0.125).w * threshold - 0.5);
    return 0.25;
}
)",
        "ps_5_0");

    expect(!withoutDither(code.data(), code.size()), "A shader without the dither is kept.");
}
}

int main()
{
    try
    {
        const auto gpu = warp();
        centredDitherIsRemoved(gpu);
        wholeDitherKeepsItsAverage(gpu);
        fadeMaskIsKept();
        std::cout << "Shader bytecode tests passed.\n";

        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';

        return 1;
    }
}
