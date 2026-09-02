# node-graph — ノードグラフの設計

作成日時: 2026-09-02 12:50
更新日時: 2026-09-02 15:05

`src/graph/` とグラフパネル（`src/app/ApplicationGraphPanel.cpp`）の設計。
terrain-editor から移植したのは**仕組み**であって、ノードの中身ではない
（[plan.md](../plan/plan.md) の「方針の転換」を参照）。

## データモデル（`graph/NodeGraph.h`）

- **単一 ID 空間**: ノード・ピン・リンクはすべて同じカウンタ（`GraphId = int`）から
  採番する。imgui-node-editor の `NodeId / PinId / LinkId` にそのまま渡せる。
  読み込み時は全 ID の max+1 から採番を再開する。
- **ピン**: `{ id, nodeId, kind(Input/Output), valueType, label }`。
  接続できるのは「別ノード + 型一致 + 出力→入力」で、**循環になる接続は
  `CanCreateLink` が弾く**（下流を DFS して生産側に届くか見る）。
  入力ピンは 1 本だけ。新しい接続は既存を置き換える。
- **設定は variant**: ノード種類ごとの設定構造体を `NodeSettings`
  （`std::variant`）で持つ。terrain-editor の「全種類の設定を 1 構造体に持つ
  ファット構造体」はやめた。
- **位置はノードが持つ**（`posX / posY / positionValid`）。terrain-editor は
  UI 側だけが持っていて保存時に吸い出していたが、こちらはデータモデルに含めて
  保存も素直にした。エディタとの同期はグラフパネルが行う
  （描画後に `ed::GetNodePosition` で書き戻し、読み込み後は
  `RequestGraphNodePlacement()` で流し込む）。

## ノードの種類（現状）

| 種類 | 保存名 | ピン | 設定 |
| --- | --- | --- | --- |
| サーフェス | `surface` | 下地(入力) / 結果(出力) | `MaterialLayer`（kind=Surface） |
| シェイプ | `shape` | 同上 | `MaterialLayer`（kind=Shape） |
| 水面 | `liquid` | 同上 | `MaterialLayer`（kind=Liquid) |
| 出力 | `output` | マテリアル(入力) | なし |

サーフェス / シェイプ / 水面は**旧レイヤーそのもの**をノード化したもの。
設定構造体は `compositor::MaterialLayer` をそのまま持ち、プロパティ UI は
`Application::DrawLayerSettings`（旧レイヤーパネルから切り出したもの）。
**レイヤーパネルとレイヤースタックは廃止済みで、合成の入口はグラフだけ。**
旧形式のプロジェクトは読み込み時にチェーンへ移行される（file-format.md の版 4）。

## 評価 — レイヤー列へのコンパイル

グラフ独自の評価器は（まだ）持たない。`NodeGraph::CompileLayers()` が
出力ノードの「マテリアル」入力から「下地」チェーンを遡り、
**レイヤー列（下から上）へ落とす**。それを `MaterialStack` に入れて
既存の GPU 評価器（`MaterialEvaluator`）へ流すので、
**合成規則も見た目もレイヤー時代と完全に一致する**。

- Application は `m_graph.Revision()` とプレビュー対象を見て、変わったときだけ
  再コンパイルする（`SyncGraphStack()`）。コンパイルはレイヤー設定のコピーだけなので安価。
- **ノードを選ぶと、そのノードまでのチェーンがプレビューになる**
  （`CompileLayersTo()`。terrain-editor と同じ作法）。選択を外す、または
  出力ノードを選ぶと、出力ノードのチェーンへ戻る。書き出しも同じ対象を使う
  （見えているものを書き出す）。
- レンダラと書き出しは常にコンパイル結果のスタック（`m_graphStack`）を使う。
- 出力ノードに何も繋がっていなければ下地 1 枚（`MakeBaseLayer`）で補う。

マスク生成のノード化や合流（DAG）を持つノードを入れる段階で、
ノード単位の評価器（terrain-editor の増分ハッシュキャッシュの方式）へ広げる。

## ノードを 1 種類増やす手順

1. `graph/NodeGraph.h` — 設定構造体を定義し、`NodeSettings` の variant へ足す。
2. `graph/NodeGraph.cpp` — ピン構成の constexpr 配列と `kNodeDefinitions` の行を足す。
   `CreateNode` の設定の初期化に分岐が要るなら足す。
3. `io/ProjectIo.cpp` — `WriteGraph` / `ReadGraph` に設定の読み書きを足す。
4. `app/ApplicationGraphPanel.cpp` — 追加メニューの項目と、
   プロパティペインの設定 UI を足す。
5. 評価に関わるなら `CompileLayers`（または将来の評価器）へ意味を足す。

## UI（グラフパネル）

- imgui-node-editor（vcpkg `imgui-node-editor`）。docking 版 ImGui と共存できる。
- エディタのコンテキストは `ed::Config::SettingsFile = nullptr` で作る
  （位置は自前で保存するため。エディタの json は使わない）。
  パン操作は中ボタン（`NavigateButtonIndex = 2`）。
- 背景はドットグリッドを自前で描く（既定のグリッド線は透明にする）。
- ノードのカードは「種類色の印 + レイヤー名 + 種類名」のヘッダと、
  左に入力ピン列 / 右に出力ピン列（24px ピッチ）。丸ピンは
  `ed::PinRect / PinPivotRect` で当たり判定を指定する。
- パネルは上下 2 段（上: エディタ / 下: 選択ノードのプロパティ）。
  境界は `ui::HorizontalSplitter`。
- **既定の置き場所は右カラムの前面タブ**（グラフ / レイヤー / …）。
  中央のタブにするとビューポートと排他になり、ノードを選んだ結果を
  プレビューで確かめられない。盤面の広さより「繋ぎ替えて見た目を確かめる」
  往復を優先した。
- ノードの追加は背景の右クリック（追加位置は右クリック地点。視界内へクランプする）。
  削除は選択して Del（エディタ既定）。A でグラフ全体を画面に収める
  （ホバー判定は `ed::Begin` より前のスクリーン座標で取る）。
- **`ed::NavigateToContent` はノードを描いた後に呼ぶこと。** 内容の矩形は
  そのフレームで live なノードから計算されるため、描画前に呼ぶと空矩形で
  何も起きない。さらにエディタはキャンバスのサイズ変化のたびに前の表示領域を
  復元する（`ed::Begin` 内）ので、ドック確定を待ってサイズが安定した
  フレームで呼ぶ（`m_graphNavigateCountdown`）。
- **エディタのフレーム内では `io.MousePos` がキャンバス座標に差し替えられている**
  （imgui_canvas が Begin で変換する）。フレーム内の `ImGui::GetMousePos()` は
  そのままキャンバス座標として使えるし、**使わなければならない**。
  `ed::ScreenToCanvas` を重ねると二重変換になり、ノードが視界の外へ飛ぶ
  （実際に踏んだ）。スクリーン座標が要る判定（ホバーなど）は Begin より前に取る。
- **`ed::GetNodePosition` は知らないノードに `(FLT_MAX, FLT_MAX)` を返す。**
  右クリックで作ったばかりのノードは、そのフレームではまだエディタに
  登録されていない（描画は次のフレーム）。フレーム末尾の位置の書き戻しが
  この FLT_MAX を信じて保存すると、次の流し込みでノードが無限遠へ飛び、
  **キャンバスの座標計算が壊れて操作不能になる**（実際に踏んだ）。
  書き戻し・流し込み・ファイル読み込みの 3 か所で座標を検証している
  （`IsValidNodePosition`。非有限と |v| > 1e6 を弾き、壊れた座標は
  ビュー中央へ置き直す）。

## 制限（既知・今後の対応）

- **アンドゥは対応済み**（ノード / リンク / 設定 / 位置を文書スナップショットで持つ）。
  ただし**ノードの移動だけでは段を積まない**。位置は毎フレーム UI から返ってくるため、
  移動を段にすると 1 ドラッグが無数の段になる。位置は他の変更の段に相乗りする。
- コピー / ペースト（Ctrl+C/V）は未対応。
- 「下地」チェーンは線形（合流なし）。マスクをノードで作れるようになったとき、
  マスク入力ピンで DAG になる。
- ペイントの対象は「グラフ有効時は選択中ノードのレイヤー」
  （`Application::CurrentPaintLayer`）。
- 選択（= プレビュー対象）は保存しない。開き直すと出力ノードのチェーンから始まる。

## ファイル形式

`.tgproj` の `graph` 節（版 4 から合成の唯一の持ち主）。詳細と旧 `layers[]`
からの移行規則は [reference/file-format.md](../reference/file-format.md) の
「ノードグラフ」「版 4」を参照。
