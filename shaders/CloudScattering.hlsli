#ifndef TG_CLOUD_SCATTERING
#define TG_CLOUD_SCATTERING
// C++ の数値積分テストも実際のシェーダと同じ式を使う。
#ifdef __cplusplus
#include <cmath>
namespace tg::renderer {
#define TG_CLOUD_INLINE inline
#else
#define TG_CLOUD_INLINE
#endif
// 球面積分が 1 の HG。g の範囲は呼び出し側で (-1,1) に限定する。
TG_CLOUD_INLINE float CloudHg(float mu,float g) {
    float denominator=1+g*g-2*g*mu;
#ifdef __cplusplus
    return (1-g*g)/(12.5663706f*std::pow(denominator,1.5f));
#else
    return (1-g*g)/(12.5663706*pow(denominator,1.5));
#endif
}
// 前方・後方ローブの凸結合。角度による分岐を作らない。
TG_CLOUD_INLINE float CloudPhase(float mu,float isotropy) {
    return 0.8f*CloudHg(mu,0.8f*isotropy)+0.2f*CloudHg(mu,-0.3f*isotropy);
}
#ifdef __cplusplus
}
#endif
#undef TG_CLOUD_INLINE
#endif
