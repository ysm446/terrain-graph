# remote-control — 起動中のアプリを LLM から操作する口（MCP）

作成日時: 2026-10-21 13:20
更新日時: 2026-10-21 14:40

起動中の terrain-graph を、LLM（Claude Code などの MCP クライアント）が操作するための仕組み。
計画は [plan.md](../plan/plan.md) の「LLM がアプリを動かしてノードを組める仕組み」の段階 5。

```
MCP クライアント ──stdio(JSON-RPC)── terrain_graph_mcp.exe ──名前付きパイプ── terrain_graph.exe
                                       （src/mcp/McpMain.cpp）  \\.\pipe\terrain-graph   （RemoteServer / ApplicationRemote）
```

- **UI と同じ処理を通す。** ノードの作成・接続は `NodeGraph::CreateNode` / `CheckLink` / `CreateLink`、
  設定は保存と同じ書き出し・読み込み（`io::WriteNodeJson` / `io::ReadNodeJson`）、パスは Path の編集操作
  （`AddPathPoint` / `ApplyPathEdgeStyle` など）を呼ぶ。検証（循環・型・範囲）も UI と同じ。
- **決まった操作だけを受ける。** 任意のコードは実行しない。知らない操作・キー・範囲外の値は失敗として返す。
- **操作列はまとめて成功するか、何も変えないか。** 失敗したらグラフを操作列の前へ戻す。
- 編集はフレームの中（UI の描画の後、アンドゥの段を畳む前）で行う。**1 回の `apply_ops` がアンドゥの 1 段**になり、
  シーンは未保存になる。使う人がウィジェットを掴んでいる間は、離すまで待つ（その段に混ざらないように）。
- 保存は `save_scene` を頼まれたときだけ（ファイルを上書きする）。

## 使い始める

1. アプリの **設定 > 遠隔操作 > 有効** を入れる（`settings.json` の `remote.enabled`）。パイプ
   `\\.\pipe\terrain-graph` が開き、状態が「待ち受け中」になる。
   - 起動引数 `--remote-control` でもその起動だけ開ける。`--remote-pipe <name>` はパイプの名前を変えて開く
     （検証で、使っている人のアプリと別の名前にするときなど）。名前は英数字と `-_.` だけ。
   - 名前は `settings.json` の `remote.pipeName` でも変えられる（設定ウィンドウには出さない）。
   - 同じ名前のパイプを別のアプリが開いていれば開かない（後から起動した側が横取りしない）。
   - ローカルからだけ受ける（`PIPE_REJECT_REMOTE_CLIENTS`）。同時に繋げるクライアントは 1 つ。
2. MCP クライアントに `terrain_graph_mcp.exe` を登録する。Claude Code ならリポジトリの `.mcp.json`
   （`build/bin/Release/terrain_graph_mcp.exe` を起動する）が使われる。ほかの場所の EXE は
   `claude mcp add terrain-graph -- path/to/terrain_graph_mcp.exe` のように登録する。
   - `terrain_graph_mcp.exe [--pipe <name>]`。パイプの名前を変えたときは合わせる。
   - アプリが起動していなくても MCP サーバは立ち上がる。ツールを呼んだときに繋ぎ、繋がらなければ理由を返す。
     アプリを起動し直したら次の呼び出しで繋ぎ直す。
3. ウィンドウを最小化している間は描画が止まるので、要求は「最小化されている」で断る。
   背面（ほかのウィンドウの後ろ）なら、背面の FPS 上限（既定 10）で処理する。

## パイプの形式

1 行 1 つの UTF-8 の JSON。要求に 1 行の返事が返る。1 つの接続の中では順に処理する。

```json
{"id": 1, "method": "apply_ops", "params": {"ops": [ … ]}}
{"id": 1, "result": { … }}
{"id": 2, "error": {"message": "ops[1]: ピンの型が違うため繋げません（Mask → Material）", "data": {"applied": false, "failedOp": 1, "op": "connect"}}}
```

MCP サーバは、ツール名をそのまま `method` に、引数を `params` に入れて送る。`error` は MCP のツール結果の
`isError: true`（本文はメッセージと `data`）になる。

## ツール（method）

| 名前 | いつ処理するか | 内容 |
|---|---|---|
| `get_status` | フレームの前 | シーン、編集中のグラフ、未保存の項目、アンドゥの段数、選択、評価が落ち着いているか、地形の実寸、カメラ |
| `get_graph` | フレームの前 | ノード、リンク、グループ。既定は短い形（下の「get_graph の形」）。`component`、`nodes` で絞る |
| `get_node` | フレームの中 | 1 ノードの `settings`（保存と同じ形）と `parameters`（UI の行：表示名・型・範囲・単位・説明・今の値） |
| `list_node_kinds` | フレームの前 | 置ける種類（kind・表示名・説明・置けるグラフ・ピン） |
| `describe_node_kind` | フレームの中 | 種類 1 つのカタログ項目（既定値・列挙・`parameters`）。初回は設定の行を描いて調べる（Debug で約 2 秒） |
| `apply_ops` | フレームの中 | 編集の操作列（下の「操作」） |
| `get_evaluation` | フレームの後 | 評価のレポート（`--evaluate-report` と同じ形）。既定で落ち着くまで待つ |
| `screenshot` | 前 → 後 | ビューポート（UI なし）かウィンドウ全体の PNG。既定で評価が落ち着くまで待つ |
| `get_camera` / `set_camera` | フレームの前 | ビューポートのカメラ |
| `sample_terrain` | フレームの前 | UV の点の標高・傾斜・ワールド座標 |
| `recompute_routes` | フレームの前 | Path / Road Path の経路探索の鎖を計算し直す（UI の「経路を計算」） |
| `undo` / `redo` | フレームの前 | `steps` 段だけ戻す / やり直す |
| `save_scene` | フレームの前 | Ctrl+S と同じ保存。まだ保存先の無いシーンは断る |
| `list_assets` | フレームの前 | 読み込み済みのマテリアル・モデル・テクスチャ（設定の参照に使う id） |
| `get_log` | フレームの前 | ステータスバーに流れたログ（`since` より後の通し番号） |

フレームの中で処理するものは、要求が届いてから最初に UI を描くフレームで処理する。

### 座標

- **地形の UV**: パスの点、`sample_terrain`、`set_camera` の `target_uv` / `eye_uv`。0〜1 の正規化座標で、
  u が東（+X）、v が南（+Z）。Heightmap の一辺を変えても同じ場所を指す。
- **ワールド座標**: m。原点が地形の中心、X 東・Y 上・Z 南。高さ 0 は地形のハイト 0.5 の所
  （標高は `get_status` の `terrain.baseElevationMeters` + ハイト × `heightMeters`）。
- **ノードの position**: グラフの画面上の座標（評価には効かない）。

### get_graph の形

ノードが百数十個のグラフでも一度に読めるよう、既定は短い形にする（天狗岳の 155 ノードで約 18 KB）。

```json
{"nodes": [{"id": 43, "kind": "maskBlend", "component": "terrain", "note": "岩の分布を除く。"},
           {"id": 47, "kind": "surface", "component": "terrain", "name": "Grass", "note": "植生（芝）のマテリアルを割り当てる。"}, …],
 "links": [[33, "Mask", 43, "Foreground"], [88, "Mask", 43, "Background"], …],
 "groups": [], "format": "…"}
```

- ノードは `id`・`kind`・`component`（`component` で絞ったときは出さない）・レイヤーの `name`・`bypass`・メモの先頭 40 文字。
- リンクは `[出力ノード, 出力ピン名, 入力ノード, 入力ピン名]`。
- `detail: true`（か `include_settings: true`）で、ノードごとの入出力ピン（型と繋がっている先）・位置・メモの全文を出す
  詳しい形になる。大きくなるので `nodes` で絞って使う。

### get_node のパス

Path / Road Path の点とエッジは数千になることがあるので、既定では `settings.path` を鎖ごとの要約
（`strands`: 両端の点の ID と UV、点とエッジの数、延長、曲線、経路探索、幅）に置き換える。
`path_region: [u0, v0, u1, v1]` を渡すと、その範囲の点と、その点に付いているエッジを保存と同じ形で
`regionPoints` / `regionEdges` に入れる。`full_settings: true` なら置き換えない。経路探索の内部点
（`waypoints`）も既定では数（`waypointCount`）だけにする。これは表示だけで、`set_params` は縮める前の
設定へ重ねる。

## 操作（apply_ops の ops）

`node` はノード ID（数）か、同じ操作列の `create_node` で付けた `key`（文字列）。
ピンは `[ノード, ピン名]`。ピン名は `get_graph` / `list_node_kinds` の名前で、大文字小文字・空白・`_`・`-` は
無視する。番号（0 始まり）でもよく、省くと最初のピン。同じ名前の可変入力（Cloud Merge / Model Merge）は、
名前だけなら空いている最初のピン、`"Shape 2"` のように番号付き（1 始まり）でも指せる。

| op | 引数 | 内容 |
|---|---|---|
| `create_node` | `kind`, `key?`, `component?`, `position?`, `name?`, `settings?`, `note?`, `bypass?` | ノードを置く。初期値は追加メニューと同じ。`component` は `terrain` / `cloud`（省くと置ける方）。`position` を省くと、そのグラフのノードの右へ縦に並べる。`name` はレイヤーを持つ種類だけ |
| `delete_node` | `node` | ノードを消す（繋がっているリンクも消える） |
| `set_params` | `node`, `settings` | 設定を変える（下の「設定の変え方」） |
| `connect` | `from: [node, 出力]`, `to: [node, 入力]` | 繋ぐ。入力に既にある接続は置き換わる（結果の `replaced`）。型違い・循環・同じノードは失敗 |
| `disconnect` | `to: [node, 入力]` か `link` | 外す |
| `set_bypass` | `node`, `bypass` | バイパス（自前の「有効」を持つ種類はそれを切り替える。UI と同じ） |
| `set_note` / `move_node` | `node`, `note` / `position` | メモ / グラフ上の位置 |
| `add_path` | `node`, `points`, `closed?`, `connect_from?`, `connect_to?`, `style?` | Path / Road Path に折れ線を足す。点は `[u, v]` か `{u, v, width?, feather?, intensity?, heightOffset?}`（省いた値はパスの既定値）。`connect_from` / `connect_to` は既存の点から伸ばす / 既存の点で終える。結果は作った点とエッジの ID |
| `set_path_points` | `node`, `points: [{id, u?, v?, width?, …}]` | 点を動かす・値を変える |
| `set_path_style` | `node`, `edges`（ID の配列か `"all"`）, `style` | エッジの設定（書いたキーだけ変える） |
| `delete_path_elements` | `node`, `points?`, `edges?` | 点・エッジを消す。鎖の途中の点は、両隣を繋ぎ直して消える（UI と同じ） |
| `select` | `node`, `preview?` | 操作の後、UI でそのノードを選ぶ。`preview: true` ならビューポートの表示もそのノードへ移す。選択と表示はアンドゥにも未保存にも数えない |
| `arrange` | `mode`: `overlaps` / `all` | ノードの重なりを解消 / 全体を左から右へ並べ直す。次のフレームにグラフのパネルで行うので、**アンドゥは別の段**になる |

`style` のキーはファイルのエッジと同じ: `curve`（`line` / `quadratic` / `cubic` / `clothoid`）、`rounding`、
`clothoidRatio`、`width` / `feather` / `intensity`（書くと鎖の上で幅を一定にする）、`meander`、`meanderWavelength`、
`route`（`none` / `road` / `flow` / `trail`）、`maxGrade`、`ridgeWeight`、`avoidWeight`。範囲はファイルの読み込みと
同じで、外の値は失敗（丸めない）。`route` を付けた鎖は `recompute_routes` で経路を計算する（結果の `warnings` に出る）。

### 設定の変え方（set_params / create_node の settings）

- 形は `get_node` の `settings`（保存と同じ形）。**変えるキーだけ**書く。今の設定へ JSON Merge Patch で重ねる
  （配列は丸ごと置き換え、`null` はそのキーを消して既定値へ戻す）。
- 重ねた設定を保存と同じ読み込みに通し、書き戻して比べる。渡した値がそのまま残らなければ失敗にする:
  - 知らないキー（外して読んでも結果が同じキー）。
  - 使えない列挙名（例: `noise.type` に `"bogus"` → 「読み込むと "fbm" になる」）。
  - 読み込みで丸められた値。
- さらに、変えたキーが UI の範囲（`get_node` / `describe_node_kind` の `parameters` の `min` / `max`）の外なら失敗にする
  （エディタで選ぶと丸まる値）。もとから範囲の外にある値は咎めない。
- テクスチャ・マテリアル・モデルの参照は、読み込み済みのアセットの id（`list_assets`）で書く。
  まだ読み込んでいないアセットを読み込む操作は無い。

### 返事

成功:

```json
{"applied": true, "undoStep": true,
 "created": {"n1": 820, "b1": 822},
 "results": [{"op": "create_node", "node": 820, "key": "n1"}, {"op": "connect", "from": [33, "Mask"], "to": [822, "Foreground"]}, …],
 "warnings": []}
```

`undoStep` はアンドゥの段が増えたか。`select` と `arrange` だけの操作列では偽で、未保存にもならない
（`arrange` は次のフレームにグラフのパネルが行い、そちらで別の段を積む）。

失敗（`error`）: メッセージは `ops[添字]: 理由`、`data` は `{"applied": false, "failedOp": 添字, "op": "connect", "note": …}`。
グラフは操作列の前のまま（作りかけのノードも残らない）。UI の範囲の検査で失敗したときは `failedOp` が無い。

## get_evaluation と screenshot

- 「落ち着いた」は `--evaluate-report` と同じ判定（本体の評価が今の版に追いつき、後処理・雲と雪煙のマスク・
  道路の Base の地形・足跡・配置の点の数の読み戻しが済んだ状態）が 10 フレーム続いたこと。
  `wait: false` なら待たずに今の状態を返す。`timeout_seconds`（既定 120）で打ち切ると `timedOut` がエラーになる。
- `get_evaluation` の `log` / `errors` / `warnings` のログは、**直近の `apply_ops`（か `recompute_routes`）より後**に出た
  警告とエラーだけ。`nodes` は既定でエラーか統計のあるノードだけ（`full: true` で全部）。
- `screenshot`: `target` は `viewport`（UI なしの描画結果。`max_size` で長辺を縮める、既定 1600）か `window`（UI 込み、
  バックバッファの読み戻し）。アプリは原寸の PNG を `path` に書き、MCP サーバは長辺 `image_max_size`（既定 1568）の
  JPEG にしてツール結果の画像として返す（ウィンドウ全体の PNG は数 MB になるため）。`path` を省くと
  `%TEMP%\terrain-graph-mcp\` に書く。

## set_camera

`target_uv`（地形の上）か `target`（ワールド）で注視点、`eye_uv` + `eye_height_m`（地形からの高さ、既定 100）か
`eye`（ワールド）で目の位置を決めると、向きと距離はそこから求める。`distance` / `yaw_degrees` / `pitch_degrees` /
`fov_degrees` で直接も決められる。`frame_terrain: true` で地形全体を収める（A キーと同じ）。`bookmark` は保存済みの視点
（Ctrl + 数字で保存したもの）。使う人の画面も一緒に動く。

## 残っていること

- 計画の段階 3 の CLI（`--apply <ops.json>`）は未実装（同じ操作を起動中のアプリへ送る形を先に作った）。
- 地理データ（GeoJSON）の取り込みと、緯度経度 ↔ UV の変換（段階 2）。
- まだ読み込んでいないアセット（マテリアル・モデル）を読み込んで割り当てる操作。
- マテリアル・ライティング・プレビュー設定の編集（今はグラフとパスだけ）。
