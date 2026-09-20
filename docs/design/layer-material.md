# レイヤーマテリアル

作成日時: 2026-09-20 11:48
更新日時: 2026-09-20 11:51

## 目的と使い方

road-material-editor の `.tglayer` を、Surface が参照できる共有マテリアルとして移植する。
地形グラフのレイヤースタックを復活させず、素材内部の合成を独立したノードグラフとして扱う。
Road、Road Path、道路メッシュは今回の対象外。

アセット欄の作成メニューからレイヤーマテリアルを作成し、ダブルクリックで開く。
素材・マスク・合成・出力を接続し、Surface のマテリアル欄で割り当てる。
「層を追加」で合成に必要なノードをまとめて追加できる。保存は通常の Ctrl+S を使う。
既存の `.tglayer` と参照する `.tgmat`・テクスチャを同じルート内に置いて読み込むこともできる。

## データと参照

ファイルは `format: terrain-graph.layer-material-asset`、`version: 1`。
`name`、`displacement`（m）、`layerBlendRange`、`materials`、任意の `materialGraph` を保持する。
素材参照は通常の `.tgmat` への `{uid, path}`、定数素材は `null`。
グラフは `nextId` と `nodes` を持ち、ノードは `id`、`kind`、`inputs`、`position`、`settings` を持つ。
素材・マスク・合成・出力の種類番号、設定名は移植元を維持する。
読み込み時に固定 UID 参照を実行時 ID に変換し、保存時に戻す。
共有素材を先に保存してから、その参照を使って `.tglayer` を保存する。
古い埋め込みプロジェクトの読み込みを維持し、新規の埋め込み保存形式は version 5 とする。
`.tgmat`、共有アセット、シーンのバージョンは変更しない。

`SurfacePresetGraph` が接続・循環・型・上限を検証して合成順をコンパイルする。
最大 4 層・32 ノード。合成の上層は素材ノードに限り、下地を連鎖させる。
レイヤーマテリアル同士の入れ子は非対応。Model の素材欄には表示しない。
内部グラフもアンドゥ用スナップショットの対象とする。

## 描画

`LayerMaterialGpu` をフレーム用アップロード領域に載せ、`LayerMaterial.hlsli` で直接評価する。
固定解像度への事前ベイクは行わない。Surface、素材プレビュー、サムネイルで同じ評価関数を使う。
BaseColor / Roughness / Metallic / AO / Height を合成し、Normal は RNM で合成する。
合成後の Height の勾配も実寸の変位量から法線に反映する。

通常座標は Surface の UV を材質内の距離として扱い、1 UV を 1 m として繰り返し長で割る。
Surface のタイル数と UV Path が適用される。ワールド座標を選んだ素材と WorldNoise は地形の実寸 XZ を使う。
内部の合成幅・高さゲートと、Surface が下地の地形へ重ねる合成は独立する。
変位は `(合成Height - 0.5) * displacement` を地形の高さ単位へ変換する。
内部マテリアルの高さに関わる設定変更は、後段のマスク等のキャッシュにも反映する。

Constant / LengthNoise / WorldNoise のマスクを使用できる。
ノイズは terrain-graph の GPU 評価を使うため、移植元の CPU ノイズと模様が完全一致する保証はない。
WheelTracks / EdgeFalloff の設定はファイルに保持するが、道路座標がない Surface ではエラーを表示する。
道路固有のマスク評価は Road 移植時の課題とする。

## UI と検証

既存のマテリアルプレビュー内に内部グラフと選択ノードの設定を表示する。
レイヤーマテリアルでは球プレビューを縮め、グラフの操作領域を確保する。
アセット欄のサムネイルは模様が分かる平面表示とする。

検証用データと画面は `data/Test/layer-material-qa/`（Git 管理外）。
グラフ・不正入力・参照変換・UID による素材移動・保存往復・入れ子拒否は `layer_material_tests` で検証する。
Debug ビルド、CTest 6 件、DXC の CompositeLayer（CsMain / CsMaskThumbnail）・MaterialThumbnail・MaterialSphere の計4エントリが成功。
通常 Debug 起動で Surface の色合成、ハイト由来の起伏・陰影、編集 UI を確認した。
移植元の定数素材 `.tglayer` の読み込みと、実アプリでのシーン・埋め込みプロジェクト version 5 の保存／再読み込みも確認した。
GPU-based validation はシェーダ／PSO 準備に長時間を要したため中断しており、完了していない。
Release ビルド、実操作による全編集経路・アンドゥ、道路描画との比較は未確認。
