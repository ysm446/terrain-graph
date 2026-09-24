# 這い松（ハイマツ）の簡易モデルを作る Blender スクリプト。
#
# 使い方（Blender 5.x）:
#   blender -b --factory-startup --python-exit-code 1 --python tools/blender/make_haimatsu.py -- \
#       --out data/models/Haimatsu [--root data] [--variants 3] [--seed 1]
#
# 出力（--out の下）:
#   Haimatsu_VarN.fbx        幹・枝（Bark）と葉のカード（Needles）の 2 スロット
#   T_Haimatsu_Needles_*.png 枝先のカード（RGBA、A でアルファ抜き）と法線（OpenGL 規約）
#   T_Haimatsu_Bark_*.png    樹皮（縦方向に繰り返す）と法線
#   Haimatsu.blend           全バリエーション（手直し用）
#   *.tgmat / *.tgmodel / *.meta  terrain-graph のアセット。既にあれば上書きしない（UID を保つ）
#
# 形: 根元から放射状に地面を這う幹が伸び、先で立ち上がる。枝の外側半分と先端に
# 上向きの枝先（十字に組んだカード 3 枚）を付ける。法線は株の外側・上へ曲げて、
# 板の向きが目立たないようにする。
import json
import math
import os
import random
import struct
import sys
import uuid
import zlib

import bpy
import numpy as np
from mathutils import Vector

UP = Vector((0.0, 0.0, 1.0))  # Blender は Z-up。FBX 書き出しで Y-up へ変換する。


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


# --- 針葉のカード -------------------------------------------------------------
# 画像は縦長（幅 512 × 高さ 1024）。下端が枝の付け根、上へ向かって伸びる 1 本の枝先。
# 針葉は 5 本 1 束。付け根ほど開き、先ほど前へ揃って筆のようになる。
class Canvas:
    def __init__(self, width, height):
        self.width, self.height = width, height
        self.color = np.zeros((height, width, 3), np.float32)
        self.alpha = np.zeros((height, width), np.float32)
        self.normal = np.zeros((height, width, 3), np.float32)
        self.normal[..., 2] = 1.0

    def stroke(self, p0, p1, w0, w1, color, stomata=0.0):
        """太さが変わる丸い線分を描く。座標は画素（x 右、y 下）。"""
        p0, p1 = np.asarray(p0, np.float32), np.asarray(p1, np.float32)
        d = p1 - p0
        length = float(np.hypot(*d))
        if length < 1e-3:
            return
        d /= length
        half = max(w0, w1) * 0.5 + 2
        x0 = int(max(0, math.floor(min(p0[0], p1[0]) - half)))
        x1 = int(min(self.width, math.ceil(max(p0[0], p1[0]) + half) + 1))
        y0 = int(max(0, math.floor(min(p0[1], p1[1]) - half)))
        y1 = int(min(self.height, math.ceil(max(p0[1], p1[1]) + half) + 1))
        if x0 >= x1 or y0 >= y1:
            return
        ys, xs = np.mgrid[y0:y1, x0:x1].astype(np.float32)
        rx, ry = xs + 0.5 - p0[0], ys + 0.5 - p0[1]
        t = np.clip((rx * d[0] + ry * d[1]) / length, 0.0, 1.0)
        # 線分に垂直な符号付き距離（左右の判定と法線に使う）。
        side = rx * -d[1] + ry * d[0]
        cx, cy = rx - d[0] * t * length, ry - d[1] * t * length
        dist = np.sqrt(cx * cx + cy * cy)
        radius = (w0 + (w1 - w0) * t) * 0.5
        coverage = np.clip(radius - dist + 0.5, 0.0, 1.0)
        mask = coverage > 0
        if not mask.any():
            return
        across = np.clip(side / np.maximum(radius, 1e-3), -1.0, 1.0)
        bulge = np.sqrt(np.clip(1.0 - across * across, 0.0, 1.0))
        shade = 0.72 + 0.28 * bulge
        # 気孔帯（片側の白っぽい筋）。
        tint = np.clip((across - 0.15) * 2.5, 0.0, 1.0)[..., None] * stomata
        base = np.asarray(color, np.float32)
        pale = np.array([0.55, 0.64, 0.60], np.float32)
        rgb = (base * (1 - tint) + pale * tint) * shade[..., None]
        # 先ほど少し明るく黄みへ。
        rgb *= (1.0 + 0.18 * t)[..., None]
        a = coverage[..., None]
        view = (slice(y0, y1), slice(x0, x1))
        self.color[view] = self.color[view] * (1 - a) + rgb * a
        self.alpha[view] = self.alpha[view] * (1 - coverage) + coverage
        # 法線（OpenGL 規約: 緑 = 画像の上）。円柱の断面として横へ傾ける。
        nx = -d[1] * across
        ny_down = d[0] * across
        n = np.stack([nx, -ny_down, bulge + 0.35], axis=-1)
        n /= np.linalg.norm(n, axis=-1, keepdims=True)
        self.normal[view] = self.normal[view] * (1 - a) + n * a


def make_needle_texture(rng, width=512, height=1024):
    canvas = Canvas(width, height)
    cx = width * 0.5
    base_y, tip_y = height - 6, height * 0.30

    def twig_point(s):
        # 付け根 s=0、先端 s=1。わずかに曲げる。
        return np.array([cx + 6 * math.sin(s * 2.4), base_y + (tip_y - base_y) * s], np.float32)

    greens = [(0.13, 0.24, 0.12), (0.10, 0.20, 0.11), (0.16, 0.27, 0.13), (0.12, 0.22, 0.15)]
    needle_length = height * 0.22

    def bundle(s, front):
        origin = twig_point(s)
        side = rng.choice((-1.0, 1.0))
        spread = math.radians(52 - 36 * s + rng.uniform(-10, 10))
        count = 5
        color = np.array(rng.choice(greens)) * rng.uniform(0.85, 1.15)
        for k in range(count):
            angle = side * (spread + math.radians(rng.uniform(-9, 9))) + math.radians((k - 2) * 3)
            # 画面から前後へ倒れた針葉は短く見える。
            tilt = math.radians(rng.uniform(0, 55))
            length = needle_length * rng.uniform(0.8, 1.1) * math.cos(tilt) * (0.8 + 0.2 * s)
            direction = np.array([math.sin(angle), -math.cos(angle)], np.float32)
            bend = np.array([-direction[1], direction[0]], np.float32) * side * rng.uniform(0, 0.06)
            p1 = origin + (direction + bend) * length
            canvas.stroke(origin, p1, rng.uniform(6.0, 7.5), 2.0, color,
                          stomata=0.35 if front else 0.15)

    samples = [0.10 + 0.9 * (i / 69) ** 0.8 for i in range(70)]
    rng.shuffle(samples)
    back, front = samples[:35], samples[35:]
    for s in back:
        bundle(s, False)
    # 小枝。先ほど細い。
    for i in range(40):
        s0, s1 = i / 40, (i + 1) / 40
        canvas.stroke(twig_point(s0), twig_point(s1), 13 - 6 * s0, 13 - 6 * s1, (0.30, 0.22, 0.16))
    for s in front:
        bundle(s, True)
    # 先端の房（冬芽のまわりで前へ揃う）。
    tip = twig_point(1.0)
    for k in range(22):
        angle = math.radians(rng.uniform(-24, 24))
        length = needle_length * rng.uniform(0.7, 1.05)
        direction = np.array([math.sin(angle), -math.cos(angle)], np.float32)
        color = np.array(rng.choice(greens)) * rng.uniform(0.95, 1.25)
        canvas.stroke(tip, tip + direction * length, 6.5, 2.0, color, stomata=0.3)

    # 抜けた画素の色を平均色で埋める（ミップで縁が黒ずまないように）。
    covered = canvas.alpha > 0.5
    mean = canvas.color[covered].mean(axis=0) if covered.any() else np.zeros(3)
    # 色は黒の上へ重ねてあるので、アルファで割って戻す。
    a = canvas.alpha[..., None]
    color = np.where(a > 0.02, np.clip(canvas.color / np.maximum(a, 1e-4), 0, 1), mean)
    normal = canvas.normal / np.linalg.norm(canvas.normal, axis=-1, keepdims=True)
    rgba = np.concatenate([to_bytes(color), to_bytes(canvas.alpha)[..., None]], axis=-1)
    return rgba, to_bytes(normal * 0.5 + 0.5)


# --- 樹皮 ---------------------------------------------------------------------
def make_bark_texture(rng, size=512):
    """縦（枝の長さ方向）に伸びた、上下左右に繰り返す樹皮。"""
    fy, fx = np.meshgrid(np.fft.fftfreq(size), np.fft.fftfreq(size), indexing="ij")

    def band(sigma_x, sigma_y):
        noise = np.fft.fft2(rng.standard_normal((size, size)))
        field = np.fft.ifft2(noise * np.exp(-(fx / sigma_x) ** 2 - (fy / sigma_y) ** 2)).real
        return (field - field.mean()) / (field.std() + 1e-8)

    height = 0.7 * band(0.030, 0.008) + 0.3 * band(0.12, 0.03)
    height = 1 / (1 + np.exp(-1.6 * height))  # 0〜1 へ
    dark = np.array([0.20, 0.16, 0.13])
    light = np.array([0.42, 0.37, 0.32])
    color = dark + (light - dark) * height[..., None]
    grain = 0.06 * band(0.25, 0.25)[..., None]
    color = np.clip(color * (1 + grain), 0, 1)
    strength = 6.0
    dhdx = (np.roll(height, -1, axis=1) - np.roll(height, 1, axis=1)) * 0.5
    dhdy = (np.roll(height, -1, axis=0) - np.roll(height, 1, axis=0)) * 0.5
    # OpenGL 規約（緑 = 画像の上）。画像の y は下向きなので符号が反転する。
    n = np.stack([-dhdx * strength, dhdy * strength, np.ones_like(height)], axis=-1)
    n /= np.linalg.norm(n, axis=-1, keepdims=True)
    return to_bytes(color), to_bytes(n * 0.5 + 0.5)


# --- 形状 ---------------------------------------------------------------------
class Geometry:
    def __init__(self):
        self.verts, self.faces, self.face_mat = [], [], []
        self.loop_uvs, self.loop_normals = [], []

    def add_face(self, indices, material, uvs, normals):
        self.faces.append(tuple(indices))
        self.face_mat.append(material)
        self.loop_uvs.append(uvs)
        self.loop_normals.append([tuple(n) for n in normals])


BARK, NEEDLES = 0, 1
CARD_LENGTH, CARD_WIDTH = 0.24, 0.12  # テクスチャの縦横比 2:1 に合わせる


def perpendicular(v):
    axis = Vector((1, 0, 0)) if abs(v.x) < 0.9 else Vector((0, 1, 0))
    return v.cross(axis).normalized()


def add_tube(geo, points, radii, sides=7, bark_tile=0.30):
    """点列に沿った筒。平行移動フレームでねじれを抑える。先端は円錐で閉じる。"""
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


def add_shoot(geo, base, axis, rng, scale=1.0):
    """枝先 1 本。軸まわりに 60 度ずつ回したカード 3 枚。"""
    axis = axis.normalized()
    length, width = CARD_LENGTH * scale, CARD_WIDTH * scale
    base = base - axis * 0.02 * scale
    tip = base + axis * length
    reference = perpendicular(axis)
    start = rng.uniform(0, math.pi)
    outward = Vector((base.x, base.y, 0.0))
    outward = outward.normalized() if outward.length > 1e-4 else Vector((1, 0, 0))
    for k in range(3):
        angle = start + k * math.pi / 3
        across = (reference * math.cos(angle) + axis.cross(reference) * math.sin(angle)).normalized()
        face = across.cross(axis).normalized()
        if face.dot(outward + UP) < 0:
            face = -face
        corners = [base - across * width / 2, base + across * width / 2,
                   tip + across * width / 2, tip - across * width / 2]
        uvs = [(0, 0), (1, 0), (1, 1), (0, 1)]
        normals = []
        for sign in (-1, 1, 1, -1):
            # 板の法線に、枝先の丸み（横）と株の外側・上を混ぜる。
            n = face * 0.35 + across * sign * 0.45 + outward * 0.35 + UP * 0.45
            normals.append(n.normalized())
        first = len(geo.verts)
        geo.verts.extend(tuple(c) for c in corners)
        geo.add_face([first, first + 1, first + 2, first + 3], NEEDLES, uvs, normals)


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


def build_plant(rng):
    geo = Geometry()
    stems = rng.randint(6, 9)
    yaw0 = rng.uniform(0, 2 * math.pi)
    for s in range(stems):
        yaw = yaw0 + 2 * math.pi * s / stems + rng.uniform(-0.35, 0.35)
        length = rng.uniform(1.3, 2.3)
        rise = math.radians(rng.uniform(28, 48))
        lie = math.radians(rng.uniform(-2, 7))
        emerge = math.radians(rng.uniform(15, 35))

        def pitch_at(t, emerge=emerge, lie=lie, rise=rise):
            if t < 0.12:
                return emerge + (lie - emerge) * (t / 0.12)
            if t < 0.72:
                return lie + math.radians(4) * math.sin(t * 13)
            return lie + (rise - lie) * ((t - 0.72) / 0.28)

        r0 = rng.uniform(0.045, 0.065)
        points, _ = grow(Vector((0, 0, -0.04)), yaw, length, pitch_at, rng, ground=r0 * 0.6)
        radii = [r0 * (1 - 0.78 * (i / (len(points) - 1))) + 0.006 for i in range(len(points))]
        add_tube(geo, points, radii, sides=8)
        add_branches(geo, points, radii, rng, order=1)
        add_foliage(geo, points, rng, start=0.40, spacing=0.055)
    return geo


def add_branches(geo, points, radii, rng, order):
    total = len(points) - 1
    distance, next_at = 0.0, rng.uniform(0.1, 0.2)
    side = rng.choice((-1, 1))
    for i in range(1, total):
        distance += (points[i] - points[i - 1]).length
        t = i / total
        if t < (0.22 if order == 1 else 0.3) or distance < next_at:
            continue
        next_at = distance + rng.uniform(0.10, 0.18) * (1.4 if order == 2 else 1)
        side = -side
        tangent = (points[i + 1] - points[i - 1]).normalized()
        yaw = math.atan2(tangent.y, tangent.x) + side * math.radians(rng.uniform(35, 70))
        base_pitch = math.asin(max(-1, min(1, tangent.z)))
        up = math.radians(rng.uniform(15, 40))
        scale = 1 - 0.45 * t
        length = rng.uniform(0.25, 0.65) * scale if order == 1 else rng.uniform(0.10, 0.25)

        def pitch_at(u, base_pitch=base_pitch, up=up):
            return base_pitch + up + math.radians(25) * u

        branch, _ = grow(points[i], yaw, length, pitch_at, rng, wander=0.4, step=0.04, ground=0.02)
        r0 = radii[i] * 0.55
        branch_radii = [r0 * (1 - 0.7 * (j / (len(branch) - 1))) + 0.003 for j in range(len(branch))]
        add_tube(geo, branch, branch_radii, sides=6 if order == 1 else 5)
        if order == 1 and length > 0.3:
            add_branches(geo, branch, branch_radii, rng, order=2)
        add_foliage(geo, branch, rng, start=0.25 if order == 1 else 0.1, spacing=0.045)


def add_foliage(geo, points, rng, start, spacing):
    """枝の外側に横向きの枝先、先端に上向きの枝先を付ける。"""
    total = len(points) - 1
    distance, next_at = 0.0, 0.0
    for i in range(1, total):
        distance += (points[i] - points[i - 1]).length
        if i / total < start or distance < next_at:
            continue
        next_at = distance + spacing * rng.uniform(0.8, 1.3)
        tangent = (points[i + 1] - points[i - 1]).normalized()
        sideways = tangent.cross(UP)
        sideways = sideways.normalized() if sideways.length > 1e-4 else perpendicular(tangent)
        sideways *= rng.choice((-1, 1))
        axis = tangent * 0.6 + sideways * rng.uniform(0.4, 0.8) + UP * rng.uniform(0.7, 1.2)
        add_shoot(geo, points[i], axis, rng, scale=rng.uniform(0.8, 1.1))
    tangent = (points[-1] - points[-2]).normalized()
    add_shoot(geo, points[-1], tangent + UP * 0.8, rng, scale=rng.uniform(0.95, 1.15))
    for k in range(rng.randint(2, 3)):
        angle = 2 * math.pi * k / 3 + rng.uniform(-0.4, 0.4)
        spread = perpendicular(tangent)
        spread = spread * math.cos(angle) + tangent.cross(spread) * math.sin(angle)
        add_shoot(geo, points[-1], tangent + UP * 0.9 + spread * 0.7, rng, scale=rng.uniform(0.8, 1.0))


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


def export_fbx(obj, path):
    location = obj.location.copy()
    obj.location = (0, 0, 0)
    bpy.ops.object.select_all(action="DESELECT")
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj
    bpy.ops.export_scene.fbx(filepath=path, use_selection=True, object_types={"MESH"},
                             axis_forward="-Z", axis_up="Y", apply_unit_scale=True,
                             mesh_smooth_type="OFF", add_leaf_bones=False, bake_anim=False,
                             path_mode="STRIP", embed_textures=False)
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


def write_material(path, name, color, normal, roughness, alpha_cutoff, two_sided):
    if os.path.exists(path):
        return  # 手で調整した値を消さない
    empty = {"channel": "r", "texture": None}
    write_json(path, {
        "alphaCutoff": alpha_cutoff, "ambientOcclusion": 1.0, "baseColorTint": [1.0, 1.0, 1.0],
        "brightness": 1.0, "flipNormalGreen": True, "format": "terrain-graph.material-asset",
        "hueShift": 0.0,
        "maps": {"ambientOcclusion": empty, "baseColor": color, "height": empty, "metallic": empty,
                 "normal": normal, "roughness": empty},
        "metallic": 0.0, "name": name, "roughness": roughness, "saturation": 1.0,
        "twoSided": two_sided, "uid": new_uid(), "version": 1})


def write_model(path, name, source, materials):
    if os.path.exists(path):
        return
    write_json(path, {"format": "terrain-graph.model-asset", "materials": materials, "name": name,
                      "source": source, "uid": new_uid(), "version": 1})


def main():
    options = parse_args()
    out, root = options["out"], options["root"]
    os.makedirs(out, exist_ok=True)
    rng = random.Random(options["seed"])
    np_rng = np.random.default_rng(options["seed"])

    paths = {key: os.path.join(out, f"T_Haimatsu_{key}.png")
             for key in ("Needles_D", "Needles_N", "Bark_D", "Bark_N")}
    needles, needles_normal = make_needle_texture(rng)
    write_png(paths["Needles_D"], needles)
    write_png(paths["Needles_N"], needles_normal)
    bark, bark_normal = make_bark_texture(np_rng)
    write_png(paths["Bark_D"], bark)
    write_png(paths["Bark_N"], bark_normal)

    bpy.ops.wm.read_factory_settings(use_empty=True)
    materials = [make_material("Bark", paths["Bark_D"], paths["Bark_N"], False),
                 make_material("Needles", paths["Needles_D"], paths["Needles_N"], True)]

    bark_mat = os.path.join(out, "MI_Haimatsu_Bark.tgmat")
    needle_mat = os.path.join(out, "MI_Haimatsu_Needles.tgmat")
    write_material(bark_mat, "MI_Haimatsu_Bark", source_ref(paths["Bark_D"], root),
                   source_ref(paths["Bark_N"], root), 0.85, 0.0, False)
    write_material(needle_mat, "MI_Haimatsu_Needles", source_ref(paths["Needles_D"], root),
                   source_ref(paths["Needles_N"], root), 0.6, 0.5, True)

    for index in range(1, options["variants"] + 1):
        name = f"Haimatsu_Var{index}"
        geo = build_plant(random.Random(options["seed"] * 1000 + index))
        obj = make_object(name, geo, materials, ((index - 1) * 5.0, 0, 0))
        fbx = os.path.join(out, name + ".fbx")
        export_fbx(obj, fbx)
        # スロットの並びは FBX で最初に現れた順（幹が先）。
        write_model(os.path.join(out, name + ".tgmodel"), name, source_ref(fbx, root),
                    [asset_ref(bark_mat, root), asset_ref(needle_mat, root)])
        triangles = sum(len(f) - 2 for f in geo.faces)
        cards = geo.face_mat.count(NEEDLES)
        print(f"{name}: {triangles} triangles, {cards // 3} shoots")

    bpy.ops.wm.save_as_mainfile(filepath=os.path.join(out, "Haimatsu.blend"))


main()
