#include "features/sun_contact_shadows/sun_contact_shadows.hpp"

#include "log.hpp"
#include "passes/sun_light_pass.hpp"
#include "performance.hpp"

#include <mutex>
#include <string>
#include <unordered_map>

namespace phantom_fx::sun_contact_shadows
{
namespace
{
// The depth a context's recording traced the sun from since its last FinishCommandList.
std::mutex tracedMutex;
std::unordered_map<ID3D11DeviceContext*, ID3D11ShaderResourceView*> traced;

// Guarded by shadowMutex.
std::mutex shadowMutex;
contact_shadow_trace::ShadowTarget shadow;
bool tracedOnce{};

bool tracedFrom(ID3D11DeviceContext* context, ID3D11ShaderResourceView* depth)
{
    std::lock_guard lock(tracedMutex);
    const auto found = traced.find(context);
    return found != traced.end() && found->second == depth;
}

void noteTraced(ID3D11DeviceContext* context, ID3D11ShaderResourceView* depth)
{
    std::lock_guard lock(tracedMutex);
    traced[context] = depth;
}
}

void detach() noexcept
{
    {
        std::lock_guard lock(shadowMutex);
        shadow = {};
    }

    std::lock_guard lock(tracedMutex);
    traced.clear();
}

Health health()
{
    if (sun_light_pass::failed() || contact_shadow_trace::failed()) return Health::unavailable("Contact shadows are unavailable (see PhantomFX.log).");

    return {};
}

ID3D11ShaderResourceView* trace(ID3D11DeviceContext* context, ID3D11ShaderResourceView* depth, const contact_shadow_trace::PassConstants& constants)
{
    std::lock_guard lock(shadowMutex);
    if (tracedFrom(context, depth)) return shadow.view.Get();

    const performance::Timed timed(context, performance::Pass::SunTrace);
    const float* direction = &constants.light[game_lights::LightDirection];
    const auto lightProjection = contact_shadow_trace::project(constants.scene.data(), {direction[0], direction[1], direction[2], 0});
    const auto dispatched = contact_shadow_trace::trace(context, depth, constants.scene.data(), lightProjection, {}, shadow);
    if (!dispatched) return nullptr;

    noteTraced(context, depth);
    if (!tracedOnce)
    {
        tracedOnce = true;
        log("Shadows: first sun trace, light direction " + std::to_string(direction[0]) + ", " + std::to_string(direction[1]) + ", "
            + std::to_string(direction[2]) + " (view space), " + contact_shadow_trace::describe(*dispatched) + ".");
    }

    return shadow.view.Get();
}

void contextFinished(ID3D11DeviceContext* context) noexcept
{
    std::lock_guard lock(tracedMutex);
    traced.erase(context);
}

void present() noexcept
{
    std::lock_guard lock(tracedMutex);
    traced.clear();
}
}
