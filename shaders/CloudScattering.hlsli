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
// 多重散乱のオクターブ近似。次数 n の太陽方向の消散は (1-spread)^n に縮み（広がりが大きいほど
// 光が深く行き渡る）、寄与は ((1-spread)*albedo)^n に縮む。光路で積分した各次数のエネルギーは
// albedo^n となり、albedo<=1 なら単散乱を超える光を作らない。
TG_CLOUD_INLINE float CloudOrderAttenuation(float spread,float order) {
#ifdef __cplusplus
    return std::pow(1.0f-spread,order);
#else
    return pow(1-spread,order);
#endif
}
TG_CLOUD_INLINE float CloudOrderContribution(float spread,float albedo,float order) {
#ifdef __cplusplus
    return std::pow((1.0f-spread)*albedo,order);
#else
    return pow((1-spread)*albedo,order);
#endif
}
#ifdef __cplusplus
}
#endif
#undef TG_CLOUD_INLINE
#endif
