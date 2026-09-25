# スギ（杉）の簡易モデルを作る Blender スクリプト。伊豆の山の中腹に広がる植林。
#
# 使い方（Blender 5.x）:
#   blender -b --factory-startup --python-exit-code 1 --python tools/blender/make_sugi.py -- \
#       --out data/Models/Sugi [--root data] [--variants 3] [--seed 1]
#
# 出力（--out の下）:
#   Sugi_VarN.fbx               幹・枝（Bark）、針葉のカード（Needles）、芯（Core）の 3 スロット。
#                               LOD0〜2 をオブジェクト名の末尾 _LOD<n> で 1 ファイルに入れる
#   T_Sugi_Needles_*.png        縄のような針葉の小枝が枝分かれした房のカード（RGBA）と法線（OpenGL 規約）
#   T_Sugi_Bark_*.png           樹皮（赤褐色。縦に細長く裂けて繊維状に剥がれる）と法線
#   Sugi.blend                  全バリエーション（手直し用）
#   *.tgmat / *.tgmodel / *.meta  terrain-graph のアセット。既にあれば上書きしない（UID を保つ）。
#
# 共通の部品は vegetation.py、決まりごとは docs/design/vegetation-assets.md。
# 骨格の作り方はカラマツ（make_karamatsu.py）と同じで、違いは次のとおり。
#   - 植林の木。高さ 18〜26 m、枝打ちと密植で下の 4.5〜5.5 割は枝が無い。樹冠は細い円錐で先が尖る。
#   - 枝は不規則に 2〜4 本ずつ出る。下の枝は少し垂れてから先が上へ反る。
#   - 針葉は短い鎌形（1〜1.5 cm）で小枝のまわりに螺旋に付くので、小枝が太い縄に見える。
#     縄が枝分かれした房がこんもり固まる。色は暗い緑（遠目に植林が黒っぽく見える）。
#   - 樹冠は密なので芯を入れる（オオシラビソ / シラビソと同じ）。
#   - 樹皮は赤褐色で、縦に細長く裂けて繊維状に剥がれる。
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


# --- 針葉のカード ---------------------------------------------------------------
# 画像は縦長（幅 512 × 高さ 1024）。下端の中央が小枝の付け根で、上へ伸びる主軸から
# 左右へ側枝が出て、側枝からさらに短い小枝が出る。どの小枝も短い針葉に覆われた縄に見える。
# カードは 50 × 25 cm なので、1 cm は 20.5 px。
class Canvas:
    def __init__(self, width, height):
        self.width, self.height = width, height
        self.color = np.zeros((height, width, 3), np.float32)
        self.alpha = np.zeros((height, width), np.float32)
        self.normal = np.zeros((height, width, 3), np.float32)
        self.normal[..., 2] = 1.0

    def stroke(self, p0, p1, w0, w1, color):
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
        side = rx * -d[1] + ry * d[0]
        dist = np.hypot(rx - d[0] * t * length, ry - d[1] * t * length)
        radius = (w0 + (w1 - w0) * t) * 0.5
        coverage = np.clip(radius - dist + 0.5, 0.0, 1.0)
        if not (coverage > 0).any():
            return
        across = np.clip(side / np.maximum(radius, 1e-3), -1.0, 1.0)
        bulge = np.sqrt(np.clip(1.0 - across * across, 0.0, 1.0))
        rgb = np.asarray(color, np.float32) * (0.78 + 0.22 * bulge)[..., None]
        a = coverage[..., None]
        view = (slice(y0, y1), slice(x0, x1))
        self.color[view] = self.color[view] * (1 - a) + rgb * a
        self.alpha[view] = self.alpha[view] * (1 - coverage) + coverage
        n = np.stack([-d[1] * across * 0.6, -d[0] * across * 0.6, bulge + 0.6], axis=-1)
        n /= np.linalg.norm(n, axis=-1, keepdims=True)
        self.normal[view] = self.normal[view] * (1 - a) + n * a


def make_needle_texture(rng, width=512, height=1024):
    canvas = Canvas(width, height)
    cm = 20.5
    base = np.array([width * 0.5, height - 6], np.float32)
    tip = np.array([width * 0.5 + rng.uniform(-14, 14), height * 0.05], np.float32)
    # 暗い緑。アルベドは G でリニア 0.04〜0.05 程度（カラマツの 0.07〜0.08 より暗い）。
    # 伸びたばかりの先の方は少し明るく黄みがかる。
    greens = [(0.13, 0.24, 0.10), (0.15, 0.26, 0.11), (0.12, 0.22, 0.095), (0.16, 0.27, 0.115)]
    fresh = np.array([0.20, 0.31, 0.12], np.float32)

    def line(p0, p1, bend):
        """付け根 p0 → 先 p1 へ、横へ bend（px）だけ弓なりに曲がる小枝。"""
        p0, p1 = np.asarray(p0, np.float32), np.asarray(p1, np.float32)
        d = p1 - p0
        normal = np.array([-d[1], d[0]], np.float32) / max(float(np.hypot(*d)), 1e-4)

        def at(s):
            return p0 + d * s + normal * bend * math.sin(math.pi * s)

        return at

    def rope(twig, span_px, thickness):
        """縄のような小枝。小枝に沿って短い針葉を左右と前へ重ね、先ほど細くする。"""
        count = max(4, int(span_px / (cm * 0.2)))
        for i in range(count):
            s = i / count
            origin = twig(s)
            ahead = twig(min(s + 0.02, 1.0)) - twig(max(s - 0.02, 0.0))
            ahead /= max(np.hypot(*ahead), 1e-4)
            across = np.array([-ahead[1], ahead[0]], np.float32)
            taper = 1.0 - 0.45 * s
            color = np.array(greens[rng.randrange(len(greens))], np.float32) * rng.uniform(0.9, 1.1)
            color = color * (1 - 0.5 * s * s) + fresh * (0.5 * s * s)
            for side in (-1, 1, 0):
                # 前へ 20〜38° に開いた短い鎌形の針葉（先が小枝の方へ曲がる）。正面の 1 本は前を向く。
                # 小枝に密着して重なり、全体で太さ 1.5〜2 cm の縄に見える。
                angle = math.radians(rng.uniform(20, 38)) * side
                direction = ahead * math.cos(angle) + across * math.sin(angle)
                length = cm * rng.uniform(0.8, 1.25) * taper * thickness
                mid = origin + direction * length * 0.6
                end = mid + (direction * 0.6 + ahead * 0.4) * length * 0.45
                w = 7.0 * thickness
                canvas.stroke(origin, mid, w, w * 0.8, color * rng.uniform(0.92, 1.08))
                canvas.stroke(mid, end, w * 0.8, 1.2, color * rng.uniform(0.95, 1.1))

    # 主軸と側枝（左右交互に 9〜12 本、付け根ほど長い）、側枝から出る短い小枝。
    # 小枝どうしが重なるくらい詰めて、こんもりした房にする。
    main = line(base, tip, rng.uniform(-18, 18))
    twigs = [(main, float(np.hypot(*(tip - base))), 1.1)]
    side = rng.choice((-1, 1))
    count = rng.randint(9, 12)
    for k in range(count):
        s0 = 0.06 + 0.84 * k / count + rng.uniform(-0.02, 0.02)
        side = -side
        angle = math.radians(rng.uniform(28, 48)) * side
        length = height * rng.uniform(0.22, 0.30) * (1.0 - 0.55 * s0)
        origin = main(s0)
        direction = np.array([math.sin(angle), -math.cos(angle)], np.float32)
        end = origin + direction * length
        branch = line(origin, end, side * rng.uniform(4, 14))
        twigs.append((branch, length, 0.95))
        for j in range(rng.randint(3, 5)):
            s1 = rng.uniform(0.15, 0.85)
            sub_angle = angle + math.radians(rng.uniform(25, 50)) * rng.choice((-1, 1))
            sub_dir = np.array([math.sin(sub_angle), -math.cos(sub_angle)], np.float32)
            sub_len = length * rng.uniform(0.3, 0.55)
            start = branch(s1)
            sub = line(start, start + sub_dir * sub_len, rng.uniform(-6, 6))
            twigs.append((sub, sub_len, 0.85))
            # さらに短い小枝（房の中を埋める）。
            for m in range(rng.randint(0, 2)):
                s2 = rng.uniform(0.3, 0.8)
                tiny_angle = sub_angle + math.radians(rng.uniform(25, 50)) * rng.choice((-1, 1))
                tiny_dir = np.array([math.sin(tiny_angle), -math.cos(tiny_angle)], np.float32)
                tiny_len = sub_len * rng.uniform(0.35, 0.6)
                tiny_start = sub(s2)
                twigs.append((line(tiny_start, tiny_start + tiny_dir * tiny_len, 0.0), tiny_len, 0.75))

    # 小枝の芯（緑褐色）を先に描き、針葉を上に重ねる。
    for twig, span, thickness in twigs:
        for i in range(16):
            s0, s1 = i / 16, (i + 1) / 16
            canvas.stroke(twig(s0), twig(s1), 7 * thickness * (1 - 0.5 * s0), 7 * thickness * (1 - 0.5 * s1),
                          (0.22, 0.24, 0.12))
    for twig, span, thickness in twigs:
        rope(twig, span, thickness)
    return finish_cutout(canvas.color, canvas.alpha, canvas.normal)


# --- 樹皮 ---------------------------------------------------------------------
def make_bark_texture(rng, size=512):
    """赤褐色の樹皮。縦に細長く裂け、繊維状の帯が剥がれかける。風雨で灰色がかった所が混じる。
    画像の x が幹の周、y が幹の長さ（筒の UV と同じ）。上下左右に繰り返す。"""
    tone = tiling_noise(rng, size, 0.03, 0.01)
    grey = np.clip(tone * 0.35 + 0.35, 0, 1)[..., None]
    color = np.array([0.44, 0.28, 0.19]) * (1 - grey) + np.array([0.46, 0.40, 0.35]) * grey
    # 縦の繊維（周方向に細かく、長さ方向に長い模様）。
    fibre = tiling_noise(rng, size, 0.45, 0.012)
    strips = tiling_noise(rng, size, 0.10, 0.006)
    height = 0.25 * fibre + 0.5 * strips
    # 帯の間の細い裂け目。
    crack = np.clip((-strips - 1.1) * 1.5, 0, 1)
    height -= crack * 0.9
    color = color * (1 + 0.06 * fibre[..., None]) * (1 - 0.5 * crack[..., None])
    # 剥がれかけた帯の縁は明るい赤褐色。
    lift = np.clip((strips - 1.2) * 1.5, 0, 1)[..., None]
    color = color * (1 - lift * 0.4) + np.array([0.56, 0.36, 0.24]) * lift * 0.4
    color = np.clip(color, 0, 1)
    return to_bytes(color), to_bytes(height_to_normal(height, 2.5) * 0.5 + 0.5)


# --- 形状 ---------------------------------------------------------------------
CARD_LENGTH, CARD_WIDTH = 0.50, 0.25   # 針葉の房のカード（m）。テクスチャの縦横比 2:1
TRUNK_BARK_TILE = 1.2
BRANCH_BARK_TILE = 0.5
BRANCH_CARD_SPACING = 0.30 * CARD_LENGTH  # 枝に沿ったカードの間隔
TWIG_CARD_SPACING = 0.30 * CARD_LENGTH    # 小枝に沿ったカードの間隔

# LOD ごとの作り分け。骨格は全段で同じ乱数から作り、細かさだけを変える。
#   sides: 幹・枝・小枝の筒の角数（0 なら作らない）  stride: 筒の節を何点おきに使うか
#   spacing / scale: カードの間隔と大きさの倍率  cards: 位置ごとのカード枚数
#   twig_cards: 小枝の位置にカードを付けるか（小枝の筒を省いても、カードだけ残せる）
LODS = [
    {"sides": (10, 5, 3), "stride": 1, "spacing": 1.0, "scale": 1.0, "cards": 2, "twig_cards": True},
    {"sides": (7, 3, 0), "stride": 2, "spacing": 2.2, "scale": 1.45, "cards": 2, "twig_cards": True},
    {"sides": (5, 3, 0), "stride": 4, "spacing": 2.4, "scale": 1.9, "cards": 1, "twig_cards": False},
]


def add_spray(geo, base, axis, rng, scale, cards):
    """針葉の房 1 つ。1 枚目はほぼ水平、2 枚目は立て気味（房は四方へこんもり開く）。"""
    axis = axis.normalized()
    length, width = CARD_LENGTH * scale, CARD_WIDTH * scale
    base = base - axis * 0.02 * scale
    tip = base + axis * length
    horizontal = axis.cross(UP)
    horizontal = horizontal.normalized() if horizontal.length > 1e-4 else perpendicular(axis)
    geo.shoots.append((base + axis * length * 0.5, length * 0.5))
    # 房はこんもり丸いので、1 枚目も水平に揃えず傾ける（揃えると水平な棚に見える）。
    roll = rng.uniform(-0.6, 0.6)
    for k in range(cards):
        angle = roll + (0 if k == 0 else rng.choice((-1, 1)) * math.radians(rng.uniform(55, 85)))
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
    """枝に沿って房を並べ、先端にも房を付ける。乱数は枝ごとに独立。
    size はカードの大きさの倍率（樹冠の上ほど小さくして先を尖らせる）。"""
    rng = random.Random(seed)
    for position, tangent, _ in along_polyline(points, start, spacing * lod["spacing"], rng):
        sideways = tangent.cross(UP)
        sideways = sideways.normalized() if sideways.length > 1e-4 else perpendicular(tangent)
        sideways *= rng.choice((-1, 1))
        # 房は枝先の方へ向き、少し上へ持ち上がる（スギの小枝は垂れにくい）。
        axis = tangent * 0.6 + sideways * rng.uniform(0.4, 0.8) + UP * rng.uniform(-0.1, 0.2)
        add_spray(geo, position, axis, rng, rng.uniform(0.85, 1.1) * lod["scale"] * size, lod["cards"])
    tangent = (points[-1] - points[-2]).normalized()
    for k in range(tip_cards):
        spread = perpendicular(tangent) * rng.uniform(-0.4, 0.4)
        add_spray(geo, points[-1], tangent + spread + UP * 0.15, rng,
                  rng.uniform(0.9, 1.1) * lod["scale"] * size, lod["cards"])


def build_tree(seed, lod):
    rng = random.Random(seed)
    geo = Geometry()
    height = rng.uniform(18.0, 26.0)
    r0 = 0.011 * height + rng.uniform(0.0, 0.03)   # 根元の半径（高さ 22 m で約 26 cm）
    crown_base = rng.uniform(0.45, 0.55)           # 枝の付き始め（枝打ちと密植で下枝が無い）
    crown_width = rng.uniform(0.10, 0.13)          # 一番下の枝の長さ（高さの割合）
    lean_yaw = rng.uniform(0, 2 * math.pi)
    lean = math.radians(rng.uniform(0.0, 2.0))

    points, _ = grow(Vector((0, 0, -0.1)), lean_yaw, height, lambda t: math.pi / 2 - lean, rng,
                     wander=0.02, step=0.25)
    radii = [r0 * (1 - 0.95 * (i / (len(points) - 1))) + 0.01 for i in range(len(points))]
    add_tube_lod(geo, points, radii, lod["sides"][0], lod["stride"], TRUNK_BARK_TILE)

    lengths = [0.0]
    for i in range(1, len(points)):
        lengths.append(lengths[-1] + (points[i] - points[i - 1]).length)
    # 枝の出る高さ。0.15〜0.3 m ごとに不規則。上ほど間隔が詰まる。
    # 間隔が広いとモミのような水平な棚に分かれて見えるので、房が上下で重なるまで詰める。
    at = height * crown_base
    turn = rng.uniform(0, 2 * math.pi)
    while at < height * 0.97:
        t = (at - height * crown_base) / (height * (1 - crown_base))  # 樹冠の下 0 → 上 1
        segment = next(i for i in range(1, len(lengths)) if lengths[i] >= at)
        u = (at - lengths[segment - 1]) / max(lengths[segment] - lengths[segment - 1], 1e-6)
        origin = points[segment - 1].lerp(points[segment], u)
        radius = radii[segment - 1] + (radii[segment] - radii[segment - 1]) * u
        count = rng.randint(1, 3)
        turn += rng.uniform(1.2, 2.6)
        for k in range(count):
            yaw = turn + 2 * math.pi * k / count + rng.uniform(-0.6, 0.6)
            # 細い円錐。下の方はわずかに膨らませ、先は尖らせる。
            length = height * crown_width * (1 - t) ** 0.95 * (1 + 0.15 * math.sin(math.pi * t)) \
                * rng.uniform(0.85, 1.1) + 0.15
            size = 0.6 + 0.4 * min(1.0, (1 - t) * 1.6)
            # 下の枝はほぼ水平で先が上へ反る。上の枝ほど斜め上へ立つ。
            rise = math.radians(rng.uniform(-8, 6) * (1 - t) + 38 * t)
            upturn = math.radians(rng.uniform(12, 24))

            def pitch_at(v, rise=rise, upturn=upturn):
                return rise + upturn * v * v

            branch, _ = grow(origin, yaw, length, pitch_at, rng, wander=0.25, step=0.2, ground=0.3)
            b0 = max(0.012, radius * 0.32 * (0.6 + 0.4 * (1 - t)))
            branch_radii = [b0 * (1 - 0.8 * (j / (len(branch) - 1))) + 0.004 for j in range(len(branch))]
            add_tube_lod(geo, branch, branch_radii, lod["sides"][1], lod["stride"], BRANCH_BARK_TILE)
            add_twigs(geo, branch, branch_radii, rng, lod, size)
            foliage_seed = rng.getrandbits(32)
            add_foliage(geo, branch, foliage_seed, 0.25, BRANCH_CARD_SPACING, lod, 2, size)
        at += rng.uniform(0.15, 0.30) * (1 - 0.35 * t)
    # 先端の芽（真上へ伸びる主軸の房）。
    tip_rng = random.Random(rng.getrandbits(32))
    for k in range(3 if lod["cards"] > 1 else 2):
        yaw = 2 * math.pi * k / 3
        axis = UP * 1.0 + Vector((math.cos(yaw), math.sin(yaw), 0)) * 0.2
        add_spray(geo, points[-1] - Vector((0, 0, CARD_LENGTH * 0.6)), axis, tip_rng,
                  lod["scale"] * 0.6, 1)
    return geo


def add_twigs(geo, branch, branch_radii, rng, lod, size):
    """枝から四方へ小枝を出す（房が枝のまわりにこんもり付く）。骨格の乱数は段によらず同じだけ使う。"""
    lengths = [0.0]
    for i in range(1, len(branch)):
        lengths.append(lengths[-1] + (branch[i] - branch[i - 1]).length)
    full = lengths[-1]
    next_at = full * 0.2 + rng.uniform(0.0, 0.15)
    side = rng.choice((-1, 1))
    for i in range(1, len(branch) - 1):
        if lengths[i] < next_at:
            continue
        s = lengths[i] / full
        next_at = lengths[i] + rng.uniform(0.18, 0.30)
        side = -side
        tangent = (branch[i + 1] - branch[i - 1]).normalized()
        yaw = math.atan2(tangent.y, tangent.x) + side * math.radians(rng.uniform(40, 70))
        # 上下にも散らす（平たい羽状ではなく、枝のまわりの塊にする）。
        pitch = math.asin(max(-1, min(1, tangent.z))) + math.radians(rng.uniform(-15, 25))
        length = rng.uniform(0.3, 0.65) * (1 - 0.5 * s) * min(1.0, full / 1.2)
        twig, _ = grow(branch[i], yaw, length, lambda v, pitch=pitch: pitch + math.radians(6) * v, rng,
                       wander=0.3, step=0.15, ground=0.3)
        t0 = branch_radii[i] * 0.5
        twig_radii = [t0 * (1 - 0.7 * (j / (len(twig) - 1))) + 0.003 for j in range(len(twig))]
        add_tube_lod(geo, twig, twig_radii, lod["sides"][2], lod["stride"], BRANCH_BARK_TILE)
        foliage_seed = rng.getrandbits(32)
        if lod["twig_cards"]:
            add_foliage(geo, twig, foliage_seed, 0.1, TWIG_CARD_SPACING, lod, 1, size)


# --- 樹冠の法線と芯 ---------------------------------------------------------------
# 針葉樹の樹冠は密な円錐なので、外形を大きく取り、樹冠全体の円錐に沿わせる（オオシラビソと同じ）。
ENVELOPE_RADIUS_SCALE = 2.4   # 房の半径に対する外形のメタボールの半径
ENVELOPE_RESOLUTION = 0.14
ENVELOPE_SMOOTH = 8
ENVELOPE_NORMAL_WEIGHT = 0.75  # 葉の法線を外形の法線へ寄せる割合
ENVELOPE_UP_BIAS = 0.3         # 下面が真っ黒にならないよう上へ足す量
CORE_RADIUS_SCALE = 1.6
CORE_RESOLUTION = 0.12
CORE_INSET = 0.15
CORE_TRIANGLES = (2400, 1100, 500)
# 芯の色（リニア）。葉より暗くし、隙間から覗く樹冠の奥に見せる。
CORE_COLOR = (0.010, 0.020, 0.010)


def main():
    options = parse_args()
    out, root = options["out"], options["root"]
    os.makedirs(out, exist_ok=True)
    rng = random.Random(options["seed"])
    np_rng = np.random.default_rng(options["seed"])

    paths = {key: os.path.join(out, f"T_Sugi_{key}.png")
             for key in ("Needles_D", "Needles_N", "Bark_D", "Bark_N")}
    needles, needles_normal = make_needle_texture(rng)
    write_png(paths["Needles_D"], needles)
    write_png(paths["Needles_N"], needles_normal)
    coverage = float((needles[..., 3] >= 128).mean())
    print(f"needle coverage: {coverage * 100:.1f}%")
    bark, bark_normal = make_bark_texture(np_rng)
    write_png(paths["Bark_D"], bark)
    write_png(paths["Bark_N"], bark_normal)

    bpy.ops.wm.read_factory_settings(use_empty=True)
    materials = [make_material("Bark", paths["Bark_D"], paths["Bark_N"], False),
                 make_material("Needles", paths["Needles_D"], paths["Needles_N"], True),
                 make_core_material(CORE_COLOR)]

    bark_mat = os.path.join(out, "MI_Sugi_Bark.tgmat")
    needle_mat = os.path.join(out, "MI_Sugi_Needles.tgmat")
    core_mat = os.path.join(out, "MI_Sugi_Core.tgmat")
    write_material(bark_mat, "MI_Sugi_Bark", source_ref(paths["Bark_D"], root),
                   source_ref(paths["Bark_N"], root), 0.85, 0.0, False)
    write_material(needle_mat, "MI_Sugi_Needles", source_ref(paths["Needles_D"], root),
                   source_ref(paths["Needles_N"], root), 0.7, 0.5, True)
    write_material(core_mat, "MI_Sugi_Core", None, None, 0.9, 0.0, False, CORE_COLOR)

    for index in range(1, options["variants"] + 1):
        name = f"Sugi_Var{index}"
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

    bpy.ops.wm.save_as_mainfile(filepath=os.path.join(out, "Sugi.blend"))


main()
