#pragma once

#include "compositor/MaterialLayer.h"
#include "compositor/TextureLibrary.h"
#include "rhi/Device.h"
#include "rhi/PipelineCache.h"

#include <string>
#include <vector>

namespace tg::compositor {

// マテリアル 1 つぶん。PBR のマップ一式に名前を付けたもの。
//
// レイヤーはマテリアルを 1 つ参照する（Quixel Mixer と同じ形）。
// マップを個別に差し替えるのではなく、マテリアルを差し替えることで見た目を変える。
// マスクだけはレイヤー固有なので、ここには入れない。
struct MaterialAsset {
    MaterialAssetId id = kNoMaterialAsset;
    std::string name;

    // 未指定のスロットは下の定数を使う。
    // ベースカラーと法線は RGB をそのまま使うのでチャンネル指定は要らない。
    TextureId baseColor = kNoTexture;  // sRGB として読む
    TextureId normal = kNoTexture;     // タンジェント空間法線（リニア）
    // スカラーのマップは「テクスチャ + チャンネル」で指定する。
    // Megascans の _ORD のように 1 枚へ詰めたテクスチャを使えるようにするため。
    MapSlot roughness;
    MapSlot metallic;
    MapSlot ambientOcclusion;
    MapSlot height;

    // **乗算の中立値なので 1.0。** マップがあるときは掛け算で効く
    // （`layerBaseColor *= テクスチャ`）ため、1 以外を既定にすると
    // 外から持ち込んだ素材のアルベドが黙って変わってしまう。
    // マップが無いスロットでは、この値がそのままベースカラーになる。
    DirectX::XMFLOAT3 baseColorTint = {1.0f, 1.0f, 1.0f};
    float roughnessValue = 0.5f;
    float metallicValue = 0.0f;
    float ambientOcclusionValue = 1.0f;

    // 一覧に出すサムネイル。マップかパラメータを変えたら作り直す。
    rhi::GpuTexture thumbnail;
    bool thumbnailDirty = true;
};

// チャンネル指定をシェーダへ渡す形へ詰める。並びは TG_CHANNEL_SLOT_* と一致させること。
uint32_t PackMaterialChannels(const MaterialAsset& asset);

// マテリアルを保持し、サムネイルを作る。
class MaterialLibrary {
public:
    void Destroy(rhi::Device& device);

    MaterialAssetId Add(const std::string& name);
    // ID を保ったまま作り直す。**アンドゥで削除を取り消すときに使う。**
    // Add で作ると新しい ID が振られ、レイヤーからの参照が切れてしまう。
    MaterialAsset& RestoreAsset(MaterialAssetId id, const std::string& name);
    MaterialAssetId Duplicate(const MaterialAsset& source);
    void Remove(rhi::Device& device, MaterialAssetId id);
    void Clear(rhi::Device& device);

    const std::vector<MaterialAsset>& Entries() const { return m_entries; }
    const MaterialAsset* Find(MaterialAssetId id) const;
    MaterialAsset* FindMutable(MaterialAssetId id);

    // サムネイルの作り直しを予約する。マップやパラメータを変えたら呼ぶ。
    void MarkThumbnailDirty(MaterialAssetId id);

    // 1 枚の ORD テクスチャを AO(R) / ラフネス(G) / ハイト(B) へまとめて割り当てる。
    // Megascans の `_ORD` の並びに合わせてある。
    void AssignOrdTexture(MaterialAssetId id, TextureId texture);

    // 予約されたサムネイルを作る。GPU 待機を伴うため、フレームの外で呼ぶ。
    void ProcessPendingWork(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                            const TextureLibrary& textures);

    // 一覧で使うサムネイルのハンドル。まだ無ければ ptr が 0。
    D3D12_GPU_DESCRIPTOR_HANDLE ThumbnailHandle(MaterialAssetId id) const;

private:
    bool BuildThumbnail(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                        const TextureLibrary& textures, MaterialAsset& asset);

    std::vector<MaterialAsset> m_entries;
    MaterialAssetId m_nextId = 1;
};

}  // namespace tg::compositor
