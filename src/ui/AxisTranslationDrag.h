#pragma once

#include <imgui.h>
#include <algorithm>

namespace tg::ui {
// 押下時の投影を固定し、軸の移動や再描画でドラッグ量が跳ねないようにする。
struct AxisTranslationDrag {
    int node=0,axis=-1;
    ImVec2 press{},direction{};
    float start=0,metersPerPixel=0;

    bool Update(ImVec2 mouse, bool down, bool cancel, bool interrupt, float* const values[3]) {
        if (axis<0) return false;
        const int selected=axis;
        float value=*values[selected];
        if (cancel) { value=start; axis=-1; }
        else if (!down || interrupt) { axis=-1; return false; }
        else {
            const float pixels=(mouse.x-press.x)*direction.x+(mouse.y-press.y)*direction.y;
            value=std::clamp(start+pixels*metersPerPixel,-10000.0f,10000.0f);
        }
        if (value==*values[selected]) return false;
        *values[selected]=value;
        return true;
    }
};
} // namespace tg::ui
