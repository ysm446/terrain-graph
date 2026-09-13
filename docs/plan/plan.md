# plan — 実装方針と優先順位

作成日時: 2026-08-31 05:46
更新日時: 2026-09-13 19:50

手続き雲は密度指定の形状複製・BVH・自動3Dベイク・密度フィールドの雲底切断まで実装済み。256個の固定上限を撤去し、形状とBVHは編集時に可変長GPUバッファへ転送する。Shapeを移動する雲トランスフォームと軸ギズモも追加済み。雲種（Humilis / Mediocris / Congestus）を選ぶ Cloud Shape Generate と、天候マップ駆動の Cloud Weather Layer を追加済み。Cloud Weather Layer は分布（帯状の伸び・塊の密度差・雲種によるスケール変化）、照明（in-scatter 確率・等方項・陰側の減衰）、距離 LOD、風に追従する照明キャッシュ、模様の変化まで実装済み。RDR2 系の雲システムへ向けた今後の予定は次の順。(1) 天候プリセットと遷移: 雲量・雲種・密度のセットを保存し、時間で補間して晴れ→曇り→嵐を連続に変える。天候マップの補間も含む。(2) 時間方向の再投影（実装済み）: 半解像度の代表画素と刻み内のサンプル位置を毎フレームずらし、前フレームの雲バッファを雲の平均距離で再投影して蓄積する。編集時は履歴を破棄する。(3) 遠景の扱い（実装済み）: 層を球殻状に曲げて地平線に沈ませ、遠景を 1/4 解像度の別パスで描く。(4) 地形連動: 標高・風上マスクから雲量・雲種を導く Mask ノードの組み合わせ例をサンプルとして用意する。(5) 降水・霧: 雲底からの降水の筋と地表付近の霧を天候マップの B チャンネル相当で扱う。Cloud Shape Generate 側の未対応は Bend・Fuse・点群入力からの複数生成。

雲塊ノードを新規追加メニューから撤去した。Cloud Layerも新規追加メニューから撤去した。新規の雲はCloud Map Generateまたは基本形状からCloud Noiseで作成する。既存の雲層はCloud Layer (Legacy)として読み込み・編集・描画を維持する。保存済みプロジェクトの読み込み・編集・描画は互換性のため維持し、既存ノードは「雲塊（旧形式）」と表示する。

モデルアセットはFBX読み込み、メッシュプレビュー、既存LOD選択、マテリアルスロット参照、保存・履歴まで対応。Crumbling Points → Model Scatter → Model Outputによるインスタンス配置にも対応。GPUの視錐台・距離カリングと描画対象の圧縮、間接描画まで対応。ScatterのPointsによる汎用のポイント散布にも対応。次は遮蔽カリング。自動LOD生成は後回しとする。

進捗管理の入口。実装の詳細な設計は [docs/design/](../design/) に置く。

- [design/rhi.md](../design/rhi.md) — bindless、ルートシグネチャ、リソース管理、シェーダ
- [design/atmospheric-sky.md](../design/atmospheric-sky.md) — 大気散乱スカイ・雲・既存モードとの互換性
- [design/volumetric-clouds.md](../design/volumetric-clouds.md) — ボリューム雲の現行仕様・懸念点・今後の方向性
- [design/rendering.md](../design/rendering.md) — 描画の流れ、露出とトーンマップ、IBL、行列の規約
- [design/compositing.md](../design/compositing.md) — チャンネル定義、ハイトブレンド、RNM、タイル評価
- [design/design-guide.md](../design/design-guide.md) — UI のレイアウト、配色、プロパティ行
- [design/mask-flowline.md](../design/mask-flowline.md) — Mask Flowline の入出力、設定、計算方法と制限
- [design/lake.md](../design/lake.md) — Lake の入出力、計算方法と制限
- [design/meandering-rivers.md](../design/meandering-rivers.md) — Meandering Rivers の入出力、計算、HDA との差分
- [design/node-graph.md](../design/node-graph.md) — ノードグラフのデータモデル、評価、エディタ UI
- [reference/nodes.md](../reference/nodes.md) — ノード 1 つずつの役割・ピン・パラメータ
- [reference/file-format.md](../reference/file-format.md) — `.tgproj` / `.tgmat` の形式

## 決定済みの技術選定

| 項目 | 選択 |
| --- | --- |
| 言語 | C++20 |
| ビルド | CMake + vcpkg（manifest モード） |
| グラフィックス API | DirectX 12（SM 6.6、bindless） |
| シェーダ | HLSL / DXC（実行時コンパイル、ホットリロード） |
| UI | Dear ImGui（docking ブランチ）+ imgui-node-editor |
| 合成モデル | **ノードグラフ**（ハイトマップとマスクを中心のデータとして流す） |
| 対象 OS | Windows のみ |

導入済みの依存: DirectX-Headers, DirectX-Agility-SDK, D3D12MemoryAllocator,
WinPixEventRuntime, DirectXShaderCompiler, stb, Dear ImGui, tinyexr, nlohmann-json。
ノードグラフ用に imgui-node-editor を追加する。

## 方針の転換（2026-09-02）

material-mixer（レイヤースタック方式）からフォークし、terrain-graph へ改名した。
**レイヤースタックは廃止し、合成の組み立てをノードグラフへ置き換える。**
グラフの仕組みは terrain-editor（`D:/GitHub/terrain-editor`）から移植する。

### 移植するのは「仕組み」であって、ノードそのものではない

terrain-editor から持ってくるのはノードグラフの**フレームワーク**で、
ノード（侵食などの中身）はコピーしない。ノードは terrain-graph の目的に合わせて
**独自に設計・実装していく**。terrain-editor のノード実装は、
似た機能を作るときの参考資料として扱う。
調査の詳細は [reference/terrain-editor-node-graph.md](../reference/terrain-editor-node-graph.md)。

仕組みとして持ってくるもの:

- グラフのデータモデル: ノード / ピン / リンク、ノード・ピン・リンク共通の単一 ID 空間
  （imgui-node-editor の ID にそのまま流用できる）。
- ノード単位のキャッシュ: `outputHash = Hash(inputHash, parameterHash, nodeId)` を
  次段へ伝播する増分ハッシュ連鎖。実データのハッシュは取らないので安価。
- imgui-node-editor によるノードエディタ UI（ノードのカード描画、ピン、リンク、
  右クリックの追加メニュー、コピー / ペースト）。

作り替えるもの（そのまま持ってこない）:

- **評価器**: terrain-editor は「第 1 入力だけを辿る線形チェーン（最大 16 段）+
  マスクは再帰 pull」で、DAG の合流を扱えない。全入力を再帰 pull する
  汎用評価（キャッシュは増分ハッシュのまま）に作り替える。リンク作成時に循環を弾く。
- **ノードの表現**: 全ノード種の設定を 1 構造体に持つ「ファット構造体」はやめ、
  設定を `std::variant` で持つ。terrain-editor はノードを 1 つ増やすのに
  7 か所の同時修正が必要だったが、**「設定構造体 + 登録テーブル + visitor の
  オーバーロード（評価 / ハッシュ / 保存 / プロパティ UI）」だけで増やせる形**にする。
- **シリアライズの kind**: enum の生 int ではなく名前（文字列）で書く
  （file-format.md の「列挙は名前で書く」に合わせる）。
- **GPU 評価**: terrain-editor は FXC (cs_5_0) + 生 D3D12 で毎回リードバックする。
  こちらは既存 RHI（DXC / SM6.6 / bindless / PipelineCache）へ載せ替える。
  まずは CPU カーネルで動かし、GPU 化は後続マイルストーンで行う。
- **プロパティ UI**: terrain-editor の PropertyWidgets ではなく、
  既存の `ui/UiStyle.h` の `Property*` ヘルパーに合わせる。
- **単位系**: terrain-editor はメートル前提（terrainSizeMeters）。
  こちらは既存コンポジタに合わせ、高さ 0〜1 の正規化グリッドを基本にする。
  実寸（m）を持つのは**ジオメトリだけ**（平面のサイズと変位量）。
  傾斜角のように実寸が要る計算は、評価器へスケールを渡す形で G3 の前に入れる。

## ディレクトリ構成

```
CMakeLists.txt / CMakePresets.json / vcpkg.json
src/
  app/          アプリ本体、エントリポイント、UI パネル
  graph/        ノードグラフ（データモデル、評価器、CPU カーネル、シリアライズ）
  compositor/   GPU 評価器（レイヤー列を合成）、テクスチャ / マテリアル / ペイントマスク
  core/         ログ、Win32 ウィンドウ、画像入出力、ファイル選択ダイアログ
  io/           プロジェクト (.tgproj) とマテリアル (.tgmat) の読み書き
  renderer/     PBR プレビュー描画、カメラ、メッシュ、IBL 環境
  rhi/          DX12 ラッパ
  ui/           Dear ImGui の統合、テーマとプロパティ行
  terrain/      ハイトマップ地形（G5 で追加）
shaders/        HLSL
assets/         同梱アセット（コミットする）
data/           手元のデータ置き場（コミットしない）
docs/
tests/
```

## マイルストーン

雲の現行仕様と方向性は [ボリューム雲](../design/volumetric-clouds.md)、実装履歴は [雲ノード](../design/cloud-nodes.md) に記録。
雲塊の編集と分布マスク付き雲層、照明調整、描画の軽量化は実装済み。地形Path・既存Volume同士の合成は未対応。別系統として [手続き雲の形状構築](../design/procedural-clouds.md)（3D直線・球配置・楕円体・形状マージ・ノイズ）を実験実装。

### 引き継いだ土台（material-mixer M0〜M6、完了）

- **M0〜M1**: CMake + vcpkg、Win32 + DX12、フレームループ、ImGui docking、
  RHI（D3D12MA、ディスクリプタ、アップロードリング、遅延破棄、PSO キャッシュ、
  DXC ホットリロード、bindless）。
- **M2**: PBR プレビュー（GGX 直接光 + IBL、EV100 露出、ACES、被写界深度、
  テセレーション、影、ディスプレイスメント）。
- **M3〜M4**: レイヤー合成（ハイトブレンド、RNM）、マスク生成
  （ノイズ / 中間結果 / テクスチャ / ペイント）。
- **M5**: マテリアル / テクスチャ / 天球のライブラリ、プロジェクト保存、
  フル解像度エクスポート（ORD / ORM パッキング）。
- **M6**: レイヤーの種類（サーフェス / シェイプ / 水面）。

このうちレイヤースタック（M3〜M4、M6 の大半）はノードグラフに置き換えられて消える。
プレビュー、ライブラリ、入出力、RHI は残る。

### G1 — ノードグラフ基盤の移植（2026-09-02 完了）

グラフが「動く仕組み」として立ち上がり、**いまのレイヤーと同じ見た目を
ノードで再現できる**ところまで。レイヤーパネルはまだ残す。
設計は [design/node-graph.md](../design/node-graph.md)。

- `src/graph/`: データモデル（Node / Pin / Link、単一 ID 空間）、
  ノード定義テーブル、循環を弾くリンク検証。
- 最初のノードは**既存のレイヤーのノード化**: サーフェス / シェイプ / 水面
  （設定は `MaterialLayer` そのまま）と出力。評価はグラフをレイヤー列へ
  コンパイルして既存の GPU 評価器へ流す。**描画結果はレイヤー方式と
  ピクセル単位で一致することを確認済み。**
- ノードエディタパネル（imgui-node-editor）: ノード描画、リンクの作成 / 削除、
  右クリックの追加メニュー、選択ノードのプロパティ（レイヤーパネルと共有）。
- グラフの保存: `.tgproj` に `graph` 節を追加（キーの追加のみなので版は上げない）。
- 未対応（G2 以降）: グラフ編集のアンドゥ、コピー / ペースト、
  マスク生成のノード化（合流のある DAG と、ノード単位の評価器はここで入れる）。

### G2 — グラフ中心の編集体験とレイヤー廃止（前半 2026-09-02 完了）

完了:

- **レイヤーパネルとレイヤースタックを撤去**し、合成の入口をグラフに一本化した。
- **アンドゥをグラフ編集に対応**（ノード / リンク / 設定 / 位置のスナップショット。
  移動だけでは段を積まない）。
- 形式を版 4 へ（`layers[]` 廃止。旧ファイルは同じ見た目のままグラフへ移行して読む）。

2026-09-02 追記: **地形スケール（第 1 段）**も入れた。平面のサイズ（m）を
変えられるようにし、カメラ・影・変位量の上限をそれに追従させた。
ハイトマップノード（入力を持たないソース）も追加。
**メートルなのはジオメトリだけ**で、テクスチャの UV とグラフのハイトは
無次元 / 正規化のままにする（[design/rendering.md](../design/rendering.md)）。

残り:

- コピー / ペースト、複製など編集操作を揃える。
- マスク生成をノード化する（ノイズ / 下地由来 / テクスチャ / ペイントを
  マスク型のピンで繋ぐ）。ここで合流のある DAG と、ノード単位の評価器
  （terrain-editor の増分ハッシュキャッシュ方式）を入れる。

### G3 — 侵食・地形系ノードの移植

terrain-editor の売りのノード群。まず CPU 実装をそのまま移植する。

- **Multi-Scale Erosion（侵食本体は 2026-09-09 実装）。** terrain-editor を参照せず、SIGGRAPH 2024 の論文から GPU 実装。高さ復元 / 多段 breaching も追加済み。
- Fluvial Erosion / Mask Fluvial。
- **Droplet Erosion（2026-09-04 完了）。** GPU 版（スナップショット方式）を解析グリッド +
  差分の足し戻しで移した。Flow / Deposit の Mask を出す。
- Sediment / Snow / Soil / Crumbling / Rock。
- **Scatter（2026-09-05 完了）。** 単純な形をばら撒き、分布と個体ごとの乱数の Mask を出す。
  1 スレッド 1 テクセルで、近くの散布点から一番強い形を採る。
- **River（段階 1 実装済み。2026-09-03）** — 川筋から河床を掘り、下流へ単調な
  水面を張って河原（岩・砂利を置く帯）のマスクを出す。設計と残り（湖岸の Bank）は
  [reference/river-node.md](../reference/river-node.md)。
- **Path / Mask Path（2026-09-04 完了）。** 地形の上に向き付きの線を引き、その足跡を
  マスクにする。River の Seed に繋いで川の出どころを線で決める、道や稜線の素材を
  貼る、が最初の使い道。設計は [design/node-graph.md の「パス」](../design/node-graph.md)。
  残り: パスに沿って掘る / 均すノード（河床、道路の切り土・盛り土、U 字谷）。
- **評価の非同期化（2026-09-04 完了）。** terrain-editor は CPU カーネルを `std::async` へ
  逃がしたが、こちらは全部 GPU なので**専用のコンピュートキュー**へ流す。出力を 2 組持ち、
  描画は前回の結果を読み続け、毎フレームフェンスをポーリングして終わったら入れ替える。
  ステータスバーに「合成を評価中…」を出す。
  設計は [design/compositing.md の「評価の非同期化」](../design/compositing.md)。

### G4 — GPU 評価

- 重いカーネルから順に、既存 RHI（DXC / SM6.6 / bindless）でコンピュート化する。
  terrain-editor の cs_5_0 シェーダは書式を SM6.6 へ直して `shaders/` に置く。
- ノード間を GPU 常駐テクスチャで繋ぎ、毎回のリードバックをやめる
  （terrain-editor は毎ノード読み戻していた）。

### G5 — 地形

ハイトマップインポート（16bit PNG / RAW / EXR）、LOD 付き地形描画、
マテリアルのスプラット適用、地形属性をグラフのマスク入力へ接続、地形ペイント。

## 開発用のコマンドライン

```
terrain_graph.exe [--project <path>] [--save-project <path>]
                   [--hdri <path>] [--texture <path>]...
                   [--export <dir>]
                   [--screenshot <path>] [--screenshot-ui <path>]
                   [--screenshot-frame <n>] [--select-node <id>]
```

`--texture` は繰り返し指定でき、起動時にテクスチャライブラリへ読み込む。

`--project` は起動時にプロジェクトを開く。`--save-project` は数フレーム描いてから
プロジェクトを保存して終了する。対話せずに保存と読み込みを往復させて確かめるための開発用。

`--export` は数フレーム描いてから合成結果を画像へ書き出して終了する。
書き出しの設定は書き出しウィンドウの既定値（2048、個別、全マップ）を使う。

`--screenshot` はビューポートの内容を、`--screenshot-ui` は UI 込みの
ウィンドウ全体を PNG に書き出して終了する。
画面キャプチャに頼らず結果を確認できるため、リモート環境や自動確認で使う。
`--select-node` は読み込んだグラフで指定 ID のノードを選択し、プロパティの画像確認に使う。

## 見つけている課題（未着手）

調べて原因まで分かっているが、まだ直していないもの。着手するときは
ここの記述だけで再現と判断ができるようにしておく。

### `起伏の強さ` が法線マップに効かない

`ComputeLayerNormal` は**法線マップがあると早期 return** する。そのため
法線マップを持つマテリアルでは、`起伏の強さ`（`heightGain`）を下げても
**形（Height）だけが平らになり、陰影は元の強さのまま**残る。
マップを持たないレイヤーでは両方に効くので、挙動が食い違っている。

- 直し方の案: 法線マップの xy に `heightGain` を掛けてから正規化する
  （傾きを gain 倍する、が実寸の意味に合う）。
- 経緯: 実寸化のときに「法線の強さ」パラメータを廃したので、
  残っているつまみは `起伏の強さ` だけ。ここが効かないのは筋が通らない。

### 起動直後の GPU ベースバリデーション（MaterialThumbnail）

`--project` でプロジェクトを開いて数フレームで撮ると、`MaterialThumbnail.hlsl` の Dispatch で
「Incompatible texture barrier layout（UAV なのに COMMON）」が 1〜2 回出ることがある
（2026-09-04、`data/path-test.tgproj` + `--screenshot-ui --screenshot-frame 6` で再現。
frame 12 では出ない）。マテリアルのサムネイル生成の状態遷移がフレームの早い時期に
噛み合っていない疑い。経路探索の作業とは無関係（変更を退避しても出る）。

## 判断を保留している点

- グラフ評価の解像度の持ち方。terrain-editor はグローバル 1 つ
  （プレビュー解像度）だった。ノードごとに持つか、当面グローバルのままにするかは
  G2 で決める。
- Path ノードは **2026-09-04 に独自設計で実装した**（2D 座標 + 向き付きエッジ、
  ビューポートで編集、Mask Path で足跡をマスクに）。terrain-editor の実装
  （main.cpp と密結合）は写していない。「パスに沿って掘る / 均す」ノードは未着手。
- 地形 LOD の方式（CDLOD / ジオメトリクリップマップ / GPU 駆動クアッドツリー）は
  G5 着手時に決める。
- リソース状態の管理単位。現在はリソース全体で 1 つしか持たない
  （ミップ連鎖の生成だけ例外的にサブリソース単位）。
  ミップごとに別状態へ遷移させたい場面が増えたら本格対応する。
- 自動露出と AgX トーンマップ。必要になった時点で追加する。
- 配布形態（シェーダの同梱方法）の見直し（旧 M5d）。グラフ移行後に再検討する。

## 雲ノードの動作確認

局所雲の移動・模様の移流と、地形再評価を伴わないパラメータ編集に対応。
`--screenshot-count N --screenshot-interval F` を `--screenshot` と併用すると、
F フレーム間隔で N 枚を `指定名_0.png` から連番保存する。既定は従来どおり 1 枚。

描画負荷は `--project <path> --benchmark-frames N` で計測できる。
起動後 120 フレーム以上かつ評価完了後に N フレームを計測し、ms/frame と FPS をログへ出して終了する。
計測時のみ VSync とフレーム制限を無効化し、保存設定は変更しない。
`--cloud-reference` を加えると雲層の照明キャッシュを無効化し、同じ実行ファイルで従来計算と比較できる。


Cloud Shape Generate (Experimental) を追加。入力なしで単独の積雲を球の集合として生成し、Shape を Cloud Replicate / Cloud Merge / Cloud Noise へ接続できる。雲種（Humilis / Mediocris / Congestus）で土台の厚さと塔の高さ・本数を切り替え、中心XYZ・サイズ（基本半径）・長さ／幅の倍率・球の間隔・乱れ・下側の切り取り・回転・半径のランダム化・二次形状（繰り返し1〜3、押し出し、広がり）・つなぎの滑らかさ・シードを持つ。土台は楕円体内の乱した格子、塔は先端へ細くなる球列、二次形状は上半球方向へ積む子球。Houdini の Cloud Shape Generate を参考にした構成で、Bend と Fuse は未対応。保存識別子は `cloudShapeGenerate`、設定は `proceduralCloud` 節。

Cloud Map Generate (Experimental) を追加。10km四方・400点・接続距離500m・雲底厚さ200mを既定とし、範囲内の一様ランダム点から近傍接続、扁平な雲底、上向きの球列を生成する。Shape を Cloud Replicate / Cloud Noise へ接続できる。

## モデルアセット

まずアセット帯のモデルエリアを追加する。後続でメッシュ・UV・マテリアルスロットの読み込み、既存マテリアルアセットの割り当て、インスタンス配置を実装する。自動LODは後回しとする。
