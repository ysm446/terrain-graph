#pragma once

#include "compositor/MaterialLibrary.h"
#include "compositor/MaterialStack.h"
#include "compositor/PaintMask.h"
#include "compositor/TextureLibrary.h"

#include <vector>
#include "rhi/ComputeQueue.h"
#include "rhi/Device.h"
#include "rhi/PipelineCache.h"

namespace tg::compositor {

// 合成結果のチャンネルセット。plan.md の定義に対応する。
struct MaterialTextureSet {
    rhi::GpuTexture baseColor;  // R11G11B10_FLOAT
    rhi::GpuTexture normal;     // R16G16_FLOAT（xy のみ、z は再構成）
    rhi::GpuTexture surface;    // R8G8B8A8_UNORM（R=Roughness, G=Metallic, B=AO）
    // R32_FLOAT。R16 だと 0〜1 の全幅が標高差なので、600 m の地形で 1 ULP が約 0.3 m。
    // ブラー / 堆積の後にここから作り直す法線が階段になるため 32bit にした。
    rhi::GpuTexture height;

    bool IsValid() const { return baseColor.IsValid(); }
};

// 川筋（フロー累積）マスクの作業リソース。**合成解像度とは別のグリッド**で
// 計算する（反復回数が解像度に比例するので、川筋の形が決まる粗さで足りる）。
// 使うレイヤーが 1 枚も無ければ作らない。
struct FluvialResources {
    // 作業用。**川筋 1 本ずつ順に使い回す**（同時に 2 本は走らない）ので 1 組でよい。
    // 一番大きいグリッドに合わせて作り、小さい川筋はその左上だけを使う。
    rhi::GpuTexture heights;         // R32_FLOAT 解析用ハイト
    rhi::GpuTexture heightsScratch;  // R32_FLOAT ぼかし / 窪み埋めの二重バッファ
    rhi::GpuTexture weights0;        // RGBA32_FLOAT 配分重み k=0..3
    rhi::GpuTexture weights1;        // RGBA32_FLOAT 配分重み k=4..7
    rhi::GpuTexture accumA;          // R32_FLOAT 流量（ヤコビ反復の ping-pong）
    rhi::GpuTexture accumB;
    rhi::GpuTexture maxScratch;      // R32_UINT 1x1 正規化用の最大値（InterlockedMax）
    uint32_t workResolution = 0;

    bool IsValid() const { return heights.IsValid(); }
};

// 堆積（Sediment）の作業リソース。**合成解像度とは別のグリッド**で回し、
// 結果は差分として合成解像度へ足し戻す（反復回数がそのまま効くため）。
struct SedimentResources {
    rhi::GpuTexture bedrock;    // R32_FLOAT 動かない基盤
    rhi::GpuTexture sediment;   // R32_FLOAT 可動な土砂（Mask 出力の元）
    rhi::GpuTexture outgoing;   // RGBA32_FLOAT 4 方向への流出
    rhi::GpuTexture original;   // R32_FLOAT 始まりの高さ（差分を出すため）
    rhi::GpuTexture maxScratch; // R32_UINT 1x1 厚みの最大値（正規化に使う）
    uint32_t resolution = 0;

    bool IsValid() const { return sediment.IsValid(); }
};

// 積雪（Snow）の作業リソース。堆積と同じく**合成解像度とは別のグリッド**で回し、
// 結果は積雪厚として合成解像度へ足し戻す。雪は下地を削らないので、
// 「元の高さ」は基盤そのもの（堆積の original に当たる枚は要らない）。
struct SnowResources {
    rhi::GpuTexture base;       // R32_FLOAT 下地のハイト（雪を除いた面）
    rhi::GpuTexture thickness;  // R32_FLOAT 積雪厚（Mask 出力の元）
    rhi::GpuTexture outflow;    // RG32_FLOAT 流出（x = 量、y = 向き + 1）
    rhi::GpuTexture scratch;    // RG32_FLOAT ならしの作業用（x = 雪面、y = 元の厚み）
    uint32_t resolution = 0;

    bool IsValid() const { return thickness.IsValid(); }
};

// 河川（River）の作業リソース。川筋と同じく**合成解像度とは別のグリッド**で回し、
// 掘った河床は差分として、水面は置き換えとして合成解像度へ書き戻す。
// マスクの元になる値（水面高 / 水際からの距離 / 掘った地形 / 湖の深さ / 半幅）は
// 解析グリッドの R32 で持つ（合成の Height は R16 で、水面の勾配を持てない）。
// 水面の被覆と水深だけは合成解像度で持つ（島の縁を合成解像度で出すため）。
struct RiverResources {
    rhi::GpuTexture heights;     // R32_FLOAT 解析用ハイト（ならした地形）
    rhi::GpuTexture scratch;     // R32_FLOAT ぼかし / 窪み埋めの二重バッファ
    rhi::GpuTexture surface;     // R32_FLOAT 窪みを埋めた面 = 水面高
    rhi::GpuTexture weights0;    // RGBA32_FLOAT 配分重み k=0..3
    rhi::GpuTexture weights1;    // RGBA32_FLOAT 配分重み k=4..7
    rhi::GpuTexture accumA;      // R32_FLOAT 流量（ヤコビ反復の ping-pong）
    rhi::GpuTexture accumB;
    rhi::GpuTexture width;       // R32_FLOAT 中心線セルの半幅（セル）。川でなければ 0
    rhi::GpuTexture jfaA;        // RGBA32_FLOAT JFA（xy = 種の座標, z = 評価値, w = 有効）
    rhi::GpuTexture jfaB;
    rhi::GpuTexture maxScratch;  // R32_UINT 1x1 流量の最大値
    rhi::GpuTexture waterLevel;  // R32_FLOAT 水面高（川の外は最寄りの川の水面高）
    rhi::GpuTexture distance;    // R32_FLOAT 水際からの距離（m。内側は負）
    rhi::GpuTexture ground;      // R32_FLOAT 掘った後の地形
    rhi::GpuTexture lakeDepth;   // R32_FLOAT 埋めた面 − 地形（湖の深さ）
    rhi::GpuTexture halfWidth;   // R32_FLOAT 最寄りの川の半幅（m）
    rhi::GpuTexture waterFine;   // R16_FLOAT 水面の被覆（合成解像度）
    rhi::GpuTexture depthFine;   // R16_FLOAT 水深（合成解像度。河床の深さで 1）
    uint32_t resolution = 0;
    uint32_t fineResolution = 0;

    bool IsValid() const { return surface.IsValid(); }
};

// 崩落（Crumbling）の作業リソース。**合成解像度で回す**（岩片は m 単位の
// 小さな形なので、粗いグリッドでは形にならない）。
//
// 積む先は 1 枚の R32_UINT にパックする。上位 20 bit が岩屑の高さ、
// 下位 12 bit がその岩片ごとの乱数。`InterlockedMax` 1 回で
// 「一番高い岩片が勝ち、その乱数も一緒に残る」が成り立つ。
struct CrumblingResources {
    rhi::GpuTexture packed;  // R32_UINT （高さ 20bit | 乱数 12bit）
    uint32_t resolution = 0;

    bool IsValid() const { return packed.IsValid(); }
};

// 評価する出力領域。全体を 1 回で評価するときは矩形に全体を渡す。
struct TileRect {
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

// レイヤースタックを GPU で評価する。
//
// 評価は「出力タイル矩形と解像度」を引数に取る形で固定する。
// 編集中はプレビュー解像度で全体を 1 パス、エクスポート時はフル解像度を
// タイル分割して順に評価する、という二段構えを最初から通すため。
//
// **プレビューの評価は非同期。** `asynchronous` で作ると出力を 2 組持ち、
// 評価は専用のコンピュートキュー（`rhi::ComputeQueue`）へ流す。描画は前回の結果
// （表側）を読み続け、終わった時点で裏側と入れ替える（`Update`）。
// 河川のように数千回ディスパッチする加工でも UI が止まらない。
// 書き出し用は同期（`Evaluate` を直接呼び、1 組だけ持つ）。
class MaterialEvaluator {
public:
    // asynchronous: 出力を 2 組持ち、評価をコンピュートキューへ流す（プレビュー用）。
    bool Create(rhi::Device& device, uint32_t resolution, bool asynchronous = false);
    void Destroy(rhi::Device& device);

    bool Resize(rhi::Device& device, uint32_t resolution);

    // 指定したタイル群を評価する。呼び出し前後の状態遷移もここで行う。
    //
    // ループはレイヤー優先。1 レイヤーぶんを全タイルで終えてから次へ進む。
    // 中間結果由来のマスクは近傍を参照するため、タイル優先で回すと
    // まだ評価されていない隣のタイルを読んでしまい、境界に継ぎ目が出る。
    // 全レイヤー・全タイルを記録できたら true。false のときは結果が半端なので、
    // 呼び出し側は「評価済み」にせず、次のフレームで評価し直すこと。
    bool Evaluate(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                  ID3D12GraphicsCommandList* commandList, const MaterialStack& stack,
                  const TextureLibrary& textures, const MaterialLibrary& materials,
                  const PaintMaskStore& paintMasks, const std::vector<TileRect>& tiles);

    // 毎フレーム呼ぶ。スタックに変更があれば評価を投入し、終わった評価があれば
    // 結果を表側へ入れ替える。全体はタイルに分割して評価する。エクスポート時と
    // 同じ経路を常に通しておくことで、タイル評価が壊れたままになるのを防ぐ。
    //
    // 非同期で作ってあれば評価はコンピュートキューへ流し、commandList には
    // 引き渡しの状態遷移だけを記録する。まだ結果が 1 つも無いとき（起動直後や
    // 解像度変更の直後）だけは、その場で同期評価して最初のフレームから絵を出す。
    void Update(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                ID3D12GraphicsCommandList* commandList, const MaterialStack& stack,
                const TextureLibrary& textures, const MaterialLibrary& materials,
                const PaintMaskStore& paintMasks);

    // 非同期の評価が走っている最中か（UI の「評価中」表示と、開発用の撮影の待ちに使う）。
    bool IsEvaluating() const;
    // 走っている評価の完了を CPU で待つ。
    void WaitForEvaluation();

    uint32_t TileSize() const { return m_tileSize; }

    void SetTileSize(uint32_t tileSize) { m_tileSize = (tileSize > 0) ? tileSize : 1; }
    uint32_t EvaluatedTileCount() const { return m_evaluatedTileCount; }

    // 描画が読む結果。非同期なら表側（評価済みで入れ替えたもの）、同期なら評価先そのもの。
    const MaterialTextureSet& Textures() const {
        return m_frontTextures.IsValid() ? m_frontTextures : m_textures;
    }
    // 読み戻しのように、状態遷移を伴う操作から触るためのもの。
    // GpuTexture は自分の状態を持つので、遷移させる側は非 const 参照が要る。
    MaterialTextureSet& TexturesMutable() {
        return m_frontTextures.IsValid() ? m_frontTextures : m_textures;
    }
    uint32_t Resolution() const { return m_resolution; }
    uint32_t EvaluatedLayerCount() const { return m_evaluatedLayerCount; }

    // --- レイヤー一覧のマスクサムネイル -------------------------------------
    // 評価のついでにレイヤー 1 枚ぶんのマスクを小さく焼く。
    // 中間結果由来のマスク（傾斜や曲率）はそのレイヤーを合成する直前の下地から
    // しか作れないので、一覧側で後から作り直すことはできない。
    //
    // 添字はスタックの index。並べ替えても次の評価で作り直されるので追従する。
    // ImGui へ渡すハンドル。まだ無ければ ptr が 0。
    D3D12_GPU_DESCRIPTOR_HANDLE MaskThumbnailHandle(size_t layerIndex) const;

    // 変更を検知していなくても次回に評価し直す。
    void Invalidate() { m_evaluatedRevision = 0; }

private:
    // ブラーレイヤー 1 枚ぶん。Height を分離型ガウスでならし、
    // ぼかした形から法線を作り直す。**1 パスを全タイル終えてから次のパスへ進む**
    // （近傍を読むため、タイル優先で回すと継ぎ目が出る）。
    // マスクの op を 1 つ焼く。**タイルには分けない**（マスク全体で 1 枚）。
    bool RunMaskOp(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                   ID3D12GraphicsCommandList* commandList, const MaskProgram& ops, size_t index,
                   const MaterialStack& stack, const TextureLibrary& textures);
    // 川筋マスクを作る。反復が要るので専用のパイプライン。
    bool ApplyFluvialMask(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                          ID3D12GraphicsCommandList* commandList, const MaskOp& op,
                          const MaterialStack& stack, rhi::GpuTexture& target);
    // op ごとの結果テクスチャを用意する。数や解像度が変わった枚だけ作り直す。
    bool EnsureMaskOpTextures(rhi::Device& device, const MaskProgram& ops);
    // 川筋の作業リソース（1 組を使い回す）。
    bool EnsureFluvialResources(rhi::Device& device, uint32_t workResolution);
    void ReleaseFluvialResources(rhi::Device& device);
    // 標高マスクの「全範囲」で使う、地形の最低 / 最高をためる 1 枚。
    // 2 テクセルだけ使う（(0,0) が最低、(1,0) が最高）。使うときだけ作る。
    bool EnsureMaskHeightRange(rhi::Device& device);

    // 堆積レイヤー 1 枚ぶん。土砂を重力で再分配し、差分を Height へ足し戻して
    // 法線を作り直す。**タイルには分けない**（グリッド全体を何度も舐めるため）。
    bool ApplySediment(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                       ID3D12GraphicsCommandList* commandList, const MaterialLayer& layer,
                       const MaterialStack& stack);
    bool EnsureSedimentResources(rhi::Device& device, uint32_t resolution);
    void ReleaseSedimentResources(rhi::Device& device);
    // 直前の堆積レイヤーが積もらせた厚みを、マスクとして焼く。
    // **堆積レイヤーの直後にしか使えない**（作業用テクスチャを使い回すため）。
    bool ApplySedimentMask(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                           ID3D12GraphicsCommandList* commandList, const MaskOp& op,
                           const MaterialStack& stack, rhi::GpuTexture& target);
    // ぼかし / 堆積のあと、Height から法線を作り直す（形と陰影を合わせる）。
    void RebuildNormalsFromHeight(rhi::Device& device, ID3D12PipelineState* pipeline,
                                  ID3D12GraphicsCommandList* commandList,
                                  const MaterialStack& stack);

    // 崩落レイヤー 1 枚ぶん。発生源のマスクから岩片を生み、斜面を下らせて積む。
    // emissionIndex は発生源マスクの SRV（無ければ kInvalidTextureIndex 相当）。
    bool ApplyCrumbling(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                        ID3D12GraphicsCommandList* commandList, const MaterialLayer& layer,
                        const MaterialStack& stack, uint32_t emissionIndex);
    bool EnsureCrumblingResources(rhi::Device& device, uint32_t resolution);
    void ReleaseCrumblingResources(rhi::Device& device);
    // 直前の崩落レイヤーが積んだ岩屑を、マスクとして焼く。
    // **崩落レイヤーの直後にしか使えない**（作業用テクスチャを使い回すため）。
    bool ApplyCrumblingMask(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                            ID3D12GraphicsCommandList* commandList, const MaskOp& op,
                            rhi::GpuTexture& target);

    // 積雪レイヤー 1 枚ぶん。雪を降らせて滑らせ、積雪厚を Height へ足し戻して
    // 法線を作り直す。**タイルには分けない**（堆積と同じ理由）。
    bool ApplySnow(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                   ID3D12GraphicsCommandList* commandList, const MaterialLayer& layer,
                   const MaterialStack& stack);
    bool EnsureSnowResources(rhi::Device& device, uint32_t resolution);
    void ReleaseSnowResources(rhi::Device& device);
    // 直前の積雪レイヤーが積もらせた厚みを、被覆のマスクとして焼く。
    // **積雪レイヤーの直後にしか使えない**（作業用テクスチャを使い回すため）。
    bool ApplySnowMask(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                       ID3D12GraphicsCommandList* commandList, const MaskOp& op,
                       const MaterialStack& stack, rhi::GpuTexture& target);

    // 河川レイヤー 1 枚ぶん。川筋から河床を掘り、水面を張って Height へ書き戻し、
    // 法線を作り直す。seedIndex は Seed マスクの SRV（無ければ kInvalidTextureIndex）。
    // **タイルには分けない**（川筋と同じ理由）。
    bool ApplyRiver(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                    ID3D12GraphicsCommandList* commandList, const MaterialLayer& layer,
                    const MaterialStack& stack, uint32_t seedIndex);
    bool EnsureRiverResources(rhi::Device& device, uint32_t resolution,
                              uint32_t fineResolution);
    void ReleaseRiverResources(rhi::Device& device);
    // 直前の河川レイヤーが残した水面 / 河原 / 水深を、マスクとして焼く。
    // **河川レイヤーの直後にしか使えない**（作業用テクスチャを使い回すため）。
    bool ApplyRiverMask(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                        ID3D12GraphicsCommandList* commandList, const MaskOp& op,
                        const MaterialStack& stack, rhi::GpuTexture& target);

    bool ApplyHeightBlur(rhi::Device& device, ID3D12GraphicsCommandList* commandList,
                         ID3D12PipelineState* blurPipeline,
                         ID3D12PipelineState* normalPipeline, const MaterialLayer& layer,
                         const MaterialStack& stack, const std::vector<TileRect>& tiles);

    FluvialResources m_fluvial;
    // 標高マスクの「全範囲」用（R32_UINT）。InterlockedMin / Max でためる。
    rhi::GpuTexture m_maskHeightRange;
    SedimentResources m_sediment;
    CrumblingResources m_crumbling;
    SnowResources m_snow;
    RiverResources m_river;
    // マスクの op の結果。添字は MaskProgram と同じ。
    std::vector<rhi::GpuTexture> m_maskOpTextures;
    std::vector<uint32_t> m_maskOpResolutions;
    // 前回焼いたときの入力ハッシュ。**変わっていない op は焼き直さない。**
    // 川筋のように重い op を、無関係な編集のたびに走らせないための仕組み。
    std::vector<uint64_t> m_maskOpHashes;

    void ReleaseTextures(rhi::Device& device);
    // レイヤー枚数ぶんのマスクサムネイルを用意する。増減した枚数だけ作る / 捨てる。
    void EnsureMaskThumbnails(rhi::Device& device, size_t layerCount);

    // 定数バッファの置き場。コンピュートキューへ記録している間はそのキューの
    // 置き場から、それ以外（同期評価 / 書き出し）はフレームのアップロードリングから取る。
    // **評価器の中で device.Upload() を直接呼ばない**（キューの仕事はフレームより長生きする）。
    rhi::UploadAllocation AllocateConstants(rhi::Device& device, uint64_t size);
    // 全体をタイルに分ける。
    std::vector<TileRect> MakeTiles() const;
    // 描画（頂点 / ドメイン / ピクセル）から読める状態へ。**グラフィックスキューでだけ**
    // 記録できる（PIXEL_SHADER_RESOURCE はコンピュートキューでは使えない）。
    void TransitionForDisplay(ID3D12GraphicsCommandList* commandList, MaterialTextureSet& set);

    // 評価先。非同期のときは裏側で、終わったら m_frontTextures と入れ替わる。
    MaterialTextureSet m_textures;
    // 描画が読む表側。同期（書き出し用）のときは持たない。
    MaterialTextureSet m_frontTextures;
    // 近傍を読むパスの作業用。マスク生成（合成パスがここを読む）と、
    // ブラーの水平パスが使う。Height と同じ形式。評価先と一緒に使うので 1 枚でよい。
    rhi::GpuTexture m_scratch;
    std::vector<rhi::GpuTexture> m_maskThumbnails;
    uint32_t m_resolution = 0;
    uint64_t m_evaluatedRevision = 0;
    uint32_t m_evaluatedLayerCount = 0;
    uint32_t m_evaluatedTileCount = 0;
    uint32_t m_tileSize = 512;

    // --- 非同期評価 ---------------------------------------------------------
    rhi::ComputeQueue m_compute;
    bool m_asynchronous = false;
    // いまコンピュートキューへ記録している最中か（AllocateConstants の振り分け）。
    bool m_recordingAsync = false;
    // 投入済みで、まだ結果を回収していない評価があるか。
    bool m_asyncInFlight = false;
    // 投入した評価が対応するスタックの版。回収時に m_evaluatedRevision へ写す。
    uint64_t m_asyncRevision = 0;
    // 表側に描ける結果があるか。無いうちは同期で評価する。
    bool m_hasResult = false;
};

}  // namespace tg::compositor
