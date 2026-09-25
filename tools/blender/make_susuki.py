# ススキ（薄）の株を作る Blender スクリプト。西天城高原の道路脇や草原の、夏の緑の株。
#
# 使い方（Blender 5.x）:
#   blender -b --factory-startup --python-exit-code 1 --python tools/blender/make_susuki.py -- \
#       --out data/Models/Susuki [--root data] [--variants 3] [--seed 1]
#
# 出力（--out の下）:
#   Susuki_VarN.fbx           枯れた稈（Stem）、葉のカード（Leaves）、株元の芯（Core）の 3 スロット。
#                             LOD0〜2 をオブジェクト名の末尾 _LOD<n> で 1 ファイルに入れる
#   T_Susuki_Leaves_*.png     細長い葉を数本ずつ描いた縦長のカード（RGBA、A でアルファ抜き）と法線（OpenGL 規約）
#   T_Susuki_Stem_*.png       去年の枯れた稈（麦わら色）と法線
#   Susuki.blend              全バリエーション（手直し用）
#   *.tgmat / *.tgmodel / *.meta   terrain-graph のアセット。既にあれば上書きしない（UID を保つ）。
#
# 共通の部品は vegetation.py、決まりごとは docs/design/vegetation-assets.md。
#
# 形: 夏の穂の出ていない株。根元（直径 30〜50 cm）から、幅 1〜1.5 cm・長さ 0.8〜1.5 m の葉が
# 密に立ち上がり、先が外へ弓なりに垂れる。株の高さは 1.2〜1.5 m、葉先の広がりは直径 1.5〜2 m。
# 葉は明るい緑で、中肋が白く通る。株の中に去年の枯れた稈が少し残る。
# 葉 1 枚ずつではなく、葉を 8〜11 枚描いた縦長のカード（幅 20 cm × 長さ 1.2 m）を曲げて使う。
import math
import os
import random
import sys

import bpy
import numpy as np
from mathutils import Vector

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vegetation import (LEAVES, UP, Geometry, LeafCanvas, add_core, add_tube_lod,  # noqa: E402
                        asset_ref, build_hull, export_fbx, finish_cutout, grow, height_to_normal,
                        make_core_material, make_material, make_object, parse_args, source_ref,
                        tiling_noise, to_bytes, write_material, write_model, write_png)

BARK = 0  # 枯れた稈のスロット（vegetation の BARK と同じ番号）


# --- 葉のカード -----------------------------------------------------------------
# 画像は縦長（幅 256 × 高さ 2048）。カードは幅 20 cm × 長さ 1.2 m（1 px ≈ 0.6〜0.8 mm）。
# 下端が株元、上端が葉先。葉は下端から立ち上がり、先ほど細く尖って横へ流れる。
class TallCanvas(LeafCanvas):
    def __init__(self, width, height):
        self.size = max(width, height)
        self.width, self.height = width, height
        self.color = np.zeros((height, width, 3), np.float32)
        self.alpha = np.zeros((height, width), np.float32)
        self.normal = np.zeros((height, width, 3), np.float32)
        self.normal[..., 2] = 1.0


def draw_blade(canvas, x0, sway, length_px, width_px, color, rng):
    """下端 x0 から上へ伸びる細長い葉。sway は葉先の横へのずれ（px）。中肋は白く、縁は細かくざらつく。
    葉の中心線を行ごとに求め、横方向の距離で塗る（縦に長いので行単位で処理する）。"""
    h, w = canvas.height, canvas.width
    top = max(0, int(h - length_px))
    for y in range(h - 1, top - 1, -1):
        s = (h - 1 - y) / max(length_px, 1)          # 株元 0 → 葉先 1
        center = x0 + sway * s ** 1.8 + 3.0 * math.sin(s * 9.0 + x0)
        # 付け根は細く、2 割で最も広く、先は長く尖る。
        half = width_px * 0.5 * min(1.0, s * 6 + 0.35) * (1 - s) ** 0.8
        if half < 0.3:
            continue
        xa, xb = int(max(0, center - half - 2)), int(min(w, center + half + 3))
        if xa >= xb:
            continue
        xs = np.arange(xa, xb, dtype=np.float32) + 0.5
        v = xs - center
        coverage = np.clip(half - np.abs(v) + 0.5, 0, 1)
        across = np.clip(v / max(half, 1e-3), -1, 1)
        midrib = np.clip(1.0 - np.abs(v) / max(half * 0.18, 0.6), 0, 1) * min(1.0, (1 - s) * 3)
        shade = 0.82 + 0.18 * (1 - np.abs(across))
        rgb = np.asarray(color, np.float32)[None, :] * shade[:, None]
        rgb = rgb * (1 - midrib[:, None] * 0.6) + np.array([0.80, 0.84, 0.66], np.float32) * midrib[:, None] * 0.6
        # 中肋で浅く折れた葉（V 字）。
        tilt = np.sign(v) * 0.3
        n = np.stack([tilt, np.zeros_like(v), np.ones_like(v)], axis=-1)
        n /= np.linalg.norm(n, axis=-1, keepdims=True)
        a = coverage[:, None]
        canvas.color[y, xa:xb] = canvas.color[y, xa:xb] * (1 - a) + rgb * a
        canvas.alpha[y, xa:xb] = canvas.alpha[y, xa:xb] * (1 - coverage) + coverage
        canvas.normal[y, xa:xb] = canvas.normal[y, xa:xb] * (1 - a) + n * a


def make_leaf_texture(rng, width=256, height=2048):
    canvas = TallCanvas(width, height)
    cm = height / 120.0
    # 夏の株の緑。アルベドは G でリニア 0.14〜0.16（ハコネダケより少し暗い。ほかの植生と釣り合わせる）。
    greens = [(0.27, 0.43, 0.14), (0.30, 0.46, 0.16), (0.25, 0.40, 0.12), (0.32, 0.48, 0.17)]
    blades = []
    for k in range(rng.randint(8, 11)):
        x0 = width * 0.5 + rng.uniform(-0.22, 0.22) * width
        length = height * rng.uniform(0.55, 0.98)
        sway = rng.uniform(-0.36, 0.36) * width
        blade_width = cm * rng.uniform(1.1, 1.6)
        blades.append((length, x0, sway, blade_width))
    blades.sort(reverse=True)  # 長い葉を奥に
    for length, x0, sway, blade_width in blades:
        color = np.array(rng.choice(greens)) * rng.uniform(0.9, 1.1)
        draw_blade(canvas, x0, sway, length, blade_width, color, rng)
    return finish_cutout(canvas.color, canvas.alpha, canvas.normal)


def make_stem_texture(rng, size=256):
    """去年の枯れた稈（麦わら色に灰色がかった筋）。画像の x が周、y が長さ。"""
    streak = tiling_noise(rng, size, 0.4, 0.01)
    color = np.array([0.62, 0.56, 0.42]) + np.array([0.05, 0.05, 0.04]) * np.clip(streak, -2, 2)[..., None]
    height = 0.05 * streak
    return to_bytes(np.clip(color, 0, 1)), to_bytes(height_to_normal(height, 2.0) * 0.5 + 0.5)


# --- 形状 ---------------------------------------------------------------------
CARD_WIDTH, CARD_LENGTH = 0.20, 1.2
STEM_TILE = 0.3
# LOD ごとの作り分け。
#   cards: 葉のカードの枚数の割合  segments: カードの長さ方向の分割数  stems: 枯れた稈を作るか
LODS = [
    {"cards": 1.0, "segments": 8, "stems": True, "stem_sides": 4},
    {"cards": 0.6, "segments": 5, "stems": True, "stem_sides": 3},
    {"cards": 0.35, "segments": 3, "stems": False, "stem_sides": 0},
]
NORMAL_UP = 0.7   # 葉の法線を上へ寄せる割合（細い葉の裏表で明暗が割れないように）


def add_blade_card(geo, base, yaw, rise, droop, length, width, segments, twist):
    """株元 base から方位 yaw へ、仰角 rise で立ち上がり、先ほど droop だけ垂れる曲がったカード。"""
    outward = Vector((math.cos(yaw), math.sin(yaw), 0.0))
    side = Vector((-math.sin(yaw), math.cos(yaw), 0.0))
    points, p = [base.copy()], base.copy()
    for i in range(segments):
        t = (i + 0.5) / segments
        pitch = rise - droop * t * t
        p = p + (outward * math.cos(pitch) + UP * math.sin(pitch)) * (length / segments)
        points.append(p.copy())
    first = len(geo.verts)
    for i, point in enumerate(points):
        t = i / segments
        # 葉の面は外を向き、先ほど少しねじれる。
        angle = twist * t
        across = (side * math.cos(angle) + UP * math.sin(angle) * 0.3).normalized()
        geo.verts.append(tuple(point - across * width / 2))
        geo.verts.append(tuple(point + across * width / 2))
    for i in range(segments):
        a, b = first + 2 * i, first + 2 * i + 1
        c, d = first + 2 * i + 3, first + 2 * i + 2
        v0, v1 = i / segments, (i + 1) / segments
        tangent = (points[i + 1] - points[i]).normalized()
        across = side
        face = across.cross(tangent).normalized()
        if face.z < 0:
            face = -face
        n = (face * (1 - NORMAL_UP) + UP * NORMAL_UP).normalized()
        geo.add_face([a, b, c, d], LEAVES, [(0, v0), (1, v0), (1, v1), (0, v1)], [n] * 4)
    geo.shoots.append((points[len(points) // 2], width * 2))


def build_clump(seed, lod):
    rng = random.Random(seed)
    geo = Geometry()
    base_radius = rng.uniform(0.15, 0.25)
    # 枯れた稈は骨格の乱数で先に決める（段で位置がずれないように）。
    stems = []
    # 株の中に隠れる程度（高いと株の上へ突き出して目立つ）。
    for _ in range(rng.randint(2, 4)):
        r = base_radius * 0.7 * math.sqrt(rng.random())
        theta = rng.uniform(0, 2 * math.pi)
        stems.append((Vector((r * math.cos(theta), r * math.sin(theta), -0.02)), theta,
                      rng.uniform(0.6, 1.0), rng.getrandbits(32)))
    for start, theta, height, stem_seed in stems:
        if not lod["stems"]:
            continue
        stem_rng = random.Random(stem_seed)
        lean = math.radians(stem_rng.uniform(3, 14))
        points, _ = grow(start, theta, height, lambda t, lean=lean: math.pi / 2 - lean, stem_rng,
                         wander=0.3, step=0.15)
        radii = [0.004 * (1 - 0.5 * i / (len(points) - 1)) + 0.001 for i in range(len(points))]
        add_tube_lod(geo, points, radii, lod["stem_sides"], 1, STEM_TILE)
    count = rng.randint(60, 76)
    cards = []
    for k in range(count):
        r = base_radius * math.sqrt(rng.random())
        theta = rng.uniform(0, 2 * math.pi)
        base = Vector((r * math.cos(theta), r * math.sin(theta), -0.03))
        yaw = theta + rng.uniform(-0.5, 0.5)
        # 内側の葉ほど立ち、外側ほど寝て大きく垂れる。
        inner = 1 - r / base_radius
        # 外側の葉は寝て大きく垂れ、噴水のような丸い株になる。
        rise = math.radians(rng.uniform(48, 72) + 16 * inner)
        droop = math.radians(rng.uniform(55, 100) * (1 - 0.35 * inner))
        length = CARD_LENGTH * rng.uniform(0.85, 1.15)
        twist = math.radians(rng.uniform(-40, 40))
        cards.append((rng.random(), base, yaw, rise, droop, length, twist))
    # 段で間引くときは乱数の小さい順に残す（段で残るカードが一致する）。
    cards.sort(key=lambda c: c[0])
    keep = max(6, int(round(len(cards) * lod["cards"])))
    for _, base, yaw, rise, droop, length, twist in cards[:keep]:
        width = CARD_WIDTH * (1.0 if lod["cards"] >= 1.0 else 1.0 / math.sqrt(lod["cards"]) * 0.85)
        add_blade_card(geo, base, yaw, rise, droop, length, width, lod["segments"], twist)
    return geo


# --- 芯 -------------------------------------------------------------------------
CORE_RADIUS_SCALE = 0.7
CORE_RESOLUTION = 0.05
CORE_INSET = 0.08
CORE_TRIANGLES = (600, 300, 150)
CORE_HEIGHT = 0.35   # 芯に使う房の高さの上限（株の高さに対する割合）。株元の暗がりだけにする（大きいと株の横に暗い塊がはみ出す）
CORE_COLOR = (0.020, 0.036, 0.010)


def main():
    options = parse_args()
    out, root = options["out"], options["root"]
    os.makedirs(out, exist_ok=True)
    rng = random.Random(options["seed"])
    np_rng = np.random.default_rng(options["seed"])

    paths = {key: os.path.join(out, f"T_Susuki_{key}.png")
             for key in ("Leaves_D", "Leaves_N", "Stem_D", "Stem_N")}
    leaves, leaves_normal = make_leaf_texture(rng)
    write_png(paths["Leaves_D"], leaves)
    write_png(paths["Leaves_N"], leaves_normal)
    print(f"leaf coverage: {float((leaves[..., 3] >= 128).mean()):.1%}")
    stem, stem_normal = make_stem_texture(np_rng)
    write_png(paths["Stem_D"], stem)
    write_png(paths["Stem_N"], stem_normal)

    bpy.ops.wm.read_factory_settings(use_empty=True)
    materials = [make_material("Stem", paths["Stem_D"], paths["Stem_N"], False),
                 make_material("Leaves", paths["Leaves_D"], paths["Leaves_N"], True),
                 make_core_material(CORE_COLOR)]

    stem_mat = os.path.join(out, "MI_Susuki_Stem.tgmat")
    leaf_mat = os.path.join(out, "MI_Susuki_Leaves.tgmat")
    core_mat = os.path.join(out, "MI_Susuki_Core.tgmat")
    write_material(stem_mat, "MI_Susuki_Stem", source_ref(paths["Stem_D"], root),
                   source_ref(paths["Stem_N"], root), 0.8, 0.0, False)
    # 艶は控えめ（艶があると空を映して白っぽく浮く）。
    write_material(leaf_mat, "MI_Susuki_Leaves", source_ref(paths["Leaves_D"], root),
                   source_ref(paths["Leaves_N"], root), 0.65, 0.5, True)
    write_material(core_mat, "MI_Susuki_Core", None, None, 0.9, 0.0, False, CORE_COLOR)

    for index in range(1, options["variants"] + 1):
        name = f"Susuki_Var{index}"
        objects = []
        seed = options["seed"] * 1000 + index
        shoots = build_clump(seed, LODS[0]).shoots
        top = max(center.z for center, _ in shoots)
        core_shoots = [(center, radius) for center, radius in shoots if center.z < top * CORE_HEIGHT]
        hull = build_hull(f"Core_{name}", core_shoots, CORE_RADIUS_SCALE, CORE_RESOLUTION)
        for level, lod in enumerate(LODS):
            geo = build_clump(seed, lod)
            add_core(geo, hull, CORE_TRIANGLES[level], CORE_INSET)
            objects.append(make_object(f"{name}_LOD{level}", geo, materials,
                                       ((index - 1) * 3.0, level * 3.0, 0)))
            cards = geo.face_mat.count(LEAVES)
            print(f"{name}_LOD{level}: {geo.triangles()} triangles, {cards} leaf quads")
        fbx = os.path.join(out, name + ".fbx")
        export_fbx(objects, fbx)
        # スロットの並びは FBX で最初に現れた順（稈が先）。LOD2 は稈が無いが、LOD0 で並びが決まる。
        write_model(os.path.join(out, name + ".tgmodel"), name, source_ref(fbx, root),
                    [asset_ref(stem_mat, root), asset_ref(leaf_mat, root), asset_ref(core_mat, root)])
        bpy.data.meshes.remove(hull)

    bpy.ops.wm.save_as_mainfile(filepath=os.path.join(out, "Susuki.blend"))


main()
