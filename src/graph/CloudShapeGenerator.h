#pragma once

#include "graph/NodeGraph.h"
#include <algorithm>
#include <cmath>
#include <numbers>

namespace tg::graph {
// 雲種ごとの縦方向の比率と塔の設定。Houdini の Humilis / Mediocris / Congestus に対応する目安。
struct CloudSpeciesProfile {
    float heightRatio;   // 土台の垂直半径 / 基本半径
    float towerHeight;   // 塔の高さ / 基本半径
    float towersPerUnit; // 土台の面積（基本半径²の倍数）あたりの塔の本数
};
inline CloudSpeciesProfile CloudSpecies(int species) {
    switch (std::clamp(species,0,2)) {
    case 0: return {0.35f,0.0f,0.0f};
    case 1: return {0.55f,0.7f,2.0f};
    default: return {0.75f,1.8f,3.0f};
    }
}

// 単独の積雲を球の集合として生成する。土台の楕円体内へ間隔に応じた球を敷き詰め、
// 雲種に応じた上向きの塔を立て、必要なら二次形状を表面へ追加する。
inline CompiledCloud GenerateCloudShape(const CloudShapeGenerateSettings& settings,GraphId nodeId) {
    CompiledCloud result;
    result.smoothness=std::clamp(settings.smoothness,0.0f,500.0f);
    const float size=std::clamp(settings.size,10.0f,5000.0f);
    const float rx=size*std::clamp(settings.length,0.1f,5.0f);
    const float rz=size*std::clamp(settings.width,0.1f,5.0f);
    const auto profile=CloudSpecies(settings.species);
    const float ry=size*profile.heightRatio;
    const float separation=std::clamp(settings.pointSeparation,0.05f,1.0f);
    const float distortion=std::clamp(settings.distortion,0.0f,1.0f);
    const float sphereRadius=std::max(1.0f,size*separation);
    const float step=sphereRadius*1.5f;
    uint32_t state=static_cast<uint32_t>(settings.seed)*747796405u+2891336453u;
    const auto random=[&]() { state=state*1664525u+1013904223u; return float(state>>8)/16777216.0f; };
    const auto symmetric=[&]() { return random()*2-1; };
    const float scaleMin=std::clamp(std::min(settings.scaleMin,settings.scaleMax),0.1f,3.0f);
    const float scaleMax=std::clamp(std::max(settings.scaleMin,settings.scaleMax),0.1f,3.0f);
    const auto scaleFactor=[&]() { return settings.randomScale ? scaleMin+(scaleMax-scaleMin)*random() : 1.0f; };

    struct Local { float x,y,z,r; };
    std::vector<Local> shapes;
    const auto budgetExceeded=[&](size_t count) { return count*sizeof(CloudPrimitive)>CloudShapeMemoryBudget; };

    // 土台。格子を乱して楕円体の内側だけを残す。
    const int nx=std::max(1,static_cast<int>(std::ceil(rx/step)));
    const int ny=std::max(1,static_cast<int>(std::ceil(ry/step)));
    const int nz=std::max(1,static_cast<int>(std::ceil(rz/step)));
    if (budgetExceeded(size_t(2*nx+1)*size_t(2*ny+1)*size_t(2*nz+1))) { result.shapeOverflow=true; return result; }
    for (int iy=-ny;iy<=ny;++iy) for (int iz=-nz;iz<=nz;++iz) for (int ix=-nx;ix<=nx;++ix) {
        const float x=ix*step+symmetric()*step*0.5f*distortion;
        const float y=iy*step+symmetric()*step*0.5f*distortion;
        const float z=iz*step+symmetric()*step*0.5f*distortion;
        const float inner=(x*x)/(rx*rx)+(y*y)/(ry*ry)+(z*z)/(rz*rz);
        if (inner>1.0f) continue;
        // 外縁ほど小さくして丸みを出す。
        const float radius=sphereRadius*(1.0f-0.35f*inner)*(1.0f+0.25f*symmetric()*distortion)*scaleFactor();
        shapes.push_back({x,y,z,std::max(1.0f,radius)});
    }
    if (shapes.empty()) shapes.push_back({0,0,0,std::min({rx,ry,rz})});

    // 塔。土台の内側から上へ球列を伸ばし、先端へ向けて細くする。
    if (profile.towerHeight>0) {
        const float area=(rx/size)*(rz/size);
        const int towers=std::max(1,static_cast<int>(std::lround(profile.towersPerUnit*area)));
        const float towerHeight=size*profile.towerHeight;
        for (int tower=0;tower<towers;++tower) {
            const float angle=random()*2*std::numbers::pi_v<float>;
            const float radial=std::sqrt(random())*0.6f;
            const float baseX=rx*radial*std::cos(angle),baseZ=rz*radial*std::sin(angle);
            const float height=towerHeight*(0.45f+0.55f*random())*(1.0f-radial*0.5f);
            const float rootRadius=sphereRadius*1.6f,topRadius=sphereRadius*0.9f;
            const int steps=std::max(1,static_cast<int>(std::ceil(height/(sphereRadius*1.2f))));
            const float lean=distortion*sphereRadius;
            const float leanX=symmetric()*lean,leanZ=symmetric()*lean;
            for (int stepIndex=0;stepIndex<=steps;++stepIndex) {
                const float u=float(stepIndex)/steps;
                const float r=(rootRadius+(topRadius-rootRadius)*u)*scaleFactor();
                shapes.push_back({baseX+leanX*u*3+symmetric()*lean*0.5f,
                    ry*0.5f+height*u,baseZ+leanZ*u*3+symmetric()*lean*0.5f,std::max(1.0f,r)});
                if (budgetExceeded(shapes.size())) { result.shapeOverflow=true; return result; }
            }
        }
    }

    // 二次形状。上半球方向へ子球を積み、繰り返しごとに小さくする。
    if (settings.secondaryShapes) {
        const int iterations=std::clamp(settings.iterations,1,3);
        const float displacement=std::clamp(settings.displacement,0.0f,1.0f);
        const float spread=std::clamp(settings.spread,0.0f,1.0f);
        size_t begin=0,end=shapes.size();
        for (int iteration=0;iteration<iterations;++iteration) {
            for (size_t i=begin;i<end;++i) {
                const Local parent=shapes[i];
                for (int child=0;child<2;++child) {
                    const float theta=random()*2*std::numbers::pi_v<float>;
                    const float tilt=spread*random()*std::numbers::pi_v<float>*0.5f;
                    const float dx=std::sin(tilt)*std::cos(theta),dy=std::cos(tilt),dz=std::sin(tilt)*std::sin(theta);
                    const float r=parent.r*0.6f*scaleFactor();
                    const float reach=parent.r*(0.5f+displacement);
                    shapes.push_back({parent.x+dx*reach,parent.y+dy*reach,parent.z+dz*reach,std::max(1.0f,r)});
                    if (budgetExceeded(shapes.size())) { result.shapeOverflow=true; return result; }
                }
            }
            begin=end; end=shapes.size();
        }
    }

    // 下側の切り取り。平面より下の中心は捨て、平面にかかる球は持ち上げる。
    const float flatten=std::clamp(settings.flattenBottom,0.0f,0.9f);
    const float planeY=-ry+flatten*2*ry;
    const float rotation=settings.rotation*std::numbers::pi_v<float>/180.0f;
    const float c=std::cos(rotation),s=std::sin(rotation);
    for (const auto& local:shapes) {
        float y=local.y;
        if (y<planeY) continue;
        if (y-local.r<planeY) y=planeY+local.r;
        const float x=local.x*c-local.z*s,z=local.x*s+local.z*c;
        result.primitives.push_back({settings.centerX+x,settings.centerY+y,settings.centerZ+z,0,
            local.r,local.r,local.r,0,nodeId,static_cast<uint32_t>(result.primitives.size())});
    }
    result.connected=!result.primitives.empty();
    return result;
}
} // namespace tg::graph
