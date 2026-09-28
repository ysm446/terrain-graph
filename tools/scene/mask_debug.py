# 結果がおかしいときの切り分け用に、確認用の地形とシーンの複製を作る。元のファイルは変えない。
# 手順は docs/design/scene-authoring.md の「切り分け」。
#
# 使い方（data/ の中でも外でもよい）:
#   マスクを見る（そのマスクで、最後に赤く塗る層を足す。赤い所がマスク = 1）:
#     python tools/scene/mask_debug.py paint Test/albura-qa/top.tgscene 537 499 88.1 --out Test/albura-qa
#       → Test/albura-qa/dbg_paint_537.tgscene など。88.1 は「ノード 88 の出力 1」
#   繋ぎを 1 本外す（例: Model Merge 261 の入力 3 を外すと、その樹種が消える）:
#     python tools/scene/mask_debug.py drop Test/albura-qa/low.tgscene 261:3 261:0 --out Test/albura-qa
#       → Test/albura-qa/dbg_drop_261_3.tgscene など
# できたシーンは tools/scene/shoot.py で撮る。見終わったら dbg_* を消す。
import argparse
import copy
import os
import sys
import uuid

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from tgscene import Scene, find_root, load_json, save_json, to_root_relative  # noqa: E402

DEBUG_MATERIAL = "MI_Debug_Mask.tgmat"


def new_uid():
    return "{" + str(uuid.uuid4()).upper() + "}"


def debug_material(root, out_rel):
    """赤く塗るマテリアル（雪のマテリアルに赤の色味を掛けた複製）を out に用意する。"""
    rel = f"{out_rel}/{DEBUG_MATERIAL}"
    path = os.path.join(root, rel)
    if not os.path.exists(path):
        data = load_json(os.path.join(root, "Materials", "MI_Snow.tgmat"))
        data.update({"name": "MI_Debug_Mask", "uid": new_uid(), "baseColorTint": [1.0, 0.05, 0.05]})
        save_json(path, data)
    return rel


def write_variant(root, scene, terrain, out_rel, tag):
    terrain.data["uid"] = new_uid()
    terrain.data["name"] = f"debug {tag}"
    terrain_rel = f"{out_rel}/dbg_{tag}.tgterrain"
    save_json(os.path.join(root, terrain_rel), terrain.data)
    data = copy.deepcopy(scene.data)
    for c in data["components"]:
        if c["role"] == "terrain":
            c["asset"] = {"path": terrain_rel, "uid": terrain.data["uid"]}
    data["sceneUid"] = new_uid()
    scene_rel = f"{out_rel}/dbg_{tag}.tgscene"
    save_json(os.path.join(root, scene_rel), data)
    print(scene_rel)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=("paint", "drop"))
    parser.add_argument("scene", help="元のシーン（確認用カメラのシーンを渡すと、その構図で見られる）")
    parser.add_argument("targets", nargs="+", help="paint: ノード[.出力]、drop: ノード:入力")
    parser.add_argument("--out", required=True, help="書き出すフォルダ（ルート相対）")
    parser.add_argument("--root")
    args = parser.parse_args()
    root = args.root or find_root()
    scene = Scene(root, to_root_relative(root, args.scene))
    os.makedirs(os.path.join(root, args.out), exist_ok=True)

    for target in args.targets:
        terrain = scene.terrain()
        if args.mode == "drop":
            node_id, index = map(int, target.split(":"))
            terrain.unlink(node_id, index)
            write_variant(root, scene, terrain, args.out, f"drop_{node_id}_{index}")
            continue
        node_id, _, pin = target.partition(".")
        node_id, pin = int(node_id), int(pin or 0)
        material = terrain.add_asset("materials", debug_material(root, args.out))
        template = next(n for n in terrain.graph["nodes"] if n.get("layer", {}).get("kind") == "surface")
        output = terrain.by_kind("output")[0]
        source, source_pin = terrain.source_of(output["id"], 0)
        extra = {k: copy.deepcopy(v) for k, v in template.items()
                 if k not in ("id", "inputs", "outputs", "kind", "position", "note")}
        extra["layer"]["material"] = material
        extra["layer"]["name"] = "Debug"
        painter = terrain.add_node("surface", len(template["inputs"]), 1,
                                   [output["position"][0] - 300, output["position"][1] + 200], extra, "debug")
        terrain.link(source["id"], painter["id"], 0, source_pin)
        terrain.link(node_id, painter["id"], 1, pin)
        terrain.link(painter["id"], output["id"], 0)
        write_variant(root, scene, terrain, args.out, f"paint_{node_id}" + (f"_{pin}" if pin else ""))


main()
