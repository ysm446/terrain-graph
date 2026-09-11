#pragma once
#include "renderer/Atmosphere.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>

namespace tg::renderer {
// 小さい形状群は従来の順序のまま。大きい群は中心位置の中央値で二分する。
inline void BuildCloudSpatialIndex(CloudGeometry& settings) {
    settings.primitiveBvh.clear();
    const uint32_t count=static_cast<uint32_t>(settings.primitives.size());
    if (count<=16) return;
    // 各葉は2〜4個を持つので節点数はN未満。reserveで再帰中の参照を安定させる。
    settings.primitiveBvh.reserve(count);
    std::vector<uint32_t> order(count);
    std::iota(order.begin(),order.end(),0u);
    const auto originals=settings.primitives;
    const auto build=[&](auto&& self,uint32_t start,uint32_t end)->void {
        const uint32_t index=static_cast<uint32_t>(settings.primitiveBvh.size());
        settings.primitiveBvh.emplace_back();
        auto& node=settings.primitiveBvh[index];
        for (int axis=0;axis<3;++axis) { node.lower[axis]=1e30f; node.upper[axis]=-1e30f; }
        node.lower[3]=1;
        for (uint32_t i=start;i<end;++i) {
            const auto& primitive=originals[order[i]];
            float minRadius=1e30f,maxRadius=1;
            for (int axis=0;axis<3;++axis) {
                node.lower[axis]=(std::min)(node.lower[axis],primitive.center[axis]);
                node.upper[axis]=(std::max)(node.upper[axis],primitive.center[axis]);
                minRadius=(std::min)(minRadius,(std::max)(primitive.radius[axis],1.0f));
                maxRadius=(std::max)(maxRadius,primitive.radius[axis]);
            }
            node.lower[3]=(std::min)(node.lower[3],minRadius/maxRadius);
            node.upper[3]=(std::max)(node.upper[3],maxRadius);
        }
        if (end-start<=TG_CLOUD_BVH_LEAF_SIZE) { node.start=start; node.count=end-start; }
        else {
            int axis=0;
            for (int candidate=1;candidate<3;++candidate)
                if (node.upper[candidate]-node.lower[candidate]>node.upper[axis]-node.lower[axis]) axis=candidate;
            const uint32_t middle=(start+end)/2;
            std::nth_element(order.begin()+start,order.begin()+middle,order.begin()+end,[&](uint32_t a,uint32_t b) {
                const float x=originals[a].center[axis],y=originals[b].center[axis];
                return x==y ? a<b : x<y;
            });
            self(self,start,middle); self(self,middle,end);
        }
        node.escape=static_cast<uint32_t>(settings.primitiveBvh.size());
    };
    build(build,0,count);
    for (uint32_t i=0;i<count;++i) settings.primitives[i]=originals[order[i]];
}

inline float CloudSpatialLowerBound(const AtmosphereSettings::PrimitiveBvhNode& node,const float* position) {
    float squared=0;
    for (int axis=0;axis<3;++axis) {
        const float delta=(std::max)({node.lower[axis]-position[axis],0.0f,position[axis]-node.upper[axis]});
        squared+=delta*delta;
    }
    return std::sqrt(squared)*node.lower[3]-node.upper[3];
}
}
