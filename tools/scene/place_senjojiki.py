# 千畳敷モデルを木曽駒ヶ岳シーンへ配置する。事前にシーンをバックアップしてアプリを閉じる。
# 既存の Scatter + 小さな Mask Area で 1 点を選ぶ。モデルの尺度は固定 1。
# 単独配置ノードが無いため、位置と方位を満たす固定シードを探索する。
import argparse
import math
from pathlib import Path

from tgscene import Geo, Scene, find_root, load_elevation, load_json, sample, save_json


def scatter_hash(x, y, seed):
    h = (x*0x27d4eb2d+y*0x9e3779b9+seed*0x85ebca6b) & 0xffffffff
    h ^= h >> 16
    h = (h*0x21f0aaad) & 0xffffffff
    h ^= h >> 15
    h = (h*0x735a2d97) & 0xffffffff
    return h ^ (h >> 15)


def instance_hash(x):
    x ^= x >> 16
    x = (x*0x7feb352d) & 0xffffffff
    x ^= x >> 15
    x = (x*0x846ca68b) & 0xffffffff
    return x ^ (x >> 16)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--scene', default='Scenes/kiso-komagatake/kiso-komagatake.tgscene')
    parser.add_argument('--floor', type=float, default=2637.0, help='シーンでの床の標高。公称標高とは別。')
    args = parser.parse_args()
    root = find_root()
    terrain = Scene(root,args.scene).terrain()
    if terrain.by_note('千畳敷駅:'):
        raise SystemExit('既に配置済みです。二重配置を避けるため終了します。')
    meta = load_json(str(Path(root)/'Models/Senjojiki/Senjojiki.dimensions.json'))
    geo = Geo.of(terrain)
    u,v = geo.uv(meta['anchorLongitude'],meta['anchorLatitude'])
    x,z = geo.world_xz(u,v)
    density = 16.0
    gx,gz = math.floor(x/density),math.floor(z/density)
    # 同じセル内で位置を決め、全体を散布してもこの点だけ通る小さな面を作る。
    best = None
    for seed in range(100000):
        px = (gx+.05+(scatter_hash(gx,gz,seed)&0xffffff)/0xffffff*.9)*density
        pz = (gz+.05+(scatter_hash(gx,gz,seed+73)&0xffffff)/0xffffff*.9)*density
        distance = math.hypot(px-x,pz-z)
        if best is None or distance<best[0]:best=(distance,seed,px,pz)
        if distance<.05:break
    distance,seed,px,pz = best
    half = math.ceil(geo.size/2/density)
    index = (gz+half)*(2*half)+gx+half
    # ModelPreview.hlsl は、上向きの場合 right=-X / forward=-Z。
    # 追加乱数が pi になる種なら元の東・北の向きを保つ。
    model_seed = min(range(100000),key=lambda s:abs((instance_hash(index^s^0xa6e1)>>8)/16777216-.5))
    yaw_error = ((instance_hash(index^model_seed^0xa6e1)>>8)/16777216-.5)*360
    output = terrain.by_kind('output')[0]
    upstream,out_index = terrain.source_of(output['id'],0)
    anchor_u,anchor_v = px/geo.size+.5,pz/geo.size+.5
    origin_x,origin_y = output['position'][0],output['position'][1]+900

    def area(points, label, row, feather=0, offset=0):
        path={'points':[{'id':i+1,'u':p[0],'v':p[1],'width':1,'feather':0,'intensity':1,'heightOffset':0} for i,p in enumerate(points)],
              'edges':[{'id':len(points)+i+1,'from':i+1,'to':(i+1)%len(points)+1,'curve':'line'} for i in range(len(points))],
              'nextId':len(points)*2+1}
        p=terrain.add_node('path',2,1,(origin_x,origin_y+row),{'path':path},'千畳敷駅: '+label)
        terrain.link(terrain.heightmap_node()['id'],p['id'])
        m=terrain.add_node('maskArea',1,1,(origin_x+250,origin_y+row),
            {'maskArea':{'feather':feather,'offset':offset,'gamma':1,'invert':False}},'千畳敷駅: '+label+'の範囲')
        terrain.link(p['id'],m['id'])
        return m

    d=1.5/geo.size
    point_mask=area([(anchor_u-d,anchor_v-d),(anchor_u+d,anchor_v-d),(anchor_u+d,anchor_v+d),(anchor_u-d,anchor_v+d)],'単独配置点（固定シード）',0)
    # 建物とテラスの周囲を囲む敷地。原点中心の矩形ではなく建物外周の凸包を使う。
    points=[geo.uv(*p) for p in meta['hotelFootprint']+meta['stationFootprint']]
    h=meta['hotelFootprint']; a,b=geo.uv(*h[11]),geo.uv(*h[0])
    dx,dy=b[0]-a[0],b[1]-a[1];length=math.hypot(dx,dy)
    # UV は +v が南なので、外向きは (dy,-dx)。
    n=(dy/length*7/geo.size,-dx/length*7/geo.size)
    points += [(a[0]+n[0],a[1]+n[1]),(b[0]+n[0],b[1]+n[1])]
    ps=sorted(set(points))
    def cross(o,a,b):return (a[0]-o[0])*(b[1]-o[1])-(a[1]-o[1])*(b[0]-o[0])
    lower=[];upper=[]
    for p in ps:
        while len(lower)>=2 and cross(lower[-2],lower[-1],p)<=0:lower.pop()
        lower.append(p)
    for p in reversed(ps):
        while len(upper)>=2 and cross(upper[-2],upper[-1],p)<=0:upper.pop()
        upper.append(p)
    site=area(lower[:-1]+upper[:-1],'敷地・植生除外',400,feather=12,offset=1.5)
    template=terrain.by_kind('heightLevels')[0]['layer'].copy()
    template['heightLevels']={'autoInput':False,'fullOutput':False,'gamma':1,
        'inputMinMeters':0,'inputMaxMeters':geo.height,'outputMinMeters':args.floor-.5-geo.base,
        'outputMaxMeters':args.floor-.5-geo.base}
    grade=terrain.add_node('heightLevels',2,1,(origin_x+500,origin_y+400),{'layer':template},
        f'千畳敷駅: 敷地を標高 {args.floor-.5:.1f} m に均す（元 DEM と公称 2612 m に差あり）')
    terrain.link(upstream['id'],grade['id'],out_index=out_index);terrain.link(site['id'],grade['id'],1)
    terrain.link(grade['id'],output['id'])
    # 既存の植生だけから敷地を引く。
    for scatter in list(terrain.by_kind('scatter')):
        source,pin=terrain.source_of(scatter['id'],1)
        if source is None:continue
        blend=terrain.add_node('maskBlend',2,1,(scatter['position'][0]-220,scatter['position'][1]+180),
            {'blend':{'mode':'subtract','intensity':1}},'千畳敷駅: 敷地には植生を置かない')
        terrain.link(source['id'],blend['id'],out_index=pin);terrain.link(site['id'],blend['id'],1)
        terrain.link(blend['id'],scatter['id'],1)
    model=terrain.add_asset('models','Models/Senjojiki/Senjojiki.tgmodel')
    scatter_layer={'kind':'scatter','scatter':{'shape':'hemisphere','orientation':'flat','seed':seed,
        'density':density,'coverage':1,'sizeMin':1,'sizeMax':1,'height':0,'heightJitter':0,
        'rotationVariation':0,'aspectVariation':0,'smoothness':0}}
    points_node=terrain.add_node('scatter',3,4,(origin_x+500,origin_y),{'layer':scatter_layer},
        '千畳敷駅: 1 点のみ。間隔・シード・Mask Area を変えると位置が変わる')
    terrain.link(grade['id'],points_node['id']);terrain.link(point_mask['id'],points_node['id'],1)
    settings={'models':[{'model':model,'weight':1}],'seed':model_seed,'scaleMin':1,'scaleMax':1,
        'alignToNormal':0,'offset':.5-meta['floorFromBottom'],'usePointSize':False,'maxDistance':0,
        'autoLod':True,'lod':0,'lodBias':1}
    placed=terrain.add_node('modelScatter',1,1,(origin_x+750,origin_y),{'modelScatter':settings},
        '千畳敷駅: FBX 実寸 1 倍・鉛直。北の向きを保つ固定シード。公称標高との差は敷地側で扱う')
    terrain.link(points_node['id'],placed['id'],out_index=3)
    end=terrain.add_node('modelOutput',1,0,(origin_x+1000,origin_y),note='千畳敷駅: 駅・ホテル・テラス')
    terrain.link(placed['id'],end['id'])
    terrain.save()
    record={'scene':args.scene,'terrain':terrain.rel_path,'pointUV':[anchor_u,anchor_v],
        'pointWorldXZ':[px,pz],'positionErrorMeters':distance,'yawErrorDegrees':yaw_error,
        'scatterSeed':seed,'modelSeed':model_seed,'pointIndex':index,'densityMeters':density,
        'sceneFloorElevation':args.floor,'publishedElevation':2612,
        'rawDemElevation':float(sample(load_elevation(terrain),u,v)),'stationModelNode':placed['id']}
    save_json(str(Path(root)/'Models/Senjojiki/Senjojiki.placement.json'),record)
    print(record)


if __name__=='__main__':main()
