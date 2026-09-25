# カラマツ（唐松）の簡易モデルを作る Blender スクリプト。八ヶ岳の麓の植林。
#
# 使い方（Blender 5.x）:
#   blender -b --factory-startup --python-exit-code 1 --python tools/blender/make_karamatsu.py -- \
#       --out data/Models/Karamatsu [--root data] [--variants 3] [--seed 1]
#
# 出力（--out の下）:
#   Karamatsu_VarN.fbx          幹・枝（Bark）、針葉のカード（Needles）、芯（Core）の 3 スロット。
#                               LOD0〜2 をオブジェクト名の末尾 _LOD<n> で 1 ファイルに入れる
#   T_Karamatsu_Needles_*.png   短枝の房（20〜40 本の柔らかい針葉）が並ぶ小枝のカード（RGBA）と法線（OpenGL 規約）
#   T_Karamatsu_Bark_*.png      樹皮（赤褐色。縦に鱗状に剥がれる）と法線
#   Karamatsu.blend             全バリエーション（手直し用）
#   *.tgmat / *.tgmodel / *.meta  terrain-graph のアセット。既にあれば上書きしない（UID を保つ）。
#
# 共通の部品は vegetation.py、決まりごとは docs/design/vegetation-assets.md。
# 骨格の作り方はオオシラビソ（make_oshirabiso.py）と同じで、違いは次のとおり。
#   - 植林の木。高さ 16〜24 m、下の 4〜5 割は枝が無い。樹冠は円錐形だが透けて軽い。
#   - 枝は輪生せず、不規則な高さから 2〜4 本ずつ出て、先が少し上へ反る。小枝は垂れる。
#   - 針葉は短枝ごとの房（長さ 2〜3.5 cm の柔らかい針葉が 20〜40 本）。明るい黄緑。
#   - 樹皮は赤褐色で、縦に鱗状に剥がれる。
import math
import os
import random
import sys

import bpy
import numpy as np
from mathutils import Vector

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vegetation import (LEAVES, UP, EnvelopeField, Geometry, add_tube_lod,  # noqa: E402
                        along_polyline, asset_ref, build_hull, export_fbx, finish_cutout, grow,
                        height_to_normal, make_core_material, make_material, make_object,
                        parse_args, perpendicular, source_ref, tiling_noise, to_bytes,
                        transfer_field_normals, write_material, write_model, write_png)


# --- 針葉のカード ---------------------------------------------------------------
# 画像は縦長（幅 512 × 高さ 1024）。下端の中央が小枝の付け根で、上へ伸びる小枝から
# 左右へ側枝が出る。針葉（長さ 1〜2.5 cm）は小枝の上面を覆うように密に付く。
# カードは 40 × 20 cm なので、1 cm は 25.6 px。
class Canvas:
    def __init__(self, width, height):
        self.width, self.height = width, height
        self.color = np.zeros((height, width, 3), np.float32)
        self.alpha = np.zeros((height, width), np.float32)
        self.normal = np.zeros((height, width, 3), np.float32)
        self.normal[..., 2] = 1.0

    def stroke(self, p0, p1, w0, w1, color, stomata=0.0):
        """太さが変わる丸い線分を描く。座標は画素（x 右、y 下）。stomata は裏の白い気孔帯の強さ。"""
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
        # 扁平な針葉なので陰影は弱く、裏の気孔帯（中央の両脇の 2 本の白い筋）を混ぜる。
        bands = np.clip(1.0 - np.abs(np.abs(across) - 0.45) * 6.0, 0.0, 1.0) * stomata
        base = np.asarray(color, np.float32)
        pale = np.array([0.62, 0.70, 0.68], np.float32)
        rgb = (base * (1 - bands[..., None]) + pale * bands[..., None]) * (0.82 + 0.18 * bulge)[..., None]
        a = coverage[..., None]
        view = (slice(y0, y1), slice(x0, x1))
        self.color[view] = self.color[view] * (1 - a) + rgb * a
        self.alpha[view] = self.alpha[view] * (1 - coverage) + coverage
        n = np.stack([-d[1] * across * 0.6, -d[0] * across * 0.6, bulge + 0.6], axis=-1)
        n /= np.linalg.norm(n, axis=-1, keepdims=True)
        self.normal[view] = self.normal[view] * (1 - a) + n * a


def make_needle_texture(rng, width=512, height=1024):
    canvas = Canvas(width, height)
    base = np.array([width * 0.5, height - 6], np.float32)
    tip = np.array([width * 0.5 + rng.uniform(-12, 12), height * 0.06], np.float32)
    # 夏の黄緑。ほかの針葉樹より明るいが、アルベドは G でリニア 0.07〜0.08 程度に収める
    # （以前の 0.16 では、インポスターが明るい塊になって浮いた）。
    greens = [(0.22, 0.32, 0.095), (0.245, 0.345, 0.11), (0.195, 0.295, 0.085), (0.26, 0.36, 0.12)]
    cm = 25.6

    def main_twig(s):
        return base + (tip - base) * s + np.array([10 * math.sin(s * 2.6), 0], np.float32)

    # 側枝: 主軸から斜め前へ、左右交互に 7〜8 本。付け根ほど長い。
    twigs = [(main_twig, 1.0)]
    side = rng.choice((-1, 1))
    count = rng.randint(4, 5)
    for k in range(count):
        s0 = 0.10 + 0.75 * k / count + rng.uniform(-0.03, 0.03)
        side = -side
        angle = math.radians(rng.uniform(40, 58)) * side
        length = height * rng.uniform(0.24, 0.32) * (1.0 - 0.5 * s0)
        origin = main_twig(s0)
        direction = np.array([math.sin(angle), -math.cos(angle)], np.float32)

        def side_twig(s, origin=origin, direction=direction, length=length):
            return origin + direction * length * s

        twigs.append((side_twig, length / (height * 0.94)))

    def needles(twig, span, front):
        # 短枝の房。小枝に沿って 2〜3 cm ごとに、20〜40 本の針葉が 1 点から放射状に開く
        # （斜め上から見るので楕円に潰れる）。房どうしの間は透けている。
        along = 0.02 + rng.uniform(0.0, 0.03)
        spacing = cm * rng.uniform(2.0, 3.0) / (span * height * 0.94)
        while along < 1.0:
            origin = twig(along)
            ahead = twig(min(along + 0.02, 1.0)) - twig(max(along - 0.02, 0.0))
            ahead /= max(np.hypot(*ahead), 1e-4)
            across = np.array([-ahead[1], ahead[0]], np.float32)
            color = np.array(rng.choice(greens)) * rng.uniform(0.85, 1.15)
            squash = rng.uniform(0.45, 0.8)
            for _ in range(rng.randint(20, 40) // (1 if not front else 2)):
                angle = rng.uniform(0, 2 * math.pi)
                direction = across * math.cos(angle) + ahead * math.sin(angle) * squash
                length = cm * rng.uniform(2.0, 3.5) * (1.0 - 0.3 * along) * rng.uniform(0.6, 1.0)
                canvas.stroke(origin, origin + direction * length, 3.2, 2.2,
                              color * rng.uniform(0.9, 1.1), 0.0)
            along += spacing * rng.uniform(0.8, 1.2)

    # 小枝（赤褐色）を先に描き、房を上に重ねる（房の間から小枝が覗く）。
    for twig, span in twigs:
        for i in range(24):
            s0, s1 = i / 24, (i + 1) / 24
            w = 6.0 if span == 1.0 else 4.0
            canvas.stroke(twig(s0), twig(s1), w - 2 * s0, w - 2 * s1, (0.42, 0.28, 0.18))
    for twig, span in twigs:
        needles(twig, span, False)
    return finish_cutout(canvas.color, canvas.alpha, canvas.normal)


# --- 樹皮 ---------------------------------------------------------------------
def make_bark_texture(rng, size=512):
    """赤褐色の樹皮。縦に長い鱗状の板（剥がれかけの薄い板）と、その間の深い割れ。
    画像の x が幹の周、y が幹の長さ（筒の UV と同じ）。上下左右に繰り返す。"""
    ys, xs = np.mgrid[0:size, 0:size].astype(np.float32)
    tone = tiling_noise(rng, size, 0.03, 0.02)
    color = np.array([0.36, 0.23, 0.16]) + np.array([0.04, 0.03, 0.02]) * np.clip(tone, -2, 2)[..., None]
    height = 0.1 * tiling_noise(rng, size, 0.2, 0.2)
    # 縦の深い割れ（板の境目）。
    cracks = tiling_noise(rng, size, 0.12, 0.02)
    crack = np.clip((cracks - 0.9) * 1.2, 0, 1)
    color = color * (1 - 0.55 * crack[..., None])
    height -= crack * 0.8
    # 鱗状の板（縦長の楕円）。剥がれかけた縁は明るい赤褐色。
    for _ in range(120):
        cx, cy = rng.uniform(0, size), rng.uniform(0, size)
        half_w, half_h = rng.uniform(8, 16), rng.uniform(20, 45)
        dx = (xs - cx + size / 2) % size - size / 2
        dy = (ys - cy + size / 2) % size - size / 2
        plate = np.clip(1 - ((dx / half_w) ** 2 + (dy / half_h) ** 2), 0, 1)
        rim = np.clip(plate * 4, 0, 1) - np.clip(plate * 4 - 1, 0, 1)
        height += np.sqrt(plate) * 0.35
        color = color * (1 + 0.10 * plate[..., None]) + np.array([0.10, 0.05, 0.02]) * rim[..., None] * 0.4
    grain = tiling_noise(rng, size, 0.4, 0.1)[..., None] * 0.02
    color = np.clip(color * (1 + grain), 0, 1)
    return to_bytes(color), to_bytes(height_to_normal(height, 3.0) * 0.5 + 0.5)


# --- 形状 ---------------------------------------------------------------------
CARD_LENGTH, CARD_WIDTH = 0.40, 0.20   # 針葉の小枝のカード（m）。テクスチャの縦横比 2:1
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
    """針葉の小枝 1 つ。1 枚目はほぼ水平（小枝は平たく広がる）、2 枚目は軸まわりに少し傾けた面。"""
    axis = axis.normalized()
    length, width = CARD_LENGTH * scale, CARD_WIDTH * scale
    base = base - axis * 0.02 * scale
    tip = base + axis * length
    horizontal = axis.cross(UP)
    horizontal = horizontal.normalized() if horizontal.length > 1e-4 else perpendicular(axis)
    geo.shoots.append((base + axis * length * 0.5, length * 0.5))
    roll = rng.uniform(-0.25, 0.25)
    for k in range(cards):
        # 2 枚目は立て気味にする（房は四方へ開くので、横から見ても面が見える）。
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
    """枝の両脇に水平なカードを並べ、先端にもカードを付ける。乱数は枝ごとに独立。
    size はカードの大きさの倍率（樹冠の上ほど小さくして先を尖らせる）。"""
    rng = random.Random(seed)
    for position, tangent, _ in along_polyline(points, start, spacing * lod["spacing"], rng):
        sideways = tangent.cross(UP)
        sideways = sideways.normalized() if sideways.length > 1e-4 else perpendicular(tangent)
        sideways *= rng.choice((-1, 1))
        # 小枝は垂れる（カードの先を下へ向ける）。
        axis = tangent * 0.5 + sideways * rng.uniform(0.5, 0.9) + UP * rng.uniform(-0.3, 0.0)
        add_spray(geo, position, axis, rng, rng.uniform(0.85, 1.1) * lod["scale"] * size, lod["cards"])
    tangent = (points[-1] - points[-2]).normalized()
    for k in range(tip_cards):
        spread = perpendicular(tangent) * rng.uniform(-0.4, 0.4)
        add_spray(geo, points[-1], tangent + spread + UP * 0.05, rng,
                  rng.uniform(0.9, 1.1) * lod["scale"] * size, lod["cards"])


def build_tree(seed, lod):
    rng = random.Random(seed)
    geo = Geometry()
    height = rng.uniform(16.0, 24.0)
    r0 = 0.011 * height + rng.uniform(0.0, 0.03)   # 根元の半径（高さ 20 m で約 23 cm）
    crown_base = rng.uniform(0.40, 0.52)           # 枝の付き始め（植林で下枝が無い）
    crown_width = rng.uniform(0.18, 0.24)          # 一番下の枝の長さ（高さの割合）
    lean_yaw = rng.uniform(0, 2 * math.pi)
    lean = math.radians(rng.uniform(0.0, 2.5))

    points, _ = grow(Vector((0, 0, -0.1)), lean_yaw, height, lambda t: math.pi / 2 - lean, rng,
                     wander=0.03, step=0.25)
    radii = [r0 * (1 - 0.95 * (i / (len(points) - 1))) + 0.01 for i in range(len(points))]
    add_tube_lod(geo, points, radii, lod["sides"][0], lod["stride"], TRUNK_BARK_TILE)

    lengths = [0.0]
    for i in range(1, len(points)):
        lengths.append(lengths[-1] + (points[i] - points[i - 1]).length)
    # 枝の出る高さ。0.25〜0.5 m ごとに不規則。上ほど間隔が詰まる。
    at = height * crown_base
    turn = rng.uniform(0, 2 * math.pi)
    while at < height * 0.97:
        t = (at - height * crown_base) / (height * (1 - crown_base))  # 樹冠の下 0 → 上 1
        segment = next(i for i in range(1, len(lengths)) if lengths[i] >= at)
        u = (at - lengths[segment - 1]) / max(lengths[segment] - lengths[segment - 1], 1e-6)
        origin = points[segment - 1].lerp(points[segment], u)
        radius = radii[segment - 1] + (radii[segment] - radii[segment - 1]) * u
        # 輪生せず、2〜4 本ずつ不規則な向きに出る。
        count = rng.randint(2, 4)
        turn += rng.uniform(1.2, 2.6)
        for k in range(count):
            yaw = turn + 2 * math.pi * k / count + rng.uniform(-0.6, 0.6)
            # 円錐の輪郭。先端付近は丸めて細くする。
            length = height * crown_width * (1 - t) ** 1.1 * rng.uniform(0.85, 1.1) + 0.12
            # カードも上ほど小さくする（大きいまま先端に集まると、樹冠の先が平らな塊になる）。
            size = 0.55 + 0.45 * min(1.0, (1 - t) * 1.6)
            rise = math.radians(rng.uniform(-8, 2) + 22 * t)   # 下の枝はほぼ水平、上ほど立つ
            upturn = math.radians(rng.uniform(8, 20))            # 先が上へ反る

            def pitch_at(v, rise=rise, upturn=upturn):
                return rise + upturn * v * v

            branch, _ = grow(origin, yaw, length, pitch_at, rng, wander=0.25, step=0.2, ground=0.3)
            b0 = max(0.012, radius * 0.35 * (0.6 + 0.4 * (1 - t)))
            branch_radii = [b0 * (1 - 0.8 * (j / (len(branch) - 1))) + 0.004 for j in range(len(branch))]
            add_tube_lod(geo, branch, branch_radii, lod["sides"][1], lod["stride"], BRANCH_BARK_TILE)
            add_twigs(geo, branch, branch_radii, rng, lod, size)
            foliage_seed = rng.getrandbits(32)
            add_foliage(geo, branch, foliage_seed, 0.3, BRANCH_CARD_SPACING, lod, 2, size)
        at += rng.uniform(0.25, 0.5) * (1 - 0.35 * t)
    # 先端の芽（真上へ伸びる主軸のカード）。
    tip_rng = random.Random(rng.getrandbits(32))
    for k in range(3 if lod["cards"] > 1 else 2):
        yaw = 2 * math.pi * k / 3
        axis = UP * 1.0 + Vector((math.cos(yaw), math.sin(yaw), 0)) * 0.25
        add_spray(geo, points[-1] - Vector((0, 0, CARD_LENGTH * 0.6)), axis, tip_rng,
                  lod["scale"] * 0.6, 1)
    return geo


def add_twigs(geo, branch, branch_radii, rng, lod, size):
    """枝の両脇へ水平に小枝を出す（平たい羽状の広がり）。骨格の乱数は段によらず同じだけ使う。"""
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
        yaw = math.atan2(tangent.y, tangent.x) + side * math.radians(rng.uniform(45, 70))
        pitch = math.asin(max(-1, min(1, tangent.z))) + math.radians(rng.uniform(-4, 6))
        length = rng.uniform(0.35, 0.8) * (1 - 0.5 * s) * min(1.0, full / 1.5)
        twig, _ = grow(branch[i], yaw, length, lambda v, pitch=pitch: pitch - math.radians(8) * v, rng,
                       wander=0.3, step=0.15, ground=0.3)
        t0 = branch_radii[i] * 0.5
        twig_radii = [t0 * (1 - 0.7 * (j / (len(twig) - 1))) + 0.003 for j in range(len(twig))]
        add_tube_lod(geo, twig, twig_radii, lod["sides"][2], lod["stride"], BRANCH_BARK_TILE)
        foliage_seed = rng.getrandbits(32)
        if lod["twig_cards"]:
            add_foliage(geo, twig, foliage_seed, 0.1, TWIG_CARD_SPACING, lod, 1, size)


# --- 樹冠の法線と芯 ---------------------------------------------------------------
# 針葉樹の樹冠は密な円錐なので、ダケカンバより外形を大きく取り、樹冠全体の円錐に沿わせる。
ENVELOPE_RADIUS_SCALE = 2.4   # 房の半径に対する外形のメタボールの半径
ENVELOPE_RESOLUTION = 0.14
ENVELOPE_SMOOTH = 8
ENVELOPE_NORMAL_WEIGHT = 0.75  # 葉の法線を外形の法線へ寄せる割合
ENVELOPE_UP_BIAS = 0.3         # 下面が真っ黒にならないよう上へ足す量
# 芯の色（リニア）。カラマツは芯を入れないが、Core のスロットの色として持つ。
CORE_COLOR = (0.030, 0.045, 0.015)


def main():
    options = parse_args()
    out, root = options["out"], options["root"]
    os.makedirs(out, exist_ok=True)
    rng = random.Random(options["seed"])
    np_rng = np.random.default_rng(options["seed"])

    paths = {key: os.path.join(out, f"T_Karamatsu_{key}.png")
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

    bark_mat = os.path.join(out, "MI_Karamatsu_Bark.tgmat")
    needle_mat = os.path.join(out, "MI_Karamatsu_Needles.tgmat")
    core_mat = os.path.join(out, "MI_Karamatsu_Core.tgmat")
    write_material(bark_mat, "MI_Karamatsu_Bark", source_ref(paths["Bark_D"], root),
                   source_ref(paths["Bark_N"], root), 0.8, 0.0, False)
    # 柔らかい針葉で艶は少ない。
    write_material(needle_mat, "MI_Karamatsu_Needles", source_ref(paths["Needles_D"], root),
                   source_ref(paths["Needles_N"], root), 0.65, 0.5, True)
    write_material(core_mat, "MI_Karamatsu_Core", None, None, 0.9, 0.0, False, CORE_COLOR)

    for index in range(1, options["variants"] + 1):
        name = f"Karamatsu_Var{index}"
        objects = []
        seed = options["seed"] * 1000 + index
        # 外形と芯は LOD0 の房から 1 度だけ作り、全 LOD で共有する（段で陰影が変わらないように）。
        shoots = build_tree(seed, LODS[0]).shoots
        envelope = build_hull(f"Envelope_{name}", shoots, ENVELOPE_RADIUS_SCALE, ENVELOPE_RESOLUTION,
                              ENVELOPE_SMOOTH)
        field = EnvelopeField(envelope)
        for level, lod in enumerate(LODS):
            geo = build_tree(seed, lod)
            transfer_field_normals(geo, field, ENVELOPE_NORMAL_WEIGHT, ENVELOPE_UP_BIAS)
            # 芯は入れない。透けた樹冠から暗い塊がそのまま見えてしまう（Core のスロットは並びのために残す）。
            # .blend では段を奥へ並べて見比べられるようにする。
            objects.append(make_object(f"{name}_LOD{level}", geo, materials,
                                       ((index - 1) * 14.0, level * 14.0, 0)))
            cards = geo.face_mat.count(LEAVES)
            print(f"{name}_LOD{level}: {geo.triangles()} triangles, {cards} cards")
        fbx = os.path.join(out, name + ".fbx")
        export_fbx(objects, fbx)
        # スロットの並びは FBX で最初に現れた順（幹が先）。
        write_model(os.path.join(out, name + ".tgmodel"), name, source_ref(fbx, root),
                    [asset_ref(bark_mat, root), asset_ref(needle_mat, root), asset_ref(core_mat, root)])
        bpy.data.meshes.remove(envelope)

    bpy.ops.wm.save_as_mainfile(filepath=os.path.join(out, "Karamatsu.blend"))


main()
