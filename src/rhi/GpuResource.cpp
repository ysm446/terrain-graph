#include "rhi/GpuResource.h"

#include "core/Log.h"

namespace tg::rhi {

void TransitionIfNeeded(ID3D12GraphicsCommandList* commandList, GpuTexture& texture,
                        D3D12_RESOURCE_STATES newState) {
    if (texture.state == newState) {
        return;
    }
    const auto barrier =
        CD3DX12_RESOURCE_BARRIER::Transition(texture.resource.Get(), texture.state, newState);
    commandList->ResourceBarrier(1, &barrier);
    texture.state = newState;
}

void TransitionMip(ID3D12GraphicsCommandList* commandList, const GpuTexture& texture,
                   uint32_t mip, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    for (uint32_t slice = 0; slice < texture.arraySize; ++slice) {
        const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
            texture.resource.Get(), before, after, texture.SubresourceIndex(mip, slice));
        commandList->ResourceBarrier(1, &barrier);
    }
}

ResourceAllocator::~ResourceAllocator() {
    Destroy();
}

bool ResourceAllocator::Create(ID3D12Device* device, IDXGIAdapter* adapter,
                               DescriptorHeap* srvHeap, DescriptorHeap* rtvHeap,
                               DescriptorHeap* dsvHeap) {
    D3D12MA::ALLOCATOR_DESC desc = {};
    desc.pDevice = device;
    desc.pAdapter = adapter;
    desc.Flags = D3D12MA::ALLOCATOR_FLAG_DEFAULT_POOLS_NOT_ZEROED;

    D3D12MA::Allocator* raw = nullptr;
    if (!TG_CHECK_HR(D3D12MA::CreateAllocator(&desc, &raw))) {
        return false;
    }
    m_allocator.Attach(raw);
    m_device = device;
    m_srvHeap = srvHeap;
    m_rtvHeap = rtvHeap;
    m_dsvHeap = dsvHeap;
    return true;
}

void ResourceAllocator::Destroy() {
    m_allocator.Reset();
    m_device = nullptr;
    m_srvHeap = nullptr;
    m_rtvHeap = nullptr;
    m_dsvHeap = nullptr;
}

bool ResourceAllocator::CreateTexture2D(const TextureDesc& desc, GpuTexture& outTexture) {
    if (!m_allocator) {
        return false;
    }
    // mipLevels = 0（フルミップ連鎖の自動決定）は、ビュー生成や
    // SubresourceIndex の計算が実ミップ数を前提とするため受け付けない。
    if (desc.mipLevels == 0) {
        TG_LOG_ERROR("CreateTexture2D: mipLevels = 0 は未対応です");
        return false;
    }
    // RTV / DSV は現状スライス 0 の 2D ビューしか作らないため、配列とは併用できない。
    if (desc.arraySize > 1 && !desc.isCube &&
        (desc.allowRenderTarget || desc.allowDepthStencil)) {
        TG_LOG_ERROR("CreateTexture2D: 配列テクスチャの RTV / DSV は未対応です");
        return false;
    }

    D3D12_RESOURCE_DESC resourceDesc = {};
    resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resourceDesc.Width = desc.width;
    resourceDesc.Height = desc.height;
    resourceDesc.DepthOrArraySize = static_cast<UINT16>(desc.isCube ? 6 : desc.arraySize);
    resourceDesc.MipLevels = static_cast<UINT16>(desc.mipLevels);
    resourceDesc.Format = desc.format;
    resourceDesc.SampleDesc.Count = 1;
    resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    resourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
    if (desc.allowUnorderedAccess) {
        resourceDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    }
    if (desc.allowRenderTarget) {
        resourceDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    }
    if (desc.allowDepthStencil) {
        resourceDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    }

    // クリア値を渡さないとレンダーターゲット・深度のクリアが最適化されず、
    // デバッグレイヤーからも警告が出る。
    D3D12_CLEAR_VALUE clearValue = {};
    const D3D12_CLEAR_VALUE* clearValuePtr = nullptr;
    if (desc.allowDepthStencil) {
        // TYPELESS のままではクリア値にも DSV にも使えない。
        clearValue.Format =
            (desc.dsvFormat != DXGI_FORMAT_UNKNOWN) ? desc.dsvFormat : desc.format;
        clearValue.DepthStencil.Depth = desc.clearDepth;
        clearValue.DepthStencil.Stencil = 0;
        clearValuePtr = &clearValue;
    } else if (desc.allowRenderTarget) {
        clearValue.Format = desc.format;
        clearValue.Color[0] = desc.clearColor[0];
        clearValue.Color[1] = desc.clearColor[1];
        clearValue.Color[2] = desc.clearColor[2];
        clearValue.Color[3] = desc.clearColor[3];
        clearValuePtr = &clearValue;
    }

    D3D12MA::ALLOCATION_DESC allocDesc = {};
    allocDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

    D3D12MA::Allocation* allocation = nullptr;
    ID3D12Resource* resource = nullptr;
    if (!TG_CHECK_HR(m_allocator->CreateResource(&allocDesc, &resourceDesc, desc.initialState,
                                                 clearValuePtr, &allocation,
                                                 IID_PPV_ARGS(&resource)))) {
        return false;
    }

    outTexture = GpuTexture{};
    outTexture.allocation.Attach(allocation);
    outTexture.resource.Attach(resource);
    outTexture.width = desc.width;
    outTexture.height = desc.height;
    outTexture.mipLevels = desc.mipLevels;
    outTexture.arraySize = desc.isCube ? 6 : desc.arraySize;
    outTexture.isCube = desc.isCube;
    outTexture.format = desc.format;
    outTexture.state = desc.initialState;

    if (desc.debugName != nullptr) {
        outTexture.resource->SetName(desc.debugName);
    }

    if (desc.createSrv && m_srvHeap != nullptr) {
        outTexture.srv = m_srvHeap->Allocate();
        if (!outTexture.srv.IsValid()) {
            ReleaseDescriptors(outTexture);
            outTexture = GpuTexture{};
            return false;
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = (desc.srvFormat != DXGI_FORMAT_UNKNOWN) ? desc.srvFormat : desc.format;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        if (desc.isCube) {
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
            srvDesc.TextureCube.MipLevels = desc.mipLevels;
        } else if (outTexture.arraySize > 1) {
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            srvDesc.Texture2DArray.MipLevels = desc.mipLevels;
            srvDesc.Texture2DArray.ArraySize = outTexture.arraySize;
        } else {
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MipLevels = desc.mipLevels;
        }
        m_device->CreateShaderResourceView(outTexture.resource.Get(), &srvDesc, outTexture.srv.cpu);
    }

    if (desc.allowUnorderedAccess && m_srvHeap != nullptr) {
        // 配列・キューブは TEXTURE2DARRAY として書き込む。
        const bool asArray = (outTexture.arraySize > 1);
        const uint32_t uavMipCount = desc.createMipUavs ? desc.mipLevels : 1;

        outTexture.mipUavs.resize(uavMipCount);
        for (uint32_t mip = 0; mip < uavMipCount; ++mip) {
            DescriptorHandle handle = m_srvHeap->Allocate();
            if (!handle.IsValid()) {
                ReleaseDescriptors(outTexture);
                outTexture = GpuTexture{};
                return false;
            }

            D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
            uavDesc.Format = (desc.uavFormat != DXGI_FORMAT_UNKNOWN) ? desc.uavFormat : desc.format;
            if (asArray) {
                uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
                uavDesc.Texture2DArray.MipSlice = mip;
                uavDesc.Texture2DArray.ArraySize = outTexture.arraySize;
            } else {
                uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                uavDesc.Texture2D.MipSlice = mip;
            }
            m_device->CreateUnorderedAccessView(outTexture.resource.Get(), nullptr, &uavDesc,
                                                handle.cpu);
            outTexture.mipUavs[mip] = handle;
        }
        outTexture.uav = outTexture.mipUavs[0];
    }

    if (desc.createMipSrvs && m_srvHeap != nullptr) {
        outTexture.mipSrvs.resize(desc.mipLevels);
        for (uint32_t mip = 0; mip < desc.mipLevels; ++mip) {
            DescriptorHandle handle = m_srvHeap->Allocate();
            if (!handle.IsValid()) {
                ReleaseDescriptors(outTexture);
                outTexture = GpuTexture{};
                return false;
            }

            D3D12_SHADER_RESOURCE_VIEW_DESC mipSrvDesc = {};
            mipSrvDesc.Format =
                (desc.srvFormat != DXGI_FORMAT_UNKNOWN) ? desc.srvFormat : desc.format;
            mipSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            if (desc.isCube) {
                mipSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
                mipSrvDesc.TextureCube.MostDetailedMip = mip;
                mipSrvDesc.TextureCube.MipLevels = 1;
            } else if (outTexture.arraySize > 1) {
                mipSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                mipSrvDesc.Texture2DArray.MostDetailedMip = mip;
                mipSrvDesc.Texture2DArray.MipLevels = 1;
                mipSrvDesc.Texture2DArray.ArraySize = outTexture.arraySize;
            } else {
                mipSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                mipSrvDesc.Texture2D.MostDetailedMip = mip;
                mipSrvDesc.Texture2D.MipLevels = 1;
            }
            m_device->CreateShaderResourceView(outTexture.resource.Get(), &mipSrvDesc, handle.cpu);
            outTexture.mipSrvs[mip] = handle;
        }
    }

    if (desc.allowRenderTarget && m_rtvHeap != nullptr) {
        outTexture.rtv = m_rtvHeap->Allocate();
        if (!outTexture.rtv.IsValid()) {
            ReleaseDescriptors(outTexture);
            outTexture = GpuTexture{};
            return false;
        }
        m_device->CreateRenderTargetView(outTexture.resource.Get(), nullptr, outTexture.rtv.cpu);
    }

    if (desc.allowDepthStencil && m_dsvHeap != nullptr) {
        outTexture.dsv = m_dsvHeap->Allocate();
        if (!outTexture.dsv.IsValid()) {
            ReleaseDescriptors(outTexture);
            outTexture = GpuTexture{};
            return false;
        }
        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
        dsvDesc.Format = (desc.dsvFormat != DXGI_FORMAT_UNKNOWN) ? desc.dsvFormat : desc.format;
        dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        m_device->CreateDepthStencilView(outTexture.resource.Get(), &dsvDesc, outTexture.dsv.cpu);
    }

    return true;
}

bool ResourceAllocator::CreateDefaultBuffer(uint64_t sizeInBytes,
                                            D3D12_RESOURCE_STATES initialState,
                                            const wchar_t* debugName, GpuBuffer& outBuffer) {
    if (!m_allocator || sizeInBytes == 0) {
        return false;
    }

    D3D12_RESOURCE_DESC resourceDesc = {};
    resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resourceDesc.Width = sizeInBytes;
    resourceDesc.Height = 1;
    resourceDesc.DepthOrArraySize = 1;
    resourceDesc.MipLevels = 1;
    resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
    resourceDesc.SampleDesc.Count = 1;
    resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    resourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    D3D12MA::ALLOCATION_DESC allocDesc = {};
    allocDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

    D3D12MA::Allocation* allocation = nullptr;
    ID3D12Resource* resource = nullptr;
    if (!TG_CHECK_HR(m_allocator->CreateResource(&allocDesc, &resourceDesc, initialState, nullptr,
                                                 &allocation, IID_PPV_ARGS(&resource)))) {
        return false;
    }

    outBuffer = GpuBuffer{};
    outBuffer.allocation.Attach(allocation);
    outBuffer.resource.Attach(resource);
    outBuffer.sizeInBytes = sizeInBytes;
    outBuffer.state = initialState;
    if (debugName != nullptr) {
        outBuffer.resource->SetName(debugName);
    }
    return true;
}

bool ResourceAllocator::CreateUploadBuffer(uint64_t sizeInBytes, const wchar_t* debugName,
                                           GpuBuffer& outBuffer) {
    if (!m_allocator || sizeInBytes == 0) {
        return false;
    }

    D3D12_RESOURCE_DESC resourceDesc = {};
    resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resourceDesc.Width = sizeInBytes;
    resourceDesc.Height = 1;
    resourceDesc.DepthOrArraySize = 1;
    resourceDesc.MipLevels = 1;
    resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
    resourceDesc.SampleDesc.Count = 1;
    resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    resourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    D3D12MA::ALLOCATION_DESC allocDesc = {};
    allocDesc.HeapType = D3D12_HEAP_TYPE_UPLOAD;

    D3D12MA::Allocation* allocation = nullptr;
    ID3D12Resource* resource = nullptr;
    if (!TG_CHECK_HR(m_allocator->CreateResource(&allocDesc, &resourceDesc,
                                                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                 &allocation, IID_PPV_ARGS(&resource)))) {
        return false;
    }

    outBuffer = GpuBuffer{};
    outBuffer.allocation.Attach(allocation);
    outBuffer.resource.Attach(resource);
    outBuffer.sizeInBytes = sizeInBytes;
    outBuffer.state = D3D12_RESOURCE_STATE_GENERIC_READ;
    if (debugName != nullptr) {
        outBuffer.resource->SetName(debugName);
    }
    return true;
}

bool ResourceAllocator::CreateReadbackBuffer(uint64_t sizeInBytes, const wchar_t* debugName,
                                            GpuBuffer& outBuffer) {
    if (!m_allocator || sizeInBytes == 0) {
        return false;
    }

    D3D12_RESOURCE_DESC resourceDesc = {};
    resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resourceDesc.Width = sizeInBytes;
    resourceDesc.Height = 1;
    resourceDesc.DepthOrArraySize = 1;
    resourceDesc.MipLevels = 1;
    resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
    resourceDesc.SampleDesc.Count = 1;
    resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    resourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    D3D12MA::ALLOCATION_DESC allocDesc = {};
    allocDesc.HeapType = D3D12_HEAP_TYPE_READBACK;

    D3D12MA::Allocation* allocation = nullptr;
    ID3D12Resource* resource = nullptr;
    if (!TG_CHECK_HR(m_allocator->CreateResource(&allocDesc, &resourceDesc,
                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                 &allocation, IID_PPV_ARGS(&resource)))) {
        return false;
    }

    outBuffer = GpuBuffer{};
    outBuffer.allocation.Attach(allocation);
    outBuffer.resource.Attach(resource);
    outBuffer.sizeInBytes = sizeInBytes;
    outBuffer.state = D3D12_RESOURCE_STATE_COPY_DEST;
    if (debugName != nullptr) {
        outBuffer.resource->SetName(debugName);
    }
    return true;
}

void ResourceAllocator::ReleaseDescriptors(GpuTexture& texture) {
    if (m_srvHeap != nullptr) {
        m_srvHeap->Free(texture.srv);
        // uav は mipUavs[0] と同じハンドルなので、二重解放しないよう mipUavs だけ返す。
        for (const DescriptorHandle& handle : texture.mipUavs) {
            m_srvHeap->Free(handle);
        }
        texture.mipUavs.clear();
        for (const DescriptorHandle& handle : texture.mipSrvs) {
            m_srvHeap->Free(handle);
        }
        texture.mipSrvs.clear();
    }
    if (m_rtvHeap != nullptr) {
        m_rtvHeap->Free(texture.rtv);
    }
    if (m_dsvHeap != nullptr) {
        m_dsvHeap->Free(texture.dsv);
    }
    texture.srv = DescriptorHandle{};
    texture.uav = DescriptorHandle{};
    texture.rtv = DescriptorHandle{};
    texture.dsv = DescriptorHandle{};
}

void ResourceAllocator::ReleaseDescriptors(GpuBuffer& buffer) {
    if (m_srvHeap == nullptr) {
        return;
    }
    m_srvHeap->Free(buffer.srv);
    m_srvHeap->Free(buffer.uav);
    buffer.srv = DescriptorHandle{};
    buffer.uav = DescriptorHandle{};
}

}  // namespace tg::rhi
