# アロラマツ（Arve / Zirbe、Pinus cembra）の簡易モデルを作る Blender スクリプト。
# スイス・エンガディンの森林限界付近の林（カラマツとの混交林）の主役。
#
# 使い方（Blender 5.x）:
#   blender -b --factory-startup --python-exit-code 1 --python tools/blender/make_arve.py -- \
#       --out data/Models/Arve [--root data] [--variants 3] [--seed 1]
#
# 出力（--out の下）:
#   Arve_VarN.fbx           幹・枝（Bark）、針葉の房のカード（Needles）、芯（Core）の 3 スロット。
#                           LOD0〜2 をオブジェクト名の末尾 _LOD<n> で 1 ファイルに入れる
#   T_Arve_Needles_*.png    枝先に集まる 5 本 1 束の針葉の房のカード（RGBA）と法線（OpenGL 規約）
#   T_Arve_Bark_*.png       樹皮（灰色で、赤褐色の割れ目のある板）と法線
#   Arve.blend              全バリエーション（手直し用）
#   *.tgmat / *.tgmodel / *.meta  terrain-graph のアセット。既にあれば上書きしない（UID を保つ）。
#
# 共通の部品は vegetation.py、決まりごとは docs/design/vegetation-assets.md。
# 形の作り方はシラビソ（make_shirabiso.py）が元で、違いは次のとおり。
#   - 高さ 8〜14 m、太い幹。開けた林の木なので枝は根元近くから付き、樹冠は太い円柱〜卵形で頭が丸い。
#   - 枝は斜め上へ出て先が上へ反る。付け根近くから小枝が密に出て、その先に針葉の房が集まる
#     （樹冠は濃く詰まり、房のこぶの集まりに見える）。
#   - 針葉は 5 本 1 束で長さ 5〜9 cm、内側の白い気孔帯で青みがかった濃い緑（ハイマツと同じ五葉松の仲間）。
#   - 一部の木（Var3 など、乱数で決める）は上の方で幹が 2〜3 本に分かれた燭台形（雷や雪で頂が折れた古木）。
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
                        height_to_normal, make_core_material, make_material, make_object,
                        parse_args, perpendicular, source_ref, tiling_noise, to_bytes,
                        transfer_field_normals, write_material, write_model, write_png)


# --- 針葉の房のカード ---------------------------------------------------------------
# 画像は縦長（幅 768 × 高さ 1024）。下端の中央が小枝の付け根で、上へ伸びる小枝の先の 7 割に
# 5 本 1 束の針葉が付き、先ほど前へ揃って筆のようになる。途中から左右へ側枝が出る。
# カードは 18 × 24 cm なので、1 cm は 42.7 px。
class Canvas:
    def __init__(self, width, height):
        self.width, self.height = width, height
        self.color = np.zeros((height, width, 3), np.float32)
        self.alpha = np.zeros((height, width), np.float32)
        self.normal = np.zeros((height, width, 3), np.float32)
        self.normal[..., 2] = 1.0

    def stroke(self, p0, p1, w0, w1, color, stomata=0.0):
        """太さが変わる丸い線分を描く。座標は画素（x 右、y 下）。stomata は片側の白い気孔帯の強さ。"""
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
        side = rx * -d[1] + ry * d[0]
        dist = np.hypot(rx - d[0] * t * length, ry - d[1] * t * length)
        radius = (w0 + (w1 - w0) * t) * 0.5
        coverage = np.clip(radius - dist + 0.5, 0.0, 1.0)
        if not (coverage > 0).any():
            return
        across = np.clip(side / np.maximum(radius, 1e-3), -1.0, 1.0)
        bulge = np.sqrt(np.clip(1.0 - across * across, 0.0, 1.0))
        # 三角柱の針葉の内側の面に白い気孔帯（片側の筋）。
        tint = np.clip((across - 0.1) * 2.2, 0.0, 1.0)[..., None] * stomata
        base = np.asarray(color, np.float32)
        pale = np.array([0.50, 0.60, 0.60], np.float32)
        rgb = (base * (1 - tint) + pale * tint) * (0.74 + 0.26 * bulge)[..., None]
        a = coverage[..., None]
        view = (slice(y0, y1), slice(x0, x1))
        self.color[view] = self.color[view] * (1 - a) + rgb * a
        self.alpha[view] = self.alpha[view] * (1 - coverage) + coverage
        n = np.stack([-d[1] * across, -d[0] * across, bulge + 0.35], axis=-1)
        n /= np.linalg.norm(n, axis=-1, keepdims=True)
        self.normal[view] = self.normal[view] * (1 - a) + n * a


def make_needle_texture(rng, width=768, height=1024):
    canvas = Canvas(width, height)
    cm = height / 24.0
    base = np.array([width * 0.5, height - 6], np.float32)
    tip = np.array([width * 0.5 + rng.uniform(-10, 10), height * 0.30], np.float32)
    # 青みがかった濃い緑。気孔帯と合わせて、遠目にハイマツより少し暗く青い。
    greens = [(0.10, 0.20, 0.13), (0.09, 0.18, 0.12), (0.11, 0.22, 0.14), (0.10, 0.19, 0.14)]

    def main_twig(s):
        return base + (tip - base) * s + np.array([8 * math.sin(s * 2.2), 0], np.float32)

    # 側枝: 主軸の途中から斜め前へ 2 本（左右）。先の房が主軸の房と並んで扇になる。
    twigs = [(main_twig, 1.0)]
    for k, side in enumerate((-1, 1)):
        s0 = rng.uniform(0.20, 0.30)
        angle = math.radians(rng.uniform(18, 26)) * side
        length = (base[1] - tip[1]) * rng.uniform(0.55, 0.68)
        origin = main_twig(s0)
        direction = np.array([math.sin(angle), -math.cos(angle)], np.float32)

        def side_twig(s, origin=origin, direction=direction, length=length):
            return origin + direction * length * s

        twigs.append((side_twig, 0.7))

    def bundle(twig, s, front):
        origin = twig(s)
        ahead = twig(min(s + 0.02, 1.0)) - twig(max(s - 0.02, 0.0))
        ahead /= max(np.hypot(*ahead), 1e-4)
        heading_angle = math.atan2(ahead[0], -ahead[1])
        side = rng.choice((-1.0, 1.0))
        # 付け根側は大きく開き、先ほど前へ揃う（筆のような房）。
        spread = math.radians(36 - 22 * s + rng.uniform(-8, 8))
        color = np.array(rng.choice(greens)) * rng.uniform(0.85, 1.15)
        for k in range(5):
            angle = heading_angle + side * (spread + math.radians(rng.uniform(-8, 8))) + math.radians((k - 2) * 4)
            tilt = math.radians(rng.uniform(0, 55))  # 画面の前後へ倒れた針葉は短く見える
            length = cm * rng.uniform(4.8, 8.0) * math.cos(tilt)
            direction = np.array([math.sin(angle), -math.cos(angle)], np.float32)
            bend = np.array([-direction[1], direction[0]], np.float32) * side * rng.uniform(0, 0.08)
            canvas.stroke(origin, origin + (direction + bend) * length, rng.uniform(4.8, 6.0), 1.8, color,
                          stomata=0.45 if front else 0.25)

    def tuft(twig, span, front):
        # 針葉は小枝の先の 7 割（数年分）にだけ付く。1 cm あたり約 1.4 束（表と裏で半分ずつ）。
        count = int(span * (base[1] - tip[1]) / cm * 0.7)
        for i in range(count):
            bundle(twig, 0.3 + 0.7 * rng.uniform(0.0, 1.0) ** 0.8, front)
        # 先端の冬芽のまわりの房。
        end = twig(1.0)
        ahead = twig(1.0) - twig(0.96)
        ahead /= max(np.hypot(*ahead), 1e-4)
        heading_angle = math.atan2(ahead[0], -ahead[1])
        for k in range(10 if front else 8):
            angle = heading_angle + math.radians(rng.uniform(-22, 22))
            direction = np.array([math.sin(angle), -math.cos(angle)], np.float32)
            color = np.array(rng.choice(greens)) * rng.uniform(0.95, 1.2)
            canvas.stroke(end, end + direction * cm * rng.uniform(4.5, 7.5), 4.6, 1.6, color, stomata=0.35)

    for twig, span in twigs:
        tuft(twig, span, False)
    for twig, span in twigs:
        w = 11.0 if span == 1.0 else 8.0
        for i in range(30):
            s0, s1 = i / 30, (i + 1) / 30
            canvas.stroke(twig(s0), twig(s1), w - 4 * s0, w - 4 * s1, (0.30, 0.24, 0.18))
    for twig, span in twigs:
        tuft(twig, span, True)
    return finish_cutout(canvas.color, canvas.alpha, canvas.normal)


# --- 樹皮 ---------------------------------------------------------------------
def make_bark_texture(rng, size=512):
    """灰色の樹皮。縦長の板に割れ、割れ目の奥は赤褐色（古い幹の姿）。上下左右に繰り返す。
    画像の x が幹の周、y が幹の長さ（筒の UV と同じ）。"""
    ys, xs = np.mgrid[0:size, 0:size].astype(np.float32)
    tone = tiling_noise(rng, size, 0.03, 0.02)
    color = np.array([0.40, 0.37, 0.34]) + np.array([0.04, 0.04, 0.04]) * np.clip(tone, -2, 2)[..., None]
    height = 0.1 * tiling_noise(rng, size, 0.2, 0.2)
    # 縦の割れ（板の境目）。奥は赤褐色で暗い。
    cracks = tiling_noise(rng, size, 0.10, 0.02)
    crack = np.clip((cracks - 0.8) * 1.3, 0, 1)
    color = color * (1 - 0.35 * crack[..., None]) + np.array([0.06, 0.01, -0.02]) * crack[..., None]
    height -= crack * 0.7
    # 板（縦長の楕円）。表面は灰色で、縁が少し明るい。
    for _ in range(90):
        cx, cy = rng.uniform(0, size), rng.uniform(0, size)
        half_w, half_h = rng.uniform(12, 22), rng.uniform(30, 60)
        dx = (xs - cx + size / 2) % size - size / 2
        dy = (ys - cy + size / 2) % size - size / 2
        plate = np.clip(1 - ((dx / half_w) ** 2 + (dy / half_h) ** 2), 0, 1)
        height += np.sqrt(plate) * 0.3
        color = color * (1 + 0.08 * plate[..., None])
    grain = tiling_noise(rng, size, 0.4, 0.1)[..., None] * 0.03
    color = np.clip(color * (1 + grain), 0, 1)
    return to_bytes(color), to_bytes(height_to_normal(height, 3.0) * 0.5 + 0.5)


# --- 形状 ---------------------------------------------------------------------
CARD_LENGTH, CARD_WIDTH = 0.30, 0.225   # 針葉の房のカード（m）。テクスチャの縦横比 4:3
TRUNK_BARK_TILE = 1.4
BRANCH_BARK_TILE = 0.5
BRANCH_CARD_SPACING = 0.42 * CARD_LENGTH  # 枝に沿ったカードの間隔
TWIG_CARD_SPACING = 0.42 * CARD_LENGTH    # 小枝に沿ったカードの間隔

# LOD ごとの作り分け。骨格は全段で同じ乱数から作り、細かさだけを変える。
#   sides: 幹・枝・小枝の筒の角数（0 なら作らない）  stride: 筒の節を何点おきに使うか
#   spacing / scale: カードの間隔と大きさの倍率  cards: 位置ごとのカード枚数
#   twig_cards: 小枝の位置にカードを付けるか（小枝の筒を省いても、カードだけ残せる）
LODS = [
    {"sides": (10, 5, 3), "stride": 1, "spacing": 1.0, "scale": 1.0, "cards": 2, "twig_cards": True},
    {"sides": (7, 3, 0), "stride": 2, "spacing": 3.0, "scale": 1.6, "cards": 2, "twig_cards": True},
    {"sides": (5, 0, 0), "stride": 4, "spacing": 2.8, "scale": 2.2, "cards": 1, "twig_cards": False},
]


def add_tuft(geo, base, axis, rng, scale, cards):
    """針葉の房 1 つ。房は筆のように立体なので、2 枚目は軸まわりに大きく回して十字に近く組む。"""
    axis = axis.normalized()
    length, width = CARD_LENGTH * scale, CARD_WIDTH * scale
    base = base - axis * 0.03 * scale
    tip = base + axis * length
    horizontal = axis.cross(UP)
    horizontal = horizontal.normalized() if horizontal.length > 1e-4 else perpendicular(axis)
    geo.shoots.append((base + axis * length * 0.55, length * 0.55))
    roll = rng.uniform(-0.4, 0.4)
    for k in range(cards):
        angle = roll + (0 if k == 0 else rng.choice((-1, 1)) * math.radians(rng.uniform(65, 95)))
        across = (horizontal * math.cos(angle) + axis.cross(horizontal) * math.sin(angle)).normalized()
        face = across.cross(axis).normalized()
        if face.z < 0:
            face = -face
        corners = [base - across * width / 2, base + across * width / 2,
                   tip + across * width / 2, tip - across * width / 2]
        normals = [(face * 0.6 + UP * 0.4).normalized()] * 4
        first = len(geo.verts)
        geo.verts.extend(tuple(c) for c in corners)
        geo.add_face([first, first + 1, first + 2, first + 3], LEAVES, [(0, 0), (1, 0), (1, 1), (0, 1)], normals)


def add_foliage(geo, points, seed, start, spacing, lod, tip_cards, size=1.0):
    """枝の start より先に斜め上を向く房を並べ、先端にも房を付ける。乱数は枝ごとに独立。"""
    rng = random.Random(seed)
    for position, tangent, _ in along_polyline(points, start, spacing * lod["spacing"], rng):
        sideways = tangent.cross(UP)
        sideways = sideways.normalized() if sideways.length > 1e-4 else perpendicular(tangent)
        sideways *= rng.choice((-1, 1))
        axis = tangent * 0.6 + sideways * rng.uniform(0.3, 0.7) + UP * rng.uniform(0.25, 0.6)
        add_tuft(geo, position, axis, rng, rng.uniform(0.85, 1.1) * lod["scale"] * size, lod["cards"])
    tangent = (points[-1] - points[-2]).normalized()
    for k in range(tip_cards):
        spread = perpendicular(tangent) * rng.uniform(-0.5, 0.5)
        add_tuft(geo, points[-1], tangent + spread + UP * 0.35, rng,
                 rng.uniform(0.9, 1.1) * lod["scale"] * size, lod["cards"])


def crown_profile(t):
    """高さの割合 t（樹冠の下 0 → 上 1）での枝の長さの割合。太い円柱〜卵形で、頭が丸い。"""
    t = min(max(t, 0.0), 1.0)
    return (1 - t ** 1.8) ** 0.9 * (0.85 + 0.15 * min(1.0, t * 4))


def add_twigs(geo, branch, branch_radii, rng, lod, size):
    """枝の付け根近くから、斜め上へ反る小枝を左右交互に出す。骨格の乱数は段によらず同じだけ使う。"""
    lengths = [0.0]
    for i in range(1, len(branch)):
        lengths.append(lengths[-1] + (branch[i] - branch[i - 1]).length)
    full = lengths[-1]
    next_at = full * 0.15 + rng.uniform(0.0, 0.1)
    side = rng.choice((-1, 1))
    for i in range(1, len(branch) - 1):
        if lengths[i] < next_at:
            continue
        s = lengths[i] / full
        next_at = lengths[i] + rng.uniform(0.14, 0.24)
        side = -side
        tangent = (branch[i + 1] - branch[i - 1]).normalized()
        yaw = math.atan2(tangent.y, tangent.x) + side * math.radians(rng.uniform(35, 65))
        pitch = math.asin(max(-1, min(1, tangent.z))) + math.radians(rng.uniform(10, 30))
        length = rng.uniform(0.35, 0.8) * (1 - 0.4 * s) * min(1.0, full / 1.5)
        twig, _ = grow(branch[i], yaw, length, lambda v, pitch=pitch: pitch + math.radians(20) * v, rng,
                       wander=0.35, step=0.2, ground=0.3)
        t0 = branch_radii[i] * 0.5
        twig_radii = [t0 * (1 - 0.7 * (j / (len(twig) - 1))) + 0.004 for j in range(len(twig))]
        add_tube_lod(geo, twig, twig_radii, lod["sides"][2], lod["stride"], BRANCH_BARK_TILE)
        foliage_seed = rng.getrandbits(32)
        if lod["twig_cards"]:
            add_foliage(geo, twig, foliage_seed, 0.15, TWIG_CARD_SPACING, lod, 2, size)


def add_whorls(geo, points, radii, rng, lod, height, crown_base, crown_width, z_from, z_to, taper=1.0):
    """幹（または燭台形の分かれた頂）に沿って輪生する枝の段を付ける。
    枝の長さは地面からの高さで決める（どの頂も同じ樹冠の輪郭に収まる）。"""
    lengths = [0.0]
    for i in range(1, len(points)):
        lengths.append(lengths[-1] + (points[i] - points[i - 1]).length)
    at = 0.0
    while at < lengths[-1] and points[0].z + at < z_from:
        at += 0.1
    turn = rng.uniform(0, 2 * math.pi)
    while at < lengths[-1] * 0.97:
        segment = next(i for i in range(1, len(lengths)) if lengths[i] >= at)
        u = (at - lengths[segment - 1]) / max(lengths[segment] - lengths[segment - 1], 1e-6)
        origin = points[segment - 1].lerp(points[segment], u)
        if origin.z > z_to:
            break
        radius = radii[segment - 1] + (radii[segment] - radii[segment - 1]) * u
        t = (origin.z - height * crown_base) / (height * (1 - crown_base))  # 樹冠の下 0 → 上 1
        count = rng.randint(4, 6)
        turn += rng.uniform(0.3, 0.9)
        for k in range(count):
            yaw = turn + 2 * math.pi * k / count + rng.uniform(-0.3, 0.3)
            length = height * crown_width * crown_profile(t) * taper * rng.uniform(0.8, 1.1) + 0.2
            size = 0.7 + 0.3 * min(1.0, (1 - t) * 2.0)
            rise = math.radians(rng.uniform(5, 20) + 20 * t)   # 斜め上へ出る
            curl = math.radians(rng.uniform(15, 35))             # 先が上へ反る

            def pitch_at(v, rise=rise, curl=curl):
                return rise + curl * v * v

            branch, _ = grow(origin, yaw, length, pitch_at, rng, wander=0.6, step=0.3, ground=0.3)
            b0 = max(0.015, radius * 0.40 * (0.6 + 0.4 * (1 - t)))
            branch_radii = [b0 * (1 - 0.8 * (j / (len(branch) - 1))) + 0.005 for j in range(len(branch))]
            add_tube_lod(geo, branch, branch_radii, lod["sides"][1], lod["stride"], BRANCH_BARK_TILE)
            add_twigs(geo, branch, branch_radii, rng, lod, size)
            foliage_seed = rng.getrandbits(32)
            add_foliage(geo, branch, foliage_seed, 0.2, BRANCH_CARD_SPACING, lod, 3, size)
        at += rng.uniform(0.40, 0.65) * (1 - 0.3 * max(t, 0.0))


def add_top_tufts(geo, top, rng, lod):
    """頂の芽（真上へ伸びる房）。"""
    for k in range(3 if lod["cards"] > 1 else 2):
        yaw = 2 * math.pi * k / 3
        axis = UP * 1.0 + Vector((math.cos(yaw), math.sin(yaw), 0)) * 0.35
        add_tuft(geo, top - Vector((0, 0, CARD_LENGTH * 0.5)), axis, rng, lod["scale"] * 0.8, 1)


def build_tree(seed, lod):
    rng = random.Random(seed)
    geo = Geometry()
    height = rng.uniform(8.0, 14.0)
    r0 = 0.020 * height + rng.uniform(0.0, 0.04)    # 根元の半径（高さ 12 m で約 26 cm）
    crown_base = rng.uniform(0.05, 0.14)            # 枝の付き始め（開けた林なので低い）
    crown_width = rng.uniform(0.20, 0.25)           # 一番広い所の枝の長さ（高さの割合）
    candelabra = rng.random() < 0.4                 # 上の方で幹が分かれた燭台形
    lean_yaw = rng.uniform(0, 2 * math.pi)
    lean = math.radians(rng.uniform(0.0, 3.0))

    split = rng.uniform(0.62, 0.74) if candelabra else 1.0
    trunk_height = height * split
    points, _ = grow(Vector((0, 0, -0.1)), lean_yaw, trunk_height, lambda t: math.pi / 2 - lean, rng,
                     wander=0.04, step=0.25)
    # 分かれる木は途中で切った幹なので、先が細りきらない。
    radii = [r0 * (1 - 0.95 * (i / (len(points) - 1)) * split) + 0.01 for i in range(len(points))]
    add_tube_lod(geo, points, radii, lod["sides"][0], lod["stride"], TRUNK_BARK_TILE)
    # 分かれた頂を先に作る（骨格の乱数の消費を段で変えないため、枝より先に形を決める）。
    leaders = []
    if candelabra:
        count = rng.randint(2, 3)
        turn = rng.uniform(0, 2 * math.pi)
        for k in range(count):
            yaw = turn + 2 * math.pi * k / count + rng.uniform(-0.3, 0.3)
            pitch = math.radians(rng.uniform(58, 72))
            length = height * (1 - split) * rng.uniform(0.95, 1.2)

            def leader_pitch(v, pitch=pitch):
                return pitch + (math.pi / 2 - pitch) * min(1.0, v * 1.6)  # すぐ立ち上がる

            leader, _ = grow(points[-1], yaw, length, leader_pitch, rng, wander=0.08, step=0.2)
            lr = radii[-1] * rng.uniform(0.65, 0.8)
            leader_radii = [lr * (1 - 0.9 * (j / (len(leader) - 1))) + 0.01 for j in range(len(leader))]
            leaders.append((leader, leader_radii))
    add_whorls(geo, points, radii, rng, lod, height, crown_base, crown_width,
               height * crown_base, height * 0.98)
    for leader, leader_radii in leaders:
        add_tube_lod(geo, leader, leader_radii, lod["sides"][1] + 2, lod["stride"], TRUNK_BARK_TILE)
        # 分かれた頂の枝は短め（頂どうしで樹冠を分け合う）。
        add_whorls(geo, leader, leader_radii, rng, lod, height, crown_base, crown_width,
                   leader[0].z + 0.3, height * 1.2, taper=0.75)
    tip_rng = random.Random(rng.getrandbits(32))
    tops = [leader[-1] for leader, _ in leaders] if leaders else [points[-1]]
    for top in tops:
        add_top_tufts(geo, top, tip_rng, lod)
    return geo


# --- 樹冠の法線と芯 ---------------------------------------------------------------
# 樹冠は房のこぶの集まりなので、シラビソより外形を小さめに取ってこぶの陰影を残す。
ENVELOPE_RADIUS_SCALE = 2.0   # 房の半径に対する外形のメタボールの半径
ENVELOPE_RESOLUTION = 0.14
ENVELOPE_SMOOTH = 8
ENVELOPE_NORMAL_WEIGHT = 0.75  # 葉の法線を外形の法線へ寄せる割合
ENVELOPE_UP_BIAS = 0.3         # 下面が真っ黒にならないよう上へ足す量
CORE_RADIUS_SCALE = 1.6
CORE_RESOLUTION = 0.12
CORE_INSET = 0.15
CORE_TRIANGLES = (2400, 1100, 500)
# 芯の色（リニア）。針葉より暗くし、隙間から覗く樹冠の奥に見せる。
CORE_COLOR = (0.010, 0.022, 0.014)


def main():
    options = parse_args()
    out, root = options["out"], options["root"]
    os.makedirs(out, exist_ok=True)
    rng = random.Random(options["seed"])
    np_rng = np.random.default_rng(options["seed"])

    paths = {key: os.path.join(out, f"T_Arve_{key}.png") for key in ("Needles_D", "Needles_N", "Bark_D", "Bark_N")}
    needles, needles_normal = make_needle_texture(rng)
    write_png(paths["Needles_D"], needles)
    write_png(paths["Needles_N"], needles_normal)
    coverage = (needles[..., 3] >= 128).mean()
    linear = (needles[..., :3][needles[..., 3] >= 128] / 255.0) ** 2.2
    print(f"needle coverage {coverage * 100:.1f}%, mean linear rgb {linear.mean(axis=0).round(3)}")
    bark, bark_normal = make_bark_texture(np_rng)
    write_png(paths["Bark_D"], bark)
    write_png(paths["Bark_N"], bark_normal)

    bpy.ops.wm.read_factory_settings(use_empty=True)
    materials = [make_material("Bark", paths["Bark_D"], paths["Bark_N"], False),
                 make_material("Needles", paths["Needles_D"], paths["Needles_N"], True),
                 make_core_material(CORE_COLOR)]

    bark_mat = os.path.join(out, "MI_Arve_Bark.tgmat")
    needle_mat = os.path.join(out, "MI_Arve_Needles.tgmat")
    core_mat = os.path.join(out, "MI_Arve_Core.tgmat")
    write_material(bark_mat, "MI_Arve_Bark", source_ref(paths["Bark_D"], root),
                   source_ref(paths["Bark_N"], root), 0.75, 0.0, False)
    write_material(needle_mat, "MI_Arve_Needles", source_ref(paths["Needles_D"], root),
                   source_ref(paths["Needles_N"], root), 0.65, 0.5, True)
    write_material(core_mat, "MI_Arve_Core", None, None, 0.9, 0.0, False, CORE_COLOR)

    for index in range(1, options["variants"] + 1):
        name = f"Arve_Var{index}"
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
                                       ((index - 1) * 12.0, level * 12.0, 0)))
            cards = geo.face_mat.count(LEAVES)
            print(f"{name}_LOD{level}: {geo.triangles()} triangles, {cards} cards")
        fbx = os.path.join(out, name + ".fbx")
        export_fbx(objects, fbx)
        # スロットの並びは FBX で最初に現れた順（幹が先）。
        write_model(os.path.join(out, name + ".tgmodel"), name, source_ref(fbx, root),
                    [asset_ref(bark_mat, root), asset_ref(needle_mat, root), asset_ref(core_mat, root)])
        bpy.data.meshes.remove(envelope)
        bpy.data.meshes.remove(hull)

    bpy.ops.wm.save_as_mainfile(filepath=os.path.join(out, "Arve.blend"))


main()
