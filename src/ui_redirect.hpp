#pragma once

#include <d3d11.h>
#include <wrl/client.h>
#include <cstddef>

// While the engine renders below the output size, its 2D pass (HUD, menus, subtitles) would draw
// into the small scene target and be stretched with it. The add-on keeps an output-size copy of the
// scene target instead: the upscaler fills it each frame before the UI draws, the 2D pass's draws
// and clears are made on it at the output size, and GammaControl reads it in place of the scene
// target. docs/DEVELOPMENT.md, "The UI at the output size".
namespace mgsv_dlss::ui_redirect
{
struct DrawCall
{
    unsigned count{}, instances{}, first{}, firstInstance{};
    int vertexOffset{};
    bool indexed{};
};

// What the upscaler fills before the UI draws.
struct Output
{
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target;
    // The engine's scene target the 2D pass meant, at the render size.
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> scene;
    unsigned width{}, height{};
};

void attach(ID3D11Device* device) noexcept;
void detach() noexcept;
// The swap chain's size.
void setOutputSize(unsigned width, unsigned height) noexcept;

// The 2D pass is recognised by its vertex shaders, the Draw2D*_vs.hlsl ones by the source paths the
// game's shaders keep, bound on the context that records it.
void vertexShaderCreated(const void* code, std::size_t size, IUnknown* shader) noexcept;
void vertexShaderBound(ID3D11DeviceContext* context, IUnknown* shader) noexcept;
// FinishCommandList clears the context's state.
void contextFinished(ID3D11DeviceContext* context) noexcept;
bool drawing2D(ID3D11DeviceContext* context) noexcept;

// On a recording context. Each returns true when the call was made on the output target instead,
// so the game's own call must be skipped.
// A draw of the engine's 2D pass into the scene target.
bool draw2D(ID3D11DeviceContext* context, const DrawCall& call) noexcept;
// A clear of the scene target within the 2D pass.
bool clearTarget(ID3D11DeviceContext* context, ID3D11RenderTargetView* view, const float color[4]) noexcept;
// GammaControl, which reads the scene target and writes the back buffer.
bool drawGamma(ID3D11DeviceContext* context, const DrawCall& call) noexcept;

// The 2D pass masks with stencil, which it clears on the scene's depth-stencil between its draws:
// every clear of a render-size depth-stencil clears the output-size one as well.
void depthStencilCleared(ID3D11DeviceContext* context, ID3D11DepthStencilView* view, UINT flags, float depth, UINT8 stencil) noexcept;

// The output target and the scene target the 2D pass last meant; empty before the first redirect.
Output output() noexcept;
}
