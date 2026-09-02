# terrain-editor ノードグラフ調査

作成日時: 2026-09-02 12:20
更新日時: 2026-09-02 12:20

ノードグラフ移植（[plan.md](../plan/plan.md) の G1〜G4）の元になる、
terrain-editor（`D:/GitHub/terrain-editor`）の実装調査。
移植方針（持ってくるもの / 作り替えるもの）は plan.md 側にまとめてあり、
ここは元実装の事実を記録する。

## 全体像

- ハイトフィールド中心のノードベース地形エディタ。C++20 / Win32 + D3D12 /
  Dear ImGui + imgui-node-editor / nlohmann_json。単一 exe。
- コアの名前空間は `rock::`（岩生成ツール時代の名残）。UI / GPU 補助は
  `terrain::ui` / `terrain::gpu` / `terrain::d3d12`。
- `docs/nodes/` に各ノードのドキュメント（`*_node.md` + `*_algorithm_guide.md`）が
  揃っており、**アルゴリズムの仕様書として移植時に有用**。

## コア（UI / D3D12 非依存）

- `src/node_graph.h`（約 1240 行）: `NodeKind` / `PinKind` / `ValueType` /
  `NodeDefinition` / `Pin` / `Node` / `Link` / 各ノードの Settings 構造体 /
  `HeightfieldGrid` / `MaskGrid` / `ColorGrid` / `NodeGraph` クラス。依存は STL のみ。
- `src/node_graph.cpp`(約 4190 行): ノード定義テーブル
  (`kNodeDefinitions`、24 種)、パラメータハッシュ、キャッシュ付き評価、
  一部ノードの CPU 実装。
- **ID 管理**: `using GraphId = int` の単一 ID 空間。ノード・ピン・リンクすべて同じ
  カウンタから採番。読み込み時は全 ID の max+1 に復元。
  imgui-node-editor の `ed::NodeId / PinId / LinkId` へそのまま流用できる。
- **ピン**: `{ id, nodeId, kind(Input/Output), valueType, label }`。
  `ValueType` = Mesh / HeightField / Mask / ColorTexture / Path。
  接続可否は「別ノード + valueType 完全一致 + Output→Input」のみ。
  入力ピンは 1 本だけ（新規接続で既存を置換）。
- **Node はファット構造体**: 全 25 個の Settings をメンバに持つ。
  ノード追加時に Node / パイプライン / ハッシュ / 適用 / シリアライズ /
  プロパティ UI の 7 箇所を同時に触る必要がある（移植では整理する）。

## 評価器

- **トポロジカルソートは無い。** 2 系統の pull 型評価:
  - Heightfield: ターゲットから**第 1 入力ピンだけ**を辿って上流へ一直線に遡り
    （最大 16 段）、ソースノードに到達したら反転して線形オペレーション列にする。
    DAG の合流は扱えない（2 本目以降の入力はマスクとして別経路で評価）。
  - Mask: 再帰 pull（深さ 16 で打ち切り）。Mask Blend は 2 入力を個別に再帰。
    ハイト由来マスクに当たるとハイト側のパイプライン評価へ橋渡しする。
- **キャッシュ**: ノード ID をキーに 4 本の `unordered_map`
  （heightfield / mask / color / mesh）。有効判定は
  `(inputHash, parameterHash, resolution)` の一致。
  `outputHash = HashCombine(inputHash, parameterHash, nodeId)` を次段の
  `inputHash` として伝播する**増分ハッシュ連鎖**。実データのハッシュは取らない。
  パラメータハッシュは Settings 構造体ごとに手書きで、
  **新ノードで書き忘れるとキャッシュが誤ヒットする**構造。
- **非同期実行**: グラフを丸ごとコピーして `std::async`、毎フレーム future を
  ポーリングして結果をマージ。評価中ノード ID を `std::atomic` で公開し、
  UI が「計算中」バッジを描く。内部は `std::execution::par` で行並列。
- **GPU はノード単位のオプトイン**: Settings の `backend` enum で選び、
  CPU 実装の冒頭で GPU 関数ポインタを試して失敗したら CPU へフォールスルー。
  コアからの結合は関数ポインタ 14 本で、コア層は D3D12 を include しない。

## CPU カーネル（`src/evaluation/`、STL のみ）

| ファイル | 行数 | 内容 |
| --- | --- | --- |
| HeightmapSource.cpp | 363 | 画像ハイトマップ読み込み（WIC 依存。移植では ImageIo に置換） |
| ShapeSource.cpp | 84 | Hemisphere / Pyramid / Box |
| HeightfieldOps.cpp | 366 | Blur、Mask Curvature / Slope / Height |
| MaskOps.cpp | 244 | Mask のリサンプル / Blend / Levels / Blur |
| MaskNoise.cpp | 148 | Perlin / fBm |
| MultiScaleErosion.cpp | 425 | Schott et al. SIGGRAPH 2024 の CPU 移植 |
| FluvialErosion.cpp | 307 | フォースフィールド粒子輸送 |
| DropletErosion.cpp | 225 | 水滴侵食 |
| Sediment.cpp | 271 | GeoGen 風土砂スライド |
| Snow.cpp / Soil.cpp / GranularSettle.cpp | 69/117/301 | 積雪 / 表土（共通コア） |
| Rock.cpp | 458 | Voronoi 散布岩 |
| Colorize.cpp | 126 | グラデーション着色 |

## ノード種類（24 種）

- Heightfield: Import Heightmap / Shape / Heightmap From Mask / Heightmap Blur /
  Multi-Scale Erosion / Fluvial Erosion / Droplet Erosion / Crumbling / Rock /
  Scatter / Sediment / Snow / Soil
- Mask: Mask Noise / Mask Blend / Mask Levels / Mask Blur / Mask Height /
  Mask Slope / Mask Curvature / Mask Fluvial / Mask Path
- Color: Colorize
- Path: Path（3D ビューポートでの線編集と密結合。移植優先度低）

## UI

- vcpkg の `imgui-node-editor`（`namespace ed = ax::NodeEditor`）。
  docking 版 ImGui と共存できる。
- ノードエディタ UI は `main.cpp`（13184 行のモノリス）内:
  `DrawNodeGraph()`（エディタ本体 + 右クリック追加メニュー）、
  `DrawRockNode()`（カード描画、入出力ピン列、出力ラベルクリックでプレビュー切替）、
  評価中の脈動枠、影、ドットグリッド背景、Ctrl+C/V のコピー / ペースト。
- `ed::Config::SettingsFile = nullptr` にして位置は自前保存
  （保存時に `ed::GetNodePosition` で吸い出す）。
- 補助: `src/ui/NodePins.cpp`（ValueType → ピン色、丸ピン描画）、
  `NodeIcon.cpp`、`NodeProperties.cpp`（約 2540 行。NodeKind の switch で
  ノード別プロパティへ分岐）、`PropertyWidgets.cpp`（行ウィジェット）。
- アンドゥは nodes / links / nodePositions / 選択 / プレビュー状態を
  まとめてスナップショットする方式。

## シリアライズ

- `.terrainproj`（JSON）。`format="terrain_editor_project"`、
  nodes[] / links[] / nodePositions{} / settings / viewport。
- `kind` / `valueType` は **enum の生 int**（歴史的な飛び番あり）。
  移植では file-format.md の方針どおり名前（文字列）で書く。
- ノード位置はグラフ本体に含めず、保存時に UI から吸い出して別キーに書く。

## GPU（D3D12）

- HLSL / cs_5_0 を `D3DCompileFromFile`（FXC）で実行時コンパイル。DXC / SM6 未使用。
- ほぼ「1 ノード = 1 シェーダファイル」。`shaders/*.hlsl` にフラット配置。
- `src/gpu/*Compute.cpp`（各 300〜620 行）が毎回 CommandAllocator / CommandList /
  DescriptorHeap / RootSignature / PSO を自前生成する重複だらけの作り。
  UPLOAD → DEFAULT(UAV) → READBACK で**毎ノード読み戻す**
  （GPU 常駐でノード間を繋がない）。
- ワーカースレッドからの GPU 要求はメインスレッドへキューでマーシャリングし、
  フェンス待ちで同期する。
- → terrain-graph では既存 RHI（DXC / SM6.6 / bindless / PipelineCache /
  UploadRing / DeletionQueue）へ載せ替える（plan.md G4）。
