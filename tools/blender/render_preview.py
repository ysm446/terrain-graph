# 生成した植生モデル（.blend）の LOD を横に並べて Cycles で 1 枚描く確認用スクリプト。
#
# 使い方（Blender 5.x）:
#   blender -b data/Models/Arve/Arve.blend --python-exit-code 1 --python tools/blender/render_preview.py -- \
#       <出力.png> <オブジェクト名の頭（例: Arve_Var）> [LOD0]
#
# 名前が「頭」で始まり「_LOD0」などで終わるオブジェクトだけを、正投影で横から並べる。
# 注意: この描き方ではアルファ抜きのカードが実際より薄く見え、芯（Core）の暗い塊が目立つ。
# 葉の量や明るさは、同じスクリプトで既存の樹種（例: Shirabiso_Var）を描いて見比べ、最後はアプリで確かめる
# （決まりごとは docs/design/vegetation-assets.md）。
import math
import os
import sys

import bpy
from mathutils import Vector

argv = sys.argv[sys.argv.index("--") + 1:]
out, prefix = os.path.abspath(argv[0]), argv[1]  # 相対のままだと Blender が別の場所へ保存する
lod = argv[2] if len(argv) > 2 else "LOD0"
objects = [o for o in bpy.data.objects if o.type == "MESH" and o.name.startswith(prefix) and o.name.endswith(lod)]
if not objects:
    raise SystemExit(f"{prefix}*{lod} のオブジェクトがありません")
for o in bpy.data.objects:
    if o.type == "MESH" and o not in objects:
        o.hide_render = True
objects.sort(key=lambda o: o.name)

# 横に並べる
x = 0.0
for o in objects:
    corners = [o.matrix_world @ Vector(c) for c in o.bound_box]
    width = max(v.x for v in corners) - min(v.x for v in corners)
    o.location = (x + width / 2, 0, 0)
    x += width + 1.0
total = x
height = max(max((o.matrix_world @ Vector(c)).z for c in o.bound_box) for o in objects)

scene = bpy.context.scene
camera = bpy.data.objects.new("PreviewCamera", bpy.data.cameras.new("PreviewCamera"))
scene.collection.objects.link(camera)
camera.data.type = "ORTHO"
camera.data.ortho_scale = max(total, height * 1.6) * 1.05
camera.location = (total / 2, -60, height * 0.5 + 6)
camera.rotation_euler = (math.radians(84), 0, 0)
scene.camera = camera
sun = bpy.data.objects.new("PreviewSun", bpy.data.lights.new("PreviewSun", "SUN"))
sun.data.energy = 4.0
sun.rotation_euler = (math.radians(45), 0, math.radians(-35))
scene.collection.objects.link(sun)
world = bpy.data.worlds.new("PreviewWorld")
scene.world = world
world.use_nodes = True
world.node_tree.nodes["Background"].inputs[0].default_value = (0.45, 0.55, 0.7, 1)
world.node_tree.nodes["Background"].inputs[1].default_value = 0.6
bpy.ops.mesh.primitive_plane_add(size=400, location=(total / 2, 0, 0))

scene.render.engine = "CYCLES"
scene.cycles.samples = 32
scene.cycles.device = "GPU"
try:
    preferences = bpy.context.preferences.addons["cycles"].preferences
    preferences.compute_device_type = "OPTIX"
    preferences.get_devices()
    for device in preferences.devices:
        device.use = True
except Exception as error:  # GPU が無ければ CPU で描く
    print(error)
scene.render.resolution_x = 1600
scene.render.resolution_y = max(600, int(1600 * max(height * 1.6, 1) / max(total, height * 1.6)))
scene.render.filepath = out
bpy.ops.render.render(write_still=True)
