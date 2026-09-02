#pragma once

// D3D12MemAlloc.h は <d3d12.h> を取り込むため、必ず先に DirectX-Headers 版を通す。
#include "rhi/Common.h"
#include "rhi/DescriptorHeap.h"

#include <D3D12MemAlloc.h>

#include <vector>

namespace tg::rhi {

// リソースの状態は当面サブリソース単位ではなく、リソース全体で 1 つだけ持つ。
// ミップ単位で別状態にしたくなった時点で拡張する。
struct GpuBuffer {
    ComPtr<D3D12MA::Allocation> allocation;
    ComPtr<ID3D12Resource> resource;
    uint64_t sizeInBytes = 0;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    DescriptorHandle srv;
    DescriptorHandle uav;

    bool IsValid() const { return resource != nullptr; }
    D3D12_GPU_VIRTUAL_ADDRESS GpuAddress() const {
        return resource ? resource->GetGPUVirtualAddress() : 0;
    }
};

struct GpuTexture {
    ComPtr<D3D12MA::Allocation> allocation;
    ComPtr<ID3D12Resource> resource;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mipLevels = 1;
    uint32_t arraySize = 1;
    bool isCube = false;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    DescriptorHandle srv;
    DescriptorHandle uav;  // ミップ 0 の UAV
    DescriptorHandle rtv;
    DescriptorHandle dsv;
    // ミップごとの UAV。プリフィルタのようにミップ単位で書き込むときに使う。
    std::vector<DescriptorHandle> mipUavs;
    // ミップごとの SRV。ミップ連鎖の生成中に、書き込み中のミップと
    // 読み出すミップをサブリソース単位で別状態にするために使う。
    std::vector<DescriptorHandle> mipSrvs;

    bool IsValid() const { return resource != nullptr; }

    // bindless 用のインデックス。シェーダへはこの値を渡す。
    uint32_t SrvIndex() const { return srv.index; }
    uint32_t UavIndex() const { return uav.index; }
    uint32_t MipUavIndex(uint32_t mip) const {
        return (mip < mipUavs.size()) ? mipUavs[mip].index : kInvalidDescriptorIndex;
    }
    uint32_t MipSrvIndex(uint32_t mip) const {
        return (mip < mipSrvs.size()) ? mipSrvs[mip].index : kInvalidDescriptorIndex;
    }

    // ミップ mip の全スライスに対応するサブリソース番号。
    uint32_t SubresourceIndex(uint32_t mip, uint32_t slice) const {
        return mip + slice * mipLevels;
    }
};

struct TextureDesc {
    uint32_t width = 1;
    uint32_t height = 1;
    uint32_t mipLevels = 1;
    // キューブマップは arraySize = 6, isCube = true にする。
    uint32_t arraySize = 1;
    bool isCube = false;
    DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
    bool allowUnorderedAccess = false;
    // ミップごとの UAV も作る。プリフィルタのようにミップ単位で書き込む場合に必要。
    bool createMipUavs = false;
    // ミップごとの SRV も作る。ミップ連鎖の生成に必要。
    bool createMipSrvs = false;
    bool allowRenderTarget = false;
    bool allowDepthStencil = false;
    bool createSrv = true;
    // TYPELESS で作る場合など、ビューのフォーマットをリソースと変えたいときに指定する。
    // 深度テクスチャも SRV とリソースでフォーマットが異なる。
    DXGI_FORMAT srvFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT uavFormat = DXGI_FORMAT_UNKNOWN;
    // 深度を SRV としても読むときに指定する（リソースは TYPELESS で作る）。
    DXGI_FORMAT dsvFormat = DXGI_FORMAT_UNKNOWN;
    float clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float clearDepth = 1.0f;
    D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON;
    const wchar_t* debugName = nullptr;
};

// --- 共通ヘルパ -----------------------------------------------------------
// 各モジュールの匿名名前空間に重複していたものの置き場。
// 状態遷移のロジックはバグの温床なので、必ずここの実装を使う。

// コンピュートのディスパッチ数。groupSize はシェーダの numthreads と一致させること。
inline uint32_t DispatchCount(uint32_t threads, uint32_t groupSize = 8) {
    return (threads + groupSize - 1) / groupSize;
}

// 追跡している状態と違うときだけ、リソース全体を遷移させて状態を更新する。
// texture.state を信頼できる場面でのみ使うこと（サブリソースが分岐中は不可）。
void TransitionIfNeeded(ID3D12GraphicsCommandList* commandList, GpuTexture& texture,
                        D3D12_RESOURCE_STATES newState);

// ミップ mip の全スライスをまとめて遷移させる。texture.state は更新しない
// （ミップ生成のように、サブリソース単位で状態が分岐している間に使うため）。
void TransitionMip(ID3D12GraphicsCommandList* commandList, const GpuTexture& texture,
                   uint32_t mip, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);

// D3D12MemoryAllocator を包み、リソース生成とディスクリプタ確保をまとめて行う。
class ResourceAllocator {
public:
    ResourceAllocator() = default;
    ~ResourceAllocator();

    ResourceAllocator(const ResourceAllocator&) = delete;
    ResourceAllocator& operator=(const ResourceAllocator&) = delete;

    bool Create(ID3D12Device* device, IDXGIAdapter* adapter, DescriptorHeap* srvHeap,
                DescriptorHeap* rtvHeap, DescriptorHeap* dsvHeap);
    void Destroy();

    bool CreateTexture2D(const TextureDesc& desc, GpuTexture& outTexture);

    // アップロードヒープ上のバッファ。CPU から直接書き込む用途に使う。
    bool CreateUploadBuffer(uint64_t sizeInBytes, const wchar_t* debugName, GpuBuffer& outBuffer);

    // DEFAULT ヒープ上のバッファ。頂点・インデックス・構造化バッファ用。
    bool CreateDefaultBuffer(uint64_t sizeInBytes, D3D12_RESOURCE_STATES initialState,
                             const wchar_t* debugName, GpuBuffer& outBuffer);

    // READBACK ヒープ上のバッファ。GPU の結果を CPU 側へ持ってくる用途に使う。
    bool CreateReadbackBuffer(uint64_t sizeInBytes, const wchar_t* debugName,
                              GpuBuffer& outBuffer);

    // ディスクリプタを即座にフリーリストへ返す。GPU がまだ参照している可能性がある場合は
    // これを直接使わず、Device::DeferRelease() で遅延解放すること。
    void ReleaseDescriptors(GpuTexture& texture);
    void ReleaseDescriptors(GpuBuffer& buffer);

    D3D12MA::Allocator* Raw() const { return m_allocator.Get(); }

private:
    ComPtr<D3D12MA::Allocator> m_allocator;
    ID3D12Device* m_device = nullptr;
    DescriptorHeap* m_srvHeap = nullptr;
    DescriptorHeap* m_rtvHeap = nullptr;
    DescriptorHeap* m_dsvHeap = nullptr;
};

}  // namespace tg::rhi
