# rhi — DirectX 12 ラッパの設計

作成日時: 2026-08-31 12:09
更新日時: 2026-08-31 12:09

`src/rhi/` の設計方針。実装は M1 で確定した。

## bindless を全面採用する

SM 6.6 の `ResourceDescriptorHeap` を使い、シェーダへはディスクリプタのインデックスを渡す。
起動時に Resource Binding Tier 3 とシェーダモデル 6.6 を要求し、
満たさない環境は起動を中止する。

レイヤー合成では 1 パスが多数のテクスチャを参照するため、
ディスクリプタテーブルをパスごとに組む方式は早晩破綻する。最初から bindless に寄せる。

シェーダ側は次のように引く。

```hlsl
RWTexture2D<float4> output = ResourceDescriptorHeap[g_layer.outputIndices.x];
```

## 全パス共通のグローバルルートシグネチャ

パスごとにルートシグネチャを作らず、次の 1 本を全パスで共有する。

| スロット | 内容 |
| --- | --- |
| `b0` | ルート定数 16 dword（テクスチャのインデックスや小さなパラメータ） |
| `b1` | ルート CBV（大きめの定数バッファ） |
| `s0` | スタティックサンプラ point clamp |
| `s1` | スタティックサンプラ linear clamp |
| `s2` | スタティックサンプラ linear wrap |
| `s3` | スタティックサンプラ aniso wrap |
| `s4` | スタティックサンプラ linear（U ラップ / V クランプ、equirect 用） |

フラグに `CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED` と `SAMPLER_HEAP_DIRECTLY_INDEXED` を立てる。

**`ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT` も必須。**
入力レイアウトを使うグラフィックス PSO は、このフラグが無いと
`CreateGraphicsPipelineState` が `E_INVALIDARG` で失敗する。原因が表に出にくい。

## フレームとリソースの寿命

- スワップチェーンは FLIP_DISCARD、3 バッファ。フレームスロットも 3。
- 各フレームスロットの待機値は「未使用なら 0」で表し、
  `m_nextFenceValue` の単調増加値を Signal する。
  スロットの初期値に 1 を入れる実装にすると、誰も Signal していない値を
  初回フレームで待って**確実にデッドロックする**。
- GPU がまだ参照している可能性のあるオブジェクトは `Device::Defer` に渡す。
  フレーム完了後に `DeletionQueue` が解放する。直接 `Reset` しない。
- 定数バッファなどの一時データは `UploadRing` から確保する。
  フレームごとに巻き戻る線形アロケータで、既定は 16 MB/フレーム。

## 初期化時の一発実行

`Device::ExecuteImmediate` は専用のコマンドリストへ記録して実行し、GPU の完了まで待つ。
メッシュの転送、テクスチャのアップロードとミップ生成、IBL の事前計算に使う。

**フレームの外でしか呼べない。** UI からの要求はフラグに積み、
次フレームの頭で `ProcessPendingWork` が処理する。

## シェーダ

- 実行時に DXC でコンパイルする。シェーダモデルは 6.6。
- `shaders/` をソースツリーのまま参照する。
  `TG_SHADER_DIR`（CMake が定義、環境変数で上書き可）を見る。
  実行ファイル横へコピーしないことで、起動したままの編集・再コンパイルが成立する。
- 更新は `shaders/` 配下のタイムスタンプ走査で検出し、PSO キャッシュを作り直す。
  破棄の前に必ず `WaitForGpu` する。
- **PSO の生成失敗もキャッシュする。** 記録しないと毎フレーム再コンパイルが走り、
  ログが埋まって原因が追いにくくなる。ホットリロード時にまとめて捨てて再挑戦する。

## リソースの状態

- 状態はリソース全体で 1 つだけ持つ。サブリソース単位の管理は未実装。
- 例外はミップ連鎖の生成。読むミップを読み取り状態、書くミップを UAV 状態にする必要があるため、
  そこだけサブリソース単位で遷移させ、`createMipSrvs` で作ったミップ別 SRV を使う。
  リソース全体を覆う SRV では状態が混在してしまう。

## PIX

`USE_PIX` は Debug / Release ともに常時定義する。
未定義だと `pix3.h` の呼び出しがコンパイル時に消え、
WinPixEventRuntime.dll への依存も消えて配置されなくなる。
性能は Release で測るものなので、Release で計測できないと意味がない。
