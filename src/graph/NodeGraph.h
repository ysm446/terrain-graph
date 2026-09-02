#pragma once

#include "compositor/MaterialLayer.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// ノードグラフのデータモデル。terrain-editor から「仕組み」を移植したもの。
//
//   - ノード・ピン・リンクは共通の単一 ID 空間から採番する
//     （imgui-node-editor の NodeId / PinId / LinkId にそのまま流用できる）。
//   - ノードの設定は種類ごとの構造体を std::variant で持つ。
//     terrain-editor の「全種類の設定を 1 構造体に持つファット構造体」はやめた。
//   - 評価は既存の GPU 評価器を使う。グラフは CompileLayers() でレイヤー列
//     （下から上）へ落とし、MaterialStack として評価する。
//     見た目はレイヤー時代と完全に一致する。
//
// UI / D3D12 には依存しない（compositor のデータ構造にだけ依存する）。
namespace tg::graph {

using GraphId = int;

enum class PinKind : uint32_t {
    Input,
    Output,
};

// ピンを流れる値の型。同じ型どうしだけ接続できる。
// いまはマテリアル（PBR チャンネルセット）だけ。マスクなどはこれから足す。
enum class ValueType : uint32_t {
    Material = 0,
};

enum class NodeKind : uint32_t {
    Surface = 0,
    Shape = 1,
    Liquid = 2,
    Output = 3,
};

struct PinDefinition {
    PinKind kind = PinKind::Input;
    ValueType valueType = ValueType::Material;
    const char* label = "";
};

// ノードの静的な定義。種類・保存名・表示名・ピン構成をテーブルで持ち、
// CreateNode() がここからピンを生成する。
struct NodeDefinition {
    NodeKind kind = NodeKind::Surface;
    const char* name = "";   // 保存名（ファイルには enum の数値ではなくこれを書く）
    const char* title = "";  // 表示名
    std::span<const PinDefinition> pins;
};

struct Pin {
    GraphId id = 0;
    GraphId nodeId = 0;
    PinKind kind = PinKind::Input;
    ValueType valueType = ValueType::Material;
    std::string label;
};

// --- ノードの設定 ---------------------------------------------------------
//
// ノードを増やすときは、(1) 設定構造体を定義し、(2) NodeSettings へ足し、
// (3) NodeGraph.cpp の定義テーブルへ登録し、(4) 保存とプロパティ UI の
// 対応を足す。それ以外の場所を触る必要がないように保つ。

// サーフェス / シェイプ / 水面。既存のレイヤーそのもの（kind も layer が持つ）。
struct LayerNodeSettings {
    compositor::MaterialLayer layer;
};

// 出力。ここに繋いだチェーンがプレビューのマテリアルになる。
struct OutputNodeSettings {};

using NodeSettings = std::variant<LayerNodeSettings, OutputNodeSettings>;

struct Node {
    GraphId id = 0;
    NodeKind kind = NodeKind::Surface;
    std::vector<Pin> inputs;
    std::vector<Pin> outputs;
    NodeSettings settings;
    // エディタ上の位置。UI が読み書きし、保存にも含める。
    // positionValid が偽の間は UI が初期位置を与える。
    float posX = 0.0f;
    float posY = 0.0f;
    bool positionValid = false;
};

struct Link {
    GraphId id = 0;
    GraphId startPin = 0;  // 出力ピン
    GraphId endPin = 0;    // 入力ピン
};

class NodeGraph {
public:
    // 「サーフェス（ベース）→ 出力」を繋いだ最小構成。
    static NodeGraph CreateDefault();

    const std::vector<Node>& Nodes() const { return m_nodes; }
    std::vector<Node>& MutableNodes() { return m_nodes; }
    const std::vector<Link>& Links() const { return m_links; }

    const Pin* FindPin(GraphId pinId) const;
    const Node* FindNode(GraphId nodeId) const;
    Node* FindMutableNode(GraphId nodeId);
    // 入力ピンに繋がっている上流ノード。無ければ nullptr。
    const Node* FindUpstreamNodeForPin(GraphId inputPinId) const;

    // 接続できるか。別ノード・型一致・入出力の組み合わせに加えて、
    // **循環ができる接続は弾く**（評価が回らなくなるため）。
    bool CanCreateLink(GraphId startPin, GraphId endPin) const;
    // 接続する。入力ピンに既にある接続は置き換える。
    bool CreateLink(GraphId startPin, GraphId endPin);
    bool DeleteLink(GraphId linkId);
    // 定義テーブルからピンを生成してノードを足す。設定は既定値。
    GraphId CreateNode(NodeKind kind);
    bool DeleteNode(GraphId nodeId);

    // 読み込み用。ID はファイルの値をそのまま使い、次の採番を max+1 に合わせる。
    void Replace(std::vector<Node> nodes, std::vector<Link> links);

    // グラフをレイヤー列（下から上）へ落とす。出力ノードの「下地」チェーンを遡る。
    // チェーンが空なら下地 1 枚（MaterialStack::MakeBaseLayer と同じもの）を返す。
    std::vector<compositor::MaterialLayer> CompileLayers() const;
    // 指定したノード**まで**のレイヤー列。ノードを選んでプレビューするときに使う。
    // レイヤー設定を持たないノード（出力など）や無効な ID は出力ノード扱いに落とす。
    std::vector<compositor::MaterialLayer> CompileLayersTo(GraphId nodeId) const;

    // 変更があったことを記録する。Application はこれを見て再コンパイルする。
    void MarkDirty() { ++m_revision; }
    uint64_t Revision() const { return m_revision; }

private:
    GraphId AllocateGraphId() { return m_nextGraphId++; }
    void RebuildNextGraphId();
    // top から「下地」チェーンを遡ってレイヤー列（下から上）にする共通部。
    std::vector<compositor::MaterialLayer> CompileChainFrom(const Node* top) const;
    // producer の出力を辿って target に届くか（循環チェック用）。
    bool ReachesDownstream(GraphId fromNodeId, GraphId targetNodeId) const;

    std::vector<Node> m_nodes;
    std::vector<Link> m_links;
    GraphId m_nextGraphId = 1;
    uint64_t m_revision = 1;
};

std::span<const NodeDefinition> NodeDefinitions();
const NodeDefinition* FindNodeDefinition(NodeKind kind);
const NodeDefinition* FindNodeDefinitionByName(std::string_view name);
// レイヤー設定を持つ種類か（サーフェス / シェイプ / 水面）。
bool IsLayerNodeKind(NodeKind kind);
// 種類に対応するレイヤー種別（レイヤー設定を持つ種類のみ意味を持つ）。
compositor::LayerKind LayerKindFor(NodeKind kind);

}  // namespace tg::graph
