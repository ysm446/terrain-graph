# ダケカンバ（岳樺）の簡易モデルを作る Blender スクリプト。
#
# 使い方（Blender 5.x）:
#   blender -b --factory-startup --python-exit-code 1 --python tools/blender/make_dakekamba.py -- \
#       --out data/models/Dakekamba [--root data] [--variants 3] [--seed 1]
#
# 出力（--out の下）:
#   Dakekamba_VarN.fbx         幹・枝（Bark）、葉のカード（Leaves）、芯（Core）の 3 スロット。
#                              LOD0〜2 をオブジェクト名の末尾 _LOD<n> で 1 ファイルに入れる
#   T_Dakekamba_Leaves_*.png   葉の付いた小枝のカード（RGBA、A でアルファ抜き）と法線（OpenGL 規約）
#   T_Dakekamba_Bark_*.png     樹皮（上下左右に繰り返す。横に伸びる皮目、はがれた部分）と法線
#   Dakekamba.blend            全バリエーション（手直し用）
#   *.tgmat / *.tgmodel / *.meta  terrain-graph のアセット。既にあれば上書きしない（UID を保つ）。
#                                 .tgmodel のマテリアルの並びだけは、足りない分を書き足す
#
# 共通の部品は vegetation.py、決まりごとは docs/design/vegetation-assets.md。
#
# 形: 森林限界付近の姿。根元の近くから幹が 2〜5 本出て、雪の重みで根元が斜めに寝たあと
# 弧を描いて立ち上がる（根曲がり）。高さ 4〜7 m。幹の上半分から 1 次枝が張り出し、
# その先の小枝に葉の房（葉の付いた小枝のカードを 2 枚組んだもの）を付ける。
# 背の高い木なので、葉の法線は樹冠を包む外形から取る（上面は上、側面は外、下面は下）。
# 下面が真っ黒にならないよう少し上へ寄せる。樹冠の内側には暗い芯を入れる。
import math
import os
import random
import sys

import bpy
import numpy as np
from mathutils import Vector

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vegetation import (LEAVES, UP, EnvelopeField, Geometry, add_core, add_tube_lod,  # noqa: E402
                        along_polyline, asset_ref, build_hull, export_fbx, finish_cutout, grow,
                        height_to_normal, heading, make_core_material, make_material, make_object,
                        parse_args, perpendicular, source_ref, tiling_noise, to_bytes,
                        transfer_field_normals, write_material, write_model, write_png)


# --- 葉のカード -----------------------------------------------------------------
# 画像は正方形（512 × 512）。下端の中央が小枝の付け根で、上へ伸びる小枝に葉が互い違いに付く。
# 葉は卵形でふちがギザギザ（長さ 5〜10 cm）。カードは 32 cm 角なので、葉 1 枚は 100〜150 px。
class LeafCanvas:
    def __init__(self, size):
        self.size = size
        self.color = np.zeros((size, size, 3), np.float32)
        self.alpha = np.zeros((size, size), np.float32)
        self.normal = np.zeros((size, size, 3), np.float32)
        self.normal[..., 2] = 1.0

    def _blend(self, view, coverage, rgb, normal):
        a = coverage[..., None]
        self.color[view] = self.color[view] * (1 - a) + rgb * a
        self.alpha[view] = self.alpha[view] * (1 - coverage) + coverage
        self.normal[view] = self.normal[view] * (1 - a) + normal * a

    def stroke(self, p0, p1, w0, w1, color):
        """太さが変わる丸い線分（小枝と葉柄）。座標は画素（x 右、y 下）。"""
        p0, p1 = np.asarray(p0, np.float32), np.asarray(p1, np.float32)
        d = p1 - p0
        length = float(np.hypot(*d))
        if length < 1e-3:
            return
        d /= length
        half = max(w0, w1) * 0.5 + 2
        x0, x1 = int(max(0, min(p0[0], p1[0]) - half)), int(min(self.size, max(p0[0], p1[0]) + half + 1))
        y0, y1 = int(max(0, min(p0[1], p1[1]) - half)), int(min(self.size, max(p0[1], p1[1]) + half + 1))
        if x0 >= x1 or y0 >= y1:
            return
        ys, xs = np.mgrid[y0:y1, x0:x1].astype(np.float32)
        rx, ry = xs + 0.5 - p0[0], ys + 0.5 - p0[1]
        t = np.clip((rx * d[0] + ry * d[1]) / length, 0.0, 1.0)
        side = rx * -d[1] + ry * d[0]
        dist = np.hypot(rx - d[0] * t * length, ry - d[1] * t * length)
        radius = (w0 + (w1 - w0) * t) * 0.5
        coverage = np.clip(radius - dist + 0.5, 0.0, 1.0)
        if not (coverage > 0).any():
            return
        across = np.clip(side / np.maximum(radius, 1e-3), -1.0, 1.0)
        bulge = np.sqrt(np.clip(1.0 - across * across, 0.0, 1.0))
        rgb = np.asarray(color, np.float32) * (0.7 + 0.3 * bulge)[..., None]
        n = np.stack([-d[1] * across, -d[0] * across, bulge + 0.35], axis=-1)
        n /= np.linalg.norm(n, axis=-1, keepdims=True)
        self._blend((slice(y0, y1), slice(x0, x1)), coverage, rgb, n)

    def leaf(self, base, angle, length, color, rng, underside=False):
        """付け根 base から角度 angle（上が 0、右回りが正）へ伸びる卵形の葉。"""
        direction = np.array([math.sin(angle), -math.cos(angle)], np.float32)
        across_dir = np.array([-direction[1], direction[0]], np.float32)
        width = length * rng.uniform(0.55, 0.68)
        reach = length + width
        x0, x1 = int(max(0, base[0] - reach)), int(min(self.size, base[0] + reach + 1))
        y0, y1 = int(max(0, base[1] - reach)), int(min(self.size, base[1] + reach + 1))
        if x0 >= x1 or y0 >= y1:
            return
        ys, xs = np.mgrid[y0:y1, x0:x1].astype(np.float32)
        rx, ry = xs + 0.5 - base[0], ys + 0.5 - base[1]
        s = (rx * direction[0] + ry * direction[1]) / length  # 付け根 0 → 先 1
        v = rx * across_dir[0] + ry * across_dir[1]           # 中肋からの距離（px）
        sc = np.clip(s, 0, 1)
        # 卵形（付け根寄りが広く、先がとがる）。ふちに細かいギザギザ（重鋸歯）。
        profile = np.power(np.clip(np.sin(np.pi * np.power(sc, 0.72)), 0, 1), 0.85) * (1 - 0.25 * sc)
        serration = 1 + 0.07 * np.abs(np.sin(sc * 46)) - 0.03
        half = width * 0.5 * profile * serration
        coverage = np.clip(half - np.abs(v) + 0.5, 0, 1) * ((s >= 0) & (s <= 1))
        if not (coverage > 0).any():
            return
        across = np.clip(v / np.maximum(half, 1e-3), -1, 1)
        # 側脈（中肋から先へ斜めに走る筋）と中肋。
        vein = np.clip(1 - np.abs(np.sin((sc - np.abs(across) * 0.35) * 30)) * 6, 0, 1) * 0.12
        midrib = np.clip(1.5 - np.abs(v), 0, 1)
        base_color = np.asarray(color, np.float32)
        if underside:
            base_color = base_color * 0.6 + np.array([0.30, 0.34, 0.24], np.float32)
        shade = 0.82 + 0.18 * (1 - np.abs(across)) + vein * 0.6
        rgb = base_color * shade[..., None] + np.array([0.08, 0.08, 0.03], np.float32) * midrib[..., None]
        # 中肋で浅く折れた葉（V 字）として法線を横へ傾ける。
        tilt = np.sign(v) * 0.22 + vein * 0.3
        n = np.stack([across_dir[0] * tilt, -across_dir[1] * tilt, np.ones_like(v)], axis=-1)
        n /= np.linalg.norm(n, axis=-1, keepdims=True)
        self._blend((slice(y0, y1), slice(x0, x1)), coverage, rgb, n)


def make_leaf_texture(rng, size=512):
    canvas = LeafCanvas(size)
    base = np.array([size * 0.5, size - 4], np.float32)
    tip = np.array([size * 0.5 + rng.uniform(-20, 20), size * 0.30], np.float32)
    greens = [(0.24, 0.40, 0.12), (0.28, 0.44, 0.14), (0.21, 0.36, 0.11), (0.30, 0.46, 0.16)]

    def twig(s):
        # 付け根 s=0、先端 s=1。少し曲げる。
        p = base + (tip - base) * s
        return p + np.array([14 * math.sin(s * 3.0), 0], np.float32)

    leaves = []
    count = rng.randint(10, 13)
    side = rng.choice((-1, 1))
    for k in range(count):
        s = 0.12 + 0.86 * (k / max(count - 1, 1))
        side = -side
        angle = side * math.radians(rng.uniform(35, 70) * (1 - 0.4 * s))
        length = size * rng.uniform(0.20, 0.27) * (0.85 + 0.25 * math.sin(math.pi * s))
        leaves.append((s, angle, length))
    # 先端の 2〜3 枚は前へ揃える。
    for k in range(rng.randint(2, 3)):
        leaves.append((1.0, math.radians(rng.uniform(-28, 28)), size * rng.uniform(0.18, 0.24)))
    rng.shuffle(leaves)
    # 小枝は奥の葉のあとに描く。半分を奥、半分を手前へ。
    back, front = leaves[: len(leaves) // 2], leaves[len(leaves) // 2:]

    def draw(group):
        for s, angle, length in group:
            origin = twig(min(s, 1.0))
            petiole_dir = np.array([math.sin(angle), -math.cos(angle)], np.float32)
            petiole_end = origin + petiole_dir * size * 0.03
            canvas.stroke(origin, petiole_end, 3.0, 2.2, (0.36, 0.30, 0.16))
            color = np.array(rng.choice(greens)) * rng.uniform(0.9, 1.12)
            canvas.leaf(petiole_end, angle, length, color, rng, underside=rng.random() < 0.18)

    draw(back)
    for i in range(30):
        s0, s1 = i / 30, (i + 1) / 30
        canvas.stroke(twig(s0), twig(s1), 9 - 5 * s0, 9 - 5 * s1, (0.38, 0.28, 0.22))
    draw(front)
    return finish_cutout(canvas.color, canvas.alpha, canvas.normal)


# --- 樹皮 ---------------------------------------------------------------------
def make_bark_texture(rng, size=512):
    """白〜淡い桃色の樹皮。横に伸びる黒い皮目と、薄くはがれた茶色の部分。上下左右に繰り返す。
    画像の x が幹の周、y が幹の長さ（筒の UV と同じ）。"""
    ys, xs = np.mgrid[0:size, 0:size].astype(np.float32)
    # 地の色のむら（桃色がかった所と灰色がかった所）。
    tone = tiling_noise(rng, size, 0.02, 0.012)
    pink = np.clip(tone * 0.5 + 0.5, 0, 1)[..., None]
    base = np.array([0.86, 0.85, 0.82]) * (1 - pink) + np.array([0.88, 0.78, 0.74]) * pink
    grain = tiling_noise(rng, size, 0.35, 0.08)[..., None] * 0.025
    color = base * (1 + grain)
    height = np.zeros((size, size), np.float32)
    # はがれた部分: 横に長いまだらを閾値で切る。縁を少し持ち上げる。
    peel_field = tiling_noise(rng, size, 0.012, 0.045)
    peel = np.clip((peel_field - 1.1) * 2.5, 0, 1)[..., None]
    color = color * (1 - peel) + np.array([0.62, 0.44, 0.34]) * peel
    height += np.clip((peel_field - 0.9) * 3, 0, 1) * 0.6 - peel[..., 0] * 0.4
    # 皮目: 横に細長い黒い筋。周（x）と長さ（y）の両方で繰り返すよう、距離は周期で測る。
    for _ in range(170):
        cx, cy = rng.uniform(0, size), rng.uniform(0, size)
        half_w, half_h = rng.uniform(8, 36), rng.uniform(1.0, 2.4)
        dx = (xs - cx + size / 2) % size - size / 2
        dy = (ys - cy + size / 2) % size - size / 2
        d = (dx / half_w) ** 2 + (dy / half_h) ** 2
        mark = np.clip(1.5 - d, 0, 1)
        color = color * (1 - mark[..., None] * 0.85) + np.array([0.22, 0.17, 0.15]) * mark[..., None] * 0.85
        height -= mark * 0.8
    color = np.clip(color, 0, 1)
    return to_bytes(color), to_bytes(height_to_normal(height, 3.0) * 0.5 + 0.5)


# --- 形状 ---------------------------------------------------------------------
LEAF_CARD = 0.32               # 葉の房のカード（m、正方形）
TRUNK_BARK_TILE = 0.9          # 幹の樹皮が縦に 1 回繰り返す長さ（m）
BRANCH_BARK_TILE = 0.5
BRANCH_LEAF_SPACING = 0.45 * LEAF_CARD  # 1 次枝の外側の房の間隔
TWIG_LEAF_SPACING = 0.32 * LEAF_CARD    # 小枝の房の間隔

# LOD ごとの作り分け。骨格は全段で同じ乱数から作り、細かさだけを変える。
#   sides: 幹・1 次枝・小枝の筒の角数（0 なら作らない）  stride: 筒の節を何点おきに使うか
#   spacing / scale: 房の間隔と大きさの倍率  cards: 房 1 つのカード枚数  tips: 枝先の房の数の上限
#   twig_leaves: 小枝の位置に房を付けるか（小枝の筒を省いても、房だけ残せる）
LODS = [
    {"sides": (10, 6, 4), "stride": 1, "spacing": 1.0, "scale": 1.0, "cards": 2, "tips": 3, "twig_leaves": True},
    {"sides": (7, 4, 0), "stride": 2, "spacing": 2.4, "scale": 1.45, "cards": 2, "tips": 1, "twig_leaves": True},
    {"sides": (5, 3, 0), "stride": 3, "spacing": 2.6, "scale": 1.75, "cards": 1, "tips": 1, "twig_leaves": False},
]


def add_spray(geo, base, axis, rng, scale, cards):
    """葉の房 1 つ。1 枚目はほぼ水平（葉が空を向く）、2 枚目は軸まわりに傾けた面。"""
    axis = axis.normalized()
    size = LEAF_CARD * scale
    base = base - axis * 0.03 * scale
    tip = base + axis * size
    horizontal = axis.cross(UP)
    horizontal = horizontal.normalized() if horizontal.length > 1e-4 else perpendicular(axis)
    geo.shoots.append((base + axis * size * 0.5, size * 0.5))
    roll = rng.uniform(-0.35, 0.35)
    for k in range(cards):
        angle = roll + (0 if k == 0 else rng.choice((-1, 1)) * math.radians(rng.uniform(55, 80)))
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
    """枝に沿って葉の房を付け、先端にも房を付ける。乱数は枝ごとに独立。"""
    rng = random.Random(seed)
    for position, tangent, _ in along_polyline(points, start, spacing * lod["spacing"], rng):
        sideways = tangent.cross(UP)
        sideways = sideways.normalized() if sideways.length > 1e-4 else perpendicular(tangent)
        sideways *= rng.choice((-1, 1))
        axis = tangent * 0.5 + sideways * rng.uniform(0.5, 0.9) + UP * rng.uniform(0.0, 0.35)
        add_spray(geo, position, axis, rng, rng.uniform(0.85, 1.1) * lod["scale"], lod["cards"])
    tangent = (points[-1] - points[-2]).normalized()
    for k in range(min(tip_count, lod["tips"])):
        spread = perpendicular(tangent) * rng.uniform(-0.5, 0.5)
        add_spray(geo, points[-1], tangent + spread + UP * 0.2, rng,
                  rng.uniform(0.9, 1.1) * lod["scale"], lod["cards"])


def build_tree(seed, lod):
    rng = random.Random(seed)
    geo = Geometry()
    stems = rng.randint(2, 5)
    lean = rng.uniform(0, 2 * math.pi)  # 雪で寝かされた向き（斜面の下）
    for s in range(stems):
        yaw = lean + rng.uniform(-0.9, 0.9)
        start = Vector((rng.uniform(-0.25, 0.25), rng.uniform(-0.25, 0.25), -0.05))
        length = rng.uniform(4.5, 7.8) * (1 - 0.12 * s)
        low = math.radians(rng.uniform(12, 32))    # 根元の寝た角度
        high = math.radians(rng.uniform(68, 84))   # 立ち上がった後の角度
        bend = rng.uniform(0.18, 0.35)             # 立ち上がりが終わる位置（長さの割合）

        def pitch_at(t, low=low, high=high, bend=bend):
            u = min(t / bend, 1.0)
            return low + (high - low) * (u * u * (3 - 2 * u))

        r0 = rng.uniform(0.08, 0.14) * (1 - 0.15 * s)
        points, _ = grow(start, yaw, length, pitch_at, rng, wander=0.12, step=0.12, ground=r0 * 0.7)
        radii = [r0 * (1 - 0.82 * (i / (len(points) - 1))) + 0.012 for i in range(len(points))]
        add_tube_lod(geo, points, radii, lod["sides"][0], lod["stride"], TRUNK_BARK_TILE)
        add_branches(geo, points, radii, rng, 1, lod)
        add_foliage(geo, points, rng.getrandbits(32), 0.75, BRANCH_LEAF_SPACING, lod, 2)
    return geo


def add_branches(geo, points, radii, rng, order, lod):
    total = len(points) - 1
    lengths = [0.0]
    for i in range(1, len(points)):
        lengths.append(lengths[-1] + (points[i] - points[i - 1]).length)
    full = lengths[-1]
    first = 0.45 if order == 1 else 0.12
    next_at = full * first + rng.uniform(0.0, 0.2)
    turn = rng.uniform(0, 2 * math.pi)
    for i in range(1, total):
        if lengths[i] < next_at:
            continue
        t = lengths[i] / full
        next_at = lengths[i] + (rng.uniform(0.22, 0.42) if order == 1 else rng.uniform(0.15, 0.28))
        turn += math.radians(137.5) + rng.uniform(-0.3, 0.3)  # 幹のまわりに散らす
        tangent = (points[i + 1] - points[i - 1]).normalized()
        if order == 1:
            yaw = turn
            # 斜め上へ広がる。上の枝ほど立つ。樹冠は中ほどが最も広い。
            base_pitch = math.radians(rng.uniform(22, 42) + 20 * t)
            crown = math.sin(math.pi * min(1.0, (t - first) / (1 - first) * 0.9 + 0.1))
            length = rng.uniform(1.3, 2.8) * (0.45 + 0.65 * crown)
            droop = math.radians(rng.uniform(-15, 10))  # 負は先が持ち上がる
        else:
            yaw = math.atan2(tangent.y, tangent.x) + rng.choice((-1, 1)) * math.radians(rng.uniform(30, 60))
            base_pitch = math.asin(max(-1, min(1, tangent.z))) + math.radians(rng.uniform(-5, 20))
            length = rng.uniform(0.45, 1.1) * (1 - 0.4 * t)
            droop = math.radians(rng.uniform(-10, 12))

        def pitch_at(u, base_pitch=base_pitch, droop=droop):
            return base_pitch - droop * u

        branch, _ = grow(points[i], yaw, length, pitch_at, rng, wander=0.7 if order == 1 else 0.5,
                         step=0.08 if order == 1 else 0.06,
                         ground=0.05)
        r0 = radii[i] * (0.48 if order == 1 else 0.5)
        branch_radii = [r0 * (1 - 0.75 * (j / (len(branch) - 1))) + 0.004 for j in range(len(branch))]
        # 骨格の乱数は段によらず同じだけ使う（段で形がずれないように）。作らない段も進める。
        sides = lod["sides"][order]
        add_tube_lod(geo, branch, branch_radii, sides, lod["stride"], BRANCH_BARK_TILE)
        if order == 1 and length > 0.6:
            add_branches(geo, branch, branch_radii, rng, 2, lod)
        foliage_seed = rng.getrandbits(32)
        if order == 1:
            if sides > 0:
                add_foliage(geo, branch, foliage_seed, 0.35, BRANCH_LEAF_SPACING, lod, 3)
        elif lod["twig_leaves"]:
            add_foliage(geo, branch, foliage_seed, 0.1, TWIG_LEAF_SPACING, lod, 2)
        # 小枝の房を省く段では、1 次枝の房で埋める（房を大きくしてある）。


# --- 樹冠の法線と芯 ---------------------------------------------------------------
ENVELOPE_RADIUS_SCALE = 3.0   # 房の半径に対する外形のメタボールの半径
ENVELOPE_RESOLUTION = 0.12
ENVELOPE_SMOOTH = 8
ENVELOPE_NORMAL_WEIGHT = 0.75  # 葉の法線を外形の法線へ寄せる割合
ENVELOPE_UP_BIAS = 0.3         # 下面が真っ黒にならないよう上へ足す量
CORE_RADIUS_SCALE = 1.6
CORE_RESOLUTION = 0.1
CORE_INSET = 0.12
CORE_TRIANGLES = (2200, 1000, 450)
# 芯の色（リニア）。葉より暗くし、隙間から覗く樹冠の奥に見せる。
CORE_COLOR = (0.018, 0.032, 0.012)


def main():
    options = parse_args()
    out, root = options["out"], options["root"]
    os.makedirs(out, exist_ok=True)
    rng = random.Random(options["seed"])
    np_rng = np.random.default_rng(options["seed"])

    paths = {key: os.path.join(out, f"T_Dakekamba_{key}.png")
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

    bark_mat = os.path.join(out, "MI_Dakekamba_Bark.tgmat")
    leaf_mat = os.path.join(out, "MI_Dakekamba_Leaves.tgmat")
    core_mat = os.path.join(out, "MI_Dakekamba_Core.tgmat")
    write_material(bark_mat, "MI_Dakekamba_Bark", source_ref(paths["Bark_D"], root),
                   source_ref(paths["Bark_N"], root), 0.7, 0.0, False)
    write_material(leaf_mat, "MI_Dakekamba_Leaves", source_ref(paths["Leaves_D"], root),
                   source_ref(paths["Leaves_N"], root), 0.55, 0.5, True)
    write_material(core_mat, "MI_Dakekamba_Core", None, None, 0.9, 0.0, False, CORE_COLOR)

    for index in range(1, options["variants"] + 1):
        name = f"Dakekamba_Var{index}"
        objects = []
        seed = options["seed"] * 1000 + index
        # 外形と芯は LOD0 の房から 1 度だけ作り、全 LOD で共有する（段で陰影が変わらないように）。
        shoots = build_tree(seed, LODS[0]).shoots
        envelope = build_hull(f"Envelope_{name}", shoots, ENVELOPE_RADIUS_SCALE, ENVELOPE_RESOLUTION,
                              ENVELOPE_SMOOTH)
        field = EnvelopeField(envelope)
        hull = build_hull(f"Core_{name}", shoots, CORE_RADIUS_SCALE, CORE_RESOLUTION)
        for level, lod in enumerate(LODS):
            geo = build_tree(seed, lod)
            transfer_field_normals(geo, field, ENVELOPE_NORMAL_WEIGHT, ENVELOPE_UP_BIAS)
            add_core(geo, hull, CORE_TRIANGLES[level], CORE_INSET)
            # .blend では段を奥へ並べて見比べられるようにする。
            objects.append(make_object(f"{name}_LOD{level}", geo, materials,
                                       ((index - 1) * 10.0, level * 10.0, 0)))
            cards = geo.face_mat.count(LEAVES)
            print(f"{name}_LOD{level}: {geo.triangles()} triangles, {cards} cards")
        fbx = os.path.join(out, name + ".fbx")
        export_fbx(objects, fbx)
        # スロットの並びは FBX で最初に現れた順（幹が先）。
        write_model(os.path.join(out, name + ".tgmodel"), name, source_ref(fbx, root),
                    [asset_ref(bark_mat, root), asset_ref(leaf_mat, root), asset_ref(core_mat, root)])
        bpy.data.meshes.remove(envelope)
        bpy.data.meshes.remove(hull)

    bpy.ops.wm.save_as_mainfile(filepath=os.path.join(out, "Dakekamba.blend"))


main()
