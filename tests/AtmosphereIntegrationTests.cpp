#include "TestSupport.h"
#include "../shaders/AtmosphereIntegration.hlsli"
#include <cmath>
#include <initializer_list>

void RunAtmosphereIntegrationTests() {
    using tg::tests::Check;
    using tg::renderer::AtmosphereSegmentWeight;
    using tg::renderer::AtmosphereRayFraction;
    tg::tests::Section("Atmosphere integration");
    Check(AtmosphereSegmentWeight(0)==1,"Vacuum limit is finite and exact");
    for(float tau : {1e-6f,0.001f,0.1f,1.0f,10.0f,80.0f}) {
        for(int count : {1,4,32}) {
            const float segment=tau/count;
            double integral=0;
            for(int i=0;i<count;++i)
                integral+=std::exp(-double(segment)*i)*segment*AtmosphereSegmentWeight(segment);
            const double exact=-std::expm1(-double(tau));
            Check(std::abs(integral-exact)<1e-6,"Homogeneous scattering matches analytic integral at any step count");
        }
    }
    // 真上を向く地表からの光路は高さと距離が一致し、指数密度の積分を解析的に検証できる。
    for(double scaleHeight : {7994.0,1200.0}) {
        double optical=0;
        for(int i=0;i<32;++i) {
            const double start=60000*AtmosphereRayFraction(i/32.0f);
            const double end=60000*AtmosphereRayFraction((i+1)/32.0f);
            optical+=std::exp(-(start+end)*0.5/scaleHeight)*(end-start);
        }
        const double exact=scaleHeight*(1-std::exp(-60000/scaleHeight));
        Check(std::abs(optical/exact-1)<0.01,"Vertical Rayleigh/Mie optical depth within one percent of exact solution");
    }
}
