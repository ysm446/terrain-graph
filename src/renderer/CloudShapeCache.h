#pragma once
#include "renderer/Atmosphere.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace tg::renderer {
// 最大54MiBの範囲で長軸を最大512点まで細分化する。短軸は最低8点。
// 細長い配置で空白に解像度を奪われ、球の輪郭が粗くなることを防ぐ。
inline std::array<uint32_t,3> CloudShapeCacheSize(const AtmosphereSettings& settings) {
    const float extent[]{settings.radiusX*2,settings.cloudThickness,settings.radiusZ*2};
    const float longest=(std::max)({extent[0],extent[1],extent[2],1.0f});
    const auto dimensions=[&](uint32_t resolution) {
        std::array<uint32_t,3> result{};
        for (int axis=0;axis<3;++axis)
            result[axis]=std::clamp(static_cast<uint32_t>(std::ceil(extent[axis]/longest*(resolution-1)))+1,8u,resolution);
        return result;
    };
    uint32_t low=192,high=512;
    while (low<high) {
        const uint32_t middle=(low+high+1)/2;
        const auto size=dimensions(middle);
        if (size[0]*size[1]*size[2]<=192u*192u*192u) low=middle;
        else high=middle-1;
    }
    return dimensions(low);
}
inline bool SameCloudShapeCache(const AtmosphereSettings& a,const AtmosphereSettings& b) {
    return a.primitiveCount==b.primitiveCount && a.primitiveSmoothness==b.primitiveSmoothness &&
        a.fieldCenterX==b.fieldCenterX && a.fieldCenterZ==b.fieldCenterZ &&
        a.radiusX==b.radiusX && a.radiusZ==b.radiusZ &&
        a.cloudBottom==b.cloudBottom && a.cloudThickness==b.cloudThickness &&
        a.primitiveRevision==b.primitiveRevision;
}
}
