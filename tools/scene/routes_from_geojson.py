# GeoJSON（OSM の線。location-viewer などの書き出し）から、登山道の Path と自動車道路の Road Path を作る。
# 手順は docs/design/scene-authoring.md の「道」。
#
# 使い方（data/ の中でも外でもよい。シーンか地形を渡す）:
#   1. 位置の確認と中身の一覧（何も書き換えない）:
#        python tools/scene/routes_from_geojson.py check Scenes/albura/albura.tgscene Scenes/albura/routes.geojson
#      GeoJSON の標高とハイトマップの差の中央値が数 m なら位置は合っている（上下が逆だと数百 m ずれる）。
#   2. 登山道 → Path（既存の Path ノードの点とエッジを置き換える）:
#        python tools/scene/routes_from_geojson.py trails <シーン> <geojson> [--categories trail,foot]
#            [--road-names "Veja Alvra,Via d'Alvra"]   （道路の中心線も幅 9 m で入れる: 道沿いの土と植生の除外用）
#   3. 自動車道路 → Road Path → Road Mesh → Lane Marking → Shoulder → Mesh Output（作り直す）:
#        python tools/scene/routes_from_geojson.py road <シーン> <geojson> --names "Veja Alvra,Via d'Alvra" --start west
#      Road Path は分岐を持てないので、指定した線を端点で繋いだ 1 本の鎖にする（分岐ではまっすぐ続く方を選ぶ）。
#      --names の代わりに --osm id,id,... でも選べる。
#
# 座標: 画像の上端が北。Heightmap の位置（中心の緯度経度）と一辺から UV にする（tgscene.Geo）。
import argparse
import math
import os
import statistics
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from tgscene import Geo, asset_ref, find_root, load_elevation, load_json, load_terrain, sample, to_root_relative  # noqa: E402

JOIN = 3.0  # 端点をつなぐ距離（m）


# --- 線の読み込みと加工 ---------------------------------------------------------------
def read_features(root, path):
    return load_json(os.path.join(root, to_root_relative(root, path)))["features"]


def lines_of(feature, geo):
    g = feature["geometry"]
    parts = [g["coordinates"]] if g["type"] == "LineString" else g["coordinates"] if g["type"] == "MultiLineString" else []
    return [[geo.metres(c[0], c[1]) for c in part] for part in parts]


def clip_to_square(line, half):
    """地形の正方形の中に入る部分に分ける。枠をまたぐ線分は枠の上で切る。"""
    def inside(p):
        return abs(p[0]) <= half and abs(p[1]) <= half

    def cross(a, b):  # a が中、b が外（またはその逆）の線分の、枠との交点（二分探索）
        lo, hi, a_in = 0.0, 1.0, inside(a)
        for _ in range(40):
            m = (lo + hi) / 2
            q = (a[0] + (b[0] - a[0]) * m, a[1] + (b[1] - a[1]) * m)
            lo, hi = (m, hi) if inside(q) == a_in else (lo, m)
        t = lo if a_in else hi
        q = (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t)
        return (max(-half, min(half, q[0])), max(-half, min(half, q[1])))

    parts, cur = [], []
    for i, p in enumerate(line):
        if inside(p):
            if not cur and i > 0:
                cur.append(cross(p, line[i - 1]))
            cur.append(p)
        elif cur:
            cur.append(cross(line[i - 1], p))
            parts.append(cur)
            cur = []
    if cur:
        parts.append(cur)
    return [c for c in parts if len(c) >= 2]


def simplify(line, tolerance):
    """Douglas-Peucker。形を保って間引く（登山道は 4 m が目安）。"""
    if len(line) < 3:
        return line

    def dist(p, a, b):
        dx, dy = b[0] - a[0], b[1] - a[1]
        length = dx * dx + dy * dy
        t = 0 if length == 0 else max(0, min(1, ((p[0] - a[0]) * dx + (p[1] - a[1]) * dy) / length))
        return math.hypot(p[0] - a[0] - t * dx, p[1] - a[1] - t * dy)

    index, worst = max(((i, dist(line[i], line[0], line[-1])) for i in range(1, len(line) - 1)), key=lambda x: x[1])
    if worst <= tolerance:
        return [line[0], line[-1]]
    return simplify(line[:index + 1], tolerance)[:-1] + simplify(line[index:], tolerance)


def resample(line, spacing):
    """spacing（m）ごとに打ち直す（道路はカーブの設計が点の間隔に依るので、一定の間隔にする）。"""
    out, carry = [line[0]], 0.0
    for a, b in zip(line, line[1:]):
        seg = math.dist(a, b)
        t = spacing - carry
        while t < seg:
            out.append((a[0] + (b[0] - a[0]) * t / seg, a[1] + (b[1] - a[1]) * t / seg))
            t += spacing
        carry = (carry + seg) % spacing
    if len(out) > 1 and math.dist(out[-1], line[-1]) < spacing * 0.4:
        out[-1] = line[-1]
    else:
        out.append(line[-1])
    return out


def split_at_junctions(lines):
    """ほかの線と共有する点（分岐）で線を割り、同じ区間の重複を捨てる。"""
    key = lambda p: (round(p[0] * 10), round(p[1] * 10))  # 0.1 m 単位で同一視
    use = defaultdict(int)
    for line in lines:
        for i, p in enumerate(line):
            use[key(p)] += 1 if i in (0, len(line) - 1) else 2
    chains = []
    for line in lines:
        c = [line[0]]
        for p in line[1:]:
            c.append(p)
            if use[key(p)] != 2:
                chains.append(c)
                c = [p]
        if len(c) >= 2:
            chains.append(c)
    seen, unique = set(), []
    for c in chains:
        k = min(tuple(key(p) for p in c), tuple(key(p) for p in reversed(c)))
        if k in seen or len({key(p) for p in c}) < 2:
            continue
        seen.add(k)
        unique.append(c)
    return unique


def select(features, names=None, osm=None, categories=None):
    out = []
    for f in features:
        p = f["properties"]
        if names and p.get("name") not in names:
            continue
        if osm and p.get("osmId") not in osm:
            continue
        if categories and p.get("category") not in categories:
            continue
        out.append(f)
    return out


def chain_lines(lines, start):
    """線を端点で繋いで 1 本にする。start（west / east / north / south）の端から歩き、
    分岐ではまっすぐ続く方を選ぶ。使わなかった線の数も返す。"""
    pick = {"west": lambda p: p[0], "east": lambda p: -p[0], "south": lambda p: p[1], "north": lambda p: -p[1]}[start]
    remaining = [list(l) for l in lines if len(l) >= 2]
    first = min(remaining, key=lambda l: min(pick(l[0]), pick(l[-1])))
    remaining.remove(first)
    if pick(first[-1]) < pick(first[0]):
        first.reverse()
    road = first
    while True:
        end, before = road[-1], road[-2]
        heading = math.atan2(end[1] - before[1], end[0] - before[0])
        best = None
        for line in remaining:
            for candidate in (line, line[::-1]):
                if math.dist(candidate[0], end) <= JOIN:
                    turn = math.atan2(candidate[1][1] - candidate[0][1], candidate[1][0] - candidate[0][0]) - heading
                    turn = abs((turn + math.pi) % (2 * math.pi) - math.pi)
                    if best is None or turn < best[0]:
                        best = (turn, line, candidate)
        if best is None:
            return road, len(remaining)
        remaining.remove(best[1])
        road += best[2][1:]


# --- Path の点とエッジ ------------------------------------------------------------------
class PathBuilder:
    def __init__(self, geo):
        self.geo, self.points, self.edges, self.next_id, self.ids = geo, [], [], 1, {}

    def point(self, p, width, feather):
        k = (round(p[0] * 10), round(p[1] * 10))
        if k in self.ids:
            return self.ids[k]
        for (x, y), pid in self.ids.items():  # 近い端点は同じ点に
            if math.hypot(x / 10 - p[0], y / 10 - p[1]) <= JOIN:
                return pid
        u, v = self.geo.uv_of_metres(p)
        pid = self.next_id
        self.next_id += 1
        self.points.append({"feather": feather, "heightOffset": 0.0, "id": pid, "intensity": 1.0,
                            "u": u, "v": v, "width": width})
        self.ids[k] = pid
        return pid

    def add(self, line, width, feather, extra=None):
        ids = [self.point(p, width, feather) for p in line]
        pairs = {(e["from"], e["to"]) for e in self.edges}
        for a, b in zip(ids, ids[1:]):
            if a == b or (a, b) in pairs or (b, a) in pairs:
                continue
            edge = {"clothoidRatio": 0.5, "curve": "quadratic", "feather": feather, "from": a, "id": 0,
                    "intensity": 1.0, "overrideValues": True, "rounding": 0.5, "to": b, "width": width}
            edge.update(extra or {})
            self.edges.append(edge)

    def finish(self):
        for e in self.edges:
            e["id"] = self.next_id
            self.next_id += 1
        return {"points": self.points, "edges": self.edges, "nextId": self.next_id}


# --- サブコマンド -----------------------------------------------------------------------
def cmd_check(root, terrain, features, args):
    geo = Geo.of(terrain)
    geo.require_location()
    elevation = load_elevation(terrain)
    diffs = []
    for f in features:
        for c in (f["geometry"]["coordinates"] if f["geometry"]["type"] == "LineString" else []):
            if len(c) >= 3:
                u, v = geo.uv(c[0], c[1])
                if 0 <= u < 1 and 0 <= v < 1:
                    diffs.append(sample(elevation, u, v) - c[2])
    if diffs:
        a = sorted(abs(d) for d in diffs)
        print(f"標高の差（ハイトマップ − GeoJSON）: {len(diffs)} 点、中央値 {statistics.median(diffs):+.1f} m、"
              f"絶対値の中央値 {statistics.median(a):.1f} m、90% {a[int(len(a) * 0.9)]:.1f} m")
    else:
        print("標高付きの点が無いので、位置は確かめられない")
    by = defaultdict(lambda: [0, 0.0])
    for f in features:
        p = f["properties"]
        length = sum(math.dist(a, b) for line in lines_of(f, geo) for a, b in zip(line, line[1:]))
        by[(p.get("category"), p.get("name"))][0] += 1
        by[(p.get("category"), p.get("name"))][1] += length
    print("種類・名前ごとの本数と長さ（範囲外も含む）:")
    for (category, name), (count, length) in sorted(by.items(), key=lambda x: -x[1][1]):
        print(f"  {category:8s} {name!s:32s} {count:4d} 本 {length / 1000:6.2f} km")


def cmd_trails(root, terrain, features, args):
    geo = Geo.of(terrain)
    half = geo.size / 2 - 1.0
    lines = []
    for f in select(features, categories=args.categories.split(",")):
        for line in lines_of(f, geo):
            lines += clip_to_square(line, half)
    chains = [simplify(c, args.tolerance) for c in split_at_junctions(lines)]
    builder = PathBuilder(geo)
    for c in chains:
        builder.add(c, args.width, args.feather)
    if args.road_names:
        road, _ = chain_lines([l for f in select(features, names=args.road_names.split(","))
                               for line in lines_of(f, geo) for l in clip_to_square(line, half)], "west")
        builder.add(simplify(road, 2.0), args.road_width, args.feather)
    path = builder.finish()
    nodes = terrain.by_kind("path")
    if len(nodes) != 1:
        raise SystemExit(f"Path ノードが {len(nodes)} 個あります（1 個のときだけ置き換える）")
    nodes[0]["path"].update(path)
    nodes[0]["path"].update({"defaultWidth": args.width, "defaultFeather": args.feather})
    terrain.save()
    print(f"Path: 線 {len(lines)} 本 → 区間 {len(chains)}、点 {len(path['points'])} / エッジ {len(path['edges'])}")


def cmd_road(root, terrain, features, args):
    geo = Geo.of(terrain)
    half = geo.size / 2 - 2.0
    chosen = select(features, names=args.names.split(",") if args.names else None,
                    osm={int(x) for x in args.osm.split(",")} if args.osm else None)
    if not chosen:
        raise SystemExit("道路の線が選ばれていません（--names か --osm）")
    lines = [l for f in chosen for line in lines_of(f, geo) for l in clip_to_square(line, half)]
    road, unused = chain_lines(lines, args.start)
    road = resample(road, args.spacing)
    turns = [abs((math.atan2(c[1] - b[1], c[0] - b[0]) - math.atan2(b[1] - a[1], b[0] - a[0]) + math.pi)
                 % (2 * math.pi) - math.pi) for a, b, c in zip(road, road[1:], road[2:])]
    length = sum(math.dist(a, b) for a, b in zip(road, road[1:]))
    print(f"道路: {len(road)} 点、{length:.0f} m、{args.spacing:.0f} m で最大 {math.degrees(max(turns)):.0f}° 曲がる"
          f"（繋がらずに残った線 {unused} 本）")

    # 既存の道路の鎖は作り直す
    old = [n["id"] for n in terrain.graph["nodes"]
           if n["kind"] in ("roadPath", "roadMesh", "laneMarking", "shoulder", "meshOutput")]
    terrain.remove_nodes(old)
    path_nodes = terrain.by_kind("path")
    anchor = path_nodes[0] if path_nodes else terrain.heightmap_node()
    base, base_pin = terrain.source_of(anchor["id"], 0) if path_nodes else (anchor, 0)
    x0, y0 = anchor["position"]

    points, edges, rid = [], [], 1
    for p in road:
        u, v = geo.uv_of_metres(p)
        points.append({"feather": 0.0, "heightOffset": 0.0, "id": rid, "intensity": 1.0, "u": u, "v": v,
                       "width": args.width})
        rid += 1
    for a, b in zip(points, points[1:]):
        edges.append({"clothoidRatio": 0.5, "curve": "quadratic", "from": a["id"], "id": rid, "rounding": 1.0,
                      "to": b["id"]})
        rid += 1
    road_mat = terrain.add_asset("materials", args.road_material)
    shoulder_mat = terrain.add_asset("materials", args.shoulder_material)
    line_mat = terrain.add_asset("materials", args.line_material)
    forward, backward = map(int, args.lanes.split(","))

    def at(dx):
        return [x0 + dx, y0 + 420.0]

    road_path = terrain.add_node("roadPath", 2, 1, at(0), {
        "path": {"defaultFeather": 0.0, "defaultIntensity": 1.0, "defaultWidth": args.width, "edges": edges,
                 "nextId": rid, "points": points},
        "roadProfile": {"bankEnabled": True, "bankPoints": [], "bankSmoothDistance": 20.0,
                        "designSpeed": args.design_speed, "friction": 0.15, "smoothBank": False,
                        "verticalPoints": []}},
        args.note or "自動車道路（GeoJSON から 1 本の鎖にした。分岐は入れない）。")
    road_mesh = terrain.add_node("roadMesh", 1, 1, at(320), {"roadMesh": {
        "width": args.width, "lanesForward": forward, "lanesBackward": backward, "surfaceOffset": 0.3,
        "uvRepeat": 4.0, "material": road_mat}})
    marking = terrain.add_node("laneMarking", 1, 1, at(640), {"laneMarking": {
        "center": {"enabled": "center" in args.markings, "dashed": True, "width": 0.15, "material": line_mat},
        "edge": {"enabled": "edge" in args.markings, "dashed": False, "width": 0.12, "material": line_mat},
        "lane": {"enabled": "lane" in args.markings, "dashed": True, "width": 0.15, "material": line_mat},
        "edgeInset": 0.3, "dashLength": 3.0, "dashGap": 6.0, "lift": 0.005, "uvRepeat": 1.2, "uvAlongU": True}})
    shoulder = terrain.add_node("shoulder", 1, 1, at(960), {"shoulder": {
        "side": "both", "width": args.shoulder_width, "crossSlope": 4.0, "stepHeight": 0.0, "stepWidth": 0.05,
        "uvRepeat": 2.0, "material": shoulder_mat, "shape": "slope"}})
    if args.boundary:
        shoulder["shoulder"]["boundary"] = asset_ref(root, args.boundary)
    output = terrain.add_node("meshOutput", 1, 0, at(1280))
    terrain.link(base["id"], road_path["id"], 0, base_pin)
    terrain.link(road_path["id"], road_mesh["id"])
    terrain.link(road_mesh["id"], marking["id"])
    terrain.link(marking["id"], shoulder["id"])
    terrain.link(shoulder["id"], output["id"])
    terrain.save()
    print(f"Road Path {road_path['id']} → Road Mesh {road_mesh['id']} → Lane Marking {marking['id']} → "
          f"Shoulder {shoulder['id']} → Mesh Output {output['id']}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=("check", "trails", "road"))
    parser.add_argument("target", help="シーン（.tgscene）か地形（.tgterrain）")
    parser.add_argument("geojson")
    parser.add_argument("--root")
    parser.add_argument("--categories", default="trail,foot", help="trails: 使う category")
    parser.add_argument("--tolerance", type=float, default=4.0, help="trails: 間引きの許容差（m）")
    parser.add_argument("--width", type=float, default=None, help="trails: 2 m、road: 6 m")
    parser.add_argument("--feather", type=float, default=4.0)
    parser.add_argument("--road-names", help="trails: 道路の中心線も入れる（名前をカンマ区切り）")
    parser.add_argument("--road-width", type=float, default=9.0, help="trails: 道路の中心線の幅（路面 + 路肩 + 余白）")
    parser.add_argument("--names", help="road: 使う線の名前（カンマ区切り）")
    parser.add_argument("--osm", help="road: 使う線の osmId（カンマ区切り）")
    parser.add_argument("--start", default="west", choices=("west", "east", "north", "south"))
    parser.add_argument("--spacing", type=float, default=12.0, help="road: 点の間隔（m）")
    parser.add_argument("--lanes", default="1,1", help="road: 車線数（前,後）")
    parser.add_argument("--markings", default="center", help="road: 引く線（center,edge,lane）")
    parser.add_argument("--shoulder-width", type=float, default=1.0)
    parser.add_argument("--design-speed", type=float, default=40.0)
    parser.add_argument("--road-material", default="LayerMaterials/道路材質.tglayer")
    parser.add_argument("--shoulder-material", default="LayerMaterials/路肩（石と泥）.tglayer")
    parser.add_argument("--line-material", default="Materials/WhiteLine.tgmat")
    parser.add_argument("--boundary", default="BoundaryMaterials/asphalt-boundary.tgboundary",
                        help="road: 路面と路肩の境目の境界マテリアル（空文字で付けない）")
    parser.add_argument("--note")
    args = parser.parse_args()
    if args.width is None:
        args.width = 6.0 if args.mode == "road" else 2.0
    root = args.root or find_root()
    terrain = load_terrain(root, args.target)
    features = read_features(root, args.geojson)
    {"check": cmd_check, "trails": cmd_trails, "road": cmd_road}[args.mode](root, terrain, features, args)


main()
