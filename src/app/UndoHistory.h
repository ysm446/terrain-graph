#pragma once

// MaterialLibrary.h は含めない。あちらは rhi（D3D12）を引き込むため、
// GPU の無いテストから使えなくなる。ID と MapSlot は MaterialLayer.h にある。
// NodeGraph.h も STL と compositor のデータ構造にしか依存しない。
#include "compositor/MaterialLayer.h"
#include "graph/NodeGraph.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tg {

// アンドゥが対象にするマテリアル 1 つぶん。
//
// **`MaterialAsset` をそのまま複製できないので、データだけを抜き出して持つ。**
// `MaterialAsset` はサムネイル（`rhi::GpuTexture`）を抱えており、
// これは ComPtr とディスクリプタハンドルを持つ。複製すると同じディスクリプタを
// 2 つの持ち主が解放してしまう。
//
// **`MaterialAsset` にフィールドを足したら、ここと Capture / Apply にも足すこと。**
struct MaterialSnapshot {
    compositor::MaterialAssetId id = compositor::kNoMaterialAsset;
    std::string name;
    compositor::TextureId baseColor = compositor::kNoTexture;
    compositor::TextureId normal = compositor::kNoTexture;
    compositor::MapSlot roughness;
    compositor::MapSlot metallic;
    compositor::MapSlot ambientOcclusion;
    compositor::MapSlot height;
    DirectX::XMFLOAT3 baseColorTint{0.5f, 0.5f, 0.5f};
    float roughnessValue = 0.5f;
    float metallicValue = 0.0f;
    float ambientOcclusionValue = 1.0f;
};

// アンドゥ 1 段ぶん。編集の対象になる文書をまるごと持つ。
//
// グラフのノードもマテリアルもただの構造体なので、まるごと複製しても数 KB で済む
// （ノード 1 個 ≈ 数百バイト）。操作ごとに do / undo を書く方式より実装が小さく、
// **「この操作だけアンドゥが効かない」という取りこぼしが起きない。**
//
// ノードの位置も含まれる（graph::Node が持つ）ので、構造を戻すと配置も一緒に戻る。
// ただし**移動だけでは段を積まない**（毎フレーム位置が返ってくるため）。
//
// テクスチャとペイントマスクは入れない。GPU リソースそのもので、
// 複製も作り直しも高くつく。参照している ID だけを持ち、
// 戻すときに存在しない ID は落とす（DocumentSnapshot を適用する側の責任）。
struct DocumentSnapshot {
    std::vector<graph::Node> graphNodes;
    std::vector<graph::Link> graphLinks;
    std::vector<MaterialSnapshot> materials;
    graph::GraphId selectedGraphNode = 0;
    int selectedMaterial = 0;
};

// アンドゥ / リドゥの履歴。スナップショットを積むだけ。
class UndoHistory {
public:
    // 積める段数。1 段が数 KB なので、100 段でも 1 MB に届かない。
    static constexpr size_t kMaxDepth = 100;

    // **変更が確定した直後に、変更前の状態を渡して呼ぶ。**
    //
    // editId は「いま掴んでいるウィジェット」の ID（`ImGui::GetActiveID()`）。
    // スライダーをドラッグしている間は毎フレーム変更が来るが、editId は同じなので
    // 1 回のドラッグが 1 段に収まる。ボタンのように掴みが無い操作は 0 が来るため、
    // 押すたびに 1 段積まれる。
    void Push(const DocumentSnapshot& before, uint32_t editId);

    // 掴んでいたウィジェットが離れたことを伝える。
    // 同じスライダーをもう一度掴んだときに別の段になるようにするため。
    void EndEdit() { m_lastEditId = 0; }
    // 直前に積んだ段の editId。「直前の編集の続き」を同じ段に畳みたいときに Push へ渡す。
    uint32_t LastEditId() const { return m_lastEditId; }

    bool CanUndo() const { return !m_undo.empty(); }
    bool CanRedo() const { return !m_redo.empty(); }

    // いまの状態を渡すと、戻した先の状態を返す。いまの状態は反対側へ積む。
    // 呼ぶ前に CanUndo / CanRedo を確かめること。
    DocumentSnapshot Undo(const DocumentSnapshot& current);
    DocumentSnapshot Redo(const DocumentSnapshot& current);

    void Clear();

    size_t UndoCount() const { return m_undo.size(); }
    size_t RedoCount() const { return m_redo.size(); }

    // 履歴に残っているすべての段。ペイントマスクの掃除で参照を数えるのに使う。
    const std::vector<DocumentSnapshot>& UndoStack() const { return m_undo; }
    const std::vector<DocumentSnapshot>& RedoStack() const { return m_redo; }

private:
    std::vector<DocumentSnapshot> m_undo;
    std::vector<DocumentSnapshot> m_redo;
    uint32_t m_lastEditId = 0;
};

}  // namespace tg
