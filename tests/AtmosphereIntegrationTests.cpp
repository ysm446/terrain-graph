#include "TestSupport.h"
#include "../shaders/AtmosphereIntegration.hlsli"
#include <cmath>
#include <initializer_list>

void RunAtmosphereIntegrationTests() {
    using tg::tests::Check;
    using tg::renderer::AtmosphereSegmentWeight;
    using tg::renderer::AtmosphereRayFraction;
    using tg::renderer::AtmosphereMoonPhase;
    using tg::renderer::AtmosphereNightBlend;
    tg::tests::Section("Atmosphere integration");
    Check(AtmosphereSegmentWeight(0)==1,"Vacuum limit is finite and exact");
    Check(AtmosphereNightBlend(0.1f)==0 && AtmosphereNightBlend(0)==0 &&
          AtmosphereNightBlend(-0.104719755f)==1, "Night light fades in only after sunset");
    Check(AtmosphereNightBlend(-1e-5f)<1e-6f, "No moonlight jump at the horizon");
    Check(AtmosphereMoonPhase(0)==0 && AtmosphereMoonPhase(1)==1, "New and full moon endpoints");
    // 描画する円盤の数値積分と、地形・雲を照らす月相係数が一致することを検証する。
    for(float phase : {0.05f,0.25f,0.5f,0.75f,1.0f}) {
        double integral=0;
        const double cosine=2*phase-1, sine=std::sqrt(1-cosine*cosine);
        constexpr int Count=256;
        for(int y=0;y<Count;++y) for(int x=0;x<Count;++x) {
            const double px=2*(x+0.5)/Count-1, py=2*(y+0.5)/Count-1;
            const double r2=px*px+py*py;
            if(r2<1) integral+=std::fmax(0,px*sine+std::sqrt(1-r2)*cosine);
        }
        integral*=6.0/(Count*Count*3.141592653589793);
        Check(std::abs(integral-AtmosphereMoonPhase(phase))<0.001,
              "Moon disk integrated light agrees with terrain and cloud illumination");
    }
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
