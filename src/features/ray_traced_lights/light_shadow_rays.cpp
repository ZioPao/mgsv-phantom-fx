#include "features/ray_traced_lights/light_shadow_rays.hpp"

#include "performance.hpp"

namespace phantom_fx::light_shadow_rays
{
void record(gi_d3d12::Recorder& recorder, std::uint32_t width, std::uint32_t height)
{
    using gi_d3d12::groups;
    using gi_d3d12::Kernel;

    recorder.mark("shadow rays for lights", performance::Pass::LightShadowRays);
    recorder.dispatch(Kernel::TraceLocalLightShadows, groups(width, 8), groups(height, 8));
    recorder.barrier();
    recorder.mark("shadow filter for lights", performance::Pass::LightShadowFilter);
    for (std::uint32_t pass = 0; pass < 3; ++pass)
    {
        recorder.dispatch(Kernel::FilterLocalLightShadows, groups(width, 8), groups(height, 8), {pass, 1u << pass});
        recorder.barrier();
    }
}
}
