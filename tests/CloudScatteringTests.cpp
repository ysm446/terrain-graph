#include "TestSupport.h"
#include "../shaders/CloudScattering.hlsli"
#include <cmath>
#include <initializer_list>

void RunCloudScatteringTests() {
    using tg::tests::Check;
    using tg::renderer::CloudPhase;
    tg::tests::Section("Cloud scattering");
    // 位相関数の球面積分は散乱次数・方向性によらず 1。
    for (float isotropy : {1.0f,0.5f,0.25f,0.125f,0.0f}) {
        double integral=0;
        bool finite=true;
        constexpr int Count=20000;
        for(int i=0;i<Count;++i) {
            const float mu=-1.0f+2.0f*(i+0.5f)/Count;
            const float phase=CloudPhase(mu,isotropy);
            finite=finite && std::isfinite(phase) && phase>=0;
            integral+=phase*(4.0*3.141592653589793/Count);
        }
        Check(finite && std::abs(integral-1.0)<1e-4,"Nonnegative, normalized spherical phase integral");
    }
    Check(std::abs(CloudPhase(-1,0)-CloudPhase(1,0))<1e-7,"Isotropic limit has no viewing-direction dependence");
    Check(std::abs(CloudPhase(-1e-5f,1)-CloudPhase(1e-5f,1))<1e-5,"No discontinuity across the sun's perpendicular plane");
    Check(CloudPhase(1,1)>CloudPhase(-1,1) && CloudPhase(-1,1)>CloudPhase(-0.5f,1),
          "Forward peak and a separate back-scattering lobe");
}
