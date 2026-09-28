# シーン作りの補助スクリプトの共通部品（tools/scene/ の各スクリプトから使う）。
#
# 手順は docs/design/scene-authoring.md。ここには次を置く:
#   - data/ のルートの見つけ方と、JSON アセットの読み書き（UTF-8、改行 LF、インデント 2）
#   - 地形グラフ（.tgterrain）のノード・ピン・リンクの扱い（ID の払い出し、繋ぎ替え、メモでの検索）
#   - Heightmap ノードの実寸と位置（緯度経度 ⇔ 地形の UV / m）、ハイトマップ画像の標高
#
# ID について: ノード・ピン・リンクの ID は 1 つの番号の空間を共有する。新しく足すときは
# `Terrain.new_id()` で既存の最大より大きい番号を払い出す。アプリで保存すると振り直されることが
# あるので、スクリプトは ID を決め打ちせず、ノードのメモ（note）や種類で探す。
import json
import math
import os
import sys

# Windows のコンソール（cp932）でも日本語とウムラウトを出せるように。
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")


# --- ルートと JSON ---------------------------------------------------------------
def find_root(start=None):
    """start（既定はカレント）の祖先から data/ を探す。data/ の中で動かしても外で動かしてもよい。"""
    path = os.path.abspath(start or os.getcwd())
    probe = path
    while True:
        if os.path.basename(probe).lower() == "data" and os.path.isfile(os.path.join(probe, "project.tgproj")):
            return probe
        candidate = os.path.join(probe, "data")
        if os.path.isfile(os.path.join(candidate, "project.tgproj")):
            return candidate
        parent = os.path.dirname(probe)
        if parent == probe:
            raise SystemExit("data/（project.tgproj のあるフォルダ）が見つかりません。--root で指定してください")
        probe = parent


def to_root_relative(root, path):
    """カレントからのパスでも、ルート相対のパスでも受け、ルート相対（/ 区切り）で返す。"""
    if os.path.exists(os.path.join(root, path)):
        return path.replace("\\", "/")
    full = os.path.abspath(path)
    if not os.path.exists(full):
        raise SystemExit(f"見つかりません: {path}")
    return os.path.relpath(full, root).replace("\\", "/")


def load_terrain(root, path):
    """地形（.tgterrain）か、シーン（.tgscene）の地形の部品を読む。"""
    rel = to_root_relative(root, path)
    return Scene(root, rel).terrain() if rel.endswith(".tgscene") else Terrain(root, rel)


def load_json(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def save_json(path, data):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(data, f, ensure_ascii=False, indent=2)
        f.write("\n")


def asset_ref(root, rel_path):
    """アセット（.tgmat / .tgmodel / .tglayer など）の参照（ルート相対パスと UID）。"""
    return {"path": rel_path.replace("\\", "/"), "uid": load_json(os.path.join(root, rel_path))["uid"]}


# --- シーン ---------------------------------------------------------------------
class Scene:
    def __init__(self, root, rel_path):
        self.root, self.rel_path = root, rel_path.replace("\\", "/")
        self.path = os.path.join(root, self.rel_path)
        self.data = load_json(self.path)

    def component_path(self, role):
        for c in self.data.get("components", []):
            if c.get("role") == role:
                return c["asset"]["path"]
        return None

    def terrain(self):
        return Terrain(self.root, self.component_path("terrain"))

    def save(self, path=None):
        save_json(path or self.path, self.data)


# --- 地形グラフ -------------------------------------------------------------------
class Terrain:
    def __init__(self, root, rel_path):
        self.root, self.rel_path = root, rel_path.replace("\\", "/")
        self.path = os.path.join(root, self.rel_path)
        self.data = load_json(self.path)

    # ノード
    @property
    def graph(self):
        return self.data["graph"]

    def nodes(self):
        return {n["id"]: n for n in self.graph["nodes"]}

    def node(self, node_id):
        return self.nodes()[node_id]

    def by_kind(self, kind):
        return [n for n in self.graph["nodes"] if n["kind"] == kind]

    def by_note(self, text):
        """メモに text を含むノード。"""
        return [n for n in self.graph["nodes"] if text in (n.get("note") or "")]

    def heightmap_node(self):
        """本流（Output から入力 0 をたどった先）の Heightmap。使われていない Heightmap が残っていることがある。"""
        nodes = self.by_kind("heightmap")
        if not nodes:
            raise SystemExit(f"{self.rel_path} に Heightmap ノードがありません")
        outputs = self.by_kind("output")
        current, seen = (outputs[0] if outputs else None), set()
        while current is not None and current["id"] not in seen and current["inputs"]:
            seen.add(current["id"])
            current, _ = self.source_of(current["id"], 0)
            if current is not None and current["kind"] == "heightmap":
                return current
        return nodes[0]

    # ID とリンク
    def new_id(self):
        g = self.graph
        if not hasattr(self, "_next"):
            ids = [n["id"] for n in g["nodes"]]
            ids += [p for n in g["nodes"] for p in n["inputs"] + n["outputs"]]
            ids += [l["id"] for l in g["links"]]
            self._next = max(ids) + 1
        self._next += 1
        return self._next - 1

    def owner_of_pin(self, pin):
        for n in self.graph["nodes"]:
            if pin in n["outputs"]:
                return n, "out", n["outputs"].index(pin)
            if pin in n["inputs"]:
                return n, "in", n["inputs"].index(pin)
        return None, None, None

    def source_of(self, node_id, index):
        """入力 index に繋がる（ノード, 出力の番号）。未接続なら (None, None)。"""
        pin = self.node(node_id)["inputs"][index]
        for l in self.graph["links"]:
            if l["end"] == pin:
                n, _, i = self.owner_of_pin(l["start"])
                return n, i
        return None, None

    def link(self, producer_id, consumer_id, in_index=0, out_index=0):
        """producer の出力 → consumer の入力。入力に既に繋がっていれば付け替える。"""
        start = self.node(producer_id)["outputs"][out_index]
        end = self.node(consumer_id)["inputs"][in_index]
        for l in self.graph["links"]:
            if l["end"] == end:
                l["start"] = start
                return l
        link = {"id": self.new_id(), "start": start, "end": end}
        self.graph["links"].append(link)
        return link

    def unlink(self, consumer_id, in_index):
        end = self.node(consumer_id)["inputs"][in_index]
        self.graph["links"] = [l for l in self.graph["links"] if l["end"] != end]

    def add_node(self, kind, n_inputs, n_outputs, position, extra=None, note=None):
        """ピンを払い出してノードを足す。extra はノードの設定（種類ごとのキー）。"""
        node = {"id": self.new_id(), "inputs": [self.new_id() for _ in range(n_inputs)], "kind": kind,
                "outputs": [self.new_id() for _ in range(n_outputs)], "position": list(position)}
        node.update(extra or {})
        if note:
            node["note"] = note
        self.graph["nodes"].append(node)
        return node

    def remove_nodes(self, node_ids):
        ids = set(node_ids)
        pins = {p for n in self.graph["nodes"] if n["id"] in ids for p in n["inputs"] + n["outputs"]}
        self.graph["nodes"] = [n for n in self.graph["nodes"] if n["id"] not in ids]
        self.graph["links"] = [l for l in self.graph["links"] if l["start"] not in pins and l["end"] not in pins]

    # アセットの並び（materials / models は {id, asset}）
    def add_asset(self, key, rel_path):
        """materials / models に rel_path を足して id を返す（既にあればその id）。"""
        entries = self.data.setdefault(key, [])
        for e in entries:
            if e["asset"]["path"] == rel_path:
                return e["id"]
        new = max([e["id"] for e in entries], default=0) + 1
        entries.append({"asset": asset_ref(self.root, rel_path), "id": new})
        return new

    def set_asset(self, key, entry_id, rel_path):
        for e in self.data[key]:
            if e["id"] == entry_id:
                e["asset"] = asset_ref(self.root, rel_path)
                return
        raise KeyError(f"{key} に id {entry_id} がありません")

    def save(self, path=None):
        save_json(path or self.path, self.data)

    def loaded_id(self, node_id):
        """シーン（.tgscene）として開いたときのノード ID（`--select-node` に渡す値）。
        シーンの部品を読むと、ノード ID・ピン ID が地形の部品の並び順に 1 から振り直される
        （`SceneComponents.cpp`。ノード、その入力、出力の順）。地形は先頭の部品なので、ここから数えられる。"""
        next_id, ids = 1, {}
        for n in self.graph["nodes"]:
            for old in [n["id"]] + n["inputs"] + n["outputs"]:
                if old not in ids:
                    ids[old] = next_id
                    next_id += 1
        return ids[node_id]


# --- 実寸と位置 -------------------------------------------------------------------
class Geo:
    """Heightmap の実寸（scale）から、緯度経度 ⇔ 地形の m / UV を変換する。
    画像の上端（v = 0）が北、右端（u = 1）が東。m は中心を原点に東が +x、北が +y。"""

    def __init__(self, scale):
        self.size = float(scale["size"])
        self.height = float(scale["height"])
        self.base = float(scale.get("baseElevation", 0.0))
        location = scale.get("location")
        self.lat0 = location["latitude"] if location else None
        self.lon0 = location["longitude"] if location else None

    @staticmethod
    def of(terrain):
        return Geo(terrain.heightmap_node()["scale"])

    def require_location(self):
        if self.lat0 is None:
            raise SystemExit("Heightmap に位置（scale.location の緯度・経度）がありません。ユーザーに中心の緯度経度を聞く")

    def metres(self, lon, lat):
        self.require_location()
        return ((lon - self.lon0) * 111320.0 * math.cos(math.radians(self.lat0)), (lat - self.lat0) * 110574.0)

    def uv_of_metres(self, p):
        return p[0] / self.size + 0.5, 0.5 - p[1] / self.size

    def uv(self, lon, lat):
        return self.uv_of_metres(self.metres(lon, lat))

    def relative(self, elevation):
        """実際の標高 → Mask Height の値（地形の底からの高さ）。"""
        return elevation - self.base

    def snow_line(self, elevation):
        """実際の標高 → Snow Cover の降雪線（Height 0.5 の基準面が 0 m）。"""
        return elevation - (self.base + self.height * 0.5)

    def world_y(self, elevation):
        """実際の標高 → ワールドの y（Height 0.5 の基準面が 0。カメラの注視点に使う）。"""
        return elevation - (self.base + self.height * 0.5)

    def world_xz(self, u, v):
        return (u - 0.5) * self.size, (v - 0.5) * self.size


def resolve_source(root, source):
    """元ファイルの参照（path と uid）をルート相対パスにする。パスが古い（ファイルを動かした）ときは、
    アプリと同じく uid で .meta を探す。"""
    if os.path.exists(os.path.join(root, source["path"])):
        return source["path"]
    uid = source.get("uid")
    for folder, _, files in os.walk(root):
        for name in files:
            if name.endswith(".meta"):
                try:
                    if load_json(os.path.join(folder, name)).get("uid") == uid:
                        return os.path.relpath(os.path.join(folder, name[:-5]), root).replace("\\", "/")
                except (OSError, ValueError):
                    continue
    raise SystemExit(f"見つかりません: {source['path']}（uid {uid}）")


def heightmap_image_path(terrain):
    """Heightmap ノードが読む画像のルート相対パス。"""
    hm = terrain.heightmap_node()
    texture_id = hm["layer"]["height"]["texture"]["texture"]
    for t in terrain.data.get("textures", []):
        if t["id"] == texture_id:
            return resolve_source(terrain.root, t["source"])
    raise SystemExit("Heightmap の画像が textures にありません")


def load_elevation(terrain):
    """ハイトマップ画像を実際の標高（m）の配列で返す（行 0 が北）。16 bit / 8 bit のグレースケール PNG を読む。"""
    import numpy as np
    from PIL import Image
    geo = Geo.of(terrain)
    image = Image.open(os.path.join(terrain.root, heightmap_image_path(terrain)))
    a = np.array(image).astype(np.float64)
    if a.ndim == 3:
        a = a[..., 0]
    full = 65535.0 if image.mode.startswith("I") or a.max() > 255 else 255.0
    return a / full * geo.height + geo.base


def sample(elevation, u, v):
    n_rows, n_cols = elevation.shape
    return elevation[min(max(int(v * n_rows), 0), n_rows - 1), min(max(int(u * n_cols), 0), n_cols - 1)]
