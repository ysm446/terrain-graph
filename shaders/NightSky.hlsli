#ifndef TG_NIGHT_SKY
#define TG_NIGHT_SKY
#include "NightSkyCoordinates.hlsli"
// StarCatalog（C++）と同じ配置。direction は赤道座標、illuminance は大気圏外照度 lux、color は輝度 1 の線形 sRGB。
struct StarEntry { float3 direction; float illuminance; float3 color; float padding; };
// 星表の星を点像として合計する。ピーク輝度は目の点像に相当する固定の基準幅 wRef（約 1.4 分角）で
// 照度 × 2/(pi wRef^2) と決め、画面上は画素幅の 1.5 倍以上のガウスで描く。積分光量ではなく
// 見た目のピークを解像度によらず保つ。幅は画素位置によるちらつきを抑える下限でもある。
float3 NightStars(float3 ray, float pixelWidth, AtmosphericParameters p) {
    StructuredBuffer<StarEntry> stars=ResourceDescriptorHeap[p.starBufferIndex];
    StructuredBuffer<uint> cells=ResourceDescriptorHeap[p.starCellIndex];
    float3 celestial;
    StarCelestialDirection(ray.x,ray.y,ray.z,p.starLatitude,p.starRotation,celestial.x,celestial.y,celestial.z);
    float column,row;
    StarCell(celestial.x,celestial.y,celestial.z,column,row);
    const int centerColumn=(int)column, centerRow=(int)row;
    // 幅は格子（1 度）の 1/3 までに抑え、隣接セルの走査範囲に点像が収まるようにする。
    const float reference=0.0004;
    const float width=clamp(max(pixelWidth*1.5,reference),1e-5,0.006);
    const float peak=2/(3.141592654*reference*reference);
    float3 sum=0;
    [loop] for(int r=centerRow-1;r<=centerRow+1;++r) {
        if(r<0||r>=TG_STAR_ROWS) continue;
        // 極付近は経度方向のセルが細いので、行の極側の縁の赤緯で走査列を広げる。
        const float edge=max(abs(r/(float)TG_STAR_ROWS-0.5),abs((r+1)/(float)TG_STAR_ROWS-0.5))*3.141592654;
        const int reach=min((int)ceil(1/max(cos(edge),1e-3)),TG_STAR_COLUMNS/2);
        const int span=min(2*reach+1,TG_STAR_COLUMNS);
        [loop] for(int i=0;i<span;++i) {
            const int c=(centerColumn-reach+i+TG_STAR_COLUMNS*2)%TG_STAR_COLUMNS;
            const uint cell=(uint)r*TG_STAR_COLUMNS+(uint)c;
            const uint begin=cells[cell], end=cells[cell+1];
            [loop] for(uint s=begin;s<end;++s) {
                StarEntry star=stars[s];
                const float3 offset=celestial-star.direction;
                const float d2=dot(offset,offset);
                sum+=star.color*(star.illuminance*peak*exp(-2*d2/(width*width)));
            }
        }
    }
    return sum;
}
// ピクセルシェーダ専用。星の角径を画素幅で広げた分だけ輝度を落とす。
float3 NightSky(float3 ray, AtmosphericParameters p) {
    if(p.nightEnabled==0) return 0;
    float pixelWidth=max(length(fwidth(ray)),1e-5);
    float3 moon=AtmosphereMoon(p);
    const float radius=0.0045;
    float3 right=normalize(cross(float3(0,1,0),moon));
    float3 up=cross(moon,right);
    float2 disk=float2(dot(ray,right),dot(ray,up))/radius;
    float r2=dot(disk,disk);
    float edge=1-smoothstep(1-pixelWidth/radius,1+pixelWidth/radius,sqrt(r2));
    float phaseCos=2*p.moonPhase-1;
    float3 surface=float3(disk,sqrt(saturate(1-r2)));
    float illuminated=max(dot(surface,float3(sqrt(saturate(1-phaseCos*phaseCos)),0,phaseCos)),0);
    // Lambert 球の満月時の円盤平均は 2/3。円盤と直接光の位相積分を揃える。
    float3 moonLight=AtmComputeSunTransmittance(moon,p.density,p.mie,p.altitude)*
        p.moonIlluminance*AtmosphereNightBlend(p.elevation)*1.5/(3.141592654*radius*radius);
    float visibleDisk=dot(ray,moon)>0 ? edge : 0;
    float3 result=moonLight*illuminated*visibleDisk;
    // 星は常に描き、見えるかどうかは空の輝度と露出に任せる（薄明でも明るい星から見え始める）。
    // 雲の透過率は呼び出し側で適用。
    if(p.starIntensity>0 && p.starCount>0 && ray.y>0) {
        float3 stars=NightStars(ray,pixelWidth,p);
        float3 transmission=AtmComputeSunTransmittance(ray,p.density,p.mie,p.altitude);
        result+=stars*transmission*p.starIntensity*(1-visibleDisk);
    }
    return result;
}
#endif
