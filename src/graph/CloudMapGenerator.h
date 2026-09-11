#pragma once

#include "graph/NodeGraph.h"
#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace tg::graph {
// 描画側の最小半径1mに揃え、退化した楕円体を作らない。
inline float CloudMapHalfThickness(const CloudMapSettings& settings,float horizontalRadius) {
    return std::max(1.0f,std::min(std::clamp(settings.bottomThickness,2.0f,2000.0f)*0.5f,
        horizontalRadius*std::clamp(settings.maxThicknessRatio,0.01f,1.0f)));
}
// セル内の近傍だけを検索する。ガイドは表示用に上限を持つが形状は作業予算まで生成する。
inline CompiledCloud GenerateCloudMap(const CloudMapSettings& settings,GraphId nodeId) {
    CompiledCloud result;
    result.smoothness=std::clamp(settings.smoothness,0.0f,500.0f);
    auto guide=std::make_shared<CloudMapGuide>();
    result.mapGuide=guide;
    const int count=std::clamp(settings.pointCount,0,10000);
    const float width=std::clamp(settings.width,100.0f,100000.0f);
    const float depth=std::clamp(settings.depth,100.0f,100000.0f);
    const float threshold=std::clamp(settings.connectionDistance,1.0f,10000.0f);
    const float density=std::clamp(settings.columnsPerKm,0.0f,50.0f)*0.001f;
    const float low=std::clamp(std::min(settings.minGrowth,settings.maxGrowth),0.0f,5000.0f);
    const float high=std::clamp(std::max(settings.minGrowth,settings.maxGrowth),0.0f,5000.0f);
    const float columnRadius=std::clamp(settings.columnRadius,10.0f,1000.0f);
    uint32_t state=static_cast<uint32_t>(settings.seed);
    const auto random=[&]() { state=state*1664525u+1013904223u; return float(state>>8)/16777216.0f; };
    const auto key=[](int x,int z) { return (uint64_t(static_cast<uint32_t>(x))<<32)|static_cast<uint32_t>(z); };
    std::unordered_map<uint64_t,std::vector<uint32_t>> cells;
    std::vector<std::pair<int,int>> coordinates;
    guide->points.reserve(count); coordinates.reserve(count);
    for (int i=0;i<count;++i) {
        const float x=random()*width,z=random()*depth;
        guide->points.push_back({settings.centerX+x-width*0.5f,settings.bottomHeight,settings.centerZ+z-depth*0.5f});
        const int cellX=static_cast<int>(std::floor(x/threshold)),cellZ=static_cast<int>(std::floor(z/threshold));
        coordinates.emplace_back(cellX,cellZ);
        cells[key(cellX,cellZ)].push_back(static_cast<uint32_t>(i));
    }
    const auto append=[&](float x,float y,float z,float rx,float ry,float rz) {
        if ((result.primitives.size()+1)*sizeof(CloudPrimitive)>CloudShapeMemoryBudget) {
            result.shapeOverflow=true; return false;
        }
        result.primitives.push_back({x,y,z,0,rx,ry,rz,0,nodeId,static_cast<uint32_t>(result.primitives.size())});
        return true;
    };
    guide->pointConnected.resize(count,0);
    auto& connected=guide->pointConnected;
    constexpr size_t GuideLimit=4096;
    for (uint32_t i=0;i<guide->points.size();++i) {
        const auto a=guide->points[i];
        const auto [cellX,cellZ]=coordinates[i];
        for (int dz=-1;dz<=1;++dz) for (int dx=-1;dx<=1;++dx) {
            const auto found=cells.find(key(cellX+dx,cellZ+dz));
            if (found==cells.end()) continue;
            for (const uint32_t j:found->second) {
                if (j<=i) continue;
                const auto b=guide->points[j];
                const float vx=b.x-a.x,vz=b.z-a.z;
                const float distance=std::hypot(vx,vz);
                if (distance>threshold || distance<0.01f) continue;
                connected[i]=connected[j]=true;
                const float radius=std::max(1.0f,distance*0.5f);
                const float halfThickness=CloudMapHalfThickness(settings,radius);
                const float baseY=settings.bottomHeight+halfThickness;
                if (!append((a.x+b.x)*0.5f,baseY,(a.z+b.z)*0.5f,radius,halfThickness,radius)) return result;
                ++guide->edgeCount;
                if (guide->edges.size()<GuideLimit) guide->edges.push_back({i,j});
                // 接続ごとの乱数列。点の配置と独立し、他の接続を増減しても既存の塔が変わらない。
                state=static_cast<uint32_t>(settings.seed)^(i*747796405u)^(j*2891336453u);
                const float expected=distance*density;
                int columns=static_cast<int>(std::floor(expected));
                if (random()<expected-columns) ++columns;
                for (int column=0;column<columns;++column) {
                    const float t=random();
                    const float height=std::min(low+(high-low)*random(),distance*std::clamp(settings.maxHeightRatio,0.0f,1.0f));
                    if (height<=0) continue;
                    const float x=a.x+vx*t,z=a.z+vz*t;
                    const float topRadius=std::min(columnRadius,radius);
                    const float rootRadius=std::min(topRadius,halfThickness);
                    const int steps=std::max(1,static_cast<int>(std::ceil(height/(rootRadius*1.5f))));
                    ++guide->columnCount;
                    if (guide->columns.size()<GuideLimit) guide->columns.push_back({{x,baseY,z},{x,baseY+height,z}});
                    for (int step=0;step<=steps;++step) {
                        const float u=float(step)/steps;
                        const float r=rootRadius+(topRadius-rootRadius)*u;
                        if (!append(x,baseY+height*u,z,r,r,r)) return result;
                    }
                }
            }
        }
    }
    for (size_t i=0;i<guide->points.size();++i) {
        if (connected[i] || settings.removeIsolated) continue;
        const auto point=guide->points[i];
        const float radius=std::clamp(settings.isolatedRadius,1.0f,1000.0f);
        const float halfThickness=CloudMapHalfThickness(settings,radius);
        if (!append(point.x,settings.bottomHeight+halfThickness,point.z,radius,halfThickness,radius)) return result;
    }
    result.connected=!result.primitives.empty();
    return result;
}
} // namespace tg::graph
