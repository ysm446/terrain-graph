#pragma once
#include <cmath>

namespace tg::renderer {
// 保存値を変えず、再生中の移動量だけを保持する。方向はラジアン。
struct CloudMotion {
    double x = 0.0, z = 0.0;
    double driftX = 0.0, driftZ = 0.0;
    void Advance(double seconds, bool playing, float speed, float direction, float noiseSpeedRatio = 0.75f) {
        if (!playing || seconds <= 0.0) return;
        const double dx = std::sin(direction) * speed * seconds;
        const double dz = std::cos(direction) * speed * seconds;
        x += dx;
        z += dz;
        // 相対移動を積算し、速度比の編集で現在の模様を飛ばさない。
        driftX += dx * (1.0 - noiseSpeedRatio);
        driftZ += dz * (1.0 - noiseSpeedRatio);
    }
    static float LocalNoiseOffset(double displacement, double scale, unsigned mode) {
        // 範囲に対する模様の相対移動量をローカル座標へ変換する。
        // 細部ノイズ (3.1 倍) と共通の周期で丸め、長時間再生の精度低下を防ぐ。
        return mode == 2 ? static_cast<float>(std::fmod(-displacement, scale * 10.0)) : 0.0f;
    }
    static float LoopOffset(double displacement, double extent) {
        return static_cast<float>(displacement-std::floor(displacement/extent)*extent);
    }
    void Reset() { x = z = driftX = driftZ = 0.0; }
};
} // namespace tg::renderer
