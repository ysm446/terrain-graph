# 千畳敷駅・ホテル千畳敷の外観モデル。Blender 5.x のバックグラウンドで実行する。
# 外周: OpenStreetMap contributors（ODbL）、way 187898269 / 187898270、2026-10-05 取得。
# 高さ・窓・屋根・テラスは公開写真からの推定。測量モデルではない。
# blender -b --factory-startup --python-exit-code 1 --python tools/blender/make_senjojiki.py -- --out data/Models/Senjojiki
import argparse
import json
import math
import sys
from pathlib import Path

import bmesh
import bpy
import numpy as np
from mathutils import Vector

sys.path.insert(0, str(Path(__file__).parent))
from vegetation import asset_ref, source_ref, write_json, write_material, write_model

HOTEL = [(137.8139279,35.777868),(137.8140015,35.7777664),(137.8137964,35.7776769),
         (137.8137817,35.7776495),(137.813713,35.7775374),(137.8137573,35.7775211),
         (137.8137265,35.777482),(137.8134829,35.7776072),(137.8135166,35.7776566),
         (137.8135749,35.7776321),(137.8136225,35.777699),(137.8136628,35.7777523)]
STATION = [(137.8134829,35.7776072),(137.8137265,35.777482),(137.8136884,35.7774319),
           (137.8136556,35.7773863),(137.8134082,35.777507)]
LON0, LAT0 = 137.81365, 35.77765
M_PER_LON = 111320 * math.cos(math.radians(LAT0))
M_PER_LAT = 110574
FLOOR_ELEVATION = 2612.0


def local(p):
    return Vector(((p[0]-LON0)*M_PER_LON, (p[1]-LAT0)*M_PER_LAT, 0))


def mesh(name, vertices, faces, material):
    data = bpy.data.meshes.new(name)
    data.from_pydata(vertices, [], faces)
    data.materials.append(material)
    bm = bmesh.new()
    bm.from_mesh(data)
    bmesh.ops.recalc_face_normals(bm, faces=list(bm.faces))
    bm.to_mesh(data)
    bm.free()
    data.update()
    uv = data.uv_layers.new(name="UVMap")
    for poly in data.polygons:
        axis = max(range(3), key=lambda i: abs(poly.normal[i]))
        axes = [i for i in range(3) if i != axis]
        for li in poly.loop_indices:
            v = data.vertices[data.loops[li].vertex_index].co
            uv.data[li].uv = (v[axes[0]]/4, v[axes[1]]/4)
    obj = bpy.data.objects.new(name, data)
    bpy.context.collection.objects.link(obj)
    return obj


def prism(name, polygon, bottom, top, material):
    n = len(polygon)
    verts = [(p.x,p.y,z) for z in (bottom,top) for p in polygon]
    faces = [tuple(reversed(range(n))),tuple(range(n,2*n))]
    faces += [(i,(i+1)%n,(i+1)%n+n,i+n) for i in range(n)]
    return mesh(name, verts, faces, material)


def box(name, center, size, material, angle=0):
    x,y,z = size
    a = Vector((math.cos(angle),math.sin(angle),0))*x/2
    b = Vector((-math.sin(angle),math.cos(angle),0))*y/2
    c = Vector(center)
    return prism(name,[c-a-b,c+a-b,c+a+b,c-a+b],c.z-z/2,c.z+z/2,material)


def beam(name, a, b, width, material):
    a,b = Vector(a),Vector(b)
    axis = b-a
    bpy.ops.mesh.primitive_cube_add(size=1, location=(a+b)/2)
    obj = bpy.context.object
    obj.name = name
    obj.rotation_euler = axis.to_track_quat('Z','Y').to_euler()
    obj.scale = (width,width,axis.length)
    bpy.ops.object.transform_apply(location=False,rotation=False,scale=True)
    obj.data.materials.append(material)
    return obj


def gable(name,a,b,width,eaves,rise,roof,wall,details):
    axis = (b-a).normalized()
    inward = Vector((axis.y,-axis.x,0))
    # a,b は北西側の軒。棟は長辺に沿う。
    left,right = a-axis*.45,b+axis*.45
    for side in (0,1):
        edge = inward*(-.5 if side == 0 else width+.5)
        verts = [left+edge+Vector((0,0,eaves)),right+edge+Vector((0,0,eaves)),
                 right+inward*width/2+Vector((0,0,eaves+rise)),left+inward*width/2+Vector((0,0,eaves+rise))]
        obj=mesh(name,verts,[(0,1,2,3)],roof)
        solid=obj.modifiers.new('RoofThickness','SOLIDIFY');solid.thickness=.14
    for p in (a,b):
        mesh(name+'_gable',[p+Vector((0,0,eaves)),p+inward*width+Vector((0,0,eaves)),
             p+inward*width/2+Vector((0,0,eaves+rise))],[(0,1,2)],wall)
    beam(name+'_ridge',left+inward*width/2+Vector((0,0,eaves+rise+.07)),
         right+inward*width/2+Vector((0,0,eaves+rise+.07)),.12,roof)
    if details:
        for t in np.arange(0,(right-left).length,.65):
            p=left+axis*float(t)
            for side in (0,1):
                beam(name+'_seam',p+inward*(-.5 if side==0 else width+.5)+Vector((0,0,eaves+.08)),
                     p+inward*width/2+Vector((0,0,eaves+rise+.08)),.035,roof)


def window(name,a,b,t,z,width,height,materials,details=True):
    axis=(b-a).normalized(); outward=Vector((-axis.y,axis.x,0))
    p=a+(b-a)*t+outward*.065+Vector((0,0,z))
    angle=math.atan2(axis.y,axis.x)
    box(name+'_reveal',p,(width+.17,.13,height+.17),materials['Trim'],angle)
    box(name+'_glass',p+outward*.08,(width,.04,height),materials['Glass'],angle)
    if details:
        for off in (-width/2,0,width/2):
            box(name+'_mullion',p+axis*off+outward*.12,(.055,.06,height),materials['Trim'],angle)
        box(name+'_sill',p+Vector((0,0,-height/2)) + outward*.12,(width+.22,.27,.08),materials['Trim'],angle)


def make_building(m, lod):
    h,s = [local(p) for p in HOTEL],[local(p) for p in STATION]
    detailed=lod==0
    prism('Hotel',h,0,7.1,m['Wall'])
    prism('HotelRoofApron',h,7.10,7.18,m['Roof'])
    prism('HotelFoundation',h,-10,0,m['Concrete'])
    prism('Station',s,-2,12.4,m['StationWall'])
    prism('StationFoundation',s,-10,-2,m['Concrete'])
    # ホテル主棟、接続棟、駅舎。外周の方位を保つ。
    gable('HotelRoof',h[11],h[0],13,7.1,1.7,m['Roof'],m['Wall'],detailed)
    gable('ConnectorRoof',h[9],h[11],16.2,7.2,1.0,m['Roof'],m['Wall'],detailed)
    gable('StationRoof',s[0],s[1],12.6,12.4,2.7,m['StationRoof'],m['StationWall'],detailed)
    for a,b in [(h[11],h[0]),(h[1],h[2])]:
        for i in range(8):
            window('GuestWindow',a,b,(i+.5)/8,5.5,2.15,1.55,m,detailed)
            window('CafeWindow',a,b,(i+.5)/8,1.95,2.55,3.15,m,detailed)
    for a,b in [(h[0],h[1]),(h[9],h[11]),(h[3],h[4])]:
        count=max(2,round((b-a).length/3.2))
        for i in range(count):
            for z in (1.8,5.35):window('SideWindow',a,b,(i+.5)/count,z,1.65,1.4,m,detailed)
    # 駅舎の小窓。ロープウェイの開口は山麓側（南東端）。
    for a,b in [(s[4],s[0]),(s[0],s[1])]:
        for z in (4.8,9.0):
            for i in range(4):window('StationWindow',a,b,(i+1)/5,z,.65,.7,m,detailed)
    a,b=s[1],s[3]
    for t in (.28,.72):window('RopewayPortal',a,b,t,2.7,3.6,5.4,m,detailed)
    # カール側の木製テラス（2023 年改修後）。寸法・段の曲線は写真による近似。
    a,b=h[11],h[0];axis=(b-a).normalized();normal=Vector((-axis.y,axis.x,0))
    length=(b-a).length
    for step in range(3):
        w=6.5-step*.5
        prism('TerraceStep',[a-axis*.5,b+axis*.5,b+axis*.5+normal*w,a-axis*.5+normal*w],
              -.8,.05+step*.16,m['Deck'])
    # テラス下の基礎。地形との隙間を塞ぐスカート。
    prism('TerraceFoundation',[a,b,b+normal*6.4,a+normal*6.4],-10,-.75,m['Concrete'])
    if detailed:
        for t in np.arange(0,length,.18):
            p=a+axis*float(t)
            beam('DeckJoint',p+normal*.12+Vector((0,0,.381)),p+normal*5.45+Vector((0,0,.381)),.012,m['Dark'])
        for end in (a,b):
            for t in np.linspace(0,6,5):
                p=end+normal*float(t)
                beam('RailPost',p+Vector((0,0,.4)),p+Vector((0,0,1.5)),.045,m['Trim'])
            for z in (.8,1.5):beam('Rail',end+Vector((0,0,z)),end+normal*6+Vector((0,0,z)),.045,m['Trim'])
        for t in (.18,.50,.80):
            p=a+(b-a)*t+normal*3.5
            box('BenchSeat',p+Vector((0,0,.84)),(2.0,.45,.12),m['Deck'],math.atan2(axis.y,axis.x))
            for x in (-.7,.7):box('BenchLeg',p+axis*x+Vector((0,0,.59)),(.1,.35,.5),m['Dark'])
        for a,b in [(h[11],h[0]),(h[9],h[11])]:
            for t in (.05,.95):
                p=a+(b-a)*t
                beam('Downpipe',p+Vector((0,0,.1)),p+Vector((0,0,7.1)),.10,m['Trim'])
        # 駅舎の屋根設備と避雷針。
        p=(s[0]+s[1]+s[3]+s[4])/4
        for off in (-3,3):
            q=p+Vector((off,0,15.2))
            box('RoofVent',q,(.85,.85,.7),m['StationRoof'])
        beam('Antenna',p+Vector((0,0,15)),p+Vector((0,0,18)),.055,m['Trim'])
    # 入口の庇と扉。
    a,b=h[9],h[11];p=a+(b-a)*.35;axis=(b-a).normalized();normal=Vector((-axis.y,axis.x,0))
    window('Entrance',a,b,.35,1.4,2.3,2.8,m,detailed)
    angle=math.atan2(axis.y,axis.x)
    box('EntranceCanopy',p+normal*1.6+Vector((0,0,3.1)),(4.8,3.2,.20),m['Roof'],angle)
    for x in (-2.15,2.15):
        q=p+axis*x+normal*2.8
        beam('CanopyPost',q,q+Vector((0,0,3.1)),.16,m['Trim'])


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--out',required=True)
    parser.add_argument('--root',default='data')
    args=parser.parse_args(sys.argv[sys.argv.index('--')+1:])
    out=Path(args.out).resolve();out.mkdir(parents=True,exist_ok=True)
    root=Path(args.root).resolve()
    bpy.ops.object.select_all(action='SELECT');bpy.ops.object.delete(use_global=False)
    scene=bpy.context.scene;scene.unit_settings.system='METRIC';scene.unit_settings.scale_length=1
    # リニア値。写真の明るさそのものではなく、屋外 PBR 用の反射率で指定する。
    specs={'Wall':((.56,.30,.20),.83),'StationWall':((.39,.14,.075),.8),
           'Roof':((.24,.045,.029),.52),'StationRoof':((.13,.17,.19),.46),
           'Glass':((.035,.075,.085),.19),'Trim':((.48,.50,.48),.42),
           'Concrete':((.21,.20,.18),.95),'Deck':((.24,.10,.039),.84),
           'Dark':((.025,.028,.026),.7)}
    mats={};refs=[]
    for name,(color,rough) in specs.items():
        mat=bpy.data.materials.new(name);mat.diffuse_color=(*color,1);mat.use_nodes=True
        bsdf=mat.node_tree.nodes.get('Principled BSDF');bsdf.inputs['Base Color'].default_value=(*color,1)
        bsdf.inputs['Roughness'].default_value=rough
        # ガラスもアプリの不透明 PBR と一致させる。
        mats[name]=mat
        path=out/f'MI_Senjojiki_{name}.tgmat'
        write_material(str(path),f'Senjojiki {name}',None,None,rough,0,False,color)
        refs.append(asset_ref(str(path),str(root)))
    objects=[];counts=[]
    for lod in range(2):
        before=set(bpy.data.objects)
        make_building(mats,lod)
        parts=[o for o in bpy.data.objects if o not in before]
        bpy.ops.object.select_all(action='DESELECT')
        for obj in parts:
            obj.select_set(True);bpy.context.view_layer.objects.active=obj
            for mod in list(obj.modifiers):bpy.ops.object.modifier_apply(modifier=mod.name)
        bpy.context.view_layer.objects.active=parts[0]
        bpy.ops.object.join();obj=bpy.context.object;obj.name=f'Senjojiki_LOD{lod}'
        # 材質スロットを tgmodel と同じ順へ揃える。
        indices={mat.name:i for i,mat in enumerate(mats.values())}
        face_slots=[indices[obj.data.materials[p.material_index].name] for p in obj.data.polygons]
        obj.data.materials.clear()
        for mat in mats.values():obj.data.materials.append(mat)
        for p,slot in zip(obj.data.polygons,face_slots):p.material_index=slot
        # ufbx 側のスロットは「面で初めて出た材質」の順なので、面も並べる。
        bm=bmesh.new();bm.from_mesh(obj.data)
        bm.faces.sort(key=lambda face: face.material_index)
        bm.to_mesh(obj.data);bm.free()
        bpy.ops.object.transform_apply(location=True,rotation=True,scale=True)
        obj.data.calc_loop_triangles();counts.append(len(obj.data.loop_triangles));objects.append(obj)
    # 両 LOD 共通の底面中心を原点にする。北=Blender +Y=FBX -Z。
    verts=[v.co for v in objects[0].data.vertices]
    lo=Vector(tuple(min(v[i] for v in verts) for i in range(3)))
    hi=Vector(tuple(max(v[i] for v in verts) for i in range(3)))
    pivot=Vector(((lo.x+hi.x)/2,(lo.y+hi.y)/2,lo.z))
    for obj in objects:
        for v in obj.data.vertices:v.co-=pivot
    bpy.ops.object.select_all(action='DESELECT')
    for obj in objects:obj.select_set(True)
    fbx=out/'Senjojiki.fbx'
    bpy.ops.export_scene.fbx(filepath=str(fbx),use_selection=True,object_types={'MESH'},
        axis_forward='-Z',axis_up='Y',apply_unit_scale=True,mesh_smooth_type='OFF',
        add_leaf_bones=False,bake_anim=False,path_mode='STRIP',embed_textures=False)
    write_model(str(out/'Senjojiki.tgmodel'),'千畳敷駅・ホテル千畳敷（推定寸法）',
                source_ref(str(fbx),str(root)),refs)
    metadata={'anchorLongitude':LON0+pivot.x/M_PER_LON,'anchorLatitude':LAT0+pivot.y/M_PER_LAT,
              'floorElevation':FLOOR_ELEVATION,'floorFromBottom':-pivot.z,'dimensionsMeters':list(hi-lo),
              'triangles':counts,'hotelFootprint':HOTEL,'stationFootprint':STATION,
              'sources':['https://www.openstreetmap.org/way/187898269','https://www.openstreetmap.org/way/187898270',
                'https://www.chuo-alps.com/hotel/','https://www.chuo-alps.com/hotel/facility/',
                'https://www.meitetsu.co.jp/exp-nagoya/eng/1270230_8916.html'],
              'attribution':'Building footprints: © OpenStreetMap contributors, ODbL 1.0. Heights and facade details are estimates.'}
    write_json(str(out/'Senjojiki.dimensions.json'),metadata)
    objects[1].hide_render=True;objects[1].hide_set(True)
    scene.render.engine='CYCLES';scene.cycles.samples=32
    scene.render.resolution_x=1400;scene.render.resolution_y=1000;scene.render.resolution_percentage=100
    scene.world.color=(.35,.35,.35)
    bpy.ops.object.light_add(type='SUN',location=(0,0,70));sun=bpy.context.object
    sun.rotation_euler=(.4,-.5,-.3);sun.data.energy=3;sun.data.angle=.10
    bpy.ops.object.camera_add(location=(-72,90,65));cam=bpy.context.object
    target=Vector((0,0,16));cam.rotation_euler=(target-cam.location).to_track_quat('-Z','Y').to_euler()
    cam.data.type='ORTHO';cam.data.ortho_scale=85;scene.camera=cam
    scene.view_settings.view_transform='AgX'
    bpy.ops.wm.save_as_mainfile(filepath=str(out/'Senjojiki.blend'))
    scene.render.filepath=str(out/'Senjojiki_preview.png');bpy.ops.render.render(write_still=True)
    print(json.dumps(metadata,ensure_ascii=True))


if __name__=='__main__':main()
