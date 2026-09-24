# ミヤマハンノキ（深山榛木）の簡易モデルを作る Blender スクリプト。
#
# 使い方（Blender 5.x）:
#   blender -b --factory-startup --python-exit-code 1 --python tools/blender/make_miyamahannoki.py -- \
#       --out data/Models/Miyamahannoki [--root data] [--variants 3] [--seed 1]
#
# 出力（--out の下）:
#   Miyamahannoki_VarN.fbx         幹・枝（Bark）、葉のカード（Leaves）、芯（Core）の 3 スロット。
#                                  LOD0〜2 をオブジェクト名の末尾 _LOD<n> で 1 ファイルに入れる
#   T_Miyamahannoki_Leaves_*.png   葉の付いた小枝のカード（RGBA、A でアルファ抜き）と法線（OpenGL 規約）
#   T_Miyamahannoki_Bark_*.png     樹皮（暗い灰褐色。横長の小さな皮目）と法線
#   Miyamahannoki.blend            全バリエーション（手直し用）
#   *.tgmat / *.tgmodel / *.meta   terrain-graph のアセット。既にあれば上書きしない（UID を保つ）。
#                                  .tgmodel のマテリアルの並びだけは、足りない分を書き足す
#
# 共通の部品は vegetation.py、決まりごとは docs/design/vegetation-assets.md。
#
# 形: 森林限界付近の株立ちの低木。根元から幹が 8〜14 本、雪に押されて斜めに広がってから
# 立ち上がり、高さ 2〜3.5 m・幅 3〜5 m の丸い藪になる。幹の中ほどから上に枝を出し、
# 小枝に幅の広い卵形の葉（長さ 5〜10 cm、明るく艶のある黄緑）を付ける。
# ハイマツ（暗い青緑の針葉のマット）の中で、明るい広葉樹の塊として見分けがつくようにする。
# 葉の法線は藪を包む外形から取り、下面が真っ黒にならないよう少し上へ寄せる。内側に暗い芯を入れる。
import math
import os
import random
import sys

import bpy
import numpy as np
from mathutils import Vector

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vegetation import (LEAVES, UP, EnvelopeField, Geometry, LeafCanvas, add_core,  # noqa: E402
                        add_tube_lod, along_polyline, asset_ref, build_hull, export_fbx,
                        finish_cutout, grow, height_to_normal, make_core_material, make_material,
                        make_object, parse_args, perpendicular, source_ref, tiling_noise, to_bytes,
                        transfer_field_normals, write_material, write_model, write_png)


# --- 葉のカード -----------------------------------------------------------------
# 画像は正方形（512 × 512）。下端の中央が小枝の付け根で、上へ伸びる小枝に葉が互い違いに付く。
# 葉は幅の広い卵形でふちが二重のギザギザ（長さ 5〜10 cm）。カードは 30 cm 角。
def make_leaf_texture(rng, size=512):
    canvas = LeafCanvas(size)
    base = np.array([size * 0.5, size - 4], np.float32)
    tip = np.array([size * 0.5 + rng.uniform(-16, 16), size * 0.34], np.float32)
    # ダケカンバより明るく黄みの強い緑（艶のある葉が光を返す）。
    greens = [(0.30, 0.46, 0.13), (0.34, 0.50, 0.15), (0.27, 0.43, 0.12), (0.36, 0.52, 0.17)]

    def twig(s):
        p = base + (tip - base) * s
        return p + np.array([10 * math.sin(s * 2.6), 0], np.float32)

    leaves = []
    count = rng.randint(8, 10)
    side = rng.choice((-1, 1))
    for k in range(count):
        s = 0.10 + 0.86 * (k / max(count - 1, 1))
        side = -side
        angle = side * math.radians(rng.uniform(40, 75) * (1 - 0.35 * s))
        length = size * rng.uniform(0.24, 0.31) * (0.85 + 0.25 * math.sin(math.pi * s))
        leaves.append((s, angle, length))
    for k in range(rng.randint(1, 2)):
        leaves.append((1.0, math.radians(rng.uniform(-20, 20)), size * rng.uniform(0.22, 0.27)))
    rng.shuffle(leaves)
    back, front = leaves[: len(leaves) // 2], leaves[len(leaves) // 2:]

    def draw(group):
        for s, angle, length in group:
            origin = twig(min(s, 1.0))
            petiole_dir = np.array([math.sin(angle), -math.cos(angle)], np.float32)
            petiole_end = origin + petiole_dir * size * 0.025
            canvas.stroke(origin, petiole_end, 3.0, 2.4, (0.30, 0.28, 0.16))
            color = np.array(rng.choice(greens)) * rng.uniform(0.9, 1.12)
            canvas.leaf(petiole_end, angle, length, color, rng, underside=rng.random() < 0.15,
                        widths=(0.70, 0.85))

    draw(back)
    for i in range(30):
        s0, s1 = i / 30, (i + 1) / 30
        canvas.stroke(twig(s0), twig(s1), 8 - 4 * s0, 8 - 4 * s1, (0.26, 0.21, 0.17))
    draw(front)
    return finish_cutout(canvas.color, canvas.alpha, canvas.normal)


# --- 樹皮 ---------------------------------------------------------------------
def make_bark_texture(rng, size=512):
    """暗い灰褐色でなめらかな樹皮。横長の小さな明るい皮目。上下左右に繰り返す。
    画像の x が幹の周、y が幹の長さ（筒の UV と同じ）。"""
    ys, xs = np.mgrid[0:size, 0:size].astype(np.float32)
    tone = tiling_noise(rng, size, 0.03, 0.02)
    color = np.array([0.24, 0.21, 0.18]) + np.array([0.03, 0.03, 0.03]) * np.clip(tone, -2, 2)[..., None]
    height = 0.1 * tiling_noise(rng, size, 0.25, 0.25)
    for _ in range(140):
        cx, cy = rng.uniform(0, size), rng.uniform(0, size)
        half_w, half_h = rng.uniform(3, 8), rng.uniform(0.8, 1.6)
        dx = (xs - cx + size / 2) % size - size / 2
        dy = (ys - cy + size / 2) % size - size / 2
        mark = np.clip(1.5 - ((dx / half_w) ** 2 + (dy / half_h) ** 2), 0, 1)
        color = color * (1 - mark[..., None] * 0.6) + np.array([0.46, 0.40, 0.32]) * mark[..., None] * 0.6
        height += mark * 0.4
    grain = tiling_noise(rng, size, 0.4, 0.1)[..., None] * 0.02
    color = np.clip(color * (1 + grain), 0, 1)
    return to_bytes(color), to_bytes(height_to_normal(height, 3.0) * 0.5 + 0.5)


# --- 形状 ---------------------------------------------------------------------
LEAF_CARD = 0.30               # 葉の小枝のカード（m、正方形）
TRUNK_BARK_TILE = 0.6
BRANCH_BARK_TILE = 0.4
BRANCH_LEAF_SPACING = 0.40 * LEAF_CARD  # 枝の外側のカードの間隔
TWIG_LEAF_SPACING = 0.30 * LEAF_CARD    # 小枝のカードの間隔

# LOD ごとの作り分け。骨格は全段で同じ乱数から作り、細かさだけを変える。
#   sides: 幹・枝・小枝の筒の角数（0 なら作らない）  stride: 筒の節を何点おきに使うか
#   spacing / scale: カードの間隔と大きさの倍率  cards: 位置ごとのカード枚数  tips: 枝先のカードの数の上限
#   twig_leaves: 小枝の位置にカードを付けるか（小枝の筒を省いても、カードだけ残せる）
LODS = [
    {"sides": (7, 5, 3), "stride": 1, "spacing": 1.0, "scale": 1.0, "cards": 2, "tips": 2, "twig_leaves": True},
    {"sides": (5, 3, 0), "stride": 2, "spacing": 2.3, "scale": 1.45, "cards": 2, "tips": 1, "twig_leaves": True},
    {"sides": (4, 0, 0), "stride": 3, "spacing": 2.6, "scale": 1.8, "cards": 1, "tips": 1, "twig_leaves": False},
]


def add_spray(geo, base, axis, rng, scale, cards):
    """葉の小枝 1 つ。1 枚目はほぼ水平（葉が空を向く）、2 枚目は軸まわりに傾けた面。"""
    axis = axis.normalized()
    size = LEAF_CARD * scale
    base = base - axis * 0.03 * scale
    tip = base + axis * size
    horizontal = axis.cross(UP)
    horizontal = horizontal.normalized() if horizontal.length > 1e-4 else perpendicular(axis)
    geo.shoots.append((base + axis * size * 0.5, size * 0.5))
    roll = rng.uniform(-0.35, 0.35)
    for k in range(cards):
        angle = roll + (0 if k == 0 else rng.choice((-1, 1)) * math.radians(rng.uniform(50, 80)))
        across = (horizontal * math.cos(angle) + axis.cross(horizontal) * math.sin(angle)).normalized()
        face = across.cross(axis).normalized()
        if face.z < 0:
            face = -face
        corners = [base - across * size / 2, base + across * size / 2,
                   tip + across * size / 2, tip - across * size / 2]
        normals = [(face * 0.6 + UP * 0.4).normalized()] * 4
        first = len(geo.verts)
        geo.verts.extend(tuple(c) for c in corners)
        geo.add_face([first, first + 1, first + 2, first + 3], LEAVES, [(0, 0), (1, 0), (1, 1), (0, 1)], normals)


def add_foliage(geo, points, seed, start, spacing, lod, tip_count):
    """枝に沿って葉の小枝を付け、先端にも付ける。乱数は枝ごとに独立。"""
    rng = random.Random(seed)
    for position, tangent, _ in along_polyline(points, start, spacing * lod["spacing"], rng):
        sideways = tangent.cross(UP)
        sideways = sideways.normalized() if sideways.length > 1e-4 else perpendicular(tangent)
        sideways *= rng.choice((-1, 1))
        axis = tangent * 0.5 + sideways * rng.uniform(0.5, 0.9) + UP * rng.uniform(0.05, 0.4)
        add_spray(geo, position, axis, rng, rng.uniform(0.85, 1.1) * lod["scale"], lod["cards"])
    tangent = (points[-1] - points[-2]).normalized()
    for k in range(min(tip_count, lod["tips"])):
        spread = perpendicular(tangent) * rng.uniform(-0.5, 0.5)
        add_spray(geo, points[-1], tangent + spread + UP * 0.3, rng,
                  rng.uniform(0.9, 1.1) * lod["scale"], lod["cards"])


def build_shrub(seed, lod):
    rng = random.Random(seed)
    geo = Geometry()
    stems = rng.randint(8, 14)
    lean = rng.uniform(0, 2 * math.pi)  # 雪で押された向き（斜面の下）
    for s in range(stems):
        # 株の周りへ放射状に広がる。雪の向きへ少し偏らせる。
        yaw = lean + 2 * math.pi * s / stems + rng.uniform(-0.4, 0.4)
        start = Vector((rng.uniform(-0.2, 0.2), rng.uniform(-0.2, 0.2), -0.05))
        length = rng.uniform(2.2, 3.8)
        low = math.radians(rng.uniform(10, 28))    # 根元の寝た角度
        high = math.radians(rng.uniform(40, 62))   # 立ち上がった後の角度（横へ広がって丸い藪になる）
        bend = rng.uniform(0.25, 0.45)

        def pitch_at(t, low=low, high=high, bend=bend):
            u = min(t / bend, 1.0)
            return low + (high - low) * (u * u * (3 - 2 * u))

        r0 = rng.uniform(0.03, 0.06)
        points, _ = grow(start, yaw, length, pitch_at, rng, wander=0.2, step=0.12, ground=r0 * 0.7)
        radii = [r0 * (1 - 0.8 * (i / (len(points) - 1))) + 0.008 for i in range(len(points))]
        add_tube_lod(geo, points, radii, lod["sides"][0], lod["stride"], TRUNK_BARK_TILE)
        add_branches(geo, points, radii, rng, 1, lod)
        add_foliage(geo, points, rng.getrandbits(32), 0.35, BRANCH_LEAF_SPACING, lod, 2)
    return geo


def add_branches(geo, points, radii, rng, order, lod):
    total = len(points) - 1
    lengths = [0.0]
    for i in range(1, len(points)):
        lengths.append(lengths[-1] + (points[i] - points[i - 1]).length)
    full = lengths[-1]
    first = 0.22 if order == 1 else 0.15
    next_at = full * first + rng.uniform(0.0, 0.15)
    side = rng.choice((-1, 1))
    for i in range(1, total):
        if lengths[i] < next_at:
            continue
        t = lengths[i] / full
        next_at = lengths[i] + (rng.uniform(0.25, 0.45) if order == 1 else rng.uniform(0.15, 0.25))
        side = -side
        tangent = (points[i + 1] - points[i - 1]).normalized()
        yaw = math.atan2(tangent.y, tangent.x) + side * math.radians(rng.uniform(35, 65))
        base_pitch = math.asin(max(-1, min(1, tangent.z))) + math.radians(rng.uniform(-10, 15))
        if order == 1:
            length = rng.uniform(0.8, 1.6) * (1 - 0.3 * t)
        else:
            length = rng.uniform(0.25, 0.6) * (1 - 0.4 * t)
        droop = math.radians(rng.uniform(-5, 15))

        def pitch_at(u, base_pitch=base_pitch, droop=droop):
            return base_pitch - droop * u

        branch, _ = grow(points[i], yaw, length, pitch_at, rng, wander=0.5, step=0.08, ground=0.05)
        r0 = radii[i] * 0.5
        branch_radii = [r0 * (1 - 0.7 * (j / (len(branch) - 1))) + 0.004 for j in range(len(branch))]
        sides = lod["sides"][order]
        add_tube_lod(geo, branch, branch_radii, sides, lod["stride"], BRANCH_BARK_TILE)
        if order == 1 and length > 0.5:
            add_branches(geo, branch, branch_radii, rng, 2, lod)
        foliage_seed = rng.getrandbits(32)
        if order == 1:
            add_foliage(geo, branch, foliage_seed, 0.3, BRANCH_LEAF_SPACING, lod, 2)
        elif lod["twig_leaves"]:
            add_foliage(geo, branch, foliage_seed, 0.1, TWIG_LEAF_SPACING, lod, 1)


# --- 樹冠の法線と芯 ---------------------------------------------------------------
# 密な丸い藪なので、ダケカンバより外形を少し大きく取り、藪のこぶに沿わせる。
ENVELOPE_RADIUS_SCALE = 2.2   # 房の半径に対する外形のメタボールの半径
ENVELOPE_RESOLUTION = 0.1
ENVELOPE_SMOOTH = 8
ENVELOPE_NORMAL_WEIGHT = 0.75  # 葉の法線を外形の法線へ寄せる割合
ENVELOPE_UP_BIAS = 0.3         # 下面が真っ黒にならないよう上へ足す量
CORE_RADIUS_SCALE = 1.6
CORE_RESOLUTION = 0.08
CORE_INSET = 0.1
CORE_TRIANGLES = (2000, 900, 400)
# 芯の色（リニア）。葉より暗くし、隙間から覗く藪の奥に見せる。
CORE_COLOR = (0.02, 0.035, 0.012)


def main():
    options = parse_args()
    out, root = options["out"], options["root"]
    os.makedirs(out, exist_ok=True)
    rng = random.Random(options["seed"])
    np_rng = np.random.default_rng(options["seed"])

    paths = {key: os.path.join(out, f"T_Miyamahannoki_{key}.png")
             for key in ("Leaves_D", "Leaves_N", "Bark_D", "Bark_N")}
    leaves, leaves_normal = make_leaf_texture(rng)
    write_png(paths["Leaves_D"], leaves)
    write_png(paths["Leaves_N"], leaves_normal)
    bark, bark_normal = make_bark_texture(np_rng)
    write_png(paths["Bark_D"], bark)
    write_png(paths["Bark_N"], bark_normal)

    bpy.ops.wm.read_factory_settings(use_empty=True)
    materials = [make_material("Bark", paths["Bark_D"], paths["Bark_N"], False),
                 make_material("Leaves", paths["Leaves_D"], paths["Leaves_N"], True),
                 make_core_material(CORE_COLOR)]

    bark_mat = os.path.join(out, "MI_Miyamahannoki_Bark.tgmat")
    leaf_mat = os.path.join(out, "MI_Miyamahannoki_Leaves.tgmat")
    core_mat = os.path.join(out, "MI_Miyamahannoki_Core.tgmat")
    write_material(bark_mat, "MI_Miyamahannoki_Bark", source_ref(paths["Bark_D"], root),
                   source_ref(paths["Bark_N"], root), 0.7, 0.0, False)
    # 艶のある葉なのでラフネスはダケカンバ（0.55）より低め。
    write_material(leaf_mat, "MI_Miyamahannoki_Leaves", source_ref(paths["Leaves_D"], root),
                   source_ref(paths["Leaves_N"], root), 0.45, 0.5, True)
    write_material(core_mat, "MI_Miyamahannoki_Core", None, None, 0.9, 0.0, False, CORE_COLOR)

    for index in range(1, options["variants"] + 1):
        name = f"Miyamahannoki_Var{index}"
        objects = []
        seed = options["seed"] * 1000 + index
        # 外形と芯は LOD0 の房から 1 度だけ作り、全 LOD で共有する（段で陰影が変わらないように）。
        shoots = build_shrub(seed, LODS[0]).shoots
        envelope = build_hull(f"Envelope_{name}", shoots, ENVELOPE_RADIUS_SCALE, ENVELOPE_RESOLUTION,
                              ENVELOPE_SMOOTH)
        field = EnvelopeField(envelope)
        hull = build_hull(f"Core_{name}", shoots, CORE_RADIUS_SCALE, CORE_RESOLUTION)
        for level, lod in enumerate(LODS):
            geo = build_shrub(seed, lod)
            transfer_field_normals(geo, field, ENVELOPE_NORMAL_WEIGHT, ENVELOPE_UP_BIAS)
            add_core(geo, hull, CORE_TRIANGLES[level], CORE_INSET)
            # .blend では段を奥へ並べて見比べられるようにする。
            objects.append(make_object(f"{name}_LOD{level}", geo, materials,
                                       ((index - 1) * 8.0, level * 8.0, 0)))
            cards = geo.face_mat.count(LEAVES)
            print(f"{name}_LOD{level}: {geo.triangles()} triangles, {cards} cards")
        fbx = os.path.join(out, name + ".fbx")
        export_fbx(objects, fbx)
        # スロットの並びは FBX で最初に現れた順（幹が先）。
        write_model(os.path.join(out, name + ".tgmodel"), name, source_ref(fbx, root),
                    [asset_ref(bark_mat, root), asset_ref(leaf_mat, root), asset_ref(core_mat, root)])
        bpy.data.meshes.remove(envelope)
        bpy.data.meshes.remove(hull)

    bpy.ops.wm.save_as_mainfile(filepath=os.path.join(out, "Miyamahannoki.blend"))


main()
