# 千畳敷駅・ホテル千畳敷の外観モデル

作成日時: 2026-10-05 19:35
更新日時: 2026-10-05 20:41

## 対象と精度

木曽駒ヶ岳シーンへ置く千畳敷駅と、それに一体化したホテル千畳敷の外観。
ユーザー了承のもと、公開資料からの推定寸法で作成した。建築図面・実測値による再現ではない。
Blender の単位は m、FBX は右手系 Y-up。倍率 1 で配置する。

- 建物の水平外周と向き: OpenStreetMap のホテル（way 187898269）と駅（way 187898270）。
- 外周から計算した目安: 駅舎 約 26 × 13 m、ホテルの長い棟 約 27 × 13 m。ホテル全体は折れ曲がった平面。
- 写真から推定: ホテル軒高 7.1 m、駅舎軒高 12.4 m、棟の上がりはホテル 1.7 m・駅舎 2.7 m。
- 桃色の外壁、赤いホテル屋根、灰色の駅舎屋根、窓枠、入口の庇、雨樋、屋根設備、木製テラスを作成。
- テラスの奥行き約 6.5 m、階段・ベンチ・手すりも近似。背面など写真で確認できない部分は簡略化した。
- 駅のロープウェイ開口は暗い面で表現。内部、搬器、索道のケーブル・鉄塔は含まない。ガラスはアプリの不透明 PBR。
- 全体の包囲箱は約 54.44 × 60.02 m、高さ 28 m。高さには地中へ隠す基礎 10 m と避雷針を含む。

## 出典

2026-10-05 に参照。

- [OpenStreetMap: ホテル外周](https://www.openstreetmap.org/way/187898269)
- [OpenStreetMap: 駅外周](https://www.openstreetmap.org/way/187898270)
- [ホテル千畳敷 公式](https://www.chuo-alps.com/hotel/)
- [公式の館内施設・テラス](https://www.chuo-alps.com/hotel/facility/)
- [名鉄の紹介・外観写真](https://www.meitetsu.co.jp/exp-nagoya/eng/1270230_8916.html)
- [改修後のテラスの参考写真](https://tawaman.livedoor.blog/archives/22941413.html)

建物外周データ: © OpenStreetMap contributors、[ODbL 1.0](https://www.openstreetmap.org/copyright)。
取得した地図・参考写真は `data/Test/senjojiki-reference/` に置いた。
写真は見比べるために用い、モデルのテクスチャとして貼り付けていない。

## ファイル

`data/Models/Senjojiki/`（Git 管理外）:

- `Senjojiki.blend`: 編集元。LOD0 と LOD1。プレビューでは LOD1 を非表示にしてある。
- `Senjojiki.fbx` / `Senjojiki.tgmodel`: アプリ用。9 材質を `MI_Senjojiki_*.tgmat` で割り当てる。
- `Senjojiki.dimensions.json`: 外周座標、寸法、原点の緯度経度、出典、三角形数。
- `Senjojiki.placement.json`: シーン内の配置点・シード・高さと誤差。
- `Senjojiki_preview.png`: Blender の外観確認。地形が無いので地中用の基礎も見える。

生成は `tools/blender/make_senjojiki.py`、配置は `tools/scene/place_senjojiki.py`。
Blender のバックグラウンド実行には `--python-exit-code 1` を付ける。
生成スクリプトは FBX と blend を再生成するが、既存の tgmat / tgmodel の UID と手直しは維持する。
ufbx は面で材質が初めて現れた順にスロットを作るため、FBX の面も材質順に並べてある。

## 配置と地形

**2026-10-05 20:41 に Model Place へ置き換えた。** 以下の「Scatter で 1 点を通す」構成は最初の版の記録。今は
Model Place 1 つ（位置は外形の中心の緯度経度から直接、方位 0°、標高 2,637 m、接地オフセット -10 m）で置き、
その Pad（建物とテラスの外周の凸包 + 余白 1.5 m）を Grading（床の 0.5 m 下へ均す。平らな幅 2 m、切土 1:1、盛土 1:1.5、
法面は 12 m まで）と Mask Mesh（フェザー 12 m。植生の除外）へ繋いでいる。シードや散布間隔には依らない。
仕組みは [nodes.md の Model Place](nodes.md)。

- 対象: `data/Scenes/kiso-komagatake/kiso-komagatake_terrain.tgterrain`。
- モデルの底面中心: 北緯 35.77765057°、東経 137.81370250°。これは駅の乗降地点ではなく建物全体の包囲箱中心。
- 現在のアプリには任意の 1 点へ手置きするノードが無いため、既存の Scatter と 3 m 四方の Mask Area で 1 点だけを通す。
- Scatter の間隔 16 m、シード 1108、Model Scatter のシード 41299。倍率 1、法線追従 0、点の大きさは使わない。
- 北の向きを保つようシードを探索。参照外周に対する配置点のずれ約 0.05 m、回転差約 0.003°。これは**計算上の合わせ込み誤差**で、元の地図や実物に対する精度ではない。
- 地形の一辺・散布間隔・シードを変えると位置や向きが変わる。変更時は配置を再計算する。スクリプトは二重配置を避けるため、配置済みなら停止する。
- 敷地内の植生を除き、Height Levels と敷地マスクで床の下を局所的に均す。外縁は 12 m で元の地形へ戻す。敷地外の元のノード設定は変更していない。

**公称標高とシーンに差がある。** 公称標高は 2,612 m だが、位置を合わせた元ハイトマップは約 2,646.1 m、
既存グラフで加工した後は約 2,636.0 m（2048² で書き出したハイトの中心付近）。
今回は周囲の地形との接続を優先し、敷地を 2,636.5 m、床を 2,637.0 m とした。
実際の標高に一致した配置ではない。正確な再現にはハイトマップの位置・標高と現地の造成面の確認が必要。

## 検証

- Blender 5.2.1 で生成・FBX 書き出し・プレビューを確認。
- Release の正面・背面・遠景を `data/Test/senjojiki-qa/v5_*.png` に撮影。読み込み・描画ログの警告とエラーは 0 件。
- Debug の `--evaluate-report`: `report.json`、エラー 0、駅の配置数 1。設定範囲の警告 10 件は `baseline-report.json` と完全一致し、既存シーン由来。
- Debug で元シーンを保存し直し、Release で再読込・撮影（`final_front.png`）。両方のログで警告・エラー 0。元の地形ノード 147 個の設定は同一で、空とカメラも維持。雲の部品は保存時の ID 衝突回避による再採番だけが変わった。
- 参考写真と比較し、棟の折れ方、屋根・壁の色、窓とテラスを確認。精密なファサード、内装、背面の詳細は未再現。
- 作業前のバックアップ: `data/Test/kiso-komagatake-before-station-20261005.zip`。
