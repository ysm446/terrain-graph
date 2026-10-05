# 地形グラフ（.tgterrain）を、ノードごとに「種類・名前・入力の出どころ・メモ・主な設定」の 1〜2 行で書き出す。
# 他のシーンのグラフを読み解くときと、自分の変更を確かめるときに使う。手順は docs/design/scene-authoring.md。
#
# 使い方（data/ の中でも外でもよい）:
#   python tools/scene/dump_graph.py Scenes/albura/albura_terrain.tgterrain [--grep 文字列] [--kind maskHeight]
#   python tools/scene/dump_graph.py Scenes/albura/albura.tgscene      （シーンを渡すと地形の部品を読む）
#
# 入力は「ノード ID.出力の番号」で表す（例: in(47.0,537.0) は 47 の出力 0 と 537 の出力 0）。
# 標高のマスク（maskHeight）には、実際の標高に直した値も併記する（地形の底 = Heightmap の最低標高）。
import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from tgscene import Geo, find_root, load_terrain  # noqa: E402

SETTINGS_KEY = {
    "maskHeight": "height", "maskSlope": "slope", "maskLevels": "levels", "maskBlend": "blend",
    "maskNoise": "noise", "maskCurvature": "curvature", "maskBlur": "blur", "maskFluvial": "fluvial",
    "maskArea": "maskArea", "maskPath": "maskPath", "maskFlowline": "flowline", "maskWind": "wind",
    "maskMap": "map", "modelScatter": "modelScatter", "modelPlace": "modelPlace", "maskMesh": "maskMesh",
    "roadMesh": "roadMesh", "shoulder": "shoulder",
    "laneMarking": "laneMarking",
}


def describe(node, geo):
    kind = node["kind"]
    if kind in SETTINGS_KEY and SETTINGS_KEY[kind] in node:
        value = node[SETTINGS_KEY[kind]]
        text = json.dumps(value, ensure_ascii=False)
        if kind == "maskHeight" and not value.get("fullRange"):
            text += f"  → 標高 {geo.base + value['min']:.0f}〜{geo.base + value['max']:.0f} m（ぼかし {value['feather']:.0f} m）"
        return text
    if kind in ("path", "roadPath"):
        path = node["path"]
        return f"点 {len(path['points'])} / エッジ {len(path['edges'])}"
    layer = node.get("layer")
    if not layer:
        return ""
    lk = layer["kind"]
    if lk == "surface":
        mask = layer.get("mask", {})
        return f"material={layer.get('material')} 定数マスク={mask.get('constant')} source={mask.get('source')}"
    if lk == "shape":
        return json.dumps(node.get("scale"), ensure_ascii=False)
    if lk in layer:
        return json.dumps(layer[lk], ensure_ascii=False)
    return ""


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("path", help="地形（.tgterrain）かシーン（.tgscene）のルート相対パス")
    parser.add_argument("--root")
    parser.add_argument("--grep", help="メモ・種類・設定にこの文字列を含むノードだけ")
    parser.add_argument("--kind", help="この種類のノードだけ（例: maskHeight）")
    parser.add_argument("--width", type=int, default=600, help="設定の行を切る長さ")
    args = parser.parse_args()
    root = args.root or find_root()
    terrain = load_terrain(root, args.path)
    geo = Geo.of(terrain)

    print(f"# {terrain.rel_path}  （{terrain.data.get('name')}）")
    print(f"# 一辺 {geo.size:.0f} m、標高 {geo.base:.0f}〜{geo.base + geo.height:.0f} m、"
          f"位置 {geo.lat0}, {geo.lon0}")
    print("# materials: " + ", ".join(f"{m['id']}={m['asset']['path']}" for m in terrain.data.get("materials", [])))
    print("# models: " + ", ".join(f"{m['id']}={m['asset']['path']}" for m in terrain.data.get("models", [])))
    for node in terrain.graph["nodes"]:
        if args.kind and node["kind"] != args.kind:
            continue
        inputs = []
        for i in range(len(node["inputs"])):
            src, out = terrain.source_of(node["id"], i)
            inputs.append(f"{src['id']}.{out}" if src else "-")
        name = node.get("layer", {}).get("name", "")
        head = f"#{node['id']} {node['kind']}" + (f" [{name}]" if name else "") + \
            f" in({','.join(inputs)}) | {node.get('note', '')}"
        detail = describe(node, geo)
        if args.grep and args.grep not in head + detail:
            continue
        print(head)
        if detail:
            print("     " + detail[:args.width])


main()
