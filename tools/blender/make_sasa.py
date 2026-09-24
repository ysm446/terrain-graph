# ササ（笹）の群落のひとまとまりを作る Blender スクリプト。
#
# 使い方（Blender 5.x）:
#   blender -b --factory-startup --python-exit-code 1 --python tools/blender/make_sasa.py -- \
#       --out data/Models/Sasa [--root data] [--variants 3] [--seed 1]
#
# 出力（--out の下）:
#   Sasa_VarN.fbx          稈（Culm）、葉のカード（Leaves）、芯（Core）の 3 スロット。
#                          LOD0〜2 をオブジェクト名の末尾 _LOD<n> で 1 ファイルに入れる
#   T_Sasa_Leaves_*.png    葉の付いた小枝のカード（RGBA、A でアルファ抜き）と法線（OpenGL 規約）
#   T_Sasa_Culm_*.png      稈（黄緑の筒に節）と法線
#   Sasa.blend             全バリエーション（手直し用）
#   *.tgmat / *.tgmodel / *.meta   terrain-graph のアセット。既にあれば上書きしない（UID を保つ）。
#
# 共通の部品は vegetation.py、決まりごとは docs/design/vegetation-assets.md。
#
# 形: 伊豆スカイラインのような、背丈ほどの高いササの群落（ハコネダケ / アズマネザサの仲間）。
# 1 本ずつではなく、約 2 m 四方の株のまとまりを 1 つのモデルにし、隣と重ねて置いて
# 切れ目のない絨毯にする。稈は高さ 0.9〜1.3 m（上面はほぼ平ら）で、葉は上の層に集まる。
# 葉は細長い披針形（長さ 15〜25 cm、幅は長さの 20〜25%）で縁は滑らか、明るい黄緑で少し艶がある。
# 稈の上半分から出る小枝の先に、葉が扇状に開いて付く。
import math
import os
import random
import sys

import bpy
import numpy as np
from mathutils import Vector

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vegetation import (LEAVES, UP, CanopyField, Geometry, LeafCanvas, add_core,  # noqa: E402
                        add_tube_lod, along_polyline, asset_ref, build_hull, export_fbx,
                        finish_cutout, grow, height_to_normal, make_core_material, make_material,
                        make_object, parse_args, perpendicular, source_ref, tiling_noise, to_bytes,
                        transfer_field_normals, write_material, write_model, write_png)


# --- 葉のカード -----------------------------------------------------------------
# 画像は正方形（512 × 512）で、カードは 32 cm 角（1 px ≈ 0.6 mm）。下端の中央が小枝の付け根で、
# 小枝の先から葉が数枚ずつ扇状に開く（ササの葉は枝先に掌状に集まる）。主軸の先と左右の横枝の先の 3 つ。
def draw_sasa_leaf(canvas, base, angle, length, width, color, rng):
    """付け根 base から角度 angle（上が 0、右回りが正）へ伸びる披針形の葉。
    縁は滑らかで、先は長く尖る。葉脈は中肋と平行に走る。"""
    size = canvas.size
    direction = np.array([math.sin(angle), -math.cos(angle)], np.float32)
    across_dir = np.array([-direction[1], direction[0]], np.float32)
    reach = length + width
    x0, x1 = int(max(0, base[0] - reach)), int(min(size, base[0] + reach + 1))
    y0, y1 = int(max(0, base[1] - reach)), int(min(size, base[1] + reach + 1))
    if x0 >= x1 or y0 >= y1:
        return
    ys, xs = np.mgrid[y0:y1, x0:x1].astype(np.float32)
    rx, ry = xs + 0.5 - base[0], ys + 0.5 - base[1]
    s = (rx * direction[0] + ry * direction[1]) / length  # 付け根 0 → 先 1
    # 葉は先へ行くほど少し垂れる（中肋を横へ曲げる）。
    bend = rng.uniform(-0.18, 0.18) * length
    v = rx * across_dir[0] + ry * across_dir[1] - bend * np.clip(s, 0, 1) ** 2
    sc = np.clip(s, 0, 1)
    # 付け根は丸く細まり、3 割あたりで最も広く、先は長く尖る。
    profile = np.power(np.clip(np.sin(np.pi * np.power(sc, 0.5)), 0, 1), 0.9) * (1 - 0.35 * sc)
    half = width * 0.5 * profile
    coverage = np.clip(half - np.abs(v) + 0.5, 0, 1) * ((s >= 0) & (s <= 1))
    if not (coverage > 0).any():
        return
    across = np.clip(v / np.maximum(half, 1e-3), -1, 1)
    # 平行脈（中肋に沿う細い筋）と中肋。縁はわずかに明るい。
    veins = np.clip(1 - np.abs(np.sin(across * math.pi * 4.5)) * 5, 0, 1) * 0.06
    midrib = np.clip(1.6 - np.abs(v), 0, 1)
    edge = np.clip((np.abs(across) - 0.85) / 0.15, 0, 1)
    base_color = np.asarray(color, np.float32)
    shade = 0.84 + 0.16 * (1 - np.abs(across)) + veins
    rgb = (base_color * shade[..., None] + np.array([0.07, 0.08, 0.02], np.float32) * midrib[..., None]
           + np.array([0.05, 0.05, 0.02], np.float32) * edge[..., None])
    # 中肋で浅く折れた葉（V 字）。平行脈で細かく波打たせる。
    tilt = np.sign(v) * 0.25 + veins * 1.5
    n = np.stack([across_dir[0] * tilt, -across_dir[1] * tilt, np.ones_like(v)], axis=-1)
    n /= np.linalg.norm(n, axis=-1, keepdims=True)
    canvas._blend((slice(y0, y1), slice(x0, x1)), coverage, rgb, n)


def make_leaf_texture(rng, size=512):
    canvas = LeafCanvas(size)
    base = np.array([size * 0.5, size - 4], np.float32)
    # 主軸の先の扇と、途中から横へ分かれた小枝の先の扇。2 つでカードの面を埋める。
    top = np.array([size * 0.5 + rng.uniform(-12, 12), size * 0.50], np.float32)
    side = rng.choice((-1, 1))
    # 横枝は左右に 1 本ずつ、分かれる高さと扇の高さを変える。
    laterals = [(0.40, np.array([size * (0.5 + side * 0.25), size * 0.64], np.float32), side),
                (0.58, np.array([size * (0.5 - side * 0.22), size * 0.56], np.float32), -side)]
    # 明るい黄緑（写真の日向の葉）。少しずつ黄み・青みを振る。
    greens = [(0.40, 0.60, 0.16), (0.44, 0.63, 0.18), (0.36, 0.56, 0.14), (0.46, 0.65, 0.21)]

    def main_twig(t):
        return base + (top - base) * t + np.array([6 * math.sin(t * 2.2), 0], np.float32)

    def side_twig(split, end, t):
        start = main_twig(split)
        return start + (end - start) * t

    def fan(origin, count, spread, lengths, lean):
        """origin から左右へ開く扇。真ん中の葉ほど長い。lean は扇全体の傾き（度）。"""
        result = []
        for k in range(count):
            u = (k + 0.5) / count * 2 - 1  # -1（左）〜 1（右）
            angle = math.radians(lean + u * spread + rng.uniform(-7, 7))
            length = size * rng.uniform(*lengths) * (1 - 0.22 * abs(u))
            width = length * rng.uniform(0.20, 0.25)
            jitter = np.array([rng.uniform(-5, 5), rng.uniform(-6, 6)], np.float32)
            result.append((origin + jitter, angle, length, width))
        return result

    leaves = fan(top, rng.randint(5, 7), rng.uniform(45, 60), (0.40, 0.48), rng.uniform(-10, 10))
    for _, end, direction in laterals:
        leaves += fan(end, rng.randint(3, 5), rng.uniform(35, 50), (0.30, 0.38), direction * rng.uniform(30, 45))
    rng.shuffle(leaves)
    back, front = leaves[: len(leaves) // 2], leaves[len(leaves) // 2:]

    def draw(group):
        for origin, angle, length, width in group:
            color = np.array(rng.choice(greens)) * rng.uniform(0.9, 1.1)
            draw_sasa_leaf(canvas, origin, angle, length, width, color, rng)

    draw(back)
    twig_color = (0.40, 0.44, 0.20)
    for i in range(24):
        t0, t1 = i / 24, (i + 1) / 24
        canvas.stroke(main_twig(t0), main_twig(t1), 5 - 2 * t0, 5 - 2 * t1, twig_color)
        for split, end, _ in laterals:
            canvas.stroke(side_twig(split, end, t0), side_twig(split, end, t1), 3.5 - t0, 3.5 - t1, twig_color)
    draw(front)
    return finish_cutout(canvas.color, canvas.alpha, canvas.normal)


# --- 稈 -----------------------------------------------------------------------
CULM_TILE = 0.15  # 稈のテクスチャ 1 枚の長さ（m）。節 1 つぶん（節間 15 cm）

def make_culm_texture(rng, size=256):
    """黄緑の稈に縦の細い筋と、節（少し膨らんだ暗い輪と、その下の薄茶の鞘の名残り）。
    画像の x が稈の周、y が稈の長さ（筒の UV と同じ）。上下左右に繰り返す。"""
    ys, xs = np.mgrid[0:size, 0:size].astype(np.float32)
    streak = tiling_noise(rng, size, 0.4, 0.01)
    color = np.array([0.40, 0.46, 0.20]) + np.array([0.03, 0.03, 0.01]) * np.clip(streak, -2, 2)[..., None]
    height = 0.05 * streak
    # 節は画像の上から 1 割の所。輪を暗く、少し盛り上げる。その下に薄茶の鞘。
    node_y = size * 0.1
    dy = (ys - node_y + size / 2) % size - size / 2
    ring = np.exp(-(dy / 2.2) ** 2)
    sheath = np.clip(1 - (dy - 6) / (size * 0.25), 0, 1) * (dy > 3)
    color = color * (1 - ring[..., None] * 0.45)
    color = color * (1 - sheath[..., None] * 0.35) + np.array([0.46, 0.40, 0.26]) * sheath[..., None] * 0.35
    height += ring * 0.5
    return to_bytes(np.clip(color, 0, 1)), to_bytes(height_to_normal(height, 3.0) * 0.5 + 0.5)


# --- 形状 ---------------------------------------------------------------------
LEAF_CARD = 0.32                  # 葉の小枝のカード（m、正方形）
LEAF_SPACING = 0.25 * LEAF_CARD   # 稈に沿ったカードの間隔
PATCH_RADIUS = 0.85               # 稈の根元を散らす円の半径（葉はその外へ 30 cm ほど張り出す）
LEAF_START = 0.6                  # 稈のどこから上に葉を付けるか（割合）

# LOD ごとの作り分け。骨格は全段で同じ乱数から作り、細かさだけを変える。
#   sides: 稈の筒の角数（0 なら作らない）  stride: 筒の節を何点おきに使うか
#   spacing / scale: カードの間隔と大きさの倍率  cards: 位置ごとのカード枚数  tips: 稈の先のカードの数の上限
# 群落は中景（数十〜数百 m）で見ることが多く、そこでは LOD2 が描かれる。LOD2 のカードを
# 大きく少なくすると、まとまりごとに丸いクッションが並んで見えるので、カードは 2 枚のまま間隔だけ広げる。
LODS = [
    {"sides": 4, "stride": 1, "spacing": 1.0, "scale": 1.0, "cards": 2, "tips": 2},
    {"sides": 3, "stride": 2, "spacing": 1.5, "scale": 1.2, "cards": 2, "tips": 1},
    {"sides": 0, "stride": 3, "spacing": 2.2, "scale": 1.4, "cards": 2, "tips": 1},
]


def add_spray(geo, base, axis, rng, scale, cards):
    """葉の小枝 1 つ。1 枚目は空へ向けて寝かせた面、2 枚目は軸まわりに傾けた面。"""
    axis = axis.normalized()
    size = LEAF_CARD * scale
    base = base - axis * 0.03 * scale
    tip = base + axis * size
    horizontal = axis.cross(UP)
    horizontal = horizontal.normalized() if horizontal.length > 1e-4 else perpendicular(axis)
    geo.shoots.append((base + axis * size * 0.6, size * 0.5))
    roll = rng.uniform(-0.35, 0.35)
    for k in range(cards):
        angle = roll + (0 if k == 0 else rng.choice((-1, 1)) * math.radians(rng.uniform(50, 80)))
        across = (horizontal * math.cos(angle) + axis.cross(horizontal) * math.sin(angle)).normalized()
        face = across.cross(axis).normalized()
        if face.z < 0:
            face = -face
        corners = [base - across * size / 2, base + across * size / 2,
                   tip + across * size / 2, tip - across * size / 2]
        normals = [(face * 0.5 + UP * 0.5).normalized()] * 4
        first = len(geo.verts)
        geo.verts.extend(tuple(c) for c in corners)
        geo.add_face([first, first + 1, first + 2, first + 3], LEAVES, [(0, 0), (1, 0), (1, 1), (0, 1)], normals)


def build_patch(seed, lod):
    rng = random.Random(seed)
    geo = Geometry()
    culms = rng.randint(58, 74)
    for _ in range(culms):
        # 円の中へ一様に散らす。外側ほどわずかに低く、外へ傾く。上面はほぼ平らにして、
        # 隣のまとまりと重ねたときに 1 つずつ丸く盛り上がって見えないようにする。
        r = PATCH_RADIUS * math.sqrt(rng.random())
        theta = rng.uniform(0, 2 * math.pi)
        start = Vector((r * math.cos(theta), r * math.sin(theta), 0.0))
        edge = r / PATCH_RADIUS
        height = rng.uniform(0.9, 1.3) * (1 - 0.06 * edge * edge)
        yaw = theta + rng.uniform(-0.6, 0.6)
        lean = math.radians(rng.uniform(4, 10) + 8 * edge)   # 鉛直からの傾き
        droop = math.radians(rng.uniform(6, 16))              # 先ほどしなって寝る

        def pitch_at(t, lean=lean, droop=droop):
            return math.pi / 2 - lean - droop * t * t

        points, yaw = grow(start, yaw, height * 1.04, pitch_at, rng, wander=0.4, step=0.1)
        r0 = rng.uniform(0.0035, 0.005)
        radii = [r0 * (1 - 0.5 * (i / (len(points) - 1))) + 0.001 for i in range(len(points))]
        add_tube_lod(geo, points, radii, lod["sides"], lod["stride"], CULM_TILE)
        add_foliage(geo, points, rng.getrandbits(32), theta, lod)
    return geo


def add_foliage(geo, points, seed, outward, lod):
    """稈の上半分に葉の小枝を付け、先にも付ける。小枝は外寄りの横へ、斜め上へ出る。"""
    rng = random.Random(seed)
    outward_dir = Vector((math.cos(outward), math.sin(outward), 0.0))
    for position, tangent, _ in along_polyline(points, LEAF_START, LEAF_SPACING * lod["spacing"], rng):
        sideways = tangent.cross(UP)
        sideways = sideways.normalized() if sideways.length > 1e-4 else perpendicular(tangent)
        sideways = (sideways * rng.choice((-1, 1)) + outward_dir * 0.4).normalized()
        axis = tangent * 0.35 + sideways * rng.uniform(0.6, 1.0) + UP * rng.uniform(0.25, 0.6)
        add_spray(geo, position, axis, rng, rng.uniform(0.85, 1.1) * lod["scale"], lod["cards"])
    tangent = (points[-1] - points[-2]).normalized()
    for k in range(lod["tips"]):
        spread = perpendicular(tangent) * rng.uniform(-0.6, 0.6)
        add_spray(geo, points[-1] - tangent * 0.05, tangent + spread + UP * 0.4, rng,
                  rng.uniform(0.9, 1.1) * lod["scale"], lod["cards"])


# --- 樹冠の法線と芯 ---------------------------------------------------------------
# 隣のまとまりと重ねて絨毯にするので、まとまりの縁で法線を外へ倒しすぎない
# （倒すと継ぎ目に暗い筋が出る）。樹冠の高さの場の法線を、上向きへ半分寄せてから使う。
CANOPY_CELL = 0.05
CANOPY_BLUR = 0.3
CANOPY_FLATTEN = 0.5      # 場の法線を上向きへ寄せる割合（0 で場のまま、1 で真上）
CANOPY_NORMAL_WEIGHT = 0.8
CORE_RADIUS_SCALE = 0.8
CORE_DROP = 0.2           # 芯を葉の層より下へ沈める量（m）
CORE_MIN_HEIGHT = 0.75    # 芯に使う房の高さの下限（一番高い房に対する割合）
CORE_RESOLUTION = 0.06
CORE_INSET = 0.1
CORE_TRIANGLES = (1500, 700, 300)
# 芯の色（リニア）。葉の層の奥の暗がりに見せる。
CORE_COLOR = (0.018, 0.032, 0.010)


class FlattenedCanopy:
    def __init__(self, shoots):
        self.field = CanopyField(shoots, CANOPY_CELL, CANOPY_BLUR)

    def normal(self, point):
        return (self.field.normal(point) * (1 - CANOPY_FLATTEN) + UP * CANOPY_FLATTEN).normalized()


def main():
    options = parse_args()
    out, root = options["out"], options["root"]
    os.makedirs(out, exist_ok=True)
    rng = random.Random(options["seed"])
    np_rng = np.random.default_rng(options["seed"])

    paths = {key: os.path.join(out, f"T_Sasa_{key}.png")
             for key in ("Leaves_D", "Leaves_N", "Culm_D", "Culm_N")}
    leaves, leaves_normal = make_leaf_texture(rng)
    write_png(paths["Leaves_D"], leaves)
    write_png(paths["Leaves_N"], leaves_normal)
    coverage = float((leaves[..., 3] >= 128).mean())
    print(f"leaf coverage: {coverage:.1%}")
    culm, culm_normal = make_culm_texture(np_rng)
    write_png(paths["Culm_D"], culm)
    write_png(paths["Culm_N"], culm_normal)

    bpy.ops.wm.read_factory_settings(use_empty=True)
    materials = [make_material("Culm", paths["Culm_D"], paths["Culm_N"], False),
                 make_material("Leaves", paths["Leaves_D"], paths["Leaves_N"], True),
                 make_core_material(CORE_COLOR)]

    culm_mat = os.path.join(out, "MI_Sasa_Culm.tgmat")
    leaf_mat = os.path.join(out, "MI_Sasa_Leaves.tgmat")
    core_mat = os.path.join(out, "MI_Sasa_Core.tgmat")
    write_material(culm_mat, "MI_Sasa_Culm", source_ref(paths["Culm_D"], root),
                   source_ref(paths["Culm_N"], root), 0.6, 0.0, False)
    # 少し艶のある葉（ミヤマハンノキと同程度）。
    write_material(leaf_mat, "MI_Sasa_Leaves", source_ref(paths["Leaves_D"], root),
                   source_ref(paths["Leaves_N"], root), 0.45, 0.5, True)
    write_material(core_mat, "MI_Sasa_Core", None, None, 0.9, 0.0, False, CORE_COLOR)

    for index in range(1, options["variants"] + 1):
        name = f"Sasa_Var{index}"
        objects = []
        seed = options["seed"] * 1000 + index
        # 樹冠の場と芯は LOD0 の房から 1 度だけ作り、全 LOD で共有する。
        shoots = build_patch(seed, LODS[0]).shoots
        field = FlattenedCanopy(shoots)
        # 芯は房を下へ沈めてから作る（葉の上面から出さない）。縁の低い房は使わない
        # （まとまりの縁で、葉の下に暗い塊が垂れて見える）。
        top = max(center.z for center, _ in shoots)
        core_shoots = [(center - UP * CORE_DROP, radius) for center, radius in shoots
                       if center.z > top * CORE_MIN_HEIGHT]
        hull = build_hull(f"Core_{name}", core_shoots, CORE_RADIUS_SCALE, CORE_RESOLUTION)
        for level, lod in enumerate(LODS):
            geo = build_patch(seed, lod)
            transfer_field_normals(geo, field, CANOPY_NORMAL_WEIGHT)
            add_core(geo, hull, CORE_TRIANGLES[level], CORE_INSET)
            objects.append(make_object(f"{name}_LOD{level}", geo, materials,
                                       ((index - 1) * 4.0, level * 4.0, 0)))
            cards = geo.face_mat.count(LEAVES)
            print(f"{name}_LOD{level}: {geo.triangles()} triangles, {cards} cards")
        fbx = os.path.join(out, name + ".fbx")
        export_fbx(objects, fbx)
        # スロットの並びは FBX で最初に現れた順（稈が先）。
        write_model(os.path.join(out, name + ".tgmodel"), name, source_ref(fbx, root),
                    [asset_ref(culm_mat, root), asset_ref(leaf_mat, root), asset_ref(core_mat, root)])
        bpy.data.meshes.remove(hull)

    bpy.ops.wm.save_as_mainfile(filepath=os.path.join(out, "Sasa.blend"))


main()
