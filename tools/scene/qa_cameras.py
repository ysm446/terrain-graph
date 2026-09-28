# 確認用カメラのシーンを作る。元のシーンの複製で、カメラ（オービット）だけを変える。
# 部品（地形・雲・空）は元のシーンのものを参照するので、元を直せばそのまま撮り直せる。
# 手順は docs/design/scene-authoring.md。
#
# 使い方（data/ の中でも外でもよい）:
#   python tools/scene/qa_cameras.py Scenes/albura/albura.tgscene --out Test/albura-qa \
#       far=0.5,0.55,9000,0,22  pass=@46.5807,9.8375,1800,270,20  top=0.5,0.5,11000,0,85
#
# 各カメラは 名前=注視点,距離(m),見る方位(度),俯角(度)。
#   注視点は u,v（画像の割合。左上が 0,0、上が北）か、@緯度,経度（Heightmap に位置があるとき）。
#   見る方位は 0 = 北を見る、90 = 東を見る（カメラは注視点の反対側に立つ）。俯角は 0 が水平、90 が真下。
#   注視点の高さは地形の標高から自動で決める。
# 目が地形に埋まる（東向きの斜面の手前など）と、真っ暗や裏返しの絵になる。埋まるときは警告を出す。
# 雲の動き（animateClouds）は止める（撮り直しで絵を比べられるように）。--keep-clouds で元のまま。
import argparse
import copy
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from tgscene import Geo, Scene, find_root, load_elevation, sample, save_json, to_root_relative  # noqa: E402


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("scene", help="元のシーン（.tgscene）")
    parser.add_argument("cameras", nargs="+", help="名前=注視点,距離,見る方位,俯角")
    parser.add_argument("--out", required=True, help="確認用シーンを書くフォルダ（ルート相対。例: Test/albura-qa）")
    parser.add_argument("--root")
    parser.add_argument("--keep-clouds", action="store_true")
    args = parser.parse_args()
    root = args.root or find_root()
    scene = Scene(root, to_root_relative(root, args.scene))
    terrain = scene.terrain()
    geo = Geo.of(terrain)
    elevation = load_elevation(terrain)
    out_dir = os.path.join(root, args.out)
    os.makedirs(out_dir, exist_ok=True)

    for spec in args.cameras:
        name, values = spec.split("=", 1)
        parts = values.split(",")
        if parts[0].startswith("@"):
            lat, lon = float(parts[0][1:]), float(parts[1])
            u, v = geo.uv(lon, lat)
        else:
            u, v = float(parts[0]), float(parts[1])
        distance, look, pitch = map(float, parts[2:5])
        ground = sample(elevation, u, v)
        x, z = geo.world_xz(u, v)
        yaw = math.radians(-look)  # 見る方位 → 目の向き（目は注視点の反対側）
        p = math.radians(pitch)
        data = copy.deepcopy(scene.data)
        camera = data["preview"]["camera"]
        camera.update({"target": [x, geo.world_y(ground), z], "distance": distance, "yaw": yaw, "pitch": p})
        if not args.keep_clouds:
            data["preview"]["atmosphere"]["animateClouds"] = False
        save_json(os.path.join(out_dir, f"{name}.tgscene"), data)

        # 目の位置と、その下の地形
        eye_x = x + distance * math.cos(p) * math.sin(yaw)
        eye_z = z + distance * math.cos(p) * math.cos(yaw)
        eye_elevation = ground + distance * math.sin(p)
        eu, ev = eye_x / geo.size + 0.5, eye_z / geo.size + 0.5
        note = ""
        if 0 <= eu <= 1 and 0 <= ev <= 1:
            below = sample(elevation, eu, ev)
            if eye_elevation < below + 5:
                note = f"  ※ 目（{eye_elevation:.0f} m）が地形（{below:.0f} m）に埋まる。俯角を上げるか方位を変える"
        print(f"{name}: 注視点 {ground:.0f} m（u {u:.3f}, v {v:.3f}）、{look:.0f}° を見る、目 {eye_elevation:.0f} m{note}")


main()
