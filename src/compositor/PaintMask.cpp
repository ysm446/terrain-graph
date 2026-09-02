#include "compositor/PaintMask.h"

#include "core/Log.h"

#include <pix3.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace tg::compositor {
namespace {

using rhi::DispatchCount;
// ペイントマスクのフォーマット。8bit あればブラシの積み上げには足りる。
constexpr DXGI_FORMAT kPaintFormat = DXGI_FORMAT_R8_UNORM;
// アンドゥの段数。1024 の R8 で 1 MB / 段。
constexpr size_t kMaxUndoSteps = 24;
// ブラシパスの groupshared 配列と一致させること。
constexpr uint32_t kMaxStrokeSamples = 64;

// 合成パスが読む状態。既定はここに戻しておく。
constexpr D3D12_RESOURCE_STATES kReadState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

// 履歴テクスチャは常に COMMON で置いておき、コピーの前後だけ遷移させる。
// Op が GpuTexture のコピー（ComPtr の共有）を持つため、
// 状態を構造体側で追い切れないことによる。
void TransitionHistory(ID3D12GraphicsCommandList* commandList, ID3D12Resource* resource,
                       D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(resource, from, to);
    commandList->ResourceBarrier(1, &barrier);
}

}  // namespace

void PaintMaskStore::Destroy(rhi::Device& device) {
    for (PaintMaskEntry& entry : m_entries) {
        device.DeferRelease(entry.texture);
    }
    m_entries.clear();
    m_pending.clear();
    ReleaseSnapshots(device, m_undo);
    ReleaseSnapshots(device, m_redo);
}

bool PaintMaskStore::CreateMaskTexture(rhi::Device& device, uint32_t resolution,
                                       rhi::GpuTexture& outTexture) {
    rhi::TextureDesc desc;
    desc.width = resolution;
    desc.height = resolution;
    desc.format = kPaintFormat;
    desc.allowUnorderedAccess = true;
    desc.createSrv = true;
    desc.initialState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    desc.debugName = L"PaintMask";
    return device.Allocator().CreateTexture2D(desc, outTexture);
}

bool PaintMaskStore::CreateHistoryTexture(rhi::Device& device, uint32_t resolution,
                                          rhi::GpuTexture& outTexture) {
    // 履歴はコピーの相手にしかならないので、ディスクリプタは要らない。
    rhi::TextureDesc desc;
    desc.width = resolution;
    desc.height = resolution;
    desc.format = kPaintFormat;
    desc.createSrv = false;
    desc.initialState = D3D12_RESOURCE_STATE_COMMON;
    desc.debugName = L"PaintMaskHistory";
    return device.Allocator().CreateTexture2D(desc, outTexture);
}

PaintMaskId PaintMaskStore::Add(rhi::Device& device, float initialValue) {
    PaintMaskEntry entry;
    if (!CreateMaskTexture(device, m_resolution, entry.texture)) {
        return kNoPaintMask;
    }
    entry.id = m_nextId++;
    m_entries.push_back(std::move(entry));

    const PaintMaskId id = m_entries.back().id;
    QueueFill(id, initialValue);
    return id;
}

PaintMaskId PaintMaskStore::Duplicate(rhi::Device& device, PaintMaskId source) {
    const PaintMaskEntry* sourceEntry = Find(source);
    if (sourceEntry == nullptr) {
        return kNoPaintMask;
    }
    PaintMaskEntry entry;
    if (!CreateMaskTexture(device, m_resolution, entry.texture)) {
        return kNoPaintMask;
    }
    entry.id = m_nextId++;
    m_entries.push_back(std::move(entry));

    // レイヤーを複製したときにテクスチャを共有しないよう、中身ごと写す。
    // Restore（履歴 = 常に COMMON）へ生きているマスクを流用すると
    // 状態追跡が壊れるため、専用の CopyMask を使う。
    Op op;
    op.type = OpType::CopyMask;
    op.target = m_entries.back().id;
    op.copySource = source;
    m_pending.push_back(std::move(op));
    return m_entries.back().id;
}

void PaintMaskStore::Remove(rhi::Device& device, PaintMaskId id) {
    const auto it = std::find_if(m_entries.begin(), m_entries.end(),
                                 [id](const PaintMaskEntry& entry) { return entry.id == id; });
    if (it == m_entries.end()) {
        return;
    }

    // 破棄するマスクを対象にした要求と履歴を先に片付ける。
    // テクスチャ本体とディスクリプタの解放はフレーム同期後（DeferRelease）なので、
    // GPU 待機は要らない。
    m_pending.erase(std::remove_if(m_pending.begin(), m_pending.end(),
                                   [id](const Op& op) { return op.target == id; }),
                    m_pending.end());

    const auto dropSnapshots = [&](std::vector<Snapshot>& stack) {
        for (Snapshot& snapshot : stack) {
            if (snapshot.id == id) {
                device.DeferRelease(snapshot.texture);
            }
        }
        stack.erase(std::remove_if(stack.begin(), stack.end(),
                                   [id](const Snapshot& s) { return s.id == id; }),
                    stack.end());
    };
    dropSnapshots(m_undo);
    dropSnapshots(m_redo);

    device.DeferRelease(it->texture);
    m_entries.erase(it);
}

std::vector<PaintMaskId> PaintMaskStore::Ids() const {
    std::vector<PaintMaskId> ids;
    ids.reserve(m_entries.size());
    for (const PaintMaskEntry& entry : m_entries) {
        ids.push_back(entry.id);
    }
    return ids;
}

const PaintMaskEntry* PaintMaskStore::Find(PaintMaskId id) const {
    if (id == kNoPaintMask) {
        return nullptr;
    }
    for (const PaintMaskEntry& entry : m_entries) {
        if (entry.id == id) {
            return &entry;
        }
    }
    return nullptr;
}

PaintMaskEntry* PaintMaskStore::FindMutable(PaintMaskId id) {
    return const_cast<PaintMaskEntry*>(Find(id));
}

uint32_t PaintMaskStore::SrvIndex(PaintMaskId id) const {
    const PaintMaskEntry* entry = Find(id);
    return (entry != nullptr) ? entry->texture.SrvIndex() : kInvalidTextureIndex;
}

std::vector<uint8_t> PaintMaskStore::ReadPixels(rhi::Device& device, PaintMaskId id) const {
    const PaintMaskEntry* entry = Find(id);
    if (entry == nullptr) {
        return {};
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT rowCount = 0;
    UINT64 rowSizeInBytes = 0;
    UINT64 totalBytes = 0;
    const D3D12_RESOURCE_DESC desc = entry->texture.resource->GetDesc();
    device.GetDevice()->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rowCount,
                                              &rowSizeInBytes, &totalBytes);

    rhi::GpuBuffer readback;
    if (!device.Allocator().CreateReadbackBuffer(totalBytes, L"PaintMaskReadback", readback)) {
        return {};
    }

    // 状態は entry 側で追っているが、この関数は const なので書き戻さない。
    // 読み出しの前後で必ず元の状態へ戻す。
    const D3D12_RESOURCE_STATES previousState = entry->texture.state;
    const bool executed = device.ExecuteImmediate([&](ID3D12GraphicsCommandList* commandList) {
        PIXBeginEvent(commandList, PIX_COLOR(200, 120, 200), "PaintMaskReadback");
        if (previousState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
            const auto toCopySource = CD3DX12_RESOURCE_BARRIER::Transition(
                entry->texture.resource.Get(), previousState, D3D12_RESOURCE_STATE_COPY_SOURCE);
            commandList->ResourceBarrier(1, &toCopySource);
        }

        const CD3DX12_TEXTURE_COPY_LOCATION destination(readback.resource.Get(), footprint);
        const CD3DX12_TEXTURE_COPY_LOCATION source(entry->texture.resource.Get(), 0);
        commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);

        if (previousState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
            const auto restore = CD3DX12_RESOURCE_BARRIER::Transition(
                entry->texture.resource.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, previousState);
            commandList->ResourceBarrier(1, &restore);
        }
        PIXEndEvent(commandList);
    });

    std::vector<uint8_t> pixels;
    if (executed) {
        void* mapped = nullptr;
        const D3D12_RANGE readRange = {0, static_cast<SIZE_T>(totalBytes)};
        if (TG_CHECK_HR(readback.resource->Map(0, &readRange, &mapped))) {
            const auto* source = static_cast<const uint8_t*>(mapped) + footprint.Offset;
            // 行ピッチは 256 バイト境界へ揃えられているので、詰め直して返す。
            pixels.resize(static_cast<size_t>(m_resolution) * m_resolution);
            for (uint32_t row = 0; row < m_resolution; ++row) {
                std::memcpy(pixels.data() + static_cast<size_t>(row) * m_resolution,
                            source + static_cast<size_t>(row) * footprint.Footprint.RowPitch,
                            m_resolution);
            }
            const D3D12_RANGE writtenRange = {0, 0};
            readback.resource->Unmap(0, &writtenRange);
        }
    }

    device.Defer(readback.resource);
    device.Defer(readback.allocation);
    return pixels;
}

PaintMaskId PaintMaskStore::AddFromPixels(rhi::Device& device, uint32_t resolution,
                                          const std::vector<uint8_t>& pixels) {
    if (resolution == 0 ||
        pixels.size() < static_cast<size_t>(resolution) * resolution) {
        return kNoPaintMask;
    }
    // 解像度はストア全体で共通。空のときだけ画像に合わせる。
    if (m_entries.empty()) {
        m_resolution = resolution;
        m_requestedResolution = resolution;
    }
    if (resolution != m_resolution) {
        TG_LOG_ERROR("ペイントマスクの解像度が揃っていません (%u != %u)", resolution,
                     m_resolution);
        return kNoPaintMask;
    }

    PaintMaskEntry entry;
    if (!CreateMaskTexture(device, m_resolution, entry.texture)) {
        return kNoPaintMask;
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT rowCount = 0;
    UINT64 rowSizeInBytes = 0;
    UINT64 totalBytes = 0;
    const D3D12_RESOURCE_DESC desc = entry.texture.resource->GetDesc();
    device.GetDevice()->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rowCount,
                                              &rowSizeInBytes, &totalBytes);

    rhi::GpuBuffer staging;
    if (!device.Allocator().CreateUploadBuffer(totalBytes, L"PaintMaskStaging", staging)) {
        device.DeferRelease(entry.texture);
        return kNoPaintMask;
    }

    void* mapped = nullptr;
    const D3D12_RANGE readRange = {0, 0};
    if (!TG_CHECK_HR(staging.resource->Map(0, &readRange, &mapped))) {
        device.DeferRelease(entry.texture);
        return kNoPaintMask;
    }
    auto* destination = static_cast<uint8_t*>(mapped) + footprint.Offset;
    for (uint32_t row = 0; row < m_resolution; ++row) {
        std::memcpy(destination + static_cast<size_t>(row) * footprint.Footprint.RowPitch,
                    pixels.data() + static_cast<size_t>(row) * m_resolution, m_resolution);
    }
    staging.resource->Unmap(0, nullptr);

    const bool executed = device.ExecuteImmediate([&](ID3D12GraphicsCommandList* commandList) {
        PIXBeginEvent(commandList, PIX_COLOR(200, 120, 200), "PaintMaskUpload");
        const auto toCopyDest = CD3DX12_RESOURCE_BARRIER::Transition(
            entry.texture.resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_COPY_DEST);
        commandList->ResourceBarrier(1, &toCopyDest);

        const CD3DX12_TEXTURE_COPY_LOCATION destinationLocation(entry.texture.resource.Get(), 0);
        const CD3DX12_TEXTURE_COPY_LOCATION sourceLocation(staging.resource.Get(), footprint);
        commandList->CopyTextureRegion(&destinationLocation, 0, 0, 0, &sourceLocation, nullptr);

        const auto toRead = CD3DX12_RESOURCE_BARRIER::Transition(
            entry.texture.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, kReadState);
        commandList->ResourceBarrier(1, &toRead);
        PIXEndEvent(commandList);
    });

    device.Defer(staging.resource);
    device.Defer(staging.allocation);
    if (!executed) {
        device.DeferRelease(entry.texture);
        return kNoPaintMask;
    }

    entry.texture.state = kReadState;
    entry.id = m_nextId++;
    m_entries.push_back(std::move(entry));
    return m_entries.back().id;
}

void PaintMaskStore::Clear(rhi::Device& device) {
    m_pending.clear();
    ReleaseSnapshots(device, m_undo);
    ReleaseSnapshots(device, m_redo);
    for (PaintMaskEntry& entry : m_entries) {
        device.DeferRelease(entry.texture);
    }
    m_entries.clear();
}

void PaintMaskStore::ProcessPendingWork(rhi::Device& device, rhi::PipelineCache& pipelineCache) {
    if (m_requestedResolution == m_resolution) {
        return;
    }

    const uint32_t resolution = m_requestedResolution;
    if (m_entries.empty()) {
        m_resolution = resolution;
        return;
    }

    // ミップを 1 段作るパスは、そのまま双一次のリサンプルとして使える。
    ID3D12PipelineState* pipeline = pipelineCache.GetCompute(L"TextureMips.hlsl", L"CsMain");
    if (pipeline == nullptr) {
        m_requestedResolution = m_resolution;
        return;
    }

    device.WaitForGpu();

    struct Resized {
        PaintMaskEntry* entry = nullptr;
        rhi::GpuTexture texture;
    };
    std::vector<Resized> resized;
    resized.reserve(m_entries.size());

    for (PaintMaskEntry& entry : m_entries) {
        Resized item;
        item.entry = &entry;
        if (!CreateMaskTexture(device, resolution, item.texture)) {
            TG_LOG_ERROR("ペイントマスクの作り直しに失敗しました");
            for (Resized& created : resized) {
                device.DeferRelease(created.texture);
            }
            m_requestedResolution = m_resolution;
            return;
        }
        resized.push_back(std::move(item));
    }

    struct MipConstants {
        uint32_t sourceIndex;
        uint32_t outputIndex;
        uint32_t outputWidth;
        uint32_t outputHeight;
    };

    const bool executed = device.ExecuteImmediate([&](ID3D12GraphicsCommandList* commandList) {
        PIXBeginEvent(commandList, PIX_COLOR(200, 120, 200), "PaintMaskResize");
        commandList->SetComputeRootSignature(pipelineCache.GlobalRootSignature());
        commandList->SetPipelineState(pipeline);

        for (Resized& item : resized) {
            TransitionIfNeeded(commandList, item.entry->texture, kReadState);

            const MipConstants constants{item.entry->texture.SrvIndex(), item.texture.UavIndex(),
                                         resolution, resolution};
            commandList->SetComputeRoot32BitConstants(0, sizeof(constants) / sizeof(uint32_t),
                                                      &constants, 0);
            commandList->Dispatch(DispatchCount(resolution), DispatchCount(resolution), 1);
        }
        PIXEndEvent(commandList);
    });

    if (!executed) {
        for (Resized& item : resized) {
            device.DeferRelease(item.texture);
        }
        m_requestedResolution = m_resolution;
        return;
    }

    for (Resized& item : resized) {
        device.DeferRelease(item.entry->texture);
        item.entry->texture = std::move(item.texture);
    }

    // 履歴は解像度が変わると使えない。アンドゥの段は捨てる。
    // 積んであった履歴のコピー要求（Capture / Restore）もサイズ不一致で
    // 成立しないため、あわせて捨てる。
    m_pending.erase(std::remove_if(m_pending.begin(), m_pending.end(),
                                   [](const Op& op) {
                                       return op.type == OpType::Capture ||
                                              op.type == OpType::Restore;
                                   }),
                    m_pending.end());
    ReleaseSnapshots(device, m_undo);
    ReleaseSnapshots(device, m_redo);

    m_resolution = resolution;
}

void PaintMaskStore::QueueStroke(const BrushStroke& stroke) {
    if (Find(stroke.target) == nullptr) {
        return;
    }
    Op op;
    op.type = OpType::Stroke;
    op.target = stroke.target;
    op.stroke = stroke;
    m_pending.push_back(std::move(op));
}

void PaintMaskStore::QueueFill(PaintMaskId id, float value) {
    if (Find(id) == nullptr) {
        return;
    }
    Op op;
    op.type = OpType::Fill;
    op.target = id;
    op.value = value;
    m_pending.push_back(std::move(op));
}

bool PaintMaskStore::PushHistory(rhi::Device& device, PaintMaskId id,
                                 std::vector<Snapshot>& stack) {
    if (Find(id) == nullptr) {
        return false;
    }

    Snapshot snapshot;
    snapshot.id = id;
    if (!CreateHistoryTexture(device, m_resolution, snapshot.texture)) {
        return false;
    }

    Op op;
    op.type = OpType::Capture;
    op.target = id;
    op.history = snapshot.texture;
    m_pending.push_back(std::move(op));

    stack.push_back(std::move(snapshot));
    return true;
}

void PaintMaskStore::QueueSnapshot(rhi::Device& device, PaintMaskId id) {
    if (!PushHistory(device, id, m_undo)) {
        return;
    }

    // 新しい操作を積んだのでリドゥは無効になる。
    ReleaseSnapshots(device, m_redo);

    if (m_undo.size() > kMaxUndoSteps) {
        device.DeferRelease(m_undo.front().texture);
        m_undo.erase(m_undo.begin());
    }
}

void PaintMaskStore::QueueUndo(rhi::Device& device) {
    if (m_undo.empty()) {
        return;
    }

    Snapshot snapshot = std::move(m_undo.back());
    m_undo.pop_back();

    // 戻す前の内容をリドゥ側へ退避してから書き戻す。
    PushHistory(device, snapshot.id, m_redo);

    Op op;
    op.type = OpType::Restore;
    op.target = snapshot.id;
    op.history = snapshot.texture;
    op.releaseHistory = true;
    m_pending.push_back(std::move(op));
}

void PaintMaskStore::QueueRedo(rhi::Device& device) {
    if (m_redo.empty()) {
        return;
    }

    Snapshot snapshot = std::move(m_redo.back());
    m_redo.pop_back();

    PushHistory(device, snapshot.id, m_undo);

    Op op;
    op.type = OpType::Restore;
    op.target = snapshot.id;
    op.history = snapshot.texture;
    op.releaseHistory = true;
    m_pending.push_back(std::move(op));
}

void PaintMaskStore::ReleaseSnapshots(rhi::Device& device, std::vector<Snapshot>& stack) {
    for (Snapshot& snapshot : stack) {
        device.DeferRelease(snapshot.texture);
    }
    stack.clear();
}

bool PaintMaskStore::Process(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                             ID3D12GraphicsCommandList* commandList,
                             const PaintContext& context) {
    if (m_pending.empty()) {
        return false;
    }

    ID3D12PipelineState* fillPipeline = pipelineCache.GetCompute(L"PaintFill.hlsl", L"CsMain");
    ID3D12PipelineState* brushPipeline = pipelineCache.GetCompute(L"PaintBrush.hlsl", L"CsMain");
    if (fillPipeline == nullptr || brushPipeline == nullptr) {
        m_pending.clear();
        return false;
    }

    PIXBeginEvent(commandList, PIX_COLOR(200, 120, 200), "PaintMasks");
    commandList->SetComputeRootSignature(pipelineCache.GlobalRootSignature());

    bool recorded = false;
    std::vector<rhi::GpuTexture> releaseAfterRecord;

    for (Op& op : m_pending) {
        PaintMaskEntry* entry = FindMutable(op.target);
        if (entry == nullptr) {
            continue;
        }

        switch (op.type) {
            case OpType::Fill: {
                TransitionIfNeeded(commandList, entry->texture,
                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

                struct FillConstants {
                    uint32_t outputIndex;
                    uint32_t width;
                    uint32_t height;
                    float value;
                };
                const FillConstants constants{entry->texture.UavIndex(), m_resolution,
                                              m_resolution, op.value};

                commandList->SetPipelineState(fillPipeline);
                commandList->SetComputeRoot32BitConstants(0, sizeof(constants) / sizeof(uint32_t),
                                                          &constants, 0);
                commandList->Dispatch(DispatchCount(m_resolution), DispatchCount(m_resolution), 1);
                recorded = true;
                break;
            }

            case OpType::Stroke: {
                if (context.uvBufferSrvIndex == kInvalidTextureIndex ||
                    context.viewportWidth == 0 || context.viewportHeight == 0) {
                    break;
                }

                TransitionIfNeeded(commandList, entry->texture,
                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

                // 線分の長さから標本数を決める。マウスが速く動いても点線に
                // ならないよう、半径の半分ごとに 1 点を目安にする。
                const float dx = op.stroke.toX - op.stroke.fromX;
                const float dy = op.stroke.toY - op.stroke.fromY;
                const float length = std::sqrt(dx * dx + dy * dy);
                const float spacing = std::max(op.stroke.brush.radiusPixels * 0.5f, 1.0f);
                const auto sampleCount = static_cast<uint32_t>(std::clamp(
                    length / spacing + 1.0f, 1.0f, static_cast<float>(kMaxStrokeSamples)));

                struct BrushConstants {
                    uint32_t uvBufferIndex;
                    uint32_t outputIndex;
                    uint32_t resolution;
                    uint32_t sampleCount;
                    uint32_t viewportWidth;
                    uint32_t viewportHeight;
                    float radiusPixels;
                    float strength;
                    float fromX;
                    float fromY;
                    float toX;
                    float toY;
                    float falloff;
                    uint32_t erase;
                };
                const BrushConstants constants{context.uvBufferSrvIndex,
                                               entry->texture.UavIndex(),
                                               m_resolution,
                                               sampleCount,
                                               context.viewportWidth,
                                               context.viewportHeight,
                                               op.stroke.brush.radiusPixels,
                                               op.stroke.brush.strength,
                                               op.stroke.fromX,
                                               op.stroke.fromY,
                                               op.stroke.toX,
                                               op.stroke.toY,
                                               op.stroke.brush.falloff,
                                               op.stroke.brush.erase ? 1u : 0u};

                commandList->SetPipelineState(brushPipeline);
                commandList->SetComputeRoot32BitConstants(0, sizeof(constants) / sizeof(uint32_t),
                                                          &constants, 0);
                commandList->Dispatch(DispatchCount(m_resolution), DispatchCount(m_resolution), 1);
                recorded = true;
                break;
            }

            case OpType::Capture: {
                TransitionIfNeeded(commandList, entry->texture, D3D12_RESOURCE_STATE_COPY_SOURCE);
                TransitionHistory(commandList, op.history.resource.Get(),
                                  D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
                commandList->CopyResource(op.history.resource.Get(),
                                          entry->texture.resource.Get());
                TransitionHistory(commandList, op.history.resource.Get(),
                                  D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
                recorded = true;
                break;
            }

            case OpType::CopyMask: {
                PaintMaskEntry* sourceEntry = FindMutable(op.copySource);
                if (sourceEntry == nullptr) {
                    break;
                }
                TransitionIfNeeded(commandList, sourceEntry->texture,
                                   D3D12_RESOURCE_STATE_COPY_SOURCE);
                TransitionIfNeeded(commandList, entry->texture, D3D12_RESOURCE_STATE_COPY_DEST);
                commandList->CopyResource(entry->texture.resource.Get(),
                                          sourceEntry->texture.resource.Get());
                recorded = true;
                break;
            }

            case OpType::Restore: {
                TransitionIfNeeded(commandList, entry->texture, D3D12_RESOURCE_STATE_COPY_DEST);
                TransitionHistory(commandList, op.history.resource.Get(),
                                  D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
                commandList->CopyResource(entry->texture.resource.Get(),
                                          op.history.resource.Get());
                TransitionHistory(commandList, op.history.resource.Get(),
                                  D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
                if (op.releaseHistory) {
                    releaseAfterRecord.push_back(op.history);
                }
                recorded = true;
                break;
            }
        }
    }

    m_pending.clear();

    // 合成パスが読める状態へ戻す。
    for (PaintMaskEntry& entry : m_entries) {
        TransitionIfNeeded(commandList, entry.texture, kReadState);
    }

    PIXEndEvent(commandList);

    // 消費した履歴はフレーム同期後に解放する。
    for (rhi::GpuTexture& texture : releaseAfterRecord) {
        device.DeferRelease(texture);
    }

    return recorded;
}

}  // namespace tg::compositor
