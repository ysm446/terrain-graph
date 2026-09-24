# 這い松（ハイマツ）の簡易モデルを作る Blender スクリプト。
#
# 使い方（Blender 5.x）:
#   blender -b --factory-startup --python-exit-code 1 --python tools/blender/make_haimatsu.py -- \
#       --out data/Models/Haimatsu [--root data] [--variants 3] [--seed 1]
#
# 出力（--out の下）:
#   Haimatsu_VarN.fbx        幹・枝（Bark）、葉のカード（Needles）、芯（Core）の 3 スロット。
#                            LOD0〜2 をオブジェクト名の末尾 _LOD<n> で 1 ファイルに入れる
#   T_Haimatsu_Needles_*.png 枝先のカード（RGBA、A でアルファ抜き）と法線（OpenGL 規約）
#   T_Haimatsu_Bark_*.png    樹皮（縦方向に繰り返す）と法線
#   Haimatsu.blend           全バリエーション（手直し用）
#   *.tgmat / *.tgmodel / *.meta  terrain-graph のアセット。既にあれば上書きしない（UID を保つ）。
#                                 .tgmodel のマテリアルの並びだけは、足りない分を書き足す
#
# 共通の部品は vegetation.py、決まりごとは docs/design/vegetation-assets.md。
#
# 形: 根元から放射状に地面を這う幹が伸び、先で立ち上がる。枝の外側半分と先端に
# 上向きの枝先（十字に組んだカード 3 枚）を付ける。
# 遠目に苔のような一塊の群落に見えるよう、
# (1) 枝先の上端の高さを上から見た格子へ書いてぼかした「樹冠の高さの場」を作り、
#     葉のカードの法線をその勾配の法線へ寄せる（カードごとの明暗を消し、株のうねりで陰影を付ける。
#     マット状の株なので常に上半球を向き、群落の縁では外へ傾く）。
# (2) 枝先ごとのメタボールを溶け合わせて内側へ縮めた「芯」を、暗い不透明な面として入れる
#     （葉の隙間を埋めて量感を出す）。
# どちらも LOD0 の枝先から 1 度だけ作り、全 LOD で共有する（段で陰影が変わらないように）。
import math
import os
import random
import sys

import bpy
import numpy as np
from mathutils import Vector

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vegetation import (LEAVES, UP, CanopyField, Geometry, add_core, add_tube_lod,  # noqa: E402
                        along_polyline, asset_ref, build_hull, export_fbx, finish_cutout, grow,
                        height_to_normal, make_core_material, make_material, make_object, parse_args,
                        perpendicular, source_ref, tiling_noise, to_bytes, transfer_field_normals,
                        write_material, write_model, write_png)


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

    return finish_cutout(canvas.color, canvas.alpha, canvas.normal)


# --- 樹皮 ---------------------------------------------------------------------
def make_bark_texture(rng, size=512):
    """縦（枝の長さ方向）に伸びた、上下左右に繰り返す樹皮。"""
    def band(sigma_x, sigma_y):
        return tiling_noise(rng, size, sigma_x, sigma_y)

    height = 0.7 * band(0.030, 0.008) + 0.3 * band(0.12, 0.03)
    height = 1 / (1 + np.exp(-1.6 * height))  # 0〜1 へ
    dark = np.array([0.20, 0.16, 0.13])
    light = np.array([0.42, 0.37, 0.32])
    color = dark + (light - dark) * height[..., None]
    grain = 0.06 * band(0.25, 0.25)[..., None]
    color = np.clip(color * (1 + grain), 0, 1)
    return to_bytes(color), to_bytes(height_to_normal(height, 6.0) * 0.5 + 0.5)


# --- 形状 ---------------------------------------------------------------------
NEEDLES = LEAVES
# 枝先の房のカード（m）。テクスチャの縦横比 2:1 に合わせる。実物の房（長さ 10〜15 cm）に揃えた大きさ。
CARD_LENGTH, CARD_WIDTH = 0.14, 0.07
# 房の間隔はカードの長さに比例させる（カードを変えても枝あたりの覆い方が変わらない）。
STEM_SHOOT_SPACING = 0.23 * CARD_LENGTH
BRANCH_SHOOT_SPACING = 0.19 * CARD_LENGTH


# LOD ごとの作り分け。骨格（幹と枝の形）は全段で同じ乱数から作り、細かさだけを変える。
#   sides: 幹・枝・小枝の筒の角数（0 なら作らない）  stride: 筒の節を何点おきに使うか
#   spacing / scale: 枝先の間隔と大きさの倍率  cards: 枝先 1 本のカード枚数  tips: 先端の房の本数
LODS = [
    {"sides": (8, 6, 5), "stride": 1, "spacing": 1.0, "scale": 1.0, "cards": 3, "tips": 3},
    {"sides": (5, 4, 3), "stride": 2, "spacing": 2.0, "scale": 1.35, "cards": 2, "tips": 1},
    {"sides": (4, 3, 0), "stride": 4, "spacing": 3.4, "scale": 1.8, "cards": 2, "tips": 0},
]


def add_shoot(geo, base, axis, rng, scale=1.0, cards=3):
    """枝先 1 本。軸まわりに 180/cards 度ずつ回したカード。"""
    axis = axis.normalized()
    length, width = CARD_LENGTH * scale, CARD_WIDTH * scale
    base = base - axis * 0.02 * scale
    tip = base + axis * length
    reference = perpendicular(axis)
    start = rng.uniform(0, math.pi)
    geo.shoots.append((base + axis * length * 0.5, length * 0.5))
    outward = Vector((base.x, base.y, 0.0))
    outward = outward.normalized() if outward.length > 1e-4 else Vector((1, 0, 0))
    for k in range(cards):
        angle = start + k * math.pi / cards
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


def build_plant(seed, lod):
    rng = random.Random(seed)
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
        add_tube_lod(geo, points, radii, lod["sides"][0], lod["stride"])
        add_branches(geo, points, radii, rng, 1, lod)
        add_foliage(geo, points, rng.getrandbits(32), 0.40, STEM_SHOOT_SPACING, lod)
    return geo


def add_branches(geo, points, radii, rng, order, lod):
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
        # 骨格の乱数は段によらず同じだけ使う（段で形がずれないように）。作らない段も進める。
        sides = lod["sides"][order]
        add_tube_lod(geo, branch, branch_radii, sides, lod["stride"])
        if order == 1 and length > 0.3:
            add_branches(geo, branch, branch_radii, rng, 2, lod)
        foliage_seed = rng.getrandbits(32)
        if sides > 0:
            add_foliage(geo, branch, foliage_seed, 0.25 if order == 1 else 0.1, BRANCH_SHOOT_SPACING, lod)


def add_foliage(geo, points, seed, start, spacing, lod):
    """枝の外側に横向きの枝先、先端に上向きの枝先を付ける。乱数は枝ごとに独立。"""
    rng = random.Random(seed)
    # 節の間にも置けるよう、枝の長さに沿って連続に進む（節の間隔より細かい間隔を許す）。
    for position, tangent, _ in along_polyline(points, start, spacing * lod["spacing"], rng):
        sideways = tangent.cross(UP)
        sideways = sideways.normalized() if sideways.length > 1e-4 else perpendicular(tangent)
        sideways *= rng.choice((-1, 1))
        axis = tangent * 0.6 + sideways * rng.uniform(0.4, 0.8) + UP * rng.uniform(0.7, 1.2)
        add_shoot(geo, position, axis, rng, rng.uniform(0.8, 1.1) * lod["scale"], lod["cards"])
    tangent = (points[-1] - points[-2]).normalized()
    add_shoot(geo, points[-1], tangent + UP * 0.8, rng, rng.uniform(0.95, 1.15) * lod["scale"], lod["cards"])
    for k in range(min(rng.randint(2, 3), lod["tips"])):
        angle = 2 * math.pi * k / 3 + rng.uniform(-0.4, 0.4)
        spread = perpendicular(tangent)
        spread = spread * math.cos(angle) + tangent.cross(spread) * math.sin(angle)
        add_shoot(geo, points[-1], tangent + UP * 0.9 + spread * 0.7, rng,
                  rng.uniform(0.8, 1.0) * lod["scale"], lod["cards"])


# --- 樹冠の法線と芯 ---------------------------------------------------------------
CANOPY_CELL = 0.05         # 樹冠の高さの場の格子（m）
CANOPY_BLUR = 0.25         # 高さの場をぼかす幅（m、ガウスの標準偏差）
CANOPY_NORMAL_WEIGHT = 0.8  # 葉の法線を樹冠の法線へ寄せる割合
CORE_RESOLUTION = 0.05     # 芯のメタボールを面にするときの格子（m）
CORE_RADIUS_SCALE = 1.0    # 枝先の半径に対する芯のメタボールの半径
CORE_INSET = 0.04          # 芯をさらに内側へ縮める量（m）
CORE_TRIANGLES = (1600, 700, 300)  # LOD ごとの芯の面の数の目安


# 芯の色（リニア）。葉より暗くし、隙間から覗く茂みの奥に見せる。
CORE_COLOR = (0.02, 0.04, 0.016)


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
                 make_material("Needles", paths["Needles_D"], paths["Needles_N"], True),
                 make_core_material(CORE_COLOR)]

    bark_mat = os.path.join(out, "MI_Haimatsu_Bark.tgmat")
    needle_mat = os.path.join(out, "MI_Haimatsu_Needles.tgmat")
    write_material(bark_mat, "MI_Haimatsu_Bark", source_ref(paths["Bark_D"], root),
                   source_ref(paths["Bark_N"], root), 0.85, 0.0, False)
    write_material(needle_mat, "MI_Haimatsu_Needles", source_ref(paths["Needles_D"], root),
                   source_ref(paths["Needles_N"], root), 0.6, 0.5, True)
    core_mat = os.path.join(out, "MI_Haimatsu_Core.tgmat")
    write_material(core_mat, "MI_Haimatsu_Core", None, None, 0.9, 0.0, False, CORE_COLOR)

    for index in range(1, options["variants"] + 1):
        name = f"Haimatsu_Var{index}"
        objects = []
        seed = options["seed"] * 1000 + index
        # 樹冠の場と芯は LOD0 の枝先から 1 度だけ作り、全 LOD で共有する（段で陰影が変わらないように）。
        shoots = build_plant(seed, LODS[0]).shoots
        field = CanopyField(shoots, CANOPY_CELL, CANOPY_BLUR)
        hull = build_hull(f"Hull_{name}", shoots, CORE_RADIUS_SCALE, CORE_RESOLUTION)
        for level, lod in enumerate(LODS):
            geo = build_plant(seed, lod)
            transfer_field_normals(geo, field, CANOPY_NORMAL_WEIGHT)
            add_core(geo, hull, CORE_TRIANGLES[level], CORE_INSET)
            # .blend では段を奥へ並べて見比べられるようにする。
            objects.append(make_object(f"{name}_LOD{level}", geo, materials,
                                       ((index - 1) * 5.0, level * 5.0, 0)))
            triangles = geo.triangles()
            cards = geo.face_mat.count(NEEDLES)
            core = geo.face_mat.count(CORE)
            print(f"{name}_LOD{level}: {triangles} triangles, {cards} cards, core {core}")
        fbx = os.path.join(out, name + ".fbx")
        export_fbx(objects, fbx)
        # スロットの並びは FBX で最初に現れた順（幹が先）。
        write_model(os.path.join(out, name + ".tgmodel"), name, source_ref(fbx, root),
                    [asset_ref(bark_mat, root), asset_ref(needle_mat, root), asset_ref(core_mat, root)])
        bpy.data.meshes.remove(hull)

    bpy.ops.wm.save_as_mainfile(filepath=os.path.join(out, "Haimatsu.blend"))


main()
