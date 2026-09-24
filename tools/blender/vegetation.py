# 植生モデルを作る Blender スクリプトの共通部分（make_haimatsu.py / make_dakekamba.py から使う）。
#
# 決まりごとは docs/design/vegetation-assets.md。ここには植物の種類に依らない部品を置く:
# 引数、PNG、形状の組み立て（筒・枝の伸ばし方）、樹冠の法線、芯、Blender のマテリアルと
# オブジェクト、FBX の書き出し、terrain-graph のアセット（.tgmat / .tgmodel / .meta）。
import json
import math
import os
import struct
import sys
import uuid
import zlib

import bmesh
import bpy
import numpy as np
from mathutils import Vector, interpolate
from mathutils.bvhtree import BVHTree

UP = Vector((0.0, 0.0, 1.0))  # Blender は Z-up。FBX 書き出しで Y-up へ変換する。
# マテリアルスロット。FBX で最初に現れた順に決まるので、形状も幹から書く。
BARK, LEAVES, CORE = 0, 1, 2


# --- 引数 ---------------------------------------------------------------------
def parse_args():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    options = {"out": None, "root": None, "variants": 3, "seed": 1}
    i = 0
    while i < len(argv):
        key = argv[i].lstrip("-")
        if key in options and i + 1 < len(argv):
            options[key] = argv[i + 1]
            i += 2
        else:
            raise SystemExit(f"不明な引数です: {argv[i]}")
    if not options["out"]:
        raise SystemExit("--out を指定してください")
    options["out"] = os.path.abspath(options["out"])
    if options["root"]:
        options["root"] = os.path.abspath(options["root"])
    else:
        # 出力先の祖先にある data/ をルートとみなす。
        path = options["out"]
        while os.path.basename(path).lower() != "data":
            parent = os.path.dirname(path)
            if parent == path:
                raise SystemExit("--root を指定してください（出力先の上に data/ がありません）")
            path = parent
        options["root"] = path
    options["variants"] = int(options["variants"])
    options["seed"] = int(options["seed"])
    return options


# --- PNG ----------------------------------------------------------------------
def write_png(path, pixels):
    pixels = np.ascontiguousarray(pixels, dtype=np.uint8)
    height, width, channels = pixels.shape
    color_type = {3: 2, 4: 6}[channels]
    raw = b"".join(b"\x00" + pixels[y].tobytes() for y in range(height))

    def chunk(tag, data):
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, color_type, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 9)))
        f.write(chunk(b"IEND", b""))


def to_bytes(values):
    return np.clip(np.round(values * 255.0), 0, 255).astype(np.uint8)


def finish_cutout(color, alpha, normal):
    """黒の上へ重ねた色をアルファで割って戻し、抜けた画素を覆いのある画素の平均色で埋める
    （ミップで縁が黒ずまないように）。RGBA と法線（OpenGL 規約）のバイト列を返す。"""
    covered = alpha > 0.5
    mean = color[covered].mean(axis=0) if covered.any() else np.zeros(3)
    a = alpha[..., None]
    filled = np.where(a > 0.02, np.clip(color / np.maximum(a, 1e-4), 0, 1), mean)
    normal = normal / np.linalg.norm(normal, axis=-1, keepdims=True)
    rgba = np.concatenate([to_bytes(filled), to_bytes(alpha)[..., None]], axis=-1)
    return rgba, to_bytes(normal * 0.5 + 0.5)


def tiling_noise(rng, size, sigma_x, sigma_y):
    """上下左右に繰り返す、平均 0・標準偏差 1 のノイズ。sigma は周波数（小さいほど大きな模様）。"""
    fy, fx = np.meshgrid(np.fft.fftfreq(size), np.fft.fftfreq(size), indexing="ij")
    noise = np.fft.fft2(rng.standard_normal((size, size)))
    field = np.fft.ifft2(noise * np.exp(-(fx / sigma_x) ** 2 - (fy / sigma_y) ** 2)).real
    return (field - field.mean()) / (field.std() + 1e-8)


def height_to_normal(height, strength):
    """繰り返すハイトから法線（OpenGL 規約: 緑 = 画像の上）。画像の y は下向きなので符号が反転する。"""
    dhdx = (np.roll(height, -1, axis=1) - np.roll(height, 1, axis=1)) * 0.5
    dhdy = (np.roll(height, -1, axis=0) - np.roll(height, 1, axis=0)) * 0.5
    n = np.stack([-dhdx * strength, dhdy * strength, np.ones_like(height)], axis=-1)
    return n / np.linalg.norm(n, axis=-1, keepdims=True)


# --- 形状 ---------------------------------------------------------------------
class Geometry:
    def __init__(self):
        self.verts, self.faces, self.face_mat = [], [], []
        self.loop_uvs, self.loop_normals = [], []
        self.shoots = []  # 葉の房の中心と半径（樹冠の場と芯に使う）

    def add_face(self, indices, material, uvs, normals):
        self.faces.append(tuple(indices))
        self.face_mat.append(material)
        self.loop_uvs.append(uvs)
        self.loop_normals.append([tuple(n) for n in normals])

    def triangles(self):
        return sum(len(f) - 2 for f in self.faces)


def perpendicular(v):
    axis = Vector((1, 0, 0)) if abs(v.x) < 0.9 else Vector((0, 1, 0))
    return v.cross(axis).normalized()


def heading(yaw, pitch):
    return Vector((math.cos(pitch) * math.cos(yaw), math.cos(pitch) * math.sin(yaw), math.sin(pitch)))


def grow(start, yaw, length, pitch_at, rng, wander=0.25, step=0.05, ground=0.0):
    """向きを少しずつ変えながら伸ばした点列。地面より下へは潜らせない。"""
    count = max(2, int(length / step))
    points, p = [start.copy()], start.copy()
    for i in range(count):
        t = i / count
        yaw += rng.gauss(0, wander) * step
        p = p + heading(yaw, pitch_at(t)) * (length / count)
        p.z = max(p.z, ground)
        points.append(p.copy())
    return points, yaw


def add_tube(geo, points, radii, sides=7, bark_tile=0.30):
    """点列に沿った筒。平行移動フレームでねじれを抑える。先端は円錐で閉じる。
    UV は u = 周、v = 長さ / bark_tile（樹皮は縦に繰り返す）。"""
    tangents = []
    for i in range(len(points)):
        a = points[max(i - 1, 0)]
        b = points[min(i + 1, len(points) - 1)]
        tangents.append((b - a).normalized())
    normal = perpendicular(tangents[0])
    rings, v_coord, travelled = [], [], 0.0
    for i, (p, t) in enumerate(zip(points, tangents)):
        if i > 0:
            normal = (normal - t * normal.dot(t)).normalized()
            travelled += (p - points[i - 1]).length
        binormal = t.cross(normal)
        ring = []
        for k in range(sides):
            angle = 2 * math.pi * k / sides
            direction = normal * math.cos(angle) + binormal * math.sin(angle)
            geo.verts.append(tuple(p + direction * radii[i]))
            ring.append((len(geo.verts) - 1, direction))
        rings.append(ring)
        v_coord.append(travelled / bark_tile)
    for i in range(len(rings) - 1):
        for k in range(sides):
            k1 = (k + 1) % sides
            a, b = rings[i][k], rings[i][k1]
            c, d = rings[i + 1][k1], rings[i + 1][k]
            u0, u1 = k / sides, (k + 1) / sides
            geo.add_face([a[0], b[0], c[0], d[0]], BARK,
                         [(u0, v_coord[i]), (u1, v_coord[i]), (u1, v_coord[i + 1]), (u0, v_coord[i + 1])],
                         [a[1], b[1], c[1], d[1]])
    tip = points[-1] + tangents[-1] * radii[-1] * 2
    geo.verts.append(tuple(tip))
    tip_index = len(geo.verts) - 1
    last = rings[-1]
    for k in range(sides):
        a, b = last[k], last[(k + 1) % sides]
        geo.add_face([a[0], b[0], tip_index], BARK,
                     [(k / sides, v_coord[-1]), ((k + 1) / sides, v_coord[-1]), ((k + 0.5) / sides, v_coord[-1] + 0.1)],
                     [a[1], b[1], tangents[-1]])


def add_tube_lod(geo, points, radii, sides, stride, bark_tile=0.30):
    """LOD に合わせて節を間引いた筒。角数 0 なら作らない。"""
    if sides <= 0:
        return
    keep = list(range(0, len(points) - 1, stride)) + [len(points) - 1]
    add_tube(geo, [points[i] for i in keep], [radii[i] for i in keep], sides=sides, bark_tile=bark_tile)


def along_polyline(points, start, spacing, rng, jitter=(0.8, 1.3)):
    """点列の長さに沿って spacing ごとの位置と接線を返す（節の間にも置ける）。
    start は置き始める割合（0〜1）。最後の節は先端の房に任せるので含めない。"""
    lengths = [0.0]
    for i in range(1, len(points)):
        lengths.append(lengths[-1] + (points[i] - points[i - 1]).length)
    along, end = lengths[-1] * start, lengths[-2]
    segment = 1
    while along < end:
        while lengths[segment] < along:
            segment += 1
        a, b = points[segment - 1], points[segment]
        t = (along - lengths[segment - 1]) / max(lengths[segment] - lengths[segment - 1], 1e-6)
        yield a.lerp(b, t), (b - a).normalized(), along / max(lengths[-1], 1e-6)
        along += spacing * rng.uniform(*jitter)


# --- 樹冠の法線 -----------------------------------------------------------------
class CanopyField:
    """房の上端の高さを上から見た格子へ書き、ぼかした樹冠の高さの場。法線は常に上半球を向く。
    這い松のようなマット状の株に使う（背の高い木は EnvelopeField）。"""

    def __init__(self, shoots, cell=0.05, blur=0.25):
        self.cell = cell
        points = np.array([[c.x, c.y, c.z + r] for c, r in shoots], np.float64)
        radii = np.array([r for _, r in shoots], np.float64)
        margin = blur * 4 + radii.max() * 2
        self.origin = points[:, :2].min(axis=0) - margin
        size = np.ceil((points[:, :2].max(axis=0) + margin - self.origin) / cell).astype(int) + 1
        height = np.zeros((size[1], size[0]))  # 地面は 0
        # 房の円の中へ上端の高さを max で書く。
        for (x, y, z), r in zip(points, radii):
            reach = max(1, int(np.ceil(r * 1.5 / cell)))
            cx, cy = int(round((x - self.origin[0]) / cell)), int(round((y - self.origin[1]) / cell))
            y0, y1, x0, x1 = cy - reach, cy + reach + 1, cx - reach, cx + reach + 1
            yy, xx = np.mgrid[y0:y1, x0:x1]
            inside = (xx - cx) ** 2 + (yy - cy) ** 2 <= reach * reach
            view = height[y0:y1, x0:x1]
            view[inside] = np.maximum(view[inside], z)
        # ガウスでぼかす（FFT、縁は余白で吸収）。
        sigma = blur / cell
        fy = np.fft.fftfreq(height.shape[0])[:, None]
        fx = np.fft.fftfreq(height.shape[1])[None, :]
        kernel = np.exp(-2 * (np.pi * sigma) ** 2 * (fx * fx + fy * fy))
        self.height = np.fft.ifft2(np.fft.fft2(height) * kernel).real
        gy, gx = np.gradient(self.height, cell)
        self.gradient = (gx, gy)

    def normal(self, point):
        u = (point.x - self.origin[0]) / self.cell
        v = (point.y - self.origin[1]) / self.cell
        i = int(np.clip(np.floor(u), 0, self.height.shape[1] - 2))
        j = int(np.clip(np.floor(v), 0, self.height.shape[0] - 2))
        fu, fv = np.clip(u - i, 0, 1), np.clip(v - j, 0, 1)

        def sample(grid):
            return ((grid[j, i] * (1 - fu) + grid[j, i + 1] * fu) * (1 - fv) +
                    (grid[j + 1, i] * (1 - fu) + grid[j + 1, i + 1] * fu) * fv)

        return Vector((-sample(self.gradient[0]), -sample(self.gradient[1]), 1.0)).normalized()


class EnvelopeField:
    """樹冠を包む滑らかな外形の、最寄りの点の法線（頂点法線を面の中で補間）。背の高い木に使う。
    上面は上、側面は外、下面は下を向く（樹冠の下面は暗くなる）。"""

    def __init__(self, envelope):
        self.coords = [v.co.copy() for v in envelope.vertices]
        self.normals = [v.normal.copy() for v in envelope.vertices]
        self.polygons = [tuple(p.vertices) for p in envelope.polygons]
        self.tree = BVHTree.FromPolygons(self.coords, self.polygons)

    def normal(self, point):
        location, _, index, _ = self.tree.find_nearest(point)
        if index is None:
            return UP.copy()
        polygon = self.polygons[index]
        weights = interpolate.poly_3d_calc([self.coords[i] for i in polygon], location)
        n = Vector()
        for i, w in zip(polygon, weights):
            n += self.normals[i] * w
        return n.normalized() if n.length > 1e-6 else UP.copy()


def transfer_field_normals(geo, field, weight, up_bias=0.0):
    """葉の法線を場の法線へ weight の割合で寄せる（up_bias だけ上へ足す）。幹と芯はそのまま。"""
    for f, face in enumerate(geo.faces):
        if geo.face_mat[f] != LEAVES:
            continue
        normals = []
        for corner, vertex in enumerate(face):
            original = Vector(geo.loop_normals[f][corner])
            target = field.normal(Vector(geo.verts[vertex]))
            n = target * weight + original * (1 - weight) + UP * up_bias
            normals.append(tuple(n.normalized()))
        geo.loop_normals[f] = normals


# --- 芯 -------------------------------------------------------------------------
def build_hull(name, shoots, radius_scale=1.0, resolution=0.05, smooth=4):
    """房ごとのメタボールを溶け合わせた滑らかな塊（bpy.types.Mesh）。芯や外形の元にする。"""
    metaball = bpy.data.metaballs.new(name)
    metaball.resolution = metaball.render_resolution = resolution
    metaball.threshold = 0.6
    obj = bpy.data.objects.new(name, metaball)
    bpy.context.scene.collection.objects.link(obj)
    for center, radius in shoots:
        element = metaball.elements.new()
        element.co = center
        element.radius = radius * radius_scale
    depsgraph = bpy.context.evaluated_depsgraph_get()
    mesh = bpy.data.meshes.new_from_object(obj.evaluated_get(depsgraph))
    bpy.data.objects.remove(obj)
    bpy.data.metaballs.remove(metaball)
    # 格子の段差をならす。
    bm = bmesh.new()
    bm.from_mesh(mesh)
    for _ in range(smooth):
        bmesh.ops.smooth_vert(bm, verts=bm.verts, factor=0.5, use_axis_x=True, use_axis_y=True, use_axis_z=True)
    bm.to_mesh(mesh)
    bm.free()
    mesh.update()
    return mesh


def add_core(geo, hull, triangles, inset):
    """塊を inset だけ縮めて面を減らした芯を足す。葉の隙間から見える、株の内側の詰まった塊。"""
    source = bpy.data.objects.new("CoreSource", hull.copy())
    bpy.context.scene.collection.objects.link(source)
    total = sum(len(p.vertices) - 2 for p in hull.polygons)
    decimate = source.modifiers.new("Decimate", "DECIMATE")
    decimate.ratio = min(1.0, triangles / max(total, 1))
    depsgraph = bpy.context.evaluated_depsgraph_get()
    mesh = bpy.data.meshes.new_from_object(source.evaluated_get(depsgraph))
    source_mesh = source.data
    bpy.data.objects.remove(source)
    bpy.data.meshes.remove(source_mesh)
    bm = bmesh.new()
    bm.from_mesh(mesh)
    bpy.data.meshes.remove(mesh)
    bmesh.ops.triangulate(bm, faces=bm.faces)
    bm.normal_update()
    for vertex in bm.verts:
        vertex.co -= vertex.normal * inset
    bm.normal_update()
    first = len(geo.verts)
    bm.verts.index_update()
    for vertex in bm.verts:
        geo.verts.append(tuple(vertex.co))
    for face in bm.faces:
        indices = [first + v.index for v in face.verts]
        uvs = [(v.co.x * 0.5, v.co.y * 0.5) for v in face.verts]
        geo.add_face(indices, CORE, uvs, [tuple(v.normal) for v in face.verts])
    bm.free()


# --- Blender ------------------------------------------------------------------
def make_material(name, color_path, normal_path, alpha):
    material = bpy.data.materials.new(name)
    nodes, links = material.node_tree.nodes, material.node_tree.links
    shader = nodes.get("Principled BSDF")
    color = nodes.new("ShaderNodeTexImage")
    color.image = bpy.data.images.load(color_path, check_existing=True)
    links.new(color.outputs["Color"], shader.inputs["Base Color"])
    if alpha:
        links.new(color.outputs["Alpha"], shader.inputs["Alpha"])
    normal_image = nodes.new("ShaderNodeTexImage")
    normal_image.image = bpy.data.images.load(normal_path, check_existing=True)
    normal_image.image.colorspace_settings.name = "Non-Color"
    normal_map = nodes.new("ShaderNodeNormalMap")
    links.new(normal_image.outputs["Color"], normal_map.inputs["Color"])
    links.new(normal_map.outputs["Normal"], shader.inputs["Normal"])
    if alpha:
        material.use_backface_culling = False
    return material


def make_core_material(color):
    material = bpy.data.materials.new("Core")
    shader = material.node_tree.nodes.get("Principled BSDF")
    shader.inputs["Base Color"].default_value = (*color, 1.0)
    shader.inputs["Roughness"].default_value = 0.9
    return material


def make_object(name, geo, materials, offset):
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata(geo.verts, [], geo.faces)
    for material in materials:
        mesh.materials.append(material)
    mesh.polygons.foreach_set("material_index", geo.face_mat)
    uv_layer = mesh.uv_layers.new(name="UVMap")
    uv_layer.data.foreach_set("uv", [c for face in geo.loop_uvs for uv in face for c in uv])
    mesh.polygons.foreach_set("use_smooth", [True] * len(geo.faces))
    mesh.normals_split_custom_set([n for face in geo.loop_normals for n in face])
    mesh.update()
    obj = bpy.data.objects.new(name, mesh)
    obj.location = offset
    bpy.context.scene.collection.objects.link(obj)
    return obj


def export_fbx(objects, path):
    """段ごとのオブジェクトを原点へ戻して 1 つの FBX へ書く。名前の _LOD<n> で段を表す。"""
    locations = [obj.location.copy() for obj in objects]
    bpy.ops.object.select_all(action="DESELECT")
    for obj in objects:
        obj.location = (0, 0, 0)
        obj.select_set(True)
    bpy.context.view_layer.objects.active = objects[0]
    bpy.ops.export_scene.fbx(filepath=path, use_selection=True, object_types={"MESH"},
                             axis_forward="-Z", axis_up="Y", apply_unit_scale=True,
                             mesh_smooth_type="OFF", add_leaf_bones=False, bake_anim=False,
                             path_mode="STRIP", embed_textures=False)
    for obj, location in zip(objects, locations):
        obj.location = location


# --- terrain-graph のアセット -------------------------------------------------
def new_uid():
    return "{" + str(uuid.uuid4()).upper() + "}"


def write_json(path, data):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(data, f, ensure_ascii=False, indent=2)
        f.write("\n")


def source_ref(path, root):
    """元ファイルの .meta を用意し、ルート相対パスと UID を返す。"""
    meta = path + ".meta"
    if os.path.exists(meta):
        with open(meta, encoding="utf-8") as f:
            uid = json.load(f)["uid"]
    else:
        uid = new_uid()
        write_json(meta, {"format": "terrain-graph.source", "uid": uid, "version": 1})
    return {"path": os.path.relpath(path, root).replace("\\", "/"), "uid": uid}


def asset_ref(path, root):
    with open(path, encoding="utf-8") as f:
        uid = json.load(f)["uid"]
    return {"path": os.path.relpath(path, root).replace("\\", "/"), "uid": uid}


def write_material(path, name, color, normal, roughness, alpha_cutoff, two_sided, tint=(1.0, 1.0, 1.0)):
    if os.path.exists(path):
        return  # 手で調整した値を消さない
    empty = {"channel": "r", "texture": None}
    write_json(path, {
        "alphaCutoff": alpha_cutoff, "ambientOcclusion": 1.0, "baseColorTint": list(tint),
        "brightness": 1.0, "flipNormalGreen": True, "format": "terrain-graph.material-asset",
        "hueShift": 0.0,
        "maps": {"ambientOcclusion": empty, "baseColor": color, "height": empty, "metallic": empty,
                 "normal": normal, "roughness": empty},
        "metallic": 0.0, "name": name, "roughness": roughness, "saturation": 1.0,
        "twoSided": two_sided, "uid": new_uid(), "version": 1})


def write_model(path, name, source, materials):
    if os.path.exists(path):
        # 手で直した値やインポスターの記録は残し、足りないマテリアルの並びだけを書き足す。
        with open(path, encoding="utf-8") as f:
            data = json.load(f)
        slots = data.setdefault("materials", [])
        if len(slots) < len(materials):
            slots.extend(materials[len(slots):])
            write_json(path, data)
        return
    write_json(path, {"format": "terrain-graph.model-asset", "materials": materials, "name": name,
                      "source": source, "uid": new_uid(), "version": 1})
