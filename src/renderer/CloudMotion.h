#pragma once
#include <cmath>

namespace tg::renderer {
// 保存値を変えず、再生中の移動量だけを保持する。方向はラジアン。
struct CloudMotion {
    double x = 0.0, z = 0.0;
    void Advance(double seconds, bool playing, float speed, float direction) {
        if (!playing || seconds <= 0.0) return;
        x += std::sin(direction) * speed * seconds;
        z += std::cos(direction) * speed * seconds;
    }
    static float LocalNoiseOffset(double displacement, double scale, unsigned mode) {
        // ローカル座標では逆向きにずらすと、ワールドでは雲の 75% の速度になる。
        // 細部ノイズ (3.1 倍) と共通の周期で丸め、長時間再生の精度低下を防ぐ。
        return mode == 2 ? static_cast<float>(std::fmod(-0.25 * displacement, scale * 10.0)) : 0.0f;
    }
    void Reset() { x = z = 0.0; }
};
} // namespace tg::renderer
