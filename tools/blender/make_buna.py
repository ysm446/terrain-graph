# ブナ（橅）の簡易モデルを作る Blender スクリプト。天城の稜線に残る太平洋側のブナ林。
#
# 使い方（Blender 5.x）:
#   blender -b --factory-startup --python-exit-code 1 --python tools/blender/make_buna.py -- \
#       --out data/Models/Buna [--root data] [--variants 3] [--seed 1]
#
# 出力（--out の下）:
#   Buna_VarN.fbx              幹・枝（Bark）、葉のカード（Leaves）、芯（Core）の 3 スロット。
#                              LOD0〜2 をオブジェクト名の末尾 _LOD<n> で 1 ファイルに入れる
#   T_Buna_Leaves_*.png        葉の付いた小枝のカード（RGBA、A でアルファ抜き）と法線（OpenGL 規約）
#   T_Buna_Bark_*.png          樹皮（灰白色でなめらか。地衣類のまだら）と法線
#   Buna.blend                 全バリエーション（手直し用）
#   *.tgmat / *.tgmodel / *.meta  terrain-graph のアセット。既にあれば上書きしない（UID を保つ）。
#                                 .tgmodel のマテリアルの並びだけは、足りない分を書き足す
#
# 共通の部品は vegetation.py、決まりごとは docs/design/vegetation-assets.md。
# 枝と葉の房の付け方はダケカンバ（make_dakekamba.py）と同じで、違いは次のとおり。
#   - 高さ 14〜20 m。幹は 1 本で、高さの 3〜4 割で 2〜4 本の大枝に分かれ、斜め上へ広がる。
#     樹冠は丸く広い（直径は高さの 6〜7 割）。
#   - 1 次枝から平たく横へ広がる小枝が出て、葉が層になって空を向く。
#   - 葉は卵形〜楕円形（長さ 5〜8 cm）で、縁は波打ち、7〜11 対のまっすぐな側脈が縁まで届く。
#     小枝にジグザグに 2 列で付く。ダケカンバの葉（重鋸歯）とは違うので、ここで描く。
#   - 樹皮は灰白色でなめらか。地衣類の白っぽいまだらと、暗い斑がある。
import math
import os
import random
import sys

import bpy
import numpy as np
from mathutils import Vector

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vegetation import (LEAVES, UP, EnvelopeField, Geometry, LeafCanvas, add_core, add_tube_lod,  # noqa: E402
                        along_polyline, asset_ref, build_hull, export_fbx, finish_cutout, grow,
                        height_to_normal, make_core_material, make_material, make_object,
                        parse_args, perpendicular, source_ref, tiling_noise, to_bytes,
                        transfer_field_normals, write_material, write_model, write_png)


# --- 葉のカード -----------------------------------------------------------------
def draw_buna_leaf(canvas, base, angle, length, color, rng, underside=False):
    """付け根 base から角度 angle（上が 0、右回りが正）へ伸びるブナの葉。
    卵形で先が短く尖り、縁は側脈の先で浅く波打つ。側脈はまっすぐ平行に縁まで届く。"""
    direction = np.array([math.sin(angle), -math.cos(angle)], np.float32)
    across_dir = np.array([-direction[1], direction[0]], np.float32)
    width = length * rng.uniform(0.52, 0.62)
    pairs = rng.randint(8, 10)
    reach = length + width
    x0, x1 = int(max(0, base[0] - reach)), int(min(canvas.size, base[0] + reach + 1))
    y0, y1 = int(max(0, base[1] - reach)), int(min(canvas.size, base[1] + reach + 1))
    if x0 >= x1 or y0 >= y1:
        return
    ys, xs = np.mgrid[y0:y1, x0:x1].astype(np.float32)
    rx, ry = xs + 0.5 - base[0], ys + 0.5 - base[1]
    s = (rx * direction[0] + ry * direction[1]) / length  # 付け根 0 → 先 1
    v = rx * across_dir[0] + ry * across_dir[1]           # 中肋からの距離（px）
    sc = np.clip(s, 0, 1)
    # 卵形。付け根は丸く、中ほどより少し下が一番広く、先は短く尖る。
    profile = np.power(np.clip(np.sin(np.pi * np.power(sc, 0.8)), 0, 1), 0.75) * (1 - 0.35 * sc ** 3)
    # 側脈は中肋から先の方へ斜めに走る。側脈の座標（中肋で 0、縁で側脈が届く位置）。
    rel = np.abs(v) / np.maximum(width * 0.5, 1e-3)
    vein_coord = (sc - rel * 0.22) * pairs
    phase = vein_coord - np.floor(vein_coord)
    # 縁は側脈の先で少し張り出し、間でくぼむ（波状）。
    wave = 1 + 0.06 * np.cos(2 * np.pi * (sc - 0.22) * pairs)
    half = width * 0.5 * profile * wave
    coverage = np.clip(half - np.abs(v) + 0.5, 0, 1) * ((s >= 0) & (s <= 1))
    if not (coverage > 0).any():
        return
    across = np.clip(v / np.maximum(half, 1e-3), -1, 1)
    vein = np.clip(1 - np.abs(phase - 0.5) * 2 * 7, 0, 1) * (rel < 0.95)  # 側脈の上（細い筋）
    midrib = np.clip(1.6 - np.abs(v), 0, 1)
    base_color = np.asarray(color, np.float32)
    if underside:
        base_color = base_color * 0.6 + np.array([0.28, 0.32, 0.20], np.float32)
    # 表は側脈がくぼんで少し暗く、脈の間がふくらむ。
    shade = 0.84 + 0.14 * (1 - np.abs(across)) - vein * 0.10
    rgb = base_color * shade[..., None] + np.array([0.07, 0.07, 0.03], np.float32) * midrib[..., None]
    # 側脈に沿ったひだ（脈の間が盛り上がる）と、中肋で浅く折れた V 字。
    pleat = np.sin(2 * np.pi * phase) * 0.28
    tilt = np.sign(v) * 0.18
    n = np.stack([across_dir[0] * tilt + direction[0] * pleat,
                  -across_dir[1] * tilt - direction[1] * pleat, np.ones_like(v)], axis=-1)
    n /= np.linalg.norm(n, axis=-1, keepdims=True)
    canvas._blend((slice(y0, y1), slice(x0, x1)), coverage, rgb, n)


# 画像は正方形（512 × 512）。下端の中央が小枝の付け根。ジグザグに伸びる小枝に葉が左右 2 列に付き、
# 途中から側枝が左右に 3〜4 本出る（葉の層がすき間なく広がる）。葉は長さ 5〜8 cm。カードは 50 cm 角なので、葉 1 枚は 55〜90 px。
def make_leaf_texture(rng, size=512):
    canvas = LeafCanvas(size)
    cm = size / 50.0
    base = np.array([size * 0.5, size - 4], np.float32)
    tip = np.array([size * 0.5 + rng.uniform(-24, 24), size * 0.10], np.float32)
    greens = [(0.19, 0.34, 0.10), (0.22, 0.37, 0.12), (0.17, 0.31, 0.09), (0.24, 0.39, 0.13)]

    def zigzag(p0, p1, count, amplitude):
        """節ごとに向きが少し折れる小枝（ブナの小枝は葉の付け根で左右に折れる）。"""
        p0, p1 = np.asarray(p0, np.float32), np.asarray(p1, np.float32)
        d = p1 - p0
        normal = np.array([-d[1], d[0]], np.float32) / max(float(np.hypot(*d)), 1e-4)
        nodes = [p0 + d * (k / count) + normal * (amplitude if k % 2 else -amplitude) * (0 < k < count)
                 for k in range(count + 1)]

        def at(s):
            x = min(max(s, 0.0), 1.0) * count
            k = min(int(x), count - 1)
            return nodes[k] + (nodes[k + 1] - nodes[k]) * (x - k)

        return at, nodes

    twigs = []
    main_count = rng.randint(8, 10)
    main, main_nodes = zigzag(base, tip, main_count, 5.0)
    twigs.append((main, main_nodes, 1.0))
    side = rng.choice((-1, 1))
    count = rng.randint(3, 4)
    for k in range(count):
        s0 = 0.12 + 0.55 * k / count + rng.uniform(-0.04, 0.04)
        side = -side
        angle = math.radians(rng.uniform(38, 60)) * side
        direction = np.array([math.sin(angle), -math.cos(angle)], np.float32)
        start = main(s0)
        length = size * rng.uniform(0.30, 0.42) * (1 - 0.3 * s0)
        branch, nodes = zigzag(start, start + direction * length, rng.randint(4, 6), 4.0)
        twigs.append((branch, nodes, 0.75))

    leaves = []
    for twig, nodes, weight in twigs:
        axis = nodes[-1] - nodes[0]
        axis_angle = math.atan2(axis[0], -axis[1])
        side = rng.choice((-1, 1))
        for k in range(1, len(nodes)):
            side = -side
            s = k / (len(nodes) - 1)
            # 葉は小枝から 50〜80° に開き、先の方ほど前を向く。付け根の葉は小さい。
            angle = axis_angle + side * math.radians(rng.uniform(50, 80) * (1 - 0.35 * s))
            length = cm * rng.uniform(6.0, 9.0) * (0.8 + 0.25 * math.sin(math.pi * min(1.0, s * 1.1))) * weight ** 0.3
            leaves.append((nodes[k], angle, length))
        # 先端の 1 枚は前を向く。
        leaves.append((nodes[-1], axis_angle + math.radians(rng.uniform(-15, 15)), cm * rng.uniform(5.0, 7.0)))
    rng.shuffle(leaves)
    back, front = leaves[: len(leaves) // 2], leaves[len(leaves) // 2:]

    def draw(group):
        for origin, angle, length in group:
            petiole_dir = np.array([math.sin(angle), -math.cos(angle)], np.float32)
            petiole_end = origin + petiole_dir * cm * 0.8
            canvas.stroke(origin, petiole_end, 2.6, 2.0, (0.34, 0.30, 0.16))
            color = np.array(rng.choice(greens)) * rng.uniform(0.9, 1.1)
            draw_buna_leaf(canvas, petiole_end, angle, length, color, rng, underside=rng.random() < 0.15)

    draw(back)
    for twig, nodes, weight in twigs:
        for i in range(30):
            s0, s1 = i / 30, (i + 1) / 30
            w = 6.5 * weight
            canvas.stroke(twig(s0), twig(s1), w * (1 - 0.5 * s0), w * (1 - 0.5 * s1), (0.36, 0.30, 0.24))
    draw(front)
    return finish_cutout(canvas.color, canvas.alpha, canvas.normal)


# --- 樹皮 ---------------------------------------------------------------------
def make_bark_texture(rng, size=512):
    """灰白色のなめらかな樹皮。地衣類の白っぽいまだらと暗い斑、うっすらとした横の皮目。
    画像の x が幹の周、y が幹の長さ（筒の UV と同じ）。上下左右に繰り返す。"""
    ys, xs = np.mgrid[0:size, 0:size].astype(np.float32)
    tone = tiling_noise(rng, size, 0.025, 0.02)
    color = np.array([0.55, 0.55, 0.52]) + np.array([0.04, 0.04, 0.035]) * np.clip(tone, -2, 2)[..., None]
    height = 0.05 * tiling_noise(rng, size, 0.15, 0.15)
    # 地衣類のまだら（白っぽい灰色と、薄い緑がかった灰色）。
    lichen = tiling_noise(rng, size, 0.03, 0.03)
    pale = np.clip((lichen - 0.3) * 0.9, 0, 1)[..., None]
    color = color * (1 - pale * 0.4) + np.array([0.72, 0.72, 0.67]) * pale * 0.4
    greenish = np.clip((tiling_noise(rng, size, 0.03, 0.03) - 1.0) * 1.5, 0, 1)[..., None]
    color = color * (1 - greenish * 0.5) + np.array([0.52, 0.56, 0.46]) * greenish * 0.5
    height += pale[..., 0] * 0.08
    # 暗い斑（小さな地衣類の縁と、剥がれた跡）。
    dark = np.clip((tiling_noise(rng, size, 0.06, 0.06) - 1.9) * 2.0, 0, 1)[..., None]
    color = color * (1 - dark * 0.4) + np.array([0.30, 0.30, 0.28]) * dark * 0.4
    # うっすらとした横の皮目。
    for _ in range(35):
        cx, cy = rng.uniform(0, size), rng.uniform(0, size)
        half_w, half_h = rng.uniform(4, 12), rng.uniform(0.8, 1.6)
        dx = (xs - cx + size / 2) % size - size / 2
        dy = (ys - cy + size / 2) % size - size / 2
        mark = np.clip(1.2 - ((dx / half_w) ** 2 + (dy / half_h) ** 2), 0, 1)
        color = color * (1 - mark[..., None] * 0.25)
        height -= mark * 0.3
    color = np.clip(color, 0, 1)
    return to_bytes(color), to_bytes(height_to_normal(height, 1.5) * 0.5 + 0.5)


# --- 形状 ---------------------------------------------------------------------
LEAF_CARD = 0.50               # 葉の付いた小枝のカード（m、正方形）
TRUNK_BARK_TILE = 1.2          # 幹の樹皮が縦に 1 回繰り返す長さ（m）
BRANCH_BARK_TILE = 0.6
BRANCH_LEAF_SPACING = 0.50 * LEAF_CARD  # 1 次枝の外側の房の間隔
TWIG_LEAF_SPACING = 0.40 * LEAF_CARD    # 小枝の房の間隔

# LOD ごとの作り分け。骨格は全段で同じ乱数から作り、細かさだけを変える。
#   sides: 幹と大枝・1 次枝・小枝の筒の角数（0 なら作らない）  stride: 筒の節を何点おきに使うか
#   spacing / scale: 房の間隔と大きさの倍率  cards: 房 1 つのカード枚数  tips: 枝先の房の数の上限
#   twig_leaves: 小枝の位置に房を付けるか（小枝の筒を省いても、房だけ残せる）
LODS = [
    {"sides": (10, 5, 3), "stride": 1, "spacing": 1.0, "scale": 1.0, "cards": 2, "tips": 3, "twig_leaves": True},
    {"sides": (8, 4, 0), "stride": 2, "spacing": 2.4, "scale": 1.45, "cards": 2, "tips": 1, "twig_leaves": True},
    {"sides": (6, 3, 0), "stride": 3, "spacing": 2.6, "scale": 1.75, "cards": 1, "tips": 1, "twig_leaves": False},
]


def add_spray(geo, base, axis, rng, scale, cards):
    """葉の房 1 つ。1 枚目はほぼ水平（ブナの葉は層になって空を向く）、2 枚目は軸まわりに傾けた面。"""
    axis = axis.normalized()
    size = LEAF_CARD * scale
    base = base - axis * 0.03 * scale
    tip = base + axis * size
    horizontal = axis.cross(UP)
    horizontal = horizontal.normalized() if horizontal.length > 1e-4 else perpendicular(axis)
    geo.shoots.append((base + axis * size * 0.5, size * 0.5))
    roll = rng.uniform(-0.3, 0.3)
    for k in range(cards):
        angle = roll + (0 if k == 0 else rng.choice((-1, 1)) * math.radians(rng.uniform(50, 75)))
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
        # 房は横へ平たく広がる（上へはあまり立てない）。
        axis = tangent * 0.5 + sideways * rng.uniform(0.5, 0.9) + UP * rng.uniform(-0.05, 0.2)
        add_spray(geo, position, axis, rng, rng.uniform(0.85, 1.1) * lod["scale"], lod["cards"])
    tangent = (points[-1] - points[-2]).normalized()
    for k in range(min(tip_count, lod["tips"])):
        spread = perpendicular(tangent) * rng.uniform(-0.5, 0.5)
        add_spray(geo, points[-1], tangent + spread + UP * 0.1, rng,
                  rng.uniform(0.9, 1.1) * lod["scale"], lod["cards"])


def build_tree(seed, lod):
    rng = random.Random(seed)
    geo = Geometry()
    height = rng.uniform(14.0, 20.0)
    r0 = 0.013 * height + rng.uniform(0.0, 0.04)   # 根元の半径（高さ 17 m で約 24 cm）
    fork = height * rng.uniform(0.28, 0.40)        # 大枝に分かれる高さ
    crown_radius = height * rng.uniform(0.30, 0.36)
    lean_yaw = rng.uniform(0, 2 * math.pi)
    lean = math.radians(rng.uniform(0.0, 4.0))

    # 幹。根元は少し太く張る。
    trunk, _ = grow(Vector((0, 0, -0.1)), lean_yaw, fork + 0.3, lambda t: math.pi / 2 - lean, rng,
                    wander=0.04, step=0.2)
    trunk_radii = [r0 * (1.25 - 0.25 * min(1.0, i / 3)) * (1 - 0.18 * (i / (len(trunk) - 1)))
                   for i in range(len(trunk))]
    add_tube_lod(geo, trunk, trunk_radii, lod["sides"][0], lod["stride"], TRUNK_BARK_TILE)
    fork_point = trunk[-1]
    fork_radius = trunk_radii[-1]

    # 大枝。1 本は主幹のように立ち、残りは斜め上へ開く。
    limbs = rng.randint(2, 4)
    turn = rng.uniform(0, 2 * math.pi)
    for k in range(limbs):
        yaw = turn + 2 * math.pi * k / limbs + rng.uniform(-0.4, 0.4)
        leader = k == 0
        # 大枝は急に立ち上がる（外へ倒すと花瓶形になり、樹冠の上が空く）。
        start_pitch = math.radians(rng.uniform(80, 87) if leader else rng.uniform(62, 74))
        end_pitch = math.radians(rng.uniform(70, 80) if leader else rng.uniform(48, 60))

        def pitch_at(t, a=start_pitch, b=end_pitch):
            return a + (b - a) * t

        mean_pitch = (start_pitch + end_pitch) * 0.5
        length = (height - fork) * rng.uniform(0.82, 0.95) / math.sin(mean_pitch)
        if not leader:
            length = min(length, (height - fork) * 0.8 / math.sin(mean_pitch) + crown_radius * 0.3)
        start = fork_point - Vector((0, 0, 0.2)) + Vector((math.cos(yaw), math.sin(yaw), 0)) * fork_radius * 0.3
        limb, _ = grow(start, yaw, length, pitch_at, rng, wander=0.15, step=0.3, ground=1.0)
        l0 = fork_radius * (0.8 if leader else rng.uniform(0.55, 0.7))
        limb_radii = [l0 * (1 - 0.85 * (j / (len(limb) - 1))) + 0.015 for j in range(len(limb))]
        add_tube_lod(geo, limb, limb_radii, lod["sides"][0], lod["stride"], TRUNK_BARK_TILE)
        add_branches(geo, limb, limb_radii, rng, lod, fork, height, crown_radius)
        add_foliage(geo, limb, rng.getrandbits(32), 0.85, BRANCH_LEAF_SPACING, lod, 3)
    return geo


def add_branches(geo, limb, limb_radii, rng, lod, fork, height, crown_radius):
    """大枝から 1 次枝を出し、1 次枝から平たく広がる小枝を出す。"""
    lengths = [0.0]
    for i in range(1, len(limb)):
        lengths.append(lengths[-1] + (limb[i] - limb[i - 1]).length)
    full = lengths[-1]
    next_at = full * 0.15 + rng.uniform(0.0, 0.3)
    turn = rng.uniform(0, 2 * math.pi)
    for i in range(1, len(limb) - 1):
        if lengths[i] < next_at:
            continue
        next_at = lengths[i] + rng.uniform(0.28, 0.5)
        turn += math.radians(137.5) + rng.uniform(-0.3, 0.3)
        p = limb[i]
        # 樹冠を丸くする。樹冠の中ほどで長く、上と下で短い。外側へ伸びる枝ほど長い。
        v = (p.z - fork) / max(height - fork, 1e-3)
        # 丸いドーム形。一番広いのは樹冠の中ほどより少し上。
        profile = math.sin(math.pi * min(1.0, 0.2 + 0.75 * v)) ** 0.5
        outward = Vector((p.x, p.y, 0))
        yaw = turn
        if outward.length > 0.3:
            out_yaw = math.atan2(outward.y, outward.x)
            yaw = out_yaw + math.atan2(math.sin(turn - out_yaw), math.cos(turn - out_yaw)) * 0.6
        room = max(0.8, crown_radius * profile - outward.length * 0.5)
        length = room * rng.uniform(0.7, 1.05)
        base_pitch = math.radians(rng.uniform(10, 35) + 20 * v)
        droop = math.radians(rng.uniform(5, 18))  # 先が少し下がってから葉の層が横へ広がる

        def pitch_at(u, base_pitch=base_pitch, droop=droop):
            return base_pitch - droop * u

        branch, _ = grow(p, yaw, length, pitch_at, rng, wander=0.5, step=0.22, ground=1.0)
        r0 = max(0.02, limb_radii[i] * rng.uniform(0.35, 0.5))
        branch_radii = [r0 * (1 - 0.8 * (j / (len(branch) - 1))) + 0.005 for j in range(len(branch))]
        add_tube_lod(geo, branch, branch_radii, lod["sides"][1], lod["stride"], BRANCH_BARK_TILE)
        add_twigs(geo, branch, branch_radii, rng, lod)
        foliage_seed = rng.getrandbits(32)
        if lod["sides"][1] > 0:
            add_foliage(geo, branch, foliage_seed, 0.45, BRANCH_LEAF_SPACING, lod, 3)


def add_twigs(geo, branch, branch_radii, rng, lod):
    """1 次枝の両脇へ、ほぼ水平に小枝を出す（葉が層になって空を向く）。骨格の乱数は段によらず同じ。"""
    lengths = [0.0]
    for i in range(1, len(branch)):
        lengths.append(lengths[-1] + (branch[i] - branch[i - 1]).length)
    full = lengths[-1]
    next_at = full * 0.15 + rng.uniform(0.0, 0.15)
    side = rng.choice((-1, 1))
    for i in range(1, len(branch) - 1):
        if lengths[i] < next_at:
            continue
        s = lengths[i] / full
        next_at = lengths[i] + rng.uniform(0.2, 0.35)
        side = -side
        tangent = (branch[i + 1] - branch[i - 1]).normalized()
        yaw = math.atan2(tangent.y, tangent.x) + side * math.radians(rng.uniform(35, 65))
        pitch = math.radians(rng.uniform(-5, 15))
        length = rng.uniform(0.7, 1.5) * (1 - 0.4 * s)
        twig, _ = grow(branch[i], yaw, length, lambda u, pitch=pitch: pitch, rng,
                       wander=0.5, step=0.18, ground=1.0)
        t0 = branch_radii[i] * 0.5
        twig_radii = [t0 * (1 - 0.7 * (j / (len(twig) - 1))) + 0.003 for j in range(len(twig))]
        add_tube_lod(geo, twig, twig_radii, lod["sides"][2], lod["stride"], BRANCH_BARK_TILE)
        foliage_seed = rng.getrandbits(32)
        if lod["twig_leaves"]:
            add_foliage(geo, twig, foliage_seed, 0.1, TWIG_LEAF_SPACING, lod, 2)
        # 小枝の房を省く段では、1 次枝の房で埋める（房を大きくしてある）。


# --- 樹冠の法線と芯 ---------------------------------------------------------------
# ダケカンバと同じく、外形を房のこぶに沿わせて房ごとに陰影を付ける。ブナの樹冠は葉の層が
# 厚いので、外形はダケカンバより少し大きく取る。寄せる割合は下げない。
ENVELOPE_RADIUS_SCALE = 2.0   # 房の半径に対する外形のメタボールの半径
ENVELOPE_RESOLUTION = 0.14
ENVELOPE_SMOOTH = 8
ENVELOPE_NORMAL_WEIGHT = 0.75  # 葉の法線を外形の法線へ寄せる割合
ENVELOPE_UP_BIAS = 0.3         # 下面が真っ黒にならないよう上へ足す量
CORE_RADIUS_SCALE = 1.6
CORE_RESOLUTION = 0.12
CORE_INSET = 0.15
CORE_TRIANGLES = (2600, 1200, 550)
# 芯の色（リニア）。葉より暗くし、隙間から覗く樹冠の奥に見せる。
CORE_COLOR = (0.016, 0.030, 0.011)


def main():
    options = parse_args()
    out, root = options["out"], options["root"]
    os.makedirs(out, exist_ok=True)
    rng = random.Random(options["seed"])
    np_rng = np.random.default_rng(options["seed"])

    paths = {key: os.path.join(out, f"T_Buna_{key}.png")
             for key in ("Leaves_D", "Leaves_N", "Bark_D", "Bark_N")}
    leaves, leaves_normal = make_leaf_texture(rng)
    write_png(paths["Leaves_D"], leaves)
    write_png(paths["Leaves_N"], leaves_normal)
    coverage = float((leaves[..., 3] >= 128).mean())
    print(f"leaf coverage: {coverage * 100:.1f}%")
    bark, bark_normal = make_bark_texture(np_rng)
    write_png(paths["Bark_D"], bark)
    write_png(paths["Bark_N"], bark_normal)

    bpy.ops.wm.read_factory_settings(use_empty=True)
    materials = [make_material("Bark", paths["Bark_D"], paths["Bark_N"], False),
                 make_material("Leaves", paths["Leaves_D"], paths["Leaves_N"], True),
                 make_core_material(CORE_COLOR)]

    bark_mat = os.path.join(out, "MI_Buna_Bark.tgmat")
    leaf_mat = os.path.join(out, "MI_Buna_Leaves.tgmat")
    core_mat = os.path.join(out, "MI_Buna_Core.tgmat")
    write_material(bark_mat, "MI_Buna_Bark", source_ref(paths["Bark_D"], root),
                   source_ref(paths["Bark_N"], root), 0.6, 0.0, False)
    # 葉は少し艶がある。
    write_material(leaf_mat, "MI_Buna_Leaves", source_ref(paths["Leaves_D"], root),
                   source_ref(paths["Leaves_N"], root), 0.5, 0.5, True)
    write_material(core_mat, "MI_Buna_Core", None, None, 0.9, 0.0, False, CORE_COLOR)

    for index in range(1, options["variants"] + 1):
        name = f"Buna_Var{index}"
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
                                       ((index - 1) * 20.0, level * 20.0, 0)))
            cards = geo.face_mat.count(LEAVES)
            print(f"{name}_LOD{level}: {geo.triangles()} triangles, {cards} cards")
        fbx = os.path.join(out, name + ".fbx")
        export_fbx(objects, fbx)
        # スロットの並びは FBX で最初に現れた順（幹が先）。
        write_model(os.path.join(out, name + ".tgmodel"), name, source_ref(fbx, root),
                    [asset_ref(bark_mat, root), asset_ref(leaf_mat, root), asset_ref(core_mat, root)])
        bpy.data.meshes.remove(envelope)
        bpy.data.meshes.remove(hull)

    bpy.ops.wm.save_as_mainfile(filepath=os.path.join(out, "Buna.blend"))


main()
