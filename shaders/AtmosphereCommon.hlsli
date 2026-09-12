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
    uint cloudCellCount; float proceduralBottomHeight; float proceduralBottomFeather; uint typeMask;
    uint primitiveCount; float primitiveSmoothness; float primitiveDisplacement; float primitiveDetail;
    uint primitiveBufferIndex; uint primitiveBvhIndex; uint primitiveBvhCount; uint primitiveRevision;
    float loopCenterX; float loopCenterZ; float loopWidth; float loopDepth;
    uint shapeCacheIndex; uint3 shapeCacheSize;
    float weatherType; float weatherAnvil; float weatherWisp; uint opticalCacheSize;
    float weatherStreets; float weatherVariation; float weatherDetailScale; float weatherPadding;
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
// 天候層の高さプロファイル。雲種 0: 層雲、0.5: 積雲、1: 積乱雲。雲底からの正規化高さ h に対する密度。
float WeatherHeightProfile(float h, float type) {
    float stratus=smoothstep(0.0,0.04,h)*smoothstep(0.20,0.11,h);
    float cumulus=smoothstep(0.0,0.08,h)*smoothstep(0.50,0.30,h);
    float cumulonimbus=smoothstep(0.0,0.10,h)*smoothstep(1.0,0.82,h);
    return type<0.5 ? lerp(stratus,cumulus,type*2) : lerp(cumulus,cumulonimbus,(type-0.5)*2);
}
float SampleWeatherMask(uint index, float2 uv) {
    Texture2D<float3> mask=ResourceDescriptorHeap[index];
    return saturate(mask.SampleLevel(g_samplerLinearClamp,uv,0).r);
}
// 天候層。雲量・雲種の2Dマップと高さプロファイルで広域の雲を作る（RDR2 / Nubis 方式）。
// viewDistance はカメラからの距離。遠景では細部ノイズを省き、参照回数を減らす。
float WeatherCloudDensity(float3 position, AtmosphericParameters p, uint noiseIndex, float viewDistance=0) {
    float3 offset=position-LocalCloudCenter(p);
    float3 q=abs(offset/LocalCloudRadii(p));
    if (any(q>=1)) return 0;
    if (p.distributionMask==0xfffffffe || p.typeMask==0xfffffffe) return 0; // 評価待ち。
    float2 uv=offset.xz/(2*float2(p.radiusX,p.radiusZ))+0.5;
    float coverage=p.coverage;
    if (p.distributionMask!=0xffffffff) coverage*=SampleWeatherMask(p.distributionMask,uv);
    float type=saturate(p.weatherType);
    if (p.typeMask!=0xffffffff) type*=SampleWeatherMask(p.typeMask,uv);
    if (coverage<=0.001) return 0;
    float h=(position.y-p.cloudBottom)/p.cloudThickness;
    if (h<=0 || h>=1) return 0;
    // 本体（雲量の場・1オクターブ目・塊の密度差）は cloudBodyOffset（移動量）、2オクターブ目と細部は windOffset（相対移動を引いた量）で進める。
    // 「模様を変化」がオフなら両者は一致し、形を保ったまま移動する。
    float3 uvw=(offset-float3(p.windOffsetX,0,p.windOffsetZ))/p.cloudScale;
    float3 body=(offset-float3(p.cloudBodyOffsetX,0,p.cloudBodyOffsetZ))/p.cloudScale;
    // 雲量の場は風向に沿って引き伸ばし、帯状（クラウドストリート）の並びを作る。
    float2 wind=float2(sin(p.windDirection),cos(p.windDirection));
    float2 along=dot(body.xz,wind)*wind, across=body.xz-along;
    float2 stretched=along/(1+3*p.weatherStreets)+across;
    float3 buv=float3(stretched.x,body.y*0.2,stretched.y);
    // 雲量は低周波の場で地域差を付ける。平均が指定値になり、値が高い場所に塊が集まり低い場所は空く。
    // 雲種が高いほど場のスケールを大きく、コントラストを強くし、少数の大きな塔と広い晴れ間を作る。
    float fieldScale=1-0.45*type;
    float broad=SampleCloudNoise(buv*0.17*fieldScale+0.11,noiseIndex,0)*0.6+SampleCloudNoise(buv*0.41*fieldScale+0.53,noiseIndex,0)*0.4;
    broad=saturate((broad-0.5)*(1+2.5*type)+0.5);
    // 塊ごとに頂上の高さを変え、平らな天井を避ける。雲量が高い場所ほど高く成長する。
    float top=lerp(lerp(0.6,0.35,type),1.0,saturate(broad*1.2-0.1));
    float profile=WeatherHeightProfile(h/top,type);
    if (profile<=0.001) return 0;
    coverage=saturate(coverage*2*broad*(0.9+0.2*type));
    // 積乱雲の上部を横へ広げる（かなとこ雲）。
    float anvil=p.weatherAnvil*saturate((type-0.5)*2)*smoothstep(0.55,0.9,h);
    coverage=saturate(coverage*(1+anvil));
    if (coverage<=0.001) return 0;
    // 刻み幅（距離適応）に対して各ノイズ成分を帯域制限し、カメラ移動時のちらつきを抑える。
    // 最小構造は Perlin-Worley の 16 セルで周期/16。刻みがその半分を超えたら平均へ寄せる。
    float stepLength=max(p.weatherDetailScale*0.03,viewDistance*0.003*64.0/max(p.samples,16u));
    float fade2=saturate(2-2*stepLength/(p.cloudScale/2.3/16));
    // 周期の異なる2オクターブで繰り返しを崩す。
    // 1オクターブ目は雲の本体なので移動量で進め、2オクターブ目と細部だけを相対移動で進めて形を変える。
    float shape=CloudShapeNoise(body,p,noiseIndex)*0.65+lerp(0.7,CloudShapeNoise(uvw*2.3+float3(0.29,0.71,0.13),p,noiseIndex),fade2)*0.35;
    // ノイズの平均が約0.7と高いので、0.5付近を中心へ戻して雲量の閾値と釣り合わせる。
    shape=saturate((shape-0.4)/0.6);
    float base=saturate(shape*profile);
    // Nubis 方式: 雲量の閾値で切り、雲量が低い場所ほど薄くする。
    float density=saturate((base-(1-coverage))/max(coverage,1e-4))*sqrt(coverage);
    if (density<=0) return 0;
    // 細部の削り。雲底付近は筋状にほどけ、上部は丸い膨らみを残す。
    // 周期は「細部の大きさ」で独立に指定。遠景では細部を省き、平均値相当で薄く削る。
    float erosion=p.detailStrength*lerp(1+p.weatherWisp,0.5,saturate(h*3));
    float detailFade=1-smoothstep(12000,30000,viewDistance);
    // 刻み幅が細部の最小構造（周期/16）の半分を超えると平均へ寄せ、サンプリングのちらつきを抑える。
    detailFade*=saturate(2-2*stepLength/(p.weatherDetailScale/16));
    float coarse=density*(1-erosion*0.35);
    if (detailFade>0.001) {
        float3 duv=(offset-float3(p.windOffsetX,0,p.windOffsetZ))/max(p.weatherDetailScale,10)+0.173;
        // 最小構造の細かい侵食チャンネルではなく、Perlin-Worley（セル 4/8/16）を細部に使う。
        float detail=SampleCloudNoise(duv,noiseIndex,1);
        float dn=lerp(1-detail,detail,saturate(h*8));
        float eroded=saturate((density-erosion*dn)/max(1-erosion*dn,1e-3));
        density=lerp(coarse,eroded,detailFade);
    } else density=coarse;
    // 塊ごとの密度差。均一な綿の板にならないよう、厚い塊と薄い塊を混ぜる。
    float variation=SampleCloudNoise(body*0.9+0.77,noiseIndex,0);
    density*=lerp(1,lerp(0.35,1.15,variation),p.weatherVariation);
    float edge=saturate((1-max(q.x,q.z))/max(p.edgeSoftness,0.01));
    return density*edge;
}
float CloudDensity(float3 position, AtmosphericParameters p, uint noiseIndex, float viewDistance=0) {
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
    if (p.localCloud == 4) return WeatherCloudDensity(position,p,noiseIndex,viewDistance);
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
    if (p.cloudMotionMode == 3 || p.localCloud == 3 || p.localCloud == 2 || p.localCloud == 4 || (p.localCloud == 1 && p.flatCloudBottom != 0)) {
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
    return (p.localCloud==2 || p.localCloud==3 || p.localCloud==4) && (asuint(p.shapeStrength)&0x80000000)!=0 && asuint(p.shapeStrength)!=0xffffffff;
}
// 天候層の照明キャッシュは風の移動に合わせて格子を1ボクセル未満の範囲でずらし、
// 雲が固定格子を横切るときに三線形補間で陰影が泳ぐのを防ぐ。
float3 CloudCacheShift(AtmosphericParameters p) {
    if (p.localCloud!=4) return 0;
    float n=max(p.opticalCacheSize&0xffffu,2u);
    float2 voxel=2*CloudRenderRadii(p).xz/(n-1);
    // 主要な構造は雲量の場なので、その移動量に格子を追従させる。
    float2 wind=float2(p.cloudBodyOffsetX,p.cloudBodyOffsetZ);
    float2 shift=wind-floor(wind/voxel)*voxel;
    return float3(shift.x,0,shift.y);
}
float3 SampleCloudOpticalCache(float3 position, AtmosphericParameters p) {
    float3 uvw=saturate((position-CloudCacheShift(p)-CloudRenderCenter(p))/CloudRenderRadii(p)*0.5+0.5);
    // XZ の格子数は範囲に応じて変わる。Y は 32 固定。
    float n=max(p.opticalCacheSize&0xffffu,2u);
    float ny=max(p.opticalCacheSize>>16,2u);
    float2 uv=(uvw.xy*float2(n-1,ny-1)+0.5)/float2(n,ny);
    float z=uvw.z*(n-1);
    Texture2DArray<float4> cache=ResourceDescriptorHeap[asuint(p.shapeStrength)&0x7fffffff];
    return lerp(cache.SampleLevel(g_samplerLinearClamp,float3(uv,floor(z)),0).rgb,
                cache.SampleLevel(g_samplerLinearClamp,float3(uv,min(floor(z)+1,n-1)),0).rgb,frac(z));
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
    // 天候層は距離適応の刻み。近景は細部の大きさに合わせて細かく、遠景ほど長く進める。
    float minStep=0, stepGrowth=0;
    if (p.localCloud==4) {
        minStep=max(p.weatherDetailScale*0.03,8);
        stepGrowth=0.003*64.0/max(p.samples,16u);
        count=2048;
    }
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
    float t=start;
    [loop] for(uint i=0;i<count && transmission>0.005;++i) {
        if (p.localCloud==4) {
            if (t>=end) break;
            stepLength=min(max(minStep,t*stepGrowth),end-t);
        }
        float sampleDistance=t+0.5*stepLength;
        float3 pos=origin+ray*sampleDistance;
        t+=stepLength;
        float emptyDistance=0;
        float density=p.localCloud==3 ? ProceduralCloudDensity(pos,p,noiseIndex,emptyDistance)
                                    : CloudDensity(pos,p,noiseIndex,sampleDistance);
        if(density<=0) {
            uint skip=(uint)min(floor(emptyDistance/stepLength),float(count-1-i));
            i+=skip; t+=skip*stepLength;
            continue;
        }
        float3 depths;
        if (HasCloudOpticalCache(p)) depths=SampleCloudOpticalCache(pos,p);
        else depths=float3(CloudOpticalDepth(pos,sun,p,noiseIndex,max(p.samples/2,16u)),
            CloudOpticalDepth(pos,float3(0,1,0),p,noiseIndex,8),
            CloudOpticalDepth(pos,float3(0,-1,0),p,noiseIndex,8));
        float sunDepth=depths.x, topDepth=depths.y, bottomDepth=depths.z;
        // 天候層は Nubis の in-scatter 確率で高次散乱と天空光を減らす。
        // 雲底と薄い縁は暗く、厚い塊の内部ほど明るい。単散乱はそのまま残す。
        float inScatter=1;
        if (p.localCloud==4) {
            float typeTop=p.weatherType<0.5 ? lerp(0.2,0.5,p.weatherType*2) : lerp(0.5,1.0,(p.weatherType-0.5)*2);
            float hh=saturate((pos.y-p.cloudBottom)/(p.cloudThickness*typeTop));
            // 密度は輪郭付近で小さいので2倍して評価し、上面の明るさを残す。
            float depthProbability=0.05+pow(saturate(density*2),lerp(0.5,2.0,smoothstep(0.3,0.85,hh)));
            float verticalProbability=pow(lerp(0.1,1.0,smoothstep(0.07,0.14,hh)),0.8);
            inScatter=lerp(1,depthProbability*verticalProbability,0.75);
        }
        // 多重散乱のオクターブ近似。太陽光の寄与は上で位相へ適用済み。
        // 地形の直射影にはこの散乱光を使わず、元の Beer 透過率だけを使う。
        float3 light=0;
        [unroll] for(uint order=0;order<4;++order) {
            float attenuation=exp2(-(float)order);
            float weight=attenuation;
            float scale=order==0 ? 1 : inScatter;
            light+=sunlight*phases[order]*exp(-sunDepth*attenuation)*scale
                +weight*0.5*(skyAbove*exp(-topDepth*attenuation)+skyBelow*exp(-bottomDepth*attenuation))*inScatter;
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
