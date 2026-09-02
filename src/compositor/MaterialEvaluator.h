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
    bool ApplyHeightBlur(rhi::Device& device, ID3D12GraphicsCommandList* commandList,
                         ID3D12PipelineState* blurPipeline,
                         ID3D12PipelineState* normalPipeline, const MaterialLayer& layer,
                         const MaterialStack& stack, const std::vector<TileRect>& tiles);

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
