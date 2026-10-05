# 千畳敷モデルを木曽駒ヶ岳シーンへ配置する。事前にシーンをバックアップしてアプリを閉じる。
# Model Place で 1 棟を置き、その敷地（Pad）を Grading と Mask Mesh へ渡す。
#   Model Place ─ Instances → Model Output
#              └ Pad ──────→ Grading（敷地を均す）/ Mask Mesh（植生を除く）
# 旧構成（Scatter の固定シードで 1 点を選ぶ）のノードが残っていれば、同じ位置のまま置き換える。
import argparse
import math
from pathlib import Path

from tgscene import Geo, Scene, find_root, load_json, save_json

NOTE = '千畳敷駅:'


def convex_hull(points):
    ps = sorted(set(points))

    def cross(o, a, b):
        return (a[0]-o[0])*(b[1]-o[1])-(a[1]-o[1])*(b[0]-o[0])
    lower, upper = [], []
    for p in ps:
        while len(lower) >= 2 and cross(lower[-2], lower[-1], p) <= 0:
            lower.pop()
        lower.append(p)
    for p in reversed(ps):
        while len(upper) >= 2 and cross(upper[-2], upper[-1], p) <= 0:
            upper.pop()
        upper.append(p)
    return lower[:-1]+upper[:-1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--scene', default='Scenes/kiso-komagatake/kiso-komagatake.tgscene')
    parser.add_argument('--floor', type=float, default=2637.0, help='シーンでの床の標高。公称標高とは別。')
    parser.add_argument('--clearance', type=float, default=0.5, help='敷地の地面を床からどれだけ下げるか（m）。')
    args = parser.parse_args()
    root = find_root()
    terrain = Scene(root, args.scene).terrain()
    if terrain.by_kind('modelPlace') and terrain.by_note(NOTE):
        raise SystemExit('既に Model Place で配置済みです。二重配置を避けるため終了します。')
    meta = load_json(str(Path(root)/'Models/Senjojiki/Senjojiki.dimensions.json'))
    geo = Geo.of(terrain)
    # モデルの接地点（底面の中心）を置く位置。anchor は外形の中心。
    anchor = geo.metres(meta['anchorLongitude'], meta['anchorLatitude'])
    u, v = geo.uv_of_metres(anchor)

    # --- 旧構成の片付け -----------------------------------------------------
    # 敷地を均していた Height Levels は外し、その先を元の地形へ繋ぎ直す。植生から敷地を引く
    # Mask Blend と Model Output は残して、入力だけ繋ぎ替える。
    old = {n['id']: n for n in terrain.by_note(NOTE)}
    blends = [n for n in old.values() if n['kind'] == 'maskBlend']
    outputs = [n for n in old.values() if n['kind'] == 'modelOutput']
    levels = [n for n in old.values() if n['kind'] == 'heightLevels']
    consumers = []  # 均した地形を読んでいた（ノード, 入力の番号）
    if levels:
        upstream, out_index = terrain.source_of(levels[0]['id'], 0)
        result_pin = levels[0]['outputs'][0]
        for link in terrain.graph['links']:
            if link['start'] == result_pin:
                node, _, index = terrain.owner_of_pin(link['end'])
                if node['id'] not in old or node['kind'] in ('maskBlend', 'modelOutput'):
                    consumers.append((node['id'], index))
        position = levels[0]['position']
    else:
        output = terrain.by_kind('output')[0]
        upstream, out_index = terrain.source_of(output['id'], 0)
        consumers.append((output['id'], 0))
        position = [output['position'][0]-340, output['position'][1]+400]
    removed = [n['id'] for n in old.values() if n['kind'] not in ('maskBlend', 'modelOutput')]
    terrain.remove_nodes(removed)

    # --- 配置 ---------------------------------------------------------------
    # 敷地は建物とテラスの外周の凸包。接地点を原点にしたモデル空間の x（東）, z（南）で渡す。
    def local(lon, lat):
        east, north = geo.metres(lon, lat)
        return (east-anchor[0], -(north-anchor[1]))
    points = [local(*p) for p in meta['hotelFootprint']+meta['stationFootprint']]
    # テラス側（ホテル外周の最後の辺）は 7 m 外へ張り出す。
    a, b = local(*meta['hotelFootprint'][11]), local(*meta['hotelFootprint'][0])
    dx, dz = b[0]-a[0], b[1]-a[1]
    length = math.hypot(dx, dz)
    n = (dz/length*7, -dx/length*7)
    points += [(a[0]+n[0], a[1]+n[1]), (b[0]+n[0], b[1]+n[1])]
    polygon = [[round(p[0], 3), round(p[1], 3)] for p in convex_hull(points)]

    model = terrain.add_asset('models', 'Models/Senjojiki/Senjojiki.tgmodel')
    x0, y0 = position[0], position[1]
    place = terrain.add_node('modelPlace', 0, 2, (x0-340, y0-400), {'modelPlace': {
        'model': model, 'offset': -meta['floorFromBottom'], 'maxDistance': 0, 'autoLod': True, 'lod': 0, 'lodBias': 1,
        'padMargin': 1.5, 'padPolygon': polygon,
        'placements': [{'u': u, 'v': v, 'elevation': args.floor, 'yaw': 0, 'scale': 1}]}},
        f'{NOTE} 駅・ホテル・テラスを 1 棟。標高は床の高さ（{args.floor:.0f} m。公称 2,612 m とは地形の差で合わない）。'
        '基礎は接地オフセットで地面へ埋める。Pad は建物とテラスの外周')
    grading = terrain.add_node('roadGrading', 2, 4, (x0, y0), {'layer': {
        'kind': 'roadGrading', 'name': 'Grading', 'enabled': True,
        'roadGrading': {'clearanceMeters': args.clearance, 'vergeMeters': 2.0, 'cutRatio': 1.0, 'fillRatio': 1.5,
                        'maxRunMeters': 12.0, 'maskSoftMeters': 0.3}}},
        f'{NOTE} 敷地を床の {args.clearance:.1f} m 下（標高 {args.floor-args.clearance:.1f} m）へ均し、切土・盛土の法面で地形へ繋ぐ'
        '。急斜面で盛土が長く伸びないよう、法面は 12 m で止める（その先は段。擁壁は未対応）')
    terrain.link(upstream['id'], grading['id'], 0, out_index)
    terrain.link(place['id'], grading['id'], 1, 1)
    for node_id, index in consumers:
        terrain.link(grading['id'], node_id, index, 0)
    site = terrain.add_node('maskMesh', 1, 1, (x0-340, y0-180),
                            {'maskMesh': {'margin': 0.0, 'feather': 12.0, 'gamma': 1.0, 'invert': False}},
                            f'{NOTE} 敷地の範囲（植生の除外に使う）')
    terrain.link(place['id'], site['id'], 0, 1)
    for blend in blends:
        terrain.link(site['id'], blend['id'], 1)
    if outputs:
        end = outputs[0]
    else:
        end = terrain.add_node('modelOutput', 1, 0, (x0, y0-400), note=f'{NOTE} 駅・ホテル・テラス')
    terrain.link(place['id'], end['id'], 0, 0)
    # 植生の除外がまだ無ければ、既存の植生の分布から敷地を引く。
    if not blends:
        for scatter in list(terrain.by_kind('scatter')):
            source, pin = terrain.source_of(scatter['id'], 1)
            if source is None:
                continue
            blend = terrain.add_node('maskBlend', 2, 1, (scatter['position'][0]-220, scatter['position'][1]+180),
                                     {'blend': {'mode': 'subtract', 'intensity': 1}}, f'{NOTE} 敷地には植生を置かない')
            terrain.link(source['id'], blend['id'], out_index=pin)
            terrain.link(site['id'], blend['id'], 1)
            terrain.link(blend['id'], scatter['id'], 1)
    terrain.save()
    world_x, world_z = geo.world_xz(u, v)
    record = {'scene': args.scene, 'terrain': terrain.rel_path, 'pointUV': [u, v], 'pointWorldXZ': [world_x, world_z],
              'sceneFloorElevation': args.floor, 'publishedElevation': meta['floorElevation'],
              'padPolygonPoints': len(polygon), 'modelPlaceNode': place['id'], 'gradingNode': grading['id'],
              'removedNodes': removed}
    save_json(str(Path(root)/'Models/Senjojiki/Senjojiki.placement.json'), record)
    print(record)


if __name__ == '__main__':
    main()
