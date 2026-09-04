#pragma once

#include "renderer/Environment.h"
#include "rhi/Device.h"
#include "rhi/PipelineCache.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace tg::renderer {

using SkyAssetId = uint32_t;
inline constexpr SkyAssetId kNoSkyAsset = 0;

// 天球の出どころ。
enum class SkySource : uint32_t {
    Procedural = 0,  // 手続き的な空
    Hdri = 1,        // equirectangular の HDR ファイル
};

// 天球 1 つぶんの中身。**名前とサムネイルを除いたもの。**
// レンダラは一覧の都合を知らなくてよいので、これだけを受け取る。
struct SkyDefinition {
    SkySource source = SkySource::Procedural;
    std::filesystem::path hdriPath;
    // **この HDRI の空を何 cd/m^2 とみなすか。** HDRI は絶対輝度で較正されて
    // いないため、基準をここで与える。**較正値はファイルごとに違う**ので、
    // レンダラ側に 1 つ持つのではなく天球が持つ。既定は SkySettings::intensity と揃えてある。
    float skyLuminance = 12000.0f;
    // 環境光の倍率。物理量ではなく、見た目を整えるためのもの。
    float iblIntensity = 1.0f;
    // source が Procedural のときに使う空のパラメータ。
    SkySettings procedural;
};

// 環境マップを作り直す必要があるか（IBL の倍率だけの違いは含めない）。
bool NeedsEnvironmentRebuild(const SkyDefinition& before, const SkyDefinition& after);
// equirect を読み直さず、較正倍率だけ作り直せば足りるか。
bool NeedsLuminanceRebuild(const SkyDefinition& before, const SkyDefinition& after);

// 天球 1 つぶん。マテリアルと同じく、名前とサムネイルを持つアセット。
struct SkyAsset {
    SkyAssetId id = kNoSkyAsset;
    std::string name;
    SkyDefinition sky;

    // 一覧に出すサムネイル。設定を変えたら作り直す。
    rhi::GpuTexture thumbnail;
    bool thumbnailDirty = true;
};

// 天球を保持し、サムネイルを作る。**一覧は決して空にしない。**
// ビューポートは常にこの中の 1 つを見ているので、空になると環境が決まらなくなる。
class SkyLibrary {
public:
    void Destroy(rhi::Device& device);

    SkyAssetId Add(const std::string& name);
    SkyAssetId Duplicate(const SkyAsset& source);
    void Remove(rhi::Device& device, SkyAssetId id);
    // すべて破棄する。プロジェクトを開く前に呼ぶ。フレームの外で呼ぶこと。
    void Clear(rhi::Device& device);
    // 空なら既定の天球を 1 つ作る。作った（または既にあった）ものの ID を返す。
    SkyAssetId EnsureDefault();

    const std::vector<SkyAsset>& Entries() const { return m_entries; }
    const SkyAsset* Find(SkyAssetId id) const;
    SkyAsset* FindMutable(SkyAssetId id);

    // ビューポートに適用している天球。一覧が空でなければ必ず 1 つ指す。
    SkyAssetId ActiveId() const { return m_activeId; }
    void SetActive(SkyAssetId id);
    const SkyAsset* Active() const { return Find(m_activeId); }
    SkyAsset* ActiveMutable() { return FindMutable(m_activeId); }

    // サムネイルの作り直しを予約する。設定を変えたら呼ぶ。
    void MarkThumbnailDirty(SkyAssetId id);

    // 予約されたサムネイルを作る。GPU 待機と HDR ファイルの読み込みを伴うため、
    // フレームの外で呼ぶこと。**1 回につき 1 枚だけ作る。**
    // HDRI は 1 枚読むのに数百ミリ秒かかるので、まとめて作ると目に見えて止まる。
    void ProcessPendingWork(rhi::Device& device, rhi::PipelineCache& pipelineCache);

    // 一覧で使うサムネイルのハンドル。まだ無ければ ptr が 0。
    D3D12_GPU_DESCRIPTOR_HANDLE ThumbnailHandle(SkyAssetId id) const;

private:
    bool BuildThumbnail(rhi::Device& device, rhi::PipelineCache& pipelineCache, SkyAsset& asset);

    std::vector<SkyAsset> m_entries;
    SkyAssetId m_nextId = 1;
    SkyAssetId m_activeId = kNoSkyAsset;
};

}  // namespace tg::renderer
