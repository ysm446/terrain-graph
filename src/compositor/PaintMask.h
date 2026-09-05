#pragma once

#include "compositor/MaterialLayer.h"
#include "rhi/Device.h"
#include "rhi/PipelineCache.h"

#include <cstdint>
#include <vector>

namespace tg::compositor {

// 1 枚のペイントマスク。
struct PaintMaskEntry {
    PaintMaskId id = kNoPaintMask;
    rhi::GpuTexture texture;  // R8_UNORM、正方
};

// ブラシの設定。レイヤーではなく編集セッションの状態なので、レイヤーとは別に持つ。
struct BrushSettings {
    // 半径はビューポートの画面ピクセルで指定する。マテリアル UV での半径は
    // ブラシパスが UV バッファの微分から求めるため、視点や UV スケールに依らず
    // 「画面上で見えている大きさ」で描ける。
    float radiusPixels = 48.0f;
    float strength = 0.25f;  // 1 回の適用で足す量
    float falloff = 2.0f;    // 1 で線形、大きいほど中心に集中する
    bool erase = false;
};

// ブラシを 1 回適用する要求。座標はビューポート（レンダーターゲット）のピクセル単位。
struct BrushStroke {
    PaintMaskId target = kNoPaintMask;
    float fromX = 0.0f;
    float fromY = 0.0f;
    float toX = 0.0f;
    float toY = 0.0f;
    BrushSettings brush;
};

// ブラシパスが UV を引くための入力。PreviewRenderer が用意する。
struct PaintContext {
    uint32_t uvBufferSrvIndex = 0xFFFFFFFFu;
    uint32_t viewportWidth = 0;
    uint32_t viewportHeight = 0;
};

// ペイントマスクを保持し、ブラシ処理とアンドゥを GPU 上で行う。
//
// 描画要求はいったん積んでおき、フレーム内の 1 か所（合成の評価より前）で
// まとめてコマンドリストへ記録する。UI から直接コマンドを積まないことで、
// フレームの内と外で呼べる操作が混ざらないようにしている。
class PaintMaskStore {
public:
    void Destroy(rhi::Device& device);

    // 新しいペイントマスクを作る。中身は initialValue で埋める。
    PaintMaskId Add(rhi::Device& device, float initialValue);
    // 中身ごと複製する。レイヤーの複製で共有状態にならないようにするため。
    PaintMaskId Duplicate(rhi::Device& device, PaintMaskId source);

    // --- プロジェクトの保存と読み込み ---------------------------------------
    // ペイントマスクは手続きで再現できないので、画像として持ち出す必要がある。
    // どちらもフレームの外で呼ぶこと（GPU 待機を伴う）。

    // 中身を 1 バイト / テクセルで読み戻す。失敗したら空を返す。
    std::vector<uint8_t> ReadPixels(rhi::Device& device, PaintMaskId id) const;
    // 画像から新しいペイントマスクを作る。pixels は resolution^2 の 1 チャンネル。
    // 空のストアなら解像度もこの画像に合わせる。
    PaintMaskId AddFromPixels(rhi::Device& device, uint32_t resolution,
                              const std::vector<uint8_t>& pixels);
    // すべて破棄する。プロジェクトを開く前に呼ぶ。解放はフレーム同期後に行われる。
    void Clear(rhi::Device& device);
    // 破棄する。解放はフレーム同期後に行われるため、GPU 待機は伴わない。
    void Remove(rhi::Device& device, PaintMaskId id);

    const PaintMaskEntry* Find(PaintMaskId id) const;
    // シェーダへ渡す SRV インデックス。無効なら kInvalidTextureIndex。
    uint32_t SrvIndex(PaintMaskId id) const;
    size_t Count() const { return m_entries.size(); }
    // 持っているマスクの ID。参照されなくなったものを掃除するのに使う。
    std::vector<PaintMaskId> Ids() const;
    uint32_t Resolution() const { return m_resolution; }
    // 中身の世代。塗る / 埋める / アンドゥ / 解像度の作り直しで進む。
    // ID は変わらないのに中身が変わるので、合成側はこれをハッシュに混ぜて
    // 「塗ったマスクで乗せた地形」から作るマスク（傾斜など）を焼き直す。
    uint64_t Revision() const { return m_revision; }

    // 解像度の変更要求。作り直しは GPU 待機を伴うため、フレームの外で処理する。
    void RequestResolution(uint32_t resolution) { m_requestedResolution = resolution; }
    uint32_t RequestedResolution() const { return m_requestedResolution; }
    void ProcessPendingWork(rhi::Device& device, rhi::PipelineCache& pipelineCache);

    // --- 要求を積む（フレームの内外どちらから呼んでもよい） -----------------
    void QueueStroke(const BrushStroke& stroke);
    void QueueFill(PaintMaskId id, float value);
    // ストロークの開始時に呼ぶ。この時点の内容をアンドゥ履歴へ積む。
    void QueueSnapshot(rhi::Device& device, PaintMaskId id);
    void QueueUndo(rhi::Device& device);
    void QueueRedo(rhi::Device& device);

    bool CanUndo() const { return !m_undo.empty(); }
    bool CanRedo() const { return !m_redo.empty(); }
    size_t UndoCount() const { return m_undo.size(); }

    // 積まれた要求をコマンドリストへ記録する。何か記録したら true。
    // 合成の評価より前に呼ぶこと。
    bool Process(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                 ID3D12GraphicsCommandList* commandList, const PaintContext& context);

private:
    enum class OpType {
        Fill,      // 一様な値で埋める
        Stroke,    // ブラシを 1 回積む
        Capture,   // マスク → 履歴テクスチャ（アンドゥ用の退避）
        Restore,   // 履歴テクスチャ → マスク
        CopyMask,  // マスク → マスク（複製用。両側とも状態を追跡して遷移する）
    };

    struct Op {
        OpType type = OpType::Fill;
        PaintMaskId target = kNoPaintMask;
        float value = 0.0f;
        BrushStroke stroke;
        // Capture の書き込み先 / Restore の読み出し元。
        // 履歴テクスチャは常に COMMON に置く取り決め（生きているマスクを入れないこと。
        // 状態追跡が壊れる。マスク同士のコピーは CopyMask を使う）。
        // GpuTexture のコピーは ComPtr の共有なので、所有権は元の持ち主に残る。
        rhi::GpuTexture history;
        // Restore で消費した履歴テクスチャを、記録後に解放するか。
        bool releaseHistory = false;
        // CopyMask の読み出し元。
        PaintMaskId copySource = kNoPaintMask;
    };

    // アンドゥ / リドゥ 1 段ぶん。対象のマスクとその時点の内容を持つ。
    struct Snapshot {
        PaintMaskId id = kNoPaintMask;
        rhi::GpuTexture texture;
    };

    PaintMaskEntry* FindMutable(PaintMaskId id);
    bool CreateMaskTexture(rhi::Device& device, uint32_t resolution, rhi::GpuTexture& outTexture);
    bool CreateHistoryTexture(rhi::Device& device, uint32_t resolution,
                              rhi::GpuTexture& outTexture);
    // 現在の内容を履歴テクスチャへ退避する要求を積み、stack へ積む。
    bool PushHistory(rhi::Device& device, PaintMaskId id, std::vector<Snapshot>& stack);
    void ReleaseSnapshots(rhi::Device& device, std::vector<Snapshot>& stack);

    std::vector<PaintMaskEntry> m_entries;
    std::vector<Op> m_pending;
    std::vector<Snapshot> m_undo;
    std::vector<Snapshot> m_redo;
    PaintMaskId m_nextId = 1;
    uint32_t m_resolution = 1024;
    uint64_t m_revision = 1;
    uint32_t m_requestedResolution = 1024;
};

}  // namespace tg::compositor
