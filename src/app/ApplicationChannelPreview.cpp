// 「チャンネル」パネル。合成結果のチャンネルを、真上から見た 1 枚の絵として見る。
//
// ビューポートの表示切り替え（Shaded の所）は、チャンネルを地形へ貼った状態で見せる。斜面で歪み、
// 視点で隠れ、3D の絵と並べられない。ここは 1 枚のテクスチャとして全体を見るためのもので、
// 3D には出ない水チャンネル（水際からの距離・水深・流れ）も見られる。
//
// 表示用のテクスチャは毎フレーム焼く（ChannelPreview.hlsl）。パネルが見えている間だけ。
// 拡大・パンは UV の範囲として渡すので、寄ってもぼけない。カーソルの位置の元の値は、同じパスが
// 小さなバッファへ書き、数フレーム遅れて読み戻す（測光の読み戻しと同じ流儀）。

#include "app/Application.h"
#include "app/ApplicationUiHelpers.h"
#include "ui/UiStyle.h"

#include <imgui.h>
#include <pix3.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace tg {
namespace {

// 表示用テクスチャの一辺。パネルの大きさに依らず固定（作り直しを避ける）。
constexpr uint32_t kChannelImageSize = 1024;
// 拡大の上限（合成 1 テクセルが十分大きく見える程度）。
constexpr float kChannelMaxZoom = 64.0f;

// シェーダ（ChannelPreview.hlsl）の TG_CHANNEL_* と揃える。
enum ChannelMode : int {
    ChannelBaseColor = 0,
    ChannelNormal,
    ChannelRoughness,
    ChannelMetallic,
    ChannelAo,
    ChannelHeight,
    ChannelWaterCover,
    ChannelWaterDepth,
    ChannelWaterDistance,
    ChannelWaterWave,
    ChannelFlow,
    ChannelRapids,
    ChannelLayers,
    ChannelModeCount,
};

const char* const kChannelPreviewLabels[ChannelModeCount] = {
    "ベースカラー", "法線", "ラフネス", "メタルネス", "AO", "ハイト",
    "水: 水面の被覆", "水: 水深", "水: 水際からの距離", "水: 波の強さ", "水: 流れ", "水: 早瀬",
    "Surface ごとの重み",
};

// 水の場の「水なし」（CompositeLayer.hlsl と揃える）。
constexpr float kWaterNone = -10000.0f;

}  // namespace

// このフレームの UI が出した要求（どのチャンネルを、どの範囲で）に合わせて、表示用のテクスチャを焼く。
// レンダラの描画の後、ImGui の描画より前に呼ぶ。合成結果は描画が読める状態になっている。
void Application::PrepareChannelPreview(ID3D12GraphicsCommandList* commandList) {
    ChannelPreviewState& state = m_channelPreview;
    // 前のフレームまでに書いた「カーソルの位置の値」を読み戻す。
    const uint32_t slot = m_device.FrameIndex();
    if (state.pending[slot] && state.readback.IsValid()) {
        state.pending[slot] = false;
        const SIZE_T offset = sizeof(float) * 4 * slot;
        const D3D12_RANGE readRange = {offset, offset + sizeof(float) * 4};
        void* mapped = nullptr;
        if (SUCCEEDED(state.readback.resource->Map(0, &readRange, &mapped))) {
            std::memcpy(state.probeValue, static_cast<const uint8_t*>(mapped) + offset, sizeof(state.probeValue));
            const D3D12_RANGE writtenRange = {0, 0};
            state.readback.resource->Unmap(0, &writtenRange);
            state.probeValueValid = state.pendingProbe[slot];
        }
    }

    const bool requested = state.requested;
    state.requested = false;
    if (!requested) return;

    const compositor::MaterialTextureSet& textures = m_renderer.Evaluator().Textures();
    if (!textures.IsValid()) return;
    ID3D12PipelineState* pipeline = m_pipelineCache.GetCompute(L"ChannelPreview.hlsl", L"CsMain");
    if (pipeline == nullptr) return;

    if (!state.image.IsValid()) {
        rhi::TextureDesc desc;
        desc.width = kChannelImageSize;
        desc.height = kChannelImageSize;
        desc.format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.allowUnorderedAccess = true;
        // Discard で初期化するために RTV フラグも付ける（TextureLibrary の表示用テクスチャと同じ理由）。
        desc.allowRenderTarget = true;
        desc.createSrv = true;
        desc.initialState = D3D12_RESOURCE_STATE_COMMON;
        desc.debugName = L"ChannelPreview";
        if (!m_device.Allocator().CreateTexture2D(desc, state.image) ||
            !m_device.Allocator().CreateStructuredBuffer(4, sizeof(float), L"ChannelPreviewProbe", state.result, true) ||
            !m_device.Allocator().CreateReadbackBuffer(sizeof(float) * 4 * rhi::kFrameCount,
                                                       L"ChannelPreviewReadback", state.readback)) {
            m_device.DeferRelease(state.image);
            m_device.DeferRelease(state.result);
            m_device.DeferRelease(state.readback);
            return;
        }
        rhi::TransitionIfNeeded(commandList, state.image, D3D12_RESOURCE_STATE_RENDER_TARGET);
        commandList->DiscardResource(state.image.resource.Get(), nullptr);
    }

    const rhi::GpuTexture* source = &textures.baseColor;
    float rangeA = 0.0f;
    switch (state.mode) {
        case ChannelNormal: source = &textures.normal; break;
        case ChannelRoughness:
        case ChannelMetallic:
        case ChannelAo: source = &textures.surface; break;
        case ChannelHeight: source = &textures.height; break;
        case ChannelWaterCover:
        case ChannelRapids: source = &textures.flow; break;
        case ChannelFlow: source = &textures.flow; rangeA = 4.0f; break;          // 速さの上限（m/s）
        case ChannelWaterDepth: source = &textures.water; rangeA = 20.0f; break;  // 一番濃くなる深さ（m）
        case ChannelWaterDistance: source = &textures.water; rangeA = state.contourMeters; break;  // 等値線の間隔（m）
        case ChannelWaterWave: source = &textures.water; break;
        case ChannelLayers: source = &textures.layers; break;
        default: break;
    }
    if (!source->IsValid()) return;

    struct Constants {
        uint32_t sourceIndex, outputIndex, width, height;
        uint32_t mode, resultIndex, probeValid, pad;
        float uvRect[4];
        float probeUv[2];
        float rangeA, rangeB;
    };
    const float half = 0.5f / std::max(state.zoom, 1.0f);
    const Constants constants{source->SrvIndex(), state.image.UavIndex(), kChannelImageSize, kChannelImageSize,
                              static_cast<uint32_t>(state.mode), state.result.uav.index,
                              state.probeHovered ? 1u : 0u, 0u,
                              {state.centerU - half, state.centerV - half, state.centerU + half, state.centerV + half},
                              {state.probeU, state.probeV}, rangeA, 0.0f};
    static_assert(sizeof(Constants) / sizeof(uint32_t) <= rhi::kRootConstantCount);

    PIXBeginEvent(commandList, PIX_COLOR(120, 170, 200), "ChannelPreview");
    rhi::TransitionIfNeeded(commandList, state.image, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    rhi::TransitionIfNeeded(commandList, state.result, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    commandList->SetComputeRootSignature(m_pipelineCache.GlobalRootSignature());
    commandList->SetPipelineState(pipeline);
    commandList->SetComputeRoot32BitConstants(0, sizeof(constants) / sizeof(uint32_t), &constants, 0);
    commandList->Dispatch(rhi::DispatchCount(kChannelImageSize), rhi::DispatchCount(kChannelImageSize), 1);
    // ImGui が SRV として読む。
    rhi::TransitionIfNeeded(commandList, state.image, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    // カーソルの位置の値を読み戻しへ写す。
    rhi::TransitionIfNeeded(commandList, state.result, D3D12_RESOURCE_STATE_COPY_SOURCE);
    commandList->CopyBufferRegion(state.readback.resource.Get(), sizeof(float) * 4 * slot,
                                  state.result.resource.Get(), 0, sizeof(float) * 4);
    state.pending[slot] = true;
    state.pendingProbe[slot] = state.probeHovered;
    state.imageReady = true;
    PIXEndEvent(commandList);
}

void Application::DestroyChannelPreview() {
    m_device.DeferRelease(m_channelPreview.image);
    m_device.DeferRelease(m_channelPreview.result);
    m_device.DeferRelease(m_channelPreview.readback);
    m_channelPreview = ChannelPreviewState{};
}

void Application::DrawChannelPreviewPanel() {
    ChannelPreviewState& state = m_channelPreview;
    // 初めて出すときは、ビューポートと同じ枠へタブで入れる（ini に配置が無いときだけ）。
    // 前面はビューポートのままにする。
    if (m_viewportDockId != 0) ImGui::SetNextWindowDockID(m_viewportDockId, ImGuiCond_FirstUseEver);
    // 開発用（--channel-preview）。撮影で確かめられるよう、このパネルを前面にしてチャンネルを選ぶ。
    if (m_options.channelPreview >= 0) {
        ImGui::SetNextWindowFocus();
        state.mode = m_options.channelPreview;
    }
    const bool open = ImGui::Begin("チャンネル", nullptr,
                                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                                       ImGuiWindowFlags_NoFocusOnAppearing);
    if (!open) {
        state.probeHovered = false;
        ImGui::End();
        return;
    }

    if (ui::BeginPropertyTable("channelPreviewRows")) {
        ui::PropertyCombo("チャンネル", &state.mode, kChannelPreviewLabels, ChannelModeCount, ChannelBaseColor,
                          "合成結果のどのチャンネルを見るか。「水」は水を張るノード（Sea / Lake / River）が"
                          "書く水チャンネル。「Surface ごとの重み」は、どの Surface がどれだけ塗ったか"
                          "（近景マテリアルの割り当てに使う）");
        ui::EndPropertyTable();
    }
    state.mode = std::clamp(state.mode, 0, ChannelModeCount - 1);

    // --- カーソルの位置の値 --------------------------------------------------------
    // 絵の上に 1 行で出す。値は数フレーム遅れて届く。
    // 水際からの距離の等値線は、見えている範囲に合わせて間隔を決める（絵の幅におよそ 30 本。1-2-5 の刻み）。
    // 固定の間隔だと、全体を見たときに線が詰まって縞（モアレ）になる。
    {
        const float visibleMeters = std::max(m_graphStack.SizeMeters(), 1.0f) / std::max(state.zoom, 1.0f);
        const float target = visibleMeters / 30.0f;
        const float decade = std::pow(10.0f, std::floor(std::log10(std::max(target, 1e-3f))));
        const float fraction = target / decade;
        state.contourMeters = decade * (fraction < 1.5f ? 1.0f : fraction < 3.5f ? 2.0f : fraction < 7.5f ? 5.0f : 10.0f);
    }
    char readout[256] = "絵の上にカーソルを載せると、その位置の値を出す";
    if (state.mode == ChannelWaterDistance) {
        std::snprintf(readout, sizeof(readout), "青は水の中、橙は陸。等値線の間隔 %.0f m", state.contourMeters);
    } else if (state.mode == ChannelFlow) {
        std::snprintf(readout, sizeof(readout), "色相が流れの向き、明るさが速さ（4 m/s で最大）");
    } else if (state.mode == ChannelWaterDepth) {
        std::snprintf(readout, sizeof(readout), "青は水の中（濃いほど深い。20 m で最大）、灰は陸");
    } else if (state.mode == ChannelLayers) {
        std::snprintf(readout, sizeof(readout), "色は Surface ごと（下から数えた番号）。重なる所は重みで混ぜた色");
    }
    if (state.probeHovered && state.probeValueValid) {
        const float size = m_renderer.PlaneSize();
        const float x = (state.probeU - 0.5f) * size;
        const float z = (state.probeV - 0.5f) * size;
        const float* v = state.probeValue;
        char value[160] = "";
        switch (state.mode) {
            case ChannelBaseColor: std::snprintf(value, sizeof(value), "リニア (%.3f, %.3f, %.3f)", v[0], v[1], v[2]); break;
            case ChannelNormal: std::snprintf(value, sizeof(value), "法線 xy (%.3f, %.3f)", v[0], v[1]); break;
            case ChannelRoughness: std::snprintf(value, sizeof(value), "ラフネス %.3f", v[0]); break;
            case ChannelMetallic: std::snprintf(value, sizeof(value), "メタルネス %.3f", v[1]); break;
            case ChannelAo: std::snprintf(value, sizeof(value), "AO %.3f", v[2]); break;
            case ChannelHeight:
                std::snprintf(value, sizeof(value), "ハイト %.4f（%.1f m）", v[0], v[0] * m_graphStack.HeightMeters());
                break;
            case ChannelWaterCover: std::snprintf(value, sizeof(value), "水面の被覆 %.2f", v[2]); break;
            case ChannelRapids: std::snprintf(value, sizeof(value), "早瀬 %.2f", v[3]); break;
            case ChannelWaterWave: std::snprintf(value, sizeof(value), "波の強さ %.2f", v[2]); break;
            case ChannelLayers: {
                // ID は Surface を下から数えた番号（MaterialEvaluator と揃える）。名前を引いて添える。
                const auto surfaceName = [&](int id) -> const char* {
                    int count = 0;
                    for (const compositor::MaterialLayer& layer : m_graphStack.Layers()) {
                        if (layer.kind == compositor::LayerKind::Surface && ++count == id) return layer.name.c_str();
                    }
                    return "なし";
                };
                const int first = static_cast<int>(v[0] + 0.5f), second = static_cast<int>(v[2] + 0.5f);
                if (v[3] > 0.004f) {
                    std::snprintf(value, sizeof(value), "%d %s %.0f%% / %d %s %.0f%%", first, surfaceName(first),
                                  v[1] * 100.0f, second, surfaceName(second), v[3] * 100.0f);
                } else {
                    std::snprintf(value, sizeof(value), "%d %s %.0f%%", first, surfaceName(first), v[1] * 100.0f);
                }
                break;
            }
            case ChannelWaterDepth:
                if (v[1] < kWaterNone * 0.5f) std::snprintf(value, sizeof(value), "水の場なし");
                else if (v[1] > 0.0f) std::snprintf(value, sizeof(value), "水深 %.2f m", v[1]);
                else std::snprintf(value, sizeof(value), "陸（水位から %.2f m 上）", -v[1]);
                break;
            case ChannelWaterDistance:
                if (v[1] < kWaterNone * 0.5f) std::snprintf(value, sizeof(value), "水の場なし");
                else std::snprintf(value, sizeof(value), "水際から %.1f m（%s）", std::abs(v[0]), v[0] > 0.0f ? "水の中" : "陸");
                break;
            case ChannelFlow: {
                const float speed = std::hypot(v[0], v[1]);
                if (speed < 1e-4f) std::snprintf(value, sizeof(value), "流れなし");
                else std::snprintf(value, sizeof(value), "流速 %.2f m/s（X %+.2f, Z %+.2f）", speed, v[0], v[1]);
                break;
            }
            default: break;
        }
        std::snprintf(readout, sizeof(readout), "X %.0f m, Z %.0f m  |  %s", x, z, value);
    }
    ImGui::TextDisabled("%s", readout);

    // --- 絵 ------------------------------------------------------------------------
    // 残りの領域いっぱいに正方形で置く。ホイールで拡大（カーソルの位置を中心に）、ドラッグでパン、
    // ダブルクリックで全体へ戻す。
    const ImVec2 available = ImGui::GetContentRegionAvail();
    const float edge = std::floor(std::min(available.x, available.y));
    state.probeHovered = false;
    if (edge >= 16.0f) {
        const ImVec2 cursor = ImGui::GetCursorScreenPos();
        const ImVec2 min(cursor.x + std::floor((available.x - edge) * 0.5f), cursor.y);
        const ImVec2 max(min.x + edge, min.y + edge);
        ImGui::SetCursorScreenPos(min);
        ImGui::InvisibleButton("##channelImage", ImVec2(edge, edge));
        const bool hovered = ImGui::IsItemHovered();
        const bool active = ImGui::IsItemActive();
        const ImGuiIO& io = ImGui::GetIO();

        const auto clampCenter = [&]() {
            const float half = 0.5f / state.zoom;
            state.centerU = std::clamp(state.centerU, half, 1.0f - half);
            state.centerV = std::clamp(state.centerV, half, 1.0f - half);
        };
        const auto uvAt = [&](const ImVec2& position, float& u, float& v) {
            const float half = 0.5f / state.zoom;
            u = state.centerU - half + (position.x - min.x) / edge * (2.0f * half);
            v = state.centerV - half + (position.y - min.y) / edge * (2.0f * half);
        };
        if (hovered && io.MouseWheel != 0.0f) {
            float u = 0.0f, v = 0.0f;
            uvAt(io.MousePos, u, v);
            const float zoom = std::clamp(state.zoom * std::pow(1.25f, io.MouseWheel), 1.0f, kChannelMaxZoom);
            // カーソルの下の点が動かないように中心をずらす。
            const float ratio = state.zoom / zoom;
            state.centerU = u + (state.centerU - u) * ratio;
            state.centerV = v + (state.centerV - v) * ratio;
            state.zoom = zoom;
        }
        if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
            state.centerU -= io.MouseDelta.x / edge / state.zoom;
            state.centerV -= io.MouseDelta.y / edge / state.zoom;
        }
        if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            state.zoom = 1.0f;
            state.centerU = state.centerV = 0.5f;
        }
        clampCenter();
        if (hovered) {
            uvAt(io.MousePos, state.probeU, state.probeV);
            state.probeHovered = state.probeU >= 0.0f && state.probeU <= 1.0f && state.probeV >= 0.0f &&
                                 state.probeV <= 1.0f;
            ImGui::SetMouseCursor(active ? ImGuiMouseCursor_ResizeAll : ImGuiMouseCursor_Arrow);
        }

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(min, max, ImGui::GetColorU32(ImGuiCol_FrameBg));
        if (state.imageReady && state.image.IsValid()) {
            drawList->AddImage(static_cast<ImTextureID>(state.image.srv.gpu.ptr), min, max);
        }
        drawList->AddRect(min, max, ImGui::GetColorU32(ImGuiCol_Border));
        if (state.zoom > 1.0f) {
            char zoomText[32];
            std::snprintf(zoomText, sizeof(zoomText), "%.1fx", state.zoom);
            drawList->AddText(ImVec2(min.x + ui::Scaled(6.0f), max.y - ImGui::GetTextLineHeight() - ui::Scaled(4.0f)),
                              ImGui::GetColorU32(ImGuiCol_TextDisabled), zoomText);
        }
        // このフレームの絵を焼いてもらう（PrepareChannelPreview）。
        state.requested = true;
    }
    ImGui::End();
}

}  // namespace tg
