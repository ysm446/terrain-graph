#pragma once

#include "rhi/Common.h"

#include <string>
#include <unordered_map>

namespace tg::rhi {

class ShaderCompiler;

// 全パス共通のルートシグネチャ。bindless 前提で、
// シェーダはリソースをディスクリプタヒープのインデックスで直接引く。
//
//   b0 : ルート定数 16 dword（テクスチャのインデックスや小さなパラメータ）
//   b1 : ルート CBV（大きめの定数バッファ）
//   s0-s3 : スタティックサンプラ
//
// これにより、パスごとにルートシグネチャを作る必要がなくなる。
inline constexpr uint32_t kRootConstantCount = 16;

// 頂点入力レイアウトの種類。頂点構造体は数が限られるので列挙で持つ。
enum class VertexLayout {
    None,          // 頂点バッファを使わない（フルスクリーン描画など）
    MeshStandard,  // position / normal / tangent / uv
};

struct GraphicsPipelineDesc {
    std::wstring shaderPath;
    std::wstring vertexEntry;
    // 空ならピクセルシェーダ無し。深度だけを描くパス（シャドウマップ）で使う。
    std::wstring pixelEntry;
    // 両方そろっていればテセレーションを使う。トポロジは 3 制御点のパッチになる。
    std::wstring hullEntry;
    std::wstring domainEntry;
    DXGI_FORMAT rtvFormat = DXGI_FORMAT_UNKNOWN;
    // 2 枚目のレンダーターゲット。UNKNOWN なら 1 枚だけ書く。
    DXGI_FORMAT rtvFormat1 = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT dsvFormat = DXGI_FORMAT_UNKNOWN;
    VertexLayout layout = VertexLayout::None;
    D3D12_CULL_MODE cullMode = D3D12_CULL_MODE_BACK;
    // ワイヤーフレーム表示で使う。既定は塗りつぶし。
    D3D12_FILL_MODE fillMode = D3D12_FILL_MODE_SOLID;
    bool depthTest = true;
    bool depthWrite = true;
    // ライン描画（ガイド線など）。IA へ LINELIST として渡す。テセレーションとは併用不可。
    bool lineTopology = false;
    // RTV0 に通常のアルファ合成（src.a / 1 - src.a）を掛ける。半透明のガイド線で使う。
    bool alphaBlend = false;

    std::wstring MakeKey() const;
};

// PSO とルートシグネチャのキャッシュ。
// シェーダのホットリロード時は InvalidateAll() で作り直す。
class PipelineCache {
public:
    PipelineCache() = default;
    ~PipelineCache();

    PipelineCache(const PipelineCache&) = delete;
    PipelineCache& operator=(const PipelineCache&) = delete;

    bool Create(ID3D12Device* device, ShaderCompiler* compiler);
    void Destroy();

    ID3D12RootSignature* GlobalRootSignature() const { return m_rootSignature.Get(); }

    // コンピュート PSO を取得する。未生成ならコンパイルして作る。失敗時は nullptr。
    ID3D12PipelineState* GetCompute(const std::wstring& relativePath,
                                    const std::wstring& entryPoint);

    // グラフィックス PSO を取得する。未生成ならコンパイルして作る。失敗時は nullptr。
    ID3D12PipelineState* GetGraphics(const GraphicsPipelineDesc& desc);

    // キャッシュを破棄する。GPU がまだ参照している可能性があるため、
    // 呼び出し側は事前に GPU 待機するか、削除キューへ渡すこと。
    void InvalidateAll();

    // 生成に成功した PSO の数。失敗を記録した null プレースホルダは数えない。
    size_t PipelineCount() const;

private:
    bool CreateGlobalRootSignature();

    ID3D12Device* m_device = nullptr;
    ShaderCompiler* m_compiler = nullptr;
    ComPtr<ID3D12RootSignature> m_rootSignature;
    std::unordered_map<std::wstring, ComPtr<ID3D12PipelineState>> m_computePipelines;
    std::unordered_map<std::wstring, ComPtr<ID3D12PipelineState>> m_graphicsPipelines;
};

}  // namespace tg::rhi
