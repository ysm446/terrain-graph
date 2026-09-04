# file-format — プロジェクトとマテリアルのファイル形式

作成日時: 2026-08-31 15:12
更新日時: 2026-09-04 14:30

実装は [src/io/ProjectIo.cpp](../../src/io/ProjectIo.cpp)。**形式を変えたらこの文書も直す。**

## 全体像

| 拡張子 | 内容 | 用途 |
| --- | --- | --- |
| `.tgproj` | プロジェクト全体（グラフ / マテリアル / テクスチャの参照 / プレビュー設定） | 作業の保存と再開 |
| `.tgmat` | マテリアル 1 つ | プロジェクト間で持ち回る（書き出し / 読み込み） |
| `<名前>.assets/paint_NNNN.png` | ペイントマスク | `.tgproj` のサイドカー |

material-mixer 時代の `.mmproj` / `.mmmat` も**読み込みだけ**受け付ける
（形式名 `material-mixer.*` は `terrain-graph.*` へ読み替える）。書き出しは常に新形式。

どちらも **UTF-8 の JSON**。人が読める形（インデント 2）で書き、差分も取れる。

### 3 つの原則

1. **プロジェクトはマテリアルの構造を丸ごと持つ。**
   `.tgproj` を開くのに `.tgmat` は要らない。`.tgmat` はあくまで持ち出し用で、
   プロジェクトが外部のマテリアルファイルに依存することはない。
2. **画像は参照で持つ。** テクスチャの中身はコピーせず、パスだけを記録する。
   パスは**そのファイルからの相対**で書き、プロジェクトごと移動しても壊れないようにする
   （ドライブが違うなど相対にできないときだけ絶対パス）。
3. **手続きで再現できないものだけ画像にする。** ペイントマスクがこれにあたるので、
   サイドカーのフォルダへ 8bit グレースケール PNG で書き出す。

### 共通のヘッダ

```json
{ "format": "terrain-graph.project", "version": 4, "app": "0.1.0" }
```

- `format` — `terrain-graph.project` または `terrain-graph.material`。違えば読み込みを断る。
- `version` — 形式の版。**読み込み側は「ファイルの版 <= 対応版」なら読む。**
  未知のキーは無視し、欠けているキーは既定値（構造体の初期値）で埋める。
- `app` — 書き出したアプリのバージョン。参考情報で、読み込みでは見ない。

### 版の履歴

| 版 | 変更 |
| --- | --- |
| 1 | 最初の形式 |
| 2 | レイヤーのハイトに `gain` を追加し、`base` の意味を変えた（下記） |
| 3 | レイヤーに `kind`（種類）を追加した（下記） |
| 4 | `layers[]` を廃止し、合成の構造を `graph` に一本化した（下記） |

**版を上げる基準は「キーが増えたか」ではなく「既存のキーの意味が変わったか」。**
キーが増えただけなら、古いビルドはそれを無視して正しく読める。意味が変わった場合は、
古いビルドが黙って違う結果を出すので、断れるように版を上げる。

#### 版 2 — ハイトの基準面

版 1 のハイトは `h = base + src * amount` で、`amount` はノイズのパラメータが兼ねていた。
版 2 では基準面 0.5 を挟む `h = base + (src - 0.5) * gain` になり、`gain` が独立した。
理由は [design/compositing.md](../design/compositing.md) を参照。

読み込み時、`gain` が**無ければ版 1 と判断**して次のように移行する。

```
gain  = noise.amount
base' = base + 0.5 * gain     ただしソースが constant のときは base のまま
```

`base + src * gain == base' + (src - 0.5) * gain` なので、**近似ではなく厳密に同じ値**になる。
定数はそもそも `src` の項が無いため、触ると逆にずれる。

版ではなくキーの有無で判定しているのは、版が上がっても移行処理が正しく動くようにするため。

#### 版 3 — レイヤーの種類

レイヤーに `kind`（`surface` / `shape` / `liquid`）が付き、
**高さの合成規則が種類ごとに変わる**（[design/compositing.md](../design/compositing.md) の
「レイヤーの種類」）。`shape` と `liquid` では `height.base` / `blendRange` の
解釈も変わる（持ち上げ / 水位、フェザー）。古いビルドはキーを無視して
全レイヤーをサーフェスとして合成し、黙って違う絵を出すため版を上げた。

`kind` の無い旧ファイルは全レイヤーを `surface` として読む。これは移行ではなく
そのままの意味（版 2 以前にはサーフェスしか無い）。

あわせて `height.texture`（レイヤー直結のハイトマップ。スカラーのマップと同じ
「テクスチャ + チャンネル」の組）と `wrapToUnderlying`（下地に沿わせる。
サーフェスのコーティング）が増えた。どちらもキーの追加だけなので版は分けない。

#### 版 4 — レイヤー廃止（グラフへの一本化）

合成の構造は `graph` 節だけが持ち、`layers[]` は書かなくなった。
古いビルドは版 4 のファイルに `layers` を見つけられず**黙って下地 1 枚に
落ちる**ため版を上げた。読み込み側の扱い:

- 版 4 以降: `graph` 節が唯一の合成。無ければ既定（ベース → 出力）へ戻す。
- 版 3 以前: グラフ節の `apply`（当時の「プレビューに適用」）がオンで保存されて
  いれば `graph` を、そうでなければ **`layers[]` を同じ見た目のまま
  「下地」チェーンへ移行**して読む（当時プレビューに出ていた側を正とする）。

レイヤー 1 枚ぶんの形（`name` / `kind` / `height` / `mask` など）は
そのまま `graph.nodes[].layer` の形として使われ続けている。
版 2 / 版 3 の移行規則もその中で同じように働く。

マスクのノードは種類ごとの設定を**まとめて全部書く**（種類を変えて戻したときに
値が消えていると驚くため）。

| キー | 使うノード | 中身 |
| --- | --- | --- |
| `map` | `maskImage` | `{ texture, channel }` |
| `fluvial` | `maskFluvial` | `{ curve, threshold, gamma, softness, edgePower, detail, concentration, resolution }` |
| `slope` | `maskSlope` | `{ detail, min, max, gamma, invert }` |
| `levels` | `maskLevels` | `{ black, white, gamma, invert }` |
| `blend` | `maskBlend` | `{ mode, intensity }` |
| `maskPath` | `maskPath` | `{ gamma, invert }` |
| `path` | `path` | `{ points[], edges[], defaultWidth, defaultFeather, defaultIntensity, nextId }`（下記） |

**マスクの繋ぎ方そのものは `links` にしかない。** レイヤー側の
`mask.source` が `node` のとき、どの op を読むかはコンパイルのたびに決まるので
保存しない（`maskOp` は保存対象外）。
レイヤーノードの `inputs` は Base / Mask の 2 本になったが、**ピンは定義から
再生成する**ので、Mask ピンを持たない古いファイルもそのまま読める
（足りないピンには新しい ID が振られ、リンクの無い入力になる）。

`preview` の `maskSaturationHatch` は、マスクのプレビューで 0 / 1 に
張り付いた所へ斜線を引くかどうか（既定は切）。

`preview.depthOfField` の `miniatureScale` は、被写界深度でシーンの距離に掛ける
縮尺の分母（1 : この値）。既定は 1（実物大）。

ノードごとの設定の意味は [nodes.md](nodes.md) にまとめてある。

マスクのノードは種類ごとに使う設定が違うが、**全部書く**（種類を変えて戻したときに
値が消えていると驚くため）。`curvature` は `{ mode, detail, sensitivity, threshold,
gamma }` で、`mode` は `ridges` / `valleys` / `absolute`。
`height`（Mask Height）は `{ fullRange, min, max, feather, gamma, invert }` で、
`min` / `max` / `feather` はメートル。無ければ既定値（版は上げない）。

レイヤーの `sediment`（`{ emission, emissionTime, detail, iterations, stabilization,
viscosity, convertTerrain, resolution, maskContrast, maskThicknessMeters }`）は
`kind` が `sediment` のノードだけが使う。

レイヤーの `crumbling`（`{ physicsCount, amount, sizeMin, sizeMax, style,
gravity, spread, seed }`）は `kind` が `crumbling` のノードだけが使う。
`style` は `classic` / `polygonal` / `shard`。

レイヤーの `snow`（`{ emission, emissionTime, iterations, settlingPasses,
motionSlopeDegrees, transportRate, surfaceSmoothing, detail, resolution,
maskThresholdMeters, maskFeatherMeters }`）は `kind` が `snow` のノードだけが使う。
無ければ既定値（版は上げない。古いファイルには単に無い）。

レイヤーの `river`（`{ threshold, detail, concentration, resolution, mainWidth, minWidth,
widthExponent, bedDepth, bankWidth, bankHardness, fillWater, minSlope, shoreWidth,
shoreHeight, shoreFeather }`）は `kind` が `river` のノードだけが使う。
長さは m、`threshold` は全セル数に対する割合、`minSlope` は無次元。
無ければ既定値（版は上げない）。

`kind` が `path` のノードは `path` を持つ。`points[]` は
`{ id, u, v, width, feather, intensity, heightOffset }`（`u` / `v` は地形平面の正規化座標、
寸法は m）、`edges[]` は `{ id, from, to, curve, rounding, clothoidRatio, route, maxGrade,
routedFrom, routedTo, waypoints }`（点の `id` を指す。from → to が向き。`curve` は
`line` / `quadratic` / `cubic` / `clothoid`、`rounding` と `clothoidRatio` は 0〜1。
無ければ直線）。経路探索の `route` は `none` / `road` / `flow`（無ければ `none`）、
`maxGrade` は許容勾配（%）、`routedFrom` / `routedTo` は計算したときの両端 `[u, v]`、
`waypoints` は内部点 `[u0, v0, u1, v1, …]`（両端を除く）。`route` が `none` なら書かず、
`routedFrom` / `routedTo` が揃っていなければ未計算として読む（内部点は捨てる）。
`nextId` は点とエッジの次の ID（パスの中でだけ一意）。端点の無いエッジは読み捨てる。
無ければ空のパス（版は上げない）。

レイヤーの `blur`（`{ radius, strength, iterations }`）は
`kind` が `blur` のノード（Heightmap Blur）だけが使う。無ければ既定値。

プレビュー設定の `shape`（形状）と `materialUvScale`（UV スケール）は**廃止した**。
ジオメトリは平面 1 種類、合成結果は等倍で貼るだけになったため。
古いファイルのキーは読み飛ばす（版は上げない）。

`normalStrength`（法線の強さ）は**廃止した**。法線は地形の実寸から作るので、
無次元の強さを持つ意味が無くなった。読み込み時は無視し、保存でも書かない。
**版は上げない。** キーが消えるだけで既存のキーの意味は変わらず、
古いビルドが新しいファイルを読んでも既定値 1.0 として従来どおり動く。

## `.tgproj`

```
{
  format / version / app
  textures[]      画像への参照。id は 1 から振り直した通し番号
  materials[]     マテリアルの中身。テクスチャは textures の id で参照する
  paintMasks[]    ペイントマスク。実体はサイドカーの PNG
  paintResolution ペイントマスクの解像度（全マスク共通）
  graph{}         ノードグラフ（合成の構造はここだけが持つ）
  skies[]         天球。ビューポートの環境
  activeSky       ビューポートに適用している天球（skies の添字）
  preview{}       カメラ / ライト / 露出 / 被写界深度 / トーンマップ / 形状など
}
```

### 天球

```json
"skies": [
  {
    "name": "夕焼け",
    "source": "hdri",              // procedural / hdri
    "hdri": "../../assets/hdr/pink_sunrise_4k.hdr",   // 使わなければ null
    "skyLuminance": 12000.0,       // この HDRI の空を何 cd/m^2 とみなすか
    "iblIntensity": 1.0,
    "procedural": { "zenithColor": [...], "horizonColor": [...],
                    "groundColor": [...], "intensity": 12000.0 }
  }
]
```

`skyLuminance` は**天球ごとに持つ**。HDRI は絶対輝度で較正されていないため
基準を外から与える必要があり、その値はファイルごとに違う。

`procedural` は `source` が `hdri` のときも書く。ソースを切り替えたときに
手で入れ直さなくて済むようにするため。

**`skies` が無いプロジェクトは、`preview` の `hdri` / `hdriSkyLuminance` /
`iblIntensity` / `sky` から天球を 1 つ作って読み込む**（天球を入れる前の形式）。
新しく保存すると `skies` の形に移る。

**id は保存のたびに 1 から振り直す。** 実行中の ID をそのまま書くと、
削除して番号が飛んだファイルになり読みにくい。読み込み側は
ファイル内の id → 実行時の ID の対応表を作って解決する。

参照できないものは `null` で表す（`"material": null` は「マテリアルなし」）。

```json
"textures": [
  { "id": 1, "name": "T_Gravel_D.png", "path": "../../assets/textures/T_Gravel_D.png" }
]
```

`name` は一覧に出す表示名で、変えてもファイル名は変わらない。

### マップの参照

マテリアルの `flipNormalGreen` は法線マップの規約（緑の向き）。
**既定は true = OpenGL 規約**（緑を反転して読む）。無ければ true として読む。

RGB をそのまま使うマップ（ベースカラー / 法線）はテクスチャ参照を直に書く。
スカラーのマップは「テクスチャ + 読むチャンネル」の組で書く
（Megascans の `_ORD` のように 1 枚へ詰めたテクスチャを使うため）。

```json
"maps": {
  "baseColor": 1,
  "normal": 2,
  "roughness":         { "texture": 3, "channel": "g" },
  "metallic":          { "texture": null, "channel": "r" },
  "ambientOcclusion":  { "texture": 3, "channel": "r" },
  "height":            { "texture": 3, "channel": "b" }
}
```

### ペイントマスク

```json
"paintMasks": [ { "id": 1, "resolution": 1024, "file": "paint_0001.png" } ]
```

`file` はサイドカーのフォルダ `<プロジェクト名>.assets/` からの相対名。
プロジェクト名から場所が決まるので、フォルダのパスは記録しない。

- 保存のたびに `paint_*.png` を書き直す。**消すのは自分が書いた名前だけ**で、
  同じフォルダの他のファイルには触らない。
- 書き出すのは**グラフのノードが参照しているマスクだけ**。どのノードからも
  参照されていないマスクは、次に開いたとき出てこない。

### ノードグラフ

`graph` 節。**合成の構造はここだけが持つ**（版 4。旧 `layers[]` は
読み込み時にグラフへ移行する。「版 4」の項を参照）。設計は
[design/node-graph.md](../design/node-graph.md) を参照。

```json
"graph": {
  "nodes": [
    { "id": 1, "kind": "surface", "position": [60.0, 120.0],
      "inputs": [2], "outputs": [3],
      "layer": { /* レイヤー 1 枚ぶんの形（name / kind / height / mask …） */ } },
    { "id": 4, "kind": "output", "position": [420.0, 120.0],
      "inputs": [5], "outputs": [] }
  ],
  "links": [ { "id": 6, "start": 3, "end": 5 } ]
}
```

- ノード・ピン・リンクは**共通の単一 ID 空間**。`inputs` / `outputs` はピン ID の
  並びで、ピンの型やラベルはノードの定義から再生成する（ファイルには書かない）。
- `kind` は名前で書く（`surface` / `shape` / `liquid` / `heightmap` /
  `heightmapBlur` / `maskImage` / `maskFluvial` / `maskSlope` / `maskLevels` /
  `maskBlend` / `output`）。知らない種類のノードは読み飛ばす。
- レイヤー設定を持つノード（surface / shape / liquid / heightmap /
  heightmapBlur）は `layer` に
  旧 `layers[]` の要素と同じ形を持つ。テクスチャ / マテリアル / ペイントの参照も
  同じ対応表で解決する。
- `links` の `start` は出力ピン、`end` は入力ピン。ピンが無い・型が合わない
  リンクは読み込みで捨てる。
- 版 3 のファイルには `apply`（当時の「プレビューに適用」）がある。
  読み込みの分岐だけに使い、書き出しはしない。

### 列挙は名前で書く

数値ではなく名前で書く。ファイルを直接読んだときに意味が分かるようにするため。

| 種別 | 値 |
| --- | --- |
| チャンネル指定 | `r` / `g` / `b` / `a` |
| レイヤーの種類 | `surface` / `shape` / `liquid` / `blur` / `sediment` |
| ハイトのソース | `constant` / `noise` / `texture` |
| ノイズ | `fbm` / `ridged` / `worley` |
| マスクのソース | `constant` / `noise` / `texture` / `height` / `slope` / `curvature` / `cavity` / `paint` / `fluvial` |
| 川筋の出力カーブ | `log` / `threshold` / `linear` |
| 書き込むチャンネル | `["baseColor", "normal", "surface", "height"]`（配列） |
| トーンマップ | `none` / `reinhard` / `aces` |

知らない名前が来たら既定値に落とす。

## `.tgmat`

マテリアル 1 つぶん。中身は `.tgproj` の `materials[]` の要素と同じで、
**テクスチャ参照だけが id ではなく相対パス**になる。

```json
{
  "format": "terrain-graph.material",
  "version": 3,
  "name": "砂利",
  "baseColorTint": [1.0, 1.0, 1.0],
  "roughness": 0.5, "metallic": 0.0, "ambientOcclusion": 1.0,
  "maps": {
    "baseColor": "../../assets/textures/T_Gravel_D.png",
    "normal":    "../../assets/textures/T_Gravel_N.png",
    "roughness": { "texture": "../../assets/textures/T_Gravel_ORD.png", "channel": "g" }
  }
}
```

読み込むと、参照している画像をテクスチャライブラリへ読み込み、
マテリアルを 1 つ**追加**する（既存のマテリアルには触らない）。
**同じパスの画像はすでに読み込んでいれば読み直さない。**
`_ORD` のように複数のマップが同じファイルを指していても 1 枚で済む。

## 壊れたファイルの扱い

例外は使わないので、失敗はすべて戻り値とログで表す。

- JSON として読めない / `format` が違う / 版が新しすぎる → **何も変更せずに断る。**
- 画像が見つからない → **警告を出して読み込みを続ける。**
  そのスロットは「なし」になる。プロジェクト全体を捨てない。
- 値の型が食い違っている → その項目だけ既定値に落とす。
- レイヤーが 1 枚も無い → 下地を 1 枚だけ置く（空のスタックは操作の起点が無い）。
