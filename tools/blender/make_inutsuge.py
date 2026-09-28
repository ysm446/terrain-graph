# 西伊豆の風衝地を想定したイヌツゲ。小葉の密な丸い樹冠を重ねる。
# Blender -b --factory-startup --python tools/blender/make_inutsuge.py -- --out data/Models/Inutsuge
# 葉の形の根拠: https://www.ffpri.go.jp/kys/business/jumokuen/jumoku/zukan/inutuge.html
# 樹形は docs/references/nishiizu/190628_Abarth_drive_02m.jpg を参考にした造形。
import math
import os
import random
import sys

import bpy
import numpy as np
from mathutils import Vector

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vegetation import (LEAVES, UP, Geometry, LeafCanvas, add_core, add_tube_lod,
                        asset_ref, build_hull, export_fbx, finish_cutout, height_to_normal,
                        make_core_material, make_material, make_object, parse_args,
                        perpendicular, source_ref, tiling_noise, to_bytes, write_material,
                        write_model, write_png)

CARD_SIZE = 0.22  # 小葉が互生する小枝。葉1枚は約2〜3 cm。
CORE_COLOR = (0.018, 0.032, 0.009)
# 同じ樹冠の形と枝を使い、房の点列を段階的に間引く。
LODS = [(1, 1.0, 2, 6, 1500), (2, 1.35, 2, 4, 800), (4, 1.85, 2, 3, 400)]


def oval_leaf(canvas, base, angle, length, color):
    """丸い先端・浅い鋸歯・弱い中肋。大きく尖った広葉樹の葉と区別する。"""
    direction = np.array([math.sin(angle), -math.cos(angle)], np.float32)
    across = np.array([-direction[1], direction[0]], np.float32)
    reach = length * 1.5
    x0, x1 = max(0, int(base[0]-reach)), min(canvas.size, int(base[0]+reach+1))
    y0, y1 = max(0, int(base[1]-reach)), min(canvas.size, int(base[1]+reach+1))
    if x0 >= x1 or y0 >= y1:
        return
    ys, xs = np.mgrid[y0:y1, x0:x1].astype(np.float32)
    rx, ry = xs+.5-base[0], ys+.5-base[1]
    s = (rx*direction[0]+ry*direction[1])/length
    v = rx*across[0]+ry*across[1]
    sc = np.clip(s, 0, 1)
    half = length*.30*np.sqrt(np.clip(1-(2*sc-1)**2, 0, 1))
    half *= 1+.025*np.sin(sc*45)
    coverage = np.clip(half-np.abs(v)+.5, 0, 1)*((s>=0)&(s<=1))
    side = np.clip(v/np.maximum(half, .01), -1, 1)
    rgb = np.asarray(color)*(0.88+.12*(1-side*side))[..., None]
    rgb += np.clip(1-np.abs(v), 0, 1)[..., None]*np.array([.015,.02,.007])
    tilt = side*.30
    normal = np.stack([across[0]*tilt,-across[1]*tilt,np.ones_like(s)],axis=-1)
    normal /= np.linalg.norm(normal,axis=-1,keepdims=True)
    canvas._blend((slice(y0,y1),slice(x0,x1)),coverage,rgb,normal)


def leaf_texture(seed):
    rng = random.Random(seed)
    c = LeafCanvas(512)
    for branch in range(5):
        x = 256+(branch-2)*76
        bottom = np.array([256.,500.])
        top = np.array([float(x),rng.uniform(32,95)])
        c.stroke(bottom,top,3,1,(.09,.10,.035))
        for i in range(13):
            t = .22+.75*i/12
            p = bottom+(top-bottom)*t
            angle = (-1 if i%2 else 1)*rng.uniform(.7,1.25)
            # 小葉の密な樹冠を、笹より暗い黄緑として読む。
            color = np.array([.32,.39,.13])*rng.uniform(.88,1.12)
            oval_leaf(c,p,angle,rng.uniform(45,68),color)
    return finish_cutout(c.color,c.alpha,c.normal)


def crown(seed):
    """大小の扁平な房を不規則に寄せた樹冠。等間隔の輪に並べると左右対称のお椀形（丸すぎる）になるため、
    高さ方向の分布・半径・大きさをばらつかせ、風衝地らしく上段を風下へ傾けて風上側を刈り込む。"""
    rng = random.Random(seed)
    width = rng.uniform(1.75, 2.15)      # 樹冠の裾の半径（m）
    height = rng.uniform(2.45, 2.85)     # 房の中心が届く高さ（m）
    wind = rng.random() * math.tau        # 風下の向き
    windward = Vector((-math.cos(wind), -math.sin(wind), 0))
    lean = Vector((math.cos(wind), math.sin(wind), 0)) * rng.uniform(.35, .55)
    lobes = []

    def add(center, size, flat):
        radii = Vector((size, size * rng.uniform(.8, 1.2), size * flat))
        lobes.append((center, radii))

    # 主な房。上ほど細く小さく、中段に多く集める。
    for _ in range(24):
        t = rng.betavariate(1.8, 1.8)
        radius = width * math.sqrt(rng.random()) * (1.0 - .6 * t ** 1.4)
        angle = rng.random() * math.tau
        offset = Vector((radius * math.cos(angle), radius * math.sin(angle), 0))
        # 風上側は枝先が枯れて詰まる。
        if offset.length > 0 and offset.normalized().dot(windward) > .3:
            offset *= rng.uniform(.6, .8)
        center = offset + Vector((0, 0, .6 + t * (height - .6))) + lean * t
        add(center, rng.uniform(.42, .88) * (1.0 - .25 * t), rng.uniform(.62, .82))
    # 裾の房。風下と横の側だけ地面近くまで葉を下ろす（全周を埋めると塊に見えるので、幹の根元は所々覗かせる）。
    # 樹冠から離れた球にならないよう、その向きにある低めの房の真下へ付ける。
    low = [c for c, r in lobes if c.z < .6 + .45 * (height - .6)]
    for i in range(4):
        angle = wind + rng.uniform(-1.9, 1.9)
        direction = Vector((math.cos(angle), math.sin(angle), 0))
        above = max(low, key=lambda c: Vector((c.x, c.y, 0)).dot(direction))
        center = Vector((above.x * 1.05, above.y * 1.05, rng.uniform(.42, .62)))
        add(center, rng.uniform(.36, .55), rng.uniform(.6, .75))
    # 縁の小さな房。輪郭をでこぼこにする（外へ少し突き出す）。
    for _ in range(8):
        base = lobes[rng.randrange(len(lobes))]
        c, r = base
        direction = Vector((rng.uniform(-1, 1), rng.uniform(-1, 1), rng.uniform(-.2, .7))).normalized()
        center = c + Vector((direction.x * r.x, direction.y * r.y, direction.z * r.z)) * rng.uniform(.75, .95)
        add(center, rng.uniform(.24, .38), rng.uniform(.65, .85))
    return lobes


def build(seed, lobes, lod):
    step, scale, cards, sides, _ = lod
    geo=Geometry()
    # 幹を先に出力してマテリアルスロットを Bark / Leaves / Core に揃える。
    for j,(center,radii) in enumerate(lobes):
        base=Vector((.15*math.sin(j),.15*math.cos(j),0))
        bend=Vector((center.x*.35,center.y*.35,.55))
        points=[base,base.lerp(bend,.5),bend,bend.lerp(center,.55),center]
        add_tube_lod(geo,points,[.045,.037,.026,.016,.005],sides,1,.5)
    for j,(center,radii) in enumerate(lobes):
        rng=random.Random(seed*100+j)
        count=int(4*math.pi*radii.x*radii.y/(CARD_SIZE*CARD_SIZE)*1.7)
        for i in range(count):
            z=1-2*(i+.5)/count
            phi=i*2.3999632297+rng.uniform(-.16,.16)
            radial=math.sqrt(max(0,1-z*z))
            n=Vector((radial*math.cos(phi),radial*math.sin(phi),z))
            offset=Vector((n.x*radii.x,n.y*radii.y,n.z*radii.z))
            # 殻からの出入りを大きくし、ところどころ間引いて、球の縁がくっきり出ないようにする。
            point=center+offset*rng.uniform(.84,1.08)
            # 他のこぶの深い内側は描かず、葉の枚数を表面へ回す。
            hidden=any(k!=j and sum(((point[a]-c[a])/r[a])**2 for a in range(3))<.82**2
                       for k,(c,r) in enumerate(lobes))
            roll=rng.random()*math.tau
            if hidden or i%step or rng.random()<.12:
                continue
            normal=(Vector((n.x/radii.x,n.y/radii.y,n.z/radii.z))+UP*.22).normalized()
            tangent=perpendicular(normal)
            across=normal.cross(tangent)
            along=tangent*math.cos(roll)+across*math.sin(roll)
            across=normal.cross(along).normalized()
            size=CARD_SIZE*scale
            for k in range(cards):
                side=across if k==0 else (across*.55+normal*.835).normalized()
                corners=[point-along*size/2-side*size/2,point-along*size/2+side*size/2,
                         point+along*size/2+side*size/2,point+along*size/2-side*size/2]
                first=len(geo.verts)
                geo.verts.extend(tuple(v) for v in corners)
                geo.add_face([first,first+1,first+2,first+3],LEAVES,
                             [(0,0),(1,0),(1,1),(0,1)],[normal]*4)
    return geo


def main():
    options=parse_args();out,root=options['out'],options['root']
    os.makedirs(out,exist_ok=True)
    bpy.ops.wm.read_factory_settings(use_empty=True)
    paths={key:os.path.join(out,'T_Inutsuge_'+key+'.png') for key in ['Leaves_D','Leaves_N','Bark_D','Bark_N']}
    leaves,normal=leaf_texture(options['seed'])
    write_png(paths['Leaves_D'],leaves);write_png(paths['Leaves_N'],normal)
    rng=np.random.default_rng(options['seed'])
    noise=tiling_noise(rng,512,.04,.1)
    bark=np.clip(np.array([.19,.18,.155])*(1+noise[...,None]*.1),0,1)
    write_png(paths['Bark_D'],to_bytes(bark))
    write_png(paths['Bark_N'],to_bytes(height_to_normal(noise,1.2)*.5+.5))
    mats=[make_material('Bark',paths['Bark_D'],paths['Bark_N'],False),
          make_material('Leaves',paths['Leaves_D'],paths['Leaves_N'],True),make_core_material(CORE_COLOR)]
    mats[0].node_tree.nodes.get('Principled BSDF').inputs['Roughness'].default_value = .8
    mats[1].node_tree.nodes.get('Principled BSDF').inputs['Roughness'].default_value = .82
    print(f'Leaf alpha coverage: {np.mean(leaves[...,3] >= 128):.3f}')
    refs=[]
    for slot,roughness,color in [('Bark',.8,(1,1,1)),('Leaves',.82,(1,1,1)),('Core',.9,CORE_COLOR)]:
        path=os.path.join(out,'MI_Inutsuge_'+slot+'.tgmat')
        write_material(path,'MI_Inutsuge_'+slot,
                       None if slot=='Core' else source_ref(paths[slot+'_D'],root),
                       None if slot=='Core' else source_ref(paths[slot+'_N'],root),
                       roughness,.5 if slot=='Leaves' else 0,slot=='Leaves',color)
        refs.append(asset_ref(path,root))
    for i in range(1,options['variants']+1):
        seed=options['seed']*1000+i;lobes=crown(seed);name=f'Inutsuge_Var{i}'
        # 芯は葉の殻より十分内側。メタボールを大きく連結させない。
        hull=build_hull('Core_'+name,[(c,min(r)*.65) for c,r in lobes],1,.05)
        objects=[]
        for level,lod in enumerate(LODS):
            geo=build(seed,lobes,lod)
            add_core(geo,hull,lod[4],.025)
            objects.append(make_object(name+f'_LOD{level}',geo,mats,((i-1)*7,level*7,0)))
            print(f'{name}_LOD{level}: {geo.triangles()} triangles, {geo.face_mat.count(LEAVES)} cards')
        fbx=os.path.join(out,name+'.fbx');export_fbx(objects,fbx)
        write_model(os.path.join(out,name+'.tgmodel'),name,source_ref(fbx,root),refs)
        bpy.data.meshes.remove(hull)
    bpy.ops.wm.save_as_mainfile(filepath=os.path.join(out,'Inutsuge.blend'))


if __name__=='__main__':
    main()
