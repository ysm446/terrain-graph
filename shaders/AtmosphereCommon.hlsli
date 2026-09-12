#ifndef TG_ATMOSPHERE_COMMON
#define TG_ATMOSPHERE_COMMON
#include "CloudLimits.hlsli"
#include "EnvCommon.hlsli"
#include "AtmosphereScattering.hlsli"
#include "CloudScattering.hlsli"
struct CloudPrimitive { float4 center; float4 radius; };
struct CloudBvhNode { float4 lower; float4 upper; uint start; uint count; uint escape; uint padding; };
struct AtmosphericParameters {
    float azimuth; float elevation; float illuminance; float density;
    float mie; float eccentricity; float altitude; float groundAlbedo;
    uint clouds; float coverage; float extinction; float cloudBottom;
    float cloudThickness; float cloudScale; uint seed; uint samples;
    float fieldCenterX; float fieldCenterZ; float fieldRadius; float fieldFalloff;
    float windSpeed; float windDirection; uint animateClouds; float windOffsetX;
    float windOffsetZ; uint lowerHemisphere; float noiseSpeedRatio; uint distributionMask;
    uint localCloud; float radiusX; float radiusZ; float edgeSoftness;
    float shapeStrength; float detailStrength; uint cloudMotionMode; uint cloudSource;
    float flatCloudBottom; float cloudSkylightIntensity; float cloudBodyOffsetX; float cloudBodyOffsetZ;
    float indirectLight; float ambientLight; uint cloudCellIndex; uint cloudNoiseType;
    uint cloudCellCount; float proceduralBottomHeight; float proceduralBottomFeather; uint cellPadding;
    uint primitiveCount; float primitiveSmoothness; float primitiveDisplacement; float primitiveDetail;
    uint primitiveBufferIndex; uint primitiveBvhIndex; uint primitiveBvhCount; uint primitiveRevision;
    float loopCenterX; float loopCenterZ; float loopWidth; float loopDepth;
    uint shapeCacheIndex; uint3 shapeCacheSize;
};
float3 AtmosphereSun(AtmosphericParameters p) {
    return float3(cos(p.elevation) * sin(p.azimuth), sin(p.elevation), cos(p.elevation) * cos(p.azimuth));
}
// ローカル雲は全軸で同じワールド周期を使う。Y も厚さから独立した 3D ノイズ。
float SampleCloudNoise(float3 uvw, uint noiseIndex, uint channel = 0) {
    Texture2DArray<float4> noise = ResourceDescriptorHeap[noiseIndex];
    float z = frac(uvw.z) * 64 - 0.5;
    float a = noise.SampleLevel(g_samplerLinearWrap, float3(uvw.xy, ((int)floor(z)+64)%64), 0)[channel];
    float b = noise.SampleLevel(g_samplerLinearWrap, float3(uvw.xy, ((int)floor(z)+65)%64), 0)[channel];
    return lerp(a,b,frac(z));
}
float CloudShapeNoise(float3 uvw, AtmosphericParameters p, uint noiseIndex) {
    if (p.cloudNoiseType == 1) return SampleCloudNoise(uvw, noiseIndex, 1);
    return saturate((SampleCloudNoise(uvw, noiseIndex) - 0.5) * 3 + 0.5);
}
float CloudDetailNoise(float3 uvw, AtmosphericParameters p, uint noiseIndex) {
    if (p.cloudNoiseType == 1) return SampleCloudNoise(uvw, noiseIndex, 2);
    return saturate((SampleCloudNoise(uvw, noiseIndex) - 0.5) * 3 + 0.5);
}
float3 LocalCloudCenter(AtmosphericParameters p) {
    return float3(p.fieldCenterX, p.cloudBottom + p.cloudThickness * 0.5, p.fieldCenterZ);
}
float3 LocalCloudRadii(AtmosphericParameters p) {
    return float3(p.radiusX, p.cloudThickness * 0.5, p.radiusZ);
}
// ベイクは元の範囲を維持し、表示・照明だけループ範囲で評価する。
float3 CloudRenderCenter(AtmosphericParameters p) {
    float3 center=LocalCloudCenter(p);
    if (p.cloudMotionMode==3) center.xz=float2(p.loopCenterX,p.loopCenterZ);
    return center;
}
float3 CloudRenderRadii(AtmosphericParameters p) {
    float3 radii=LocalCloudRadii(p);
    if (p.cloudMotionMode==3) radii.xz=float2(p.loopWidth,p.loopDepth)*0.5;
    return radii;
}
bool LoopCloudPosition(inout float3 position, AtmosphericParameters p, out float seamDistance) {
    seamDistance=1e9;
    if (p.cloudMotionMode!=3) return true;
    float2 extent=max(float2(p.loopWidth,p.loopDepth),1);
    float2 lower=float2(p.loopCenterX,p.loopCenterZ)-extent*0.5;
    float2 uv=(position.xz-lower)/extent;
    if (any(uv<0) || any(uv>1)) return false;
    float2 local=frac((position.xz-lower-float2(p.windOffsetX,p.windOffsetZ))/extent)*extent;
    position.xz=lower+local;
    // 循環の継ぎ目の向こうには別の雲があるため、空間スキップを継ぎ目までに制限する。
    float2 edge=min(local,extent-local);
    seamDistance=min(edge.x,edge.y);
    return true;
}
float CloudBottomFade(float h, float shape, float detail) {
    // 同じ高さを基準に、浅い起伏と薄い密度の縁を残す。既存ノイズを共有する。
    float bottom = 0.02 + 0.16*(1-shape) + 0.03*(1-detail);
    return smoothstep(bottom,bottom+0.08,h);
}
float LocalCloudDensity(float3 position, AtmosphericParameters p, uint noiseIndex) {
    float3 offset = position - LocalCloudCenter(p);
    float3 q = offset / LocalCloudRadii(p);
    float h = (position.y-p.cloudBottom)/p.cloudThickness;
    if (p.flatCloudBottom != 0) {
        if (h <= 0 || h >= 1) return 0;
        // 雲底を半楕円体の底面に置く。下部ノイズは高さを固定し、底の凹凸を抑える。
        q.y = lerp(q.y,h,p.flatCloudBottom);
        offset.y = (lerp(h,max(h,0.12),p.flatCloudBottom)-0.5)*p.cloudThickness;
    }
    float edge = 1 - length(q);
    if (edge <= 0) return 0;
    float3 wind = p.cloudMotionMode != 0 ? float3(p.windOffsetX,0,p.windOffsetZ) : 0;
    float3 uvw = (offset-wind) / p.cloudScale + 0.5;
    float shape = CloudShapeNoise(uvw,p,noiseIndex);
    float detail = CloudDetailNoise(uvw*3.1+0.173,p,noiseIndex);
    // 輪郭は交差区間の内側だけを削る。
    float density = saturate((edge - p.shapeStrength*(1-shape)*0.65) / p.edgeSoftness);
    density = saturate(density - p.detailStrength*(1-detail)*(1-density));
    return density * (p.flatCloudBottom != 0 ? CloudBottomFade(h/max(p.flatCloudBottom,1e-5),shape,detail) : 1);
}
// 雲層の塊配置。呼び出し側でセル数に合わせて座標を折り返す。
float3 CloudCellRandom(int2 cell, uint seed) {
    uint2 wrapped=uint2(cell);
    uint value=wrapped.x*1597334677u+wrapped.y*3812015801u+seed*2798796415u;
    value=(value^(value>>16))*2246822519u;
    uint a=(value^(value>>13))*3266489917u;
    uint b=(a^(a>>16))*2246822519u;
    uint c=(b^(b>>13))*3266489917u;
    return float3(a&65535u,b&65535u,c&65535u)/65535.0;
}
// シードにのみ依存する最大 32×32 セル分の情報。
struct CloudCellData { float4 placement; float4 shape; float4 lobe; };
CloudCellData BuildCloudCell(int2 cell, uint seed) {
    float3 random=CloudCellRandom(cell,seed);
    float3 dimensions=CloudCellRandom(cell,seed+137u);
    float3 lobes=CloudCellRandom(cell,seed+719u);
    CloudCellData data;
    data.placement=float4(0.5+(random.xy-0.5)*0.9,lerp(0.38,0.95,sqrt(random.z)),lerp(0.28,1.0,dimensions.z));
    float sine,cosine; sincos(dimensions.x*6.2831853,sine,cosine);
    data.shape=float4(sine,cosine,lerp(0.45,1.0,dimensions.y),lerp(0.45,1.0,lobes.y));
    data.lobe=float4(lerp(-0.35,0.35,lobes.x),0,0,0);
    return data;
}
CloudCellData LoadCloudCell(int2 cell, AtmosphericParameters p) {
    int count = clamp((int)p.cloudCellCount, 1, 32);
    int2 wrapped=(cell%count+count)%count;
    if (p.cloudCellIndex==0xffffffff) return BuildCloudCell(wrapped,p.seed);
    Texture2DArray<float4> cells=ResourceDescriptorHeap[p.cloudCellIndex];
    CloudCellData data;
    data.placement=cells.Load(int4(wrapped,0,0));
    data.shape=cells.Load(int4(wrapped,1,0));
    data.lobe=cells.Load(int4(wrapped,2,0));
    return data;
}
float CloudLayerBody(float2 position, float h, AtmosphericParameters p, float distribution) {
    int2 cell=int2(floor(position));
    float body=-1;
    // 半径は 1 セル未満。隣接セルも評価し、セル境界で塊を切らない。
    [unroll] for(int z=-1;z<=1;++z) [unroll] for(int x=-1;x<=1;++x) {
        int2 neighbor=cell+int2(x,z);
        CloudCellData data=LoadCloudCell(neighbor,p);
        float2 center=float2(neighbor)+data.placement.xy;
        float radius=(0.65+0.3*sqrt(p.coverage))*data.placement.z;
        float sine=data.shape.x,cosine=data.shape.y;
        float2 delta=position-center;
        float2 rotated=float2(cosine*delta.x+sine*delta.y,-sine*delta.x+cosine*delta.y);
        float2 axes=radius*float2(data.shape.z,1);
        float height=data.placement.w;
        float vertical=lerp(2*h-1,h,p.flatCloudBottom)/height;
        float2 horizontal=rotated/axes;
        float cluster=1-length(float3(horizontal,vertical));
        // 異なる高さの膨らみを重ね、ひとつの半楕円体の頂点を崩す。
        // 各ローブの水平支持範囲は主塊の半径内に収める。
        float2 lobeOffset=float2(0.35,data.lobe.x);
        float lobeHeight=data.shape.w;
        float lobeVertical=lerp(2*h-1,h,p.flatCloudBottom)/lobeHeight;
        float lobe=1-length(float3((horizontal-lobeOffset)/float2(0.55,0.6),lobeVertical));
        float blend=saturate(0.5+0.5*(lobe-cluster)/0.25);
        cluster=lerp(cluster,lobe,blend)+0.25*blend*(1-blend);
        body=max(body,cluster);
    }
    // マスクの灰色は塊の輪郭を縮める。内部の密度を一律には薄めない。
    return body-(1-sqrt(p.coverage*distribution));
}
// 64 枚の 2D 配列で周期 3D 密度を持つ。XY はハードウェア補間、Z のみ手動補間。
// 楕円体の距離近似をmで評価。最も近い2距離だけを保持するため入力順に依存しない。
void AccumulateCloudPrimitive(float3 position, AtmosphericParameters p, uint i,
                              inout float distance, inout float second, inout float emptyDistance) {
    StructuredBuffer<CloudPrimitive> primitives=ResourceDescriptorHeap[p.primitiveBufferIndex];
    CloudPrimitive primitive=primitives[i];
    float3 radii=max(primitive.radius.xyz,1);
    float3 offset=position-primitive.center.xyz;
    float k0=length(offset/radii), k1=length(offset/(radii*radii));
    // 1-Lipschitzな距離下界。unionの膨張と最大変位を引けば安全に空白を飛ばせる。
    emptyDistance=min(emptyDistance,min(radii.x,min(radii.y,radii.z))*(k0-1));
    float d=k1>1e-7 ? k0*(k0-1)/k1 : -min(radii.x,min(radii.y,radii.z));
    if (d<distance) { second=distance; distance=d; }
    else second=min(second,d);
}
// 最も近い2距離の多項式smooth minimum。膨張は形状数に依存せず最大 k/4。
float CloudSmoothUnionExpansion(AtmosphericParameters p) { return p.primitiveSmoothness*0.25; }
float CloudSmoothUnion(float distance, float second, float k) {
    if (k<=0) return distance;
    float h=max(k-(second-distance),0)/k;
    return distance-h*h*k*0.25;
}
float CloudBvhLowerBound(float3 position, CloudBvhNode node) {
    return length(max(max(node.lower.xyz-position,position-node.upper.xyz),0))*node.lower.w-node.upper.w;
}
// 距離と安全な空白距離の下界を格子に保存する。ノイズは含めない。
float ProceduralCloudShape(float3 position, AtmosphericParameters p, out float emptyDistance) {
    float distance=1e9, second=1e9;
    emptyDistance=1e9;
    const float expansion=CloudSmoothUnionExpansion(p)+p.primitiveDisplacement;
    if (p.primitiveBvhCount>0) {
        StructuredBuffer<CloudBvhNode> nodes=ResourceDescriptorHeap[p.primitiveBvhIndex];
        float lower=CloudBvhLowerBound(position,nodes[0]);
#ifndef TG_BAKE_CLOUD_SHAPE
        if (lower>expansion+0.01) { emptyDistance=lower; return lower-CloudSmoothUnionExpansion(p); }
#endif
        uint nodeIndex=0;
        [loop] while (nodeIndex<p.primitiveBvhCount) {
            CloudBvhNode node=nodes[nodeIndex];
            lower=CloudBvhLowerBound(position,node);
            // 2番目の距離より遠い、または最小距離+kより遠い部分木は結果に影響しないため厳密に除外できる。
            if (lower>min(second,distance+p.primitiveSmoothness)+0.01) {
                emptyDistance=min(emptyDistance,lower);
                nodeIndex=node.escape;
            } else {
                [loop] for (uint i=node.start;i<node.start+node.count;++i)
                    AccumulateCloudPrimitive(position,p,i,distance,second,emptyDistance);
                ++nodeIndex;
            }
        }
    } else {
        [loop] for(uint i=0;i<p.primitiveCount;++i)
            AccumulateCloudPrimitive(position,p,i,distance,second,emptyDistance);
    }
    return CloudSmoothUnion(distance,second,p.primitiveSmoothness);
}
float ProceduralCloudSourceDensity(float3 position, AtmosphericParameters p, uint noiseIndex, out float emptyDistance) {
    if (p.flatCloudBottom!=0 && position.y<=p.proceduralBottomHeight) {
        emptyDistance=max(0,p.proceduralBottomHeight-position.y);
        return 0;
    }
    float distance;
    if (p.shapeCacheIndex != 0xffffffff) {
        float3 uvw=(position-LocalCloudCenter(p))/LocalCloudRadii(p)*0.5+0.5;
        emptyDistance=0;
        if (any(uvw<0) || any(uvw>1)) {
            emptyDistance=length(max(abs(position-LocalCloudCenter(p))-LocalCloudRadii(p),0));
            return 0;
        }
        float3 grid=uvw*float3(p.shapeCacheSize-1);
        float2 uv=(grid.xy+0.5)/float2(p.shapeCacheSize.xy);
        Texture2DArray<float2> cache=ResourceDescriptorHeap[p.shapeCacheIndex];
        float2 value=lerp(cache.SampleLevel(g_samplerLinearClamp,float3(uv,floor(grid.z)),0),
            cache.SampleLevel(g_samplerLinearClamp,float3(uv,min(floor(grid.z)+1,p.shapeCacheSize.z-1)),0),frac(grid.z));
        distance=value.x;
        // 補間点から格子頂点までの最大距離を引き、細い雲を飛び越さない。
        float diagonal=length(2*LocalCloudRadii(p)/float3(p.shapeCacheSize-1));
        emptyDistance=value.y-diagonal;
    } else distance=ProceduralCloudShape(position,p,emptyDistance);
    emptyDistance=max(0,emptyDistance-CloudSmoothUnionExpansion(p)-p.primitiveDisplacement-0.01);
    if (distance>p.primitiveDisplacement) return 0;
    float3 noisePosition=position;
    if (p.cloudMotionMode==3) noisePosition-=float3(p.cloudBodyOffsetX,0,p.cloudBodyOffsetZ);
    float3 uvw=noisePosition/(4*max(p.cloudScale,1));
    // 大きな輪郭の変位と小さな侵食を分離。雲全体で同じワールド座標を使う。
    // 2は手続き雲の従来Perlin。0/1は既存のfBM / Perlin-Worleyと共通。
    uint shapeChannel=p.cloudNoiseType==2 ? 3u : (p.cloudNoiseType==1 ? 1u : 0u);
    uint detailChannel=p.cloudNoiseType==1 ? 2u : shapeChannel;
    distance+=((SampleCloudNoise(uvw,noiseIndex,shapeChannel)*0.7+SampleCloudNoise(uvw*2+0.37,noiseIndex,shapeChannel)*0.3)*2-1)*p.primitiveDisplacement;
    distance+=(1-SampleCloudNoise(uvw*3.1+0.173,noiseIndex,detailChannel))*p.primitiveDetail;
    float density=smoothstep(0,max(p.edgeSoftness,1),-distance);
    if (p.flatCloudBottom!=0 && p.proceduralBottomFeather>0)
        density*=smoothstep(0,p.proceduralBottomFeather,position.y-p.proceduralBottomHeight);
    return density;
}
float ProceduralCloudDensity(float3 position, AtmosphericParameters p, uint noiseIndex, out float emptyDistance) {
    float seamDistance;
    emptyDistance=0;
    if (!LoopCloudPosition(position,p,seamDistance)) return 0;
    float density=ProceduralCloudSourceDensity(position,p,noiseIndex,emptyDistance);
    emptyDistance=min(emptyDistance,seamDistance);
    return density;
}
float CloudDensity(float3 position, AtmosphericParameters p, uint noiseIndex) {
    if (p.clouds == 0) return 0;
    if (p.localCloud==3) { float emptyDistance; return ProceduralCloudDensity(position,p,noiseIndex,emptyDistance); }
    if (p.cloudMotionMode==3) {
        float seamDistance;
        if (!LoopCloudPosition(position,p,seamDistance)) return 0;
        p.cloudMotionMode=1;
        p.windOffsetX=p.cloudBodyOffsetX;
        p.windOffsetZ=p.cloudBodyOffsetZ;
        p.cloudBodyOffsetX=p.cloudBodyOffsetZ=0;
    }
    if (p.localCloud == 1) return LocalCloudDensity(position,p,noiseIndex);
    if (p.localCloud == 2) {
        float3 offset = position - LocalCloudCenter(p);
        float3 q = abs(offset / LocalCloudRadii(p));
        if (any(q >= 1)) return 0;
        if (p.distributionMask == 0xfffffffe) return 0; // 評価待ち。
        float distribution = 1;
        if (p.distributionMask != 0xffffffff) {
            Texture2D<float3> mask = ResourceDescriptorHeap[p.distributionMask];
            float2 uv = offset.xz / (2 * float2(p.radiusX,p.radiusZ)) + 0.5;
            distribution = saturate(mask.SampleLevel(g_samplerLinearClamp,uv,0).r);
        }
        if (distribution <= 0.001 || p.coverage <= 0) return 0;
        float h = (position.y-p.cloudBottom)/p.cloudThickness;
        float3 noiseOffset = offset;
        if (p.flatCloudBottom != 0)
            noiseOffset.y = (lerp(h,max(h,0.12),p.flatCloudBottom)-0.5)*p.cloudThickness;
        float3 uvw = (noiseOffset-float3(p.windOffsetX,0,p.windOffsetZ))/p.cloudScale+0.5;
        // 低周波の座標変形で大きな輪郭を曲げる。変形後の座標でセルを選ぶ。
        // 周波数 0.5 は既存の 10 周期の巻き戻しと整合する。
        float broadX=SampleCloudNoise(uvw*0.5,noiseIndex);
        float broadZ=SampleCloudNoise(uvw*0.5+float3(0.37,0.19,0.61),noiseIndex);
        float2 bodyPosition=(offset.xz-float2(p.cloudBodyOffsetX,p.cloudBodyOffsetZ))/p.cloudScale;
        bodyPosition+=(float2(broadX,broadZ)-0.5)*1.6;
        float body=CloudLayerBody(bodyPosition,h,p,distribution);
        if(body<=0) return 0;
        // 表面だけでなく中規模の入り江や切れ目も作り、丸い土台を残さない。
        float shape = CloudShapeNoise(uvw*2+0.317,p,noiseIndex);
        float detail = CloudDetailNoise(uvw*3.1+0.173,p,noiseIndex);
        float profile = saturate((1-q.y)/max(p.edgeSoftness,0.01));
        if (p.flatCloudBottom != 0)
            profile = CloudBottomFade(h/max(p.flatCloudBottom,1e-5),shape,detail)
                *lerp(profile,saturate(2*(1-h)/max(p.edgeSoftness,0.01)),p.flatCloudBottom);
        float edge = saturate((1-max(q.x,q.z))/max(p.edgeSoftness,0.01));
        float density = saturate((body-0.48*(1-shape))/max(p.edgeSoftness,0.01));
        density = saturate(density-p.detailStrength*(1-detail)*(1-density));
        return density * profile * edge;
    }
    float h = (position.y - p.cloudBottom) / p.cloudThickness;
    if (h <= 0 || h >= 1 || p.clouds == 0 || p.coverage <= 0) return 0;
    float2 wind = float2(p.windOffsetX,p.windOffsetZ);
    float3 uvw = float3((position.x-wind.x) / p.cloudScale, h, (position.z-wind.y) / p.cloudScale);
    float n = SampleCloudNoise(uvw, noiseIndex, p.cloudNoiseType == 1 ? 1 : 0);
    float falloff = min(p.fieldFalloff, p.fieldRadius);
    float fieldFade = saturate((p.fieldRadius-length(position.xz-float2(p.fieldCenterX,p.fieldCenterZ)))/max(falloff,1));
    // 移植元の積雲プロファイル。高さの分布を適用してから雲量の閾値を引く。
    float profile = saturate(h*5)*saturate(1-h)*1.6;
    // 雲量の閾値で残った密度を 0〜1 へ戻す。本体と影の消散係数は共通。
    return saturate((saturate(n*profile)-(1-p.coverage))/max(p.coverage,1e-4))*fieldFade;
}
// 視線・自己遮蔽・地形の雲影は同じ有限円柱との交差を使う。
bool CloudInterval(float3 origin, float3 ray, float limit, AtmosphericParameters p,
                   out float start, out float end) {
    start=0; end=limit;
    if (p.cloudMotionMode == 3 || p.localCloud == 3 || p.localCloud == 2 || (p.localCloud == 1 && p.flatCloudBottom != 0)) {
        float3 offset = origin-CloudRenderCenter(p);
        float3 radii = CloudRenderRadii(p);
        [unroll] for (uint axis=0; axis<3; ++axis) {
            if (abs(ray[axis]) < 1e-7) {
                if (abs(offset[axis]) >= radii[axis]) return false;
            } else {
                float a=(-radii[axis]-offset[axis])/ray[axis];
                float b=(radii[axis]-offset[axis])/ray[axis];
                start=max(start,min(a,b)); end=min(end,max(a,b));
            }
        }
        return end>start;
    }
    if (p.localCloud == 1) {
        float3 offset = (origin-LocalCloudCenter(p))/LocalCloudRadii(p);
        float3 direction = ray/LocalCloudRadii(p);
        float a=dot(direction,direction), b=dot(offset,direction), c=dot(offset,offset)-1;
        float discriminant=b*b-a*c;
        if (discriminant<0 || a<=0) return false;
        float root=sqrt(discriminant);
        start=max(0,(-b-root)/a); end=min(limit,(-b+root)/a);
        return end>start;
    }
    if (abs(ray.y)<1e-6) {
        if (origin.y<=p.cloudBottom || origin.y>=p.cloudBottom+p.cloudThickness) return false;
    } else {
        float a=(p.cloudBottom-origin.y)/ray.y, b=(p.cloudBottom+p.cloudThickness-origin.y)/ray.y;
        start=max(start,min(a,b)); end=min(end,max(a,b));
    }
    float2 offset=origin.xz-float2(p.fieldCenterX,p.fieldCenterZ);
    float a=dot(ray.xz,ray.xz), b=dot(offset,ray.xz);
    float c=dot(offset,offset)-p.fieldRadius*p.fieldRadius;
    if(a<1e-12) {
        if(c>0) return false;
    } else {
        float discriminant=b*b-a*c;
        if(discriminant<0) return false;
        float root=sqrt(discriminant);
        start=max(start,(-b-root)/a); end=min(end,(-b+root)/a);
    }
    return end>start;
}
// 形状群では大きな包囲箱の厚さを刻み幅に使わず、ノイズと密度境界を解像する。
uint CloudMarchCount(float distance, AtmosphericParameters p, uint baseSamples) {
    if (p.localCloud==3) {
        float stepLength=max(5,min(p.edgeSoftness*0.5,p.cloudScale*0.05));
        return (uint)clamp(ceil(distance/stepLength),baseSamples,512u);
    }
    return (uint)clamp(ceil(distance/(p.cloudThickness/baseSamples)),baseSamples,baseSamples*4);
}
float CloudOpticalDepth(float3 origin, float3 ray, AtmosphericParameters p, uint noiseIndex, uint baseSamples) {
    float start,end;
    if(!CloudInterval(origin,ray,1e9,p,start,end)) return 0;
    // 長い斜光路でも雲層の出口まで積分し、自己遮蔽と地形影の減衰を揃える。
    uint count=CloudMarchCount(end-start,p,baseSamples);
    float stepLength=(end-start)/count, optical=0;
    [loop] for(uint i=0;i<count;++i) {
        float3 position=origin+ray*(start+(i+0.5)*stepLength);
        float emptyDistance=0;
        float density=p.localCloud==3 ? ProceduralCloudDensity(position,p,noiseIndex,emptyDistance)
                                    : CloudDensity(position,p,noiseIndex);
        optical+=density*stepLength;
        if (density<=0) i+=(uint)min(floor(emptyDistance/stepLength),float(count-1-i));
    }
    return optical*p.extinction;
}
// 雲層専用。shapeStrength と同じ領域をキャッシュ SRV として使用する。
bool HasCloudOpticalCache(AtmosphericParameters p) {
    return (p.localCloud==2 || p.localCloud==3) && (asuint(p.shapeStrength)&0x80000000)!=0 && asuint(p.shapeStrength)!=0xffffffff;
}
float3 SampleCloudOpticalCache(float3 position, AtmosphericParameters p) {
    float3 uvw=saturate((position-CloudRenderCenter(p))/CloudRenderRadii(p)*0.5+0.5);
    float2 uv=(uvw.xy*float2(63,31)+0.5)/float2(64,32);
    float z=uvw.z*63;
    Texture2DArray<float4> cache=ResourceDescriptorHeap[asuint(p.shapeStrength)&0x7fffffff];
    return lerp(cache.SampleLevel(g_samplerLinearClamp,float3(uv,floor(z)),0).rgb,
                cache.SampleLevel(g_samplerLinearClamp,float3(uv,min(floor(z)+1,63)),0).rgb,frac(z));
}
float CloudShadow(float3 position, AtmosphericParameters p, uint noiseIndex) {
    float3 sun=AtmosphereSun(p);
    if(p.clouds==0 || sun.y<=0.001) return 1;
    if (HasCloudOpticalCache(p)) {
        float start,end;
        if (!CloudInterval(position,sun,1e9,p,start,end)) return 1;
        return exp(-SampleCloudOpticalCache(position+sun*start,p).x);
    }
    return exp(-CloudOpticalDepth(position,sun,p,noiseIndex,max(p.samples/2,16u)));
}
float4 IntegrateCloud(float3 origin, float3 ray, float limit, AtmosphericParameters p,
                      uint noiseIndex, uint lightingIndex) {
    if(p.clouds==0) return float4(0,0,0,1);
    float start,end;
    if(!CloudInterval(origin,ray,limit,p,start,end)) return float4(0,0,0,1);
    uint count=CloudMarchCount(end-start,p,p.samples);
    if (p.localCloud==3) count=(uint)clamp(ceil((end-start)/max(5,min(p.edgeSoftness*0.5,p.cloudScale*0.05))),p.samples,2048u);
    float stepLength=(end-start)/count;
    float3 sun=AtmosphereSun(p);
    float3 sunlight=AtmComputeSunTransmittance(sun,p.density,p.mie,p.altitude+p.cloudBottom)*p.illuminance;
    float mu=dot(ray,sun);
    Texture2D<float4> cloudLighting=ResourceDescriptorHeap[lightingIndex];
    // 地形と共通の倍率を天空照明にだけ適用。太陽光と光学的厚さは変えない。
    float3 skyAbove=cloudLighting.Load(int3(0,0,0)).rgb*p.cloudSkylightIntensity*p.ambientLight;
    float3 skyBelow=cloudLighting.Load(int3(1,0,0)).rgb*p.cloudSkylightIntensity*p.ambientLight;
    float4 phases=float4(CloudPhase(mu,1),CloudPhase(mu,0.5),CloudPhase(mu,0.25),CloudPhase(mu,0.125));
    // 寄与と消散を分離するオクターブ近似。0.85 は有限次数で失われる光の調整値。
    // 単散乱は維持し、高次の等方化した太陽光を残す（設計資料 atmospheric-sky.md）。
    const float contribution=0.85;
    phases*=float4(1,contribution,contribution*contribution,contribution*contribution*contribution);
    phases.yzw*=p.indirectLight; // 雲自体の明るさを高次散乱だけで調整する。
    float transmission=1; float3 radiance=0;
    [loop] for(uint i=0;i<count && transmission>0.005;++i) {
        float3 pos=origin+ray*(start+(i+0.5)*stepLength);
        float emptyDistance=0;
        float density=p.localCloud==3 ? ProceduralCloudDensity(pos,p,noiseIndex,emptyDistance)
                                    : CloudDensity(pos,p,noiseIndex);
        if(density<=0) {
            i+=(uint)min(floor(emptyDistance/stepLength),float(count-1-i));
            continue;
        }
        float3 depths;
        if (HasCloudOpticalCache(p)) depths=SampleCloudOpticalCache(pos,p);
        else depths=float3(CloudOpticalDepth(pos,sun,p,noiseIndex,max(p.samples/2,16u)),
            CloudOpticalDepth(pos,float3(0,1,0),p,noiseIndex,8),
            CloudOpticalDepth(pos,float3(0,-1,0),p,noiseIndex,8));
        float sunDepth=depths.x, topDepth=depths.y, bottomDepth=depths.z;
        // 多重散乱のオクターブ近似。太陽光の寄与は上で位相へ適用済み。
        // 地形の直射影にはこの散乱光を使わず、元の Beer 透過率だけを使う。
        float3 light=0;
        [unroll] for(uint order=0;order<4;++order) {
            float attenuation=exp2(-(float)order);
            float weight=attenuation;
            light+=sunlight*phases[order]*exp(-sunDepth*attenuation)
                +weight*0.5*(skyAbove*exp(-topDepth*attenuation)+skyBelow*exp(-bottomDepth*attenuation));
        }
        // 区間内一定の密度・光源に対する解析積分（Beer-Lambert）。
        float opacity=1-exp(-density*p.extinction*stepLength);
        radiance+=transmission*opacity*light;
        transmission*=1-opacity;
    }
    return float4(radiance,transmission);
}
float3 AtmosphericSky(float3 ray, AtmosphericParameters p, uint lutIndex, float3 groundRadiance=0) {
    Texture2D<float4> lut=ResourceDescriptorHeap[lutIndex];
    // 空の延長は初期実装と同じ上半球の折り返し。背景・IBL・雲照明で共用する。
    float3 sampleRay=ray;
    if(p.lowerHemisphere==0 && ray.y<0)
        sampleRay=normalize(float3(ray.x,max(-ray.y,0.005),ray.z));
    float3 sky=AtmComputeScattering(sampleRay,AtmosphereSun(p),p.density,p.mie,p.eccentricity,
        lut,g_samplerLinearClamp,true,p.altitude,groundRadiance);
    if(p.lowerHemisphere==0 && ray.y<0)
        sky*=lerp(1,p.groundAlbedo,smoothstep(0,0.08,-ray.y));
    return max(0,sky*p.illuminance);
}
#endif
