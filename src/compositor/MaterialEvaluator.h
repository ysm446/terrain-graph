#pragma once

#include "compositor/MaterialLibrary.h"
#include "compositor/MaterialStack.h"
#include "compositor/PaintMask.h"
#include "compositor/TextureLibrary.h"

#include <vector>
#include "rhi/Device.h"
#include "rhi/PipelineCache.h"

namespace tg::compositor {

// 合成結果のチャンネルセット。plan.md の定義に対応する。
struct MaterialTextureSet {
    rhi::GpuTexture baseColor;  // R11G11B10_FLOAT
    rhi::GpuTexture normal;     // R16G16_FLOAT（xy のみ、z は再構成）
    rhi::GpuTexture surface;    // R8G8B8A8_UNORM（R=Roughness, G=Metallic, B=AO）
    rhi::GpuTexture height;     // R16_FLOAT
    // 近傍を読むパスの作業用。マスク生成（合成パスがここを読む）と、
    // ブラーの水平パスが使う。どちらも Height と同じ形式。
    rhi::GpuTexture scratch;  // R16_FLOAT

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
    rhi::GpuTexture bedrock;   // R32_FLOAT 動かない基盤
    rhi::GpuTexture sediment;  // R32_FLOAT 可動な土砂
    rhi::GpuTexture outgoing;  // RGBA32_FLOAT 4 方向への流出
    rhi::GpuTexture original;  // R32_FLOAT 始まりの高さ（差分を出すため）
    uint32_t resolution = 0;

    bool IsValid() const { return sediment.IsValid(); }
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
class MaterialEvaluator {
public:
    bool Create(rhi::Device& device, uint32_t resolution);
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

    // スタックに変更があったときだけ全体を評価し直す。
    // 全体はタイルに分割して評価する。エクスポート時と同じ経路を常に通しておくことで、
    // タイル評価が壊れたままになるのを防ぐ。
    void EvaluateIfDirty(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                         ID3D12GraphicsCommandList* commandList, const MaterialStack& stack,
                         const TextureLibrary& textures, const MaterialLibrary& materials,
                         const PaintMaskStore& paintMasks);

    uint32_t TileSize() const { return m_tileSize; }

    void SetTileSize(uint32_t tileSize) { m_tileSize = (tileSize > 0) ? tileSize : 1; }
    uint32_t EvaluatedTileCount() const { return m_evaluatedTileCount; }

    const MaterialTextureSet& Textures() const { return m_textures; }
    // 読み戻しのように、状態遷移を伴う操作から触るためのもの。
    // GpuTexture は自分の状態を持つので、遷移させる側は非 const 参照が要る。
    MaterialTextureSet& TexturesMutable() { return m_textures; }
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

    // 堆積レイヤー 1 枚ぶん。土砂を重力で再分配し、差分を Height へ足し戻して
    // 法線を作り直す。**タイルには分けない**（グリッド全体を何度も舐めるため）。
    bool ApplySediment(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                       ID3D12GraphicsCommandList* commandList, const MaterialLayer& layer,
                       const MaterialStack& stack);
    bool EnsureSedimentResources(rhi::Device& device, uint32_t resolution);
    void ReleaseSedimentResources(rhi::Device& device);
    // ぼかし / 堆積のあと、Height から法線を作り直す（形と陰影を合わせる）。
    void RebuildNormalsFromHeight(rhi::Device& device, ID3D12PipelineState* pipeline,
                                  ID3D12GraphicsCommandList* commandList,
                                  const MaterialStack& stack);

    bool ApplyHeightBlur(rhi::Device& device, ID3D12GraphicsCommandList* commandList,
                         ID3D12PipelineState* blurPipeline,
                         ID3D12PipelineState* normalPipeline, const MaterialLayer& layer,
                         const MaterialStack& stack, const std::vector<TileRect>& tiles);

    FluvialResources m_fluvial;
    SedimentResources m_sediment;
    // マスクの op の結果。添字は MaskProgram と同じ。
    std::vector<rhi::GpuTexture> m_maskOpTextures;
    std::vector<uint32_t> m_maskOpResolutions;
    // 前回焼いたときの入力ハッシュ。**変わっていない op は焼き直さない。**
    // 川筋のように重い op を、無関係な編集のたびに走らせないための仕組み。
    std::vector<uint64_t> m_maskOpHashes;

    void ReleaseTextures(rhi::Device& device);
    // レイヤー枚数ぶんのマスクサムネイルを用意する。増減した枚数だけ作る / 捨てる。
    void EnsureMaskThumbnails(rhi::Device& device, size_t layerCount);

    MaterialTextureSet m_textures;
    std::vector<rhi::GpuTexture> m_maskThumbnails;
    uint32_t m_resolution = 0;
    uint64_t m_evaluatedRevision = 0;
    uint32_t m_evaluatedLayerCount = 0;
    uint32_t m_evaluatedTileCount = 0;
    uint32_t m_tileSize = 512;
};

}  // namespace tg::compositor
